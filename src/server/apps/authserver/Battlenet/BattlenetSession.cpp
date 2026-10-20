/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "BattlenetSession.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"
#include "LoginRestServer.h"
#include "RealmList.h"
#include "bgs/low/pb/client/account_service.pb.h"
#include "bgs/low/pb/client/authentication_service.pb.h"
#include "bgs/low/pb/client/challenge_service.pb.h"
#include "bgs/low/pb/client/connection_service.pb.h"
#include "bgs/low/pb/client/game_utilities_service.pb.h"
#include "bgs/low/pb/client/rpc_types.pb.h"
#include <boost/asio/write.hpp>
#include <boost/json.hpp>
#include <zlib.h>
#include <array>
#include <chrono>
#include <optional>

namespace
{
    // The client stops reading a frame whose body claims more than this (FUN_141f25650).
    constexpr uint32 MAX_BODY_SIZE = 0xFFFFF;
    constexpr std::size_t READ_CHUNK = 4096;

    // Our ProcessId on every connection; the label must not be 0 (rpc_types.proto).
    constexpr uint32 SERVER_PROCESS_LABEL = 1;

    uint32 ServerEpoch()
    {
        static uint32 const epoch = uint32(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        return epoch;
    }

    enum ConnectionMethod : uint32
    {
        CONNECTION_CONNECT            = 1,
        CONNECTION_BIND               = 2,
        CONNECTION_ECHO               = 3,
        CONNECTION_FORCE_DISCONNECT   = 4,
        CONNECTION_KEEP_ALIVE         = 5,
        CONNECTION_ENCRYPT            = 6,
        CONNECTION_REQUEST_DISCONNECT = 7
    };

    enum AuthenticationMethod : uint32
    {
        AUTHENTICATION_LOGON                    = 1,
        AUTHENTICATION_VERIFY_WEB_CREDENTIALS   = 7,
        AUTHENTICATION_GENERATE_WEB_CREDENTIALS = 8,

        AUTHENTICATION_LISTENER_ON_LOGON_COMPLETE = 5
    };

    enum AccountMethod : uint32
    {
        ACCOUNT_GET_ACCOUNT_STATE      = 30,
        ACCOUNT_GET_GAME_ACCOUNT_STATE = 31
    };

    enum GameUtilitiesMethod : uint32
    {
        GAME_UTILITIES_PROCESS_CLIENT_REQUEST        = 1,
        GAME_UTILITIES_GET_ALL_VALUES_FOR_ATTRIBUTE  = 10
    };

    constexpr uint32 CHALLENGE_LISTENER_ON_EXTERNAL_CHALLENGE = 3;

    // "WoW" as the fixed32 program FourCC, and the region the game accounts live in.
    constexpr uint32 PROGRAM_WOW = 0x576F57;
    constexpr uint32 BNET_REGION = 1;
    // The one sub-region every realm is in; the client echoes it back opaquely.
    constexpr char const* SUB_REGION = "1-1-0";

    // EntityId shapes the client checks (FUN_141def5c0, FUN_141def4e0).
    void SetAccountEntity(bgs::protocol::EntityId* entity, uint32 accountId)
    {
        entity->set_high(UI64LIT(0x100000000000000));
        entity->set_low(accountId);
    }

    void SetGameAccountEntity(bgs::protocol::EntityId* entity, uint32 accountId)
    {
        entity->set_high(UI64LIT(0x200000000000000) | (uint64(BNET_REGION) << 32) | PROGRAM_WOW);
        entity->set_low(accountId);
    }

    // GameUtilities commands carry a suffix (CVar auroraClientRequestSuffix, "wotlk1"): match the
    // name up to it.
    bool IsCommand(std::string const& name, std::string_view command)
    {
        return name.size() >= command.size() && std::string_view(name).substr(0, command.size()) == command;
    }

    bgs::protocol::Attribute const* FindAttribute(google::protobuf::RepeatedPtrField<bgs::protocol::Attribute> const& attributes,
        std::string_view name)
    {
        for (bgs::protocol::Attribute const& attribute : attributes)
            if (attribute.name() == name)
                return &attribute;
        return nullptr;
    }

    // A client blob is "<TypeName>:" + JSON + "\0", uncompressed (FUN_142575f90).
    boost::json::value ParseClientJson(std::string const& blob, std::string_view typeName)
    {
        std::string_view text(blob);
        while (!text.empty() && text.back() == '\0')
            text.remove_suffix(1);
        if (text.size() <= typeName.size() || text.substr(0, typeName.size()) != typeName || text[typeName.size()] != ':')
            return nullptr;

        boost::system::error_code error;
        boost::json::value json = boost::json::parse(text.substr(typeName.size() + 1), error);
        return error ? boost::json::value(nullptr) : json;
    }

    // A server blob is a u32 LE uncompressed size, then a zlib stream of "<TypeName>:" + JSON + "\0"
    // (JamJSONZlib.cpp, FUN_142582120).
    std::string MakeServerJson(std::string_view typeName, boost::json::value const& json)
    {
        std::string text = std::string(typeName) + ":" + boost::json::serialize(json);
        text.push_back('\0');

        uLongf compressedSize = compressBound(uLong(text.size()));
        std::string blob(4 + compressedSize, '\0');
        uint32 size = uint32(text.size());
        blob[0] = char(size);
        blob[1] = char(size >> 8);
        blob[2] = char(size >> 16);
        blob[3] = char(size >> 24);
        if (compress2(reinterpret_cast<Bytef*>(blob.data() + 4), &compressedSize,
            reinterpret_cast<Bytef const*>(text.data()), uLong(text.size()), Z_BEST_COMPRESSION) != Z_OK)
            return {};

        blob.resize(4 + compressedSize);
        return blob;
    }

    // Realm addresses pack region, site and realm id (the "%d-%u-%u" formatter FUN_14155a080).
    uint32 RealmAddress(uint32 realmId)
    {
        return (BNET_REGION << 24) | (1u << 16) | (realmId & 0xFFFF);
    }

    boost::json::object GameVersion()
    {
        return { { "versionMajor", 3 }, { "versionMinor", 4 }, { "versionRevision", 3 }, { "versionBuild", 54261 } };
    }

    boost::json::object RealmEntry(Realm const& realm)
    {
        uint32 population = 1; // low
        if (realm.Flags & REALM_FLAG_FULL)
            population = 3;
        else if (realm.PopulationLevel > 1.5f)
            population = 2;

        boost::json::object entry;
        entry["wowRealmAddress"] = RealmAddress(realm.Id.Realm);
        entry["cfgTimezonesID"] = realm.Timezone;
        entry["populationState"] = (realm.Flags & REALM_FLAG_OFFLINE) ? 0 : population;
        entry["cfgCategoriesID"] = realm.Timezone;
        entry["version"] = GameVersion();
        entry["cfgRealmsID"] = realm.Id.Realm;
        // the worldserver marks a running realm with REALM_FLAG_VERSION_MISMATCH in realmlist
        // (worldserver Main.cpp); to the client that bit means an incompatible realm, and the
        // version travels in `version` here, so neither build flag is passed on
        entry["flags"] = uint32(realm.Flags) & ~uint32(REALM_FLAG_VERSION_MISMATCH | REALM_FLAG_SPECIFYBUILD);
        entry["name"] = realm.Name;
        entry["cfgConfigsID"] = realm.Type;
        entry["cfgContentSetID"] = 0;
        entry["cfgLanguagesID"] = 1;
        return entry;
    }
}

namespace Battlenet
{
    Session::Session(boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context& sslContext)
        : _socket(std::move(socket), sslContext), _readSize(0), _closed(false), _requestToken(0), _accountId(0)
    {
        boost::system::error_code error;
        auto endpoint = _socket.next_layer().remote_endpoint(error);
        _remote = error ? std::string("?") : endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
        _readBuffer.resize(READ_CHUNK);
    }

    void Session::Start()
    {
        LOG_DEBUG("server.battlenet", "{} connected, starting TLS", _remote);
        _socket.async_handshake(boost::asio::ssl::stream_base::server,
            [self = shared_from_this()](boost::system::error_code const& error)
            {
                if (error)
                {
                    LOG_INFO("server.battlenet", "{} TLS handshake failed: {}", self->_remote, error.message());
                    self->Close();
                    return;
                }

                self->ReadMore();
            });
    }

    void Session::ReadMore()
    {
        if (_closed)
            return;

        if (_readBuffer.size() - _readSize < READ_CHUNK)
            _readBuffer.resize(_readSize + READ_CHUNK);

        _socket.async_read_some(boost::asio::buffer(_readBuffer.data() + _readSize, _readBuffer.size() - _readSize),
            [self = shared_from_this()](boost::system::error_code const& error, std::size_t transferred)
            {
                self->OnRead(error, transferred);
            });
    }

    void Session::OnRead(boost::system::error_code const& error, std::size_t transferred)
    {
        if (error)
        {
            LOG_DEBUG("server.battlenet", "{} read ended: {}", _remote, error.message());
            Close();
            return;
        }

        _readSize += transferred;
        if (!ParseFrames())
        {
            Close();
            return;
        }

        ReadMore();
    }

    bool Session::ParseFrames()
    {
        std::size_t offset = 0;
        while (_readSize - offset >= 2)
        {
            uint8 const* frame = _readBuffer.data() + offset;
            std::size_t headerSize = (std::size_t(frame[0]) << 8) | frame[1];
            if (_readSize - offset < 2 + headerSize)
                break;

            bgs::protocol::Header header;
            if (!header.ParseFromArray(frame + 2, int(headerSize)) || !header.IsInitialized())
            {
                LOG_INFO("server.battlenet", "{} sent an unreadable RPC header", _remote);
                return false;
            }

            if (header.size() > MAX_BODY_SIZE)
            {
                LOG_INFO("server.battlenet", "{} sent an RPC body of {} bytes", _remote, header.size());
                return false;
            }

            std::size_t total = 2 + headerSize + header.size();
            if (_readSize - offset < total)
                break;

            uint8 const* body = frame + 2 + headerSize;
            if (header.service_id() == RESPONSE_SERVICE_ID)
                HandleResponse(header);
            else
                HandleRequest(header, body, header.size());

            if (_closed)
                return true;

            offset += total;
        }

        if (offset)
        {
            std::copy(_readBuffer.begin() + offset, _readBuffer.begin() + _readSize, _readBuffer.begin());
            _readSize -= offset;
        }

        return true;
    }

    void Session::HandleRequest(bgs::protocol::Header const& header, uint8 const* body, std::size_t size)
    {
        // The client runs bindless (ConnectRequest.use_bindless_rpc): the service is named by hash.
        switch (header.service_hash())
        {
            case SERVICE_CONNECTION:
                HandleConnectionService(header, body, size);
                return;
            case SERVICE_AUTHENTICATION:
                HandleAuthenticationService(header, body, size);
                return;
            case SERVICE_ACCOUNT:
                HandleAccountService(header, body, size);
                return;
            case SERVICE_GAME_UTILITIES:
                HandleGameUtilitiesService(header, body, size);
                return;
            default:
                LOG_DEBUG("server.battlenet", "{} called unhandled service {:08X} method {}", _remote,
                    header.service_hash(), header.method_id());
                SendResponse(header.token(), RPC_STATUS_INVALID_SERVICE, nullptr);
                return;
        }
    }

    void Session::HandleResponse(bgs::protocol::Header const& header)
    {
        // The server makes no calls of its own yet, so nothing waits on a response.
        LOG_DEBUG("server.battlenet", "{} answered token {} with status {}", _remote, header.token(), header.status());
    }

    void Session::HandleConnectionService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size)
    {
        switch (header.method_id())
        {
            case CONNECTION_CONNECT:
            {
                bgs::protocol::connection::v1::ConnectRequest request;
                if (!request.ParseFromArray(body, int(size)))
                {
                    LOG_INFO("server.battlenet", "{} sent an unreadable ConnectRequest", _remote);
                    Close();
                    return;
                }

                bgs::protocol::connection::v1::ConnectResponse response;
                response.mutable_server_id()->set_label(SERVER_PROCESS_LABEL);
                response.mutable_server_id()->set_epoch(ServerEpoch());
                if (request.has_client_id())
                    *response.mutable_client_id() = request.client_id();
                response.set_server_time(uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
                response.set_use_bindless_rpc(request.use_bindless_rpc());

                LOG_DEBUG("server.battlenet", "{} Connect (sdk {}, bindless {})", _remote,
                    request.client_sdk_info().sdk_name(), request.use_bindless_rpc());
                SendResponse(header.token(), RPC_STATUS_OK, &response);
                return;
            }
            case CONNECTION_ECHO:
            {
                bgs::protocol::connection::v1::EchoRequest request;
                if (!request.ParseFromArray(body, int(size)))
                {
                    Close();
                    return;
                }

                bgs::protocol::connection::v1::EchoResponse response;
                if (request.has_time())
                    response.set_time(request.time());
                if (request.has_payload())
                    response.set_payload(request.payload());
                SendResponse(header.token(), RPC_STATUS_OK, &response);
                return;
            }
            case CONNECTION_KEEP_ALIVE:
                // NO_RESPONSE.
                return;
            case CONNECTION_REQUEST_DISCONNECT:
            {
                bgs::protocol::connection::v1::DisconnectRequest request;
                request.ParseFromArray(body, int(size));
                LOG_DEBUG("server.battlenet", "{} asked to disconnect ({})", _remote, request.error_code());
                Close();
                return;
            }
            default:
                SendResponse(header.token(), RPC_STATUS_NOT_IMPLEMENTED, nullptr);
                return;
        }
    }

    void Session::HandleAuthenticationService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size)
    {
        switch (header.method_id())
        {
            case AUTHENTICATION_LOGON:
            {
                bgs::protocol::authentication::v1::LogonRequest request;
                if (!request.ParseFromArray(body, int(size)))
                {
                    SendResponse(header.token(), RPC_STATUS_BAD_REQUEST, nullptr);
                    return;
                }

                _locale = request.locale();
                LOG_DEBUG("server.battlenet", "{} Logon (program {}, platform {}, locale {}, version {}, cached credentials {})",
                    _remote, request.program(), request.platform(), request.locale(), request.application_version(),
                    request.has_cached_web_credentials());

                // Re-login with cached credentials takes no challenge (a challenge then is the
                // client's fatal 0x80000134); none are issued yet, so such a logon is refused.
                if (request.has_cached_web_credentials())
                {
                    SendResponse(header.token(), RPC_STATUS_DENIED, nullptr);
                    return;
                }

                bgs::protocol::NoData response;
                SendResponse(header.token(), RPC_STATUS_OK, &response);

                // The URL is the payload, used verbatim (FUN_141481250); payload_type is not read.
                bgs::protocol::challenge::v1::ChallengeExternalRequest challenge;
                challenge.set_payload_type("web_auth_url");
                challenge.set_payload(sLoginRestServer.GetFormUrl());
                SendRequest(SERVICE_CHALLENGE_LISTENER, CHALLENGE_LISTENER_ON_EXTERNAL_CHALLENGE, challenge);
                return;
            }
            case AUTHENTICATION_VERIFY_WEB_CREDENTIALS:
            {
                bgs::protocol::authentication::v1::VerifyWebCredentialsRequest request;
                if (!request.ParseFromArray(body, int(size)))
                {
                    SendResponse(header.token(), RPC_STATUS_BAD_REQUEST, nullptr);
                    return;
                }

                std::optional<LoginTicket> ticket = sLoginRestServer.RedeemTicket(request.web_credentials());
                if (!ticket)
                {
                    LOG_INFO("server.battlenet", "{} presented an unknown or expired login ticket", _remote);
                    SendResponse(header.token(), RPC_STATUS_DENIED, nullptr);
                    return;
                }

                _accountId = ticket->AccountId;
                _accountName = ticket->AccountName;
                _sessionKey.resize(64);
                Acore::Crypto::GetRandomBytes(_sessionKey.data(), _sessionKey.size());

                bgs::protocol::NoData response;
                SendResponse(header.token(), RPC_STATUS_OK, &response);

                bgs::protocol::authentication::v1::LogonResult result;
                result.set_error_code(0);
                SetAccountEntity(result.mutable_account_id(), _accountId);
                SetGameAccountEntity(result.add_game_account_id(), _accountId);
                result.add_available_region(BNET_REGION);
                result.set_connected_region(BNET_REGION);
                result.set_battle_tag(_accountName + "#1");
                result.set_geoip_country("US");
                result.set_session_key(_sessionKey.data(), _sessionKey.size());
                result.set_restricted_mode(false);
                SendRequest(SERVICE_AUTHENTICATION_LISTENER, AUTHENTICATION_LISTENER_ON_LOGON_COMPLETE, result);

                LOG_INFO("server.battlenet", "{} logged in as {} (account {})", _remote, _accountName, _accountId);
                return;
            }
            case AUTHENTICATION_GENERATE_WEB_CREDENTIALS:
                // A reconnect token; a failure here is not fatal to the client (FUN_14148d430).
                SendResponse(header.token(), RPC_STATUS_NOT_IMPLEMENTED, nullptr);
                return;
            default:
                LOG_DEBUG("server.battlenet", "{} called unhandled AuthenticationService method {}", _remote, header.method_id());
                SendResponse(header.token(), RPC_STATUS_NOT_IMPLEMENTED, nullptr);
                return;
        }
    }

    void Session::HandleAccountService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size)
    {
        if (!IsAuthenticated())
        {
            SendResponse(header.token(), RPC_STATUS_DENIED, nullptr);
            return;
        }

        switch (header.method_id())
        {
            case ACCOUNT_GET_ACCOUNT_STATE:
            {
                // Only the status is checked (FUN_141481020).
                bgs::protocol::account::v1::GetAccountStateResponse response;
                response.mutable_state()->mutable_privacy_info()->set_is_using_rid(false);
                response.mutable_tags();
                SendResponse(header.token(), RPC_STATUS_OK, &response);
                return;
            }
            case ACCOUNT_GET_GAME_ACCOUNT_STATE:
            {
                bgs::protocol::account::v1::GetGameAccountStateRequest request;
                request.ParseFromArray(body, int(size));
                if (request.game_account_id().low() != _accountId)
                {
                    SendResponse(header.token(), RPC_STATUS_DENIED, nullptr);
                    return;
                }

                // game_level_info.name is the game account's display name (FUN_141481e90).
                bgs::protocol::account::v1::GetGameAccountStateResponse response;
                auto* state = response.mutable_state();
                state->mutable_game_level_info()->set_name("WoW1");
                state->mutable_game_level_info()->set_program(PROGRAM_WOW);
                state->mutable_game_status()->set_program(PROGRAM_WOW);
                response.mutable_tags();
                SendResponse(header.token(), RPC_STATUS_OK, &response);
                return;
            }
            default:
                LOG_DEBUG("server.battlenet", "{} called unhandled AccountService method {}", _remote, header.method_id());
                SendResponse(header.token(), RPC_STATUS_NOT_IMPLEMENTED, nullptr);
                return;
        }
    }

    void Session::HandleGameUtilitiesService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size)
    {
        if (!IsAuthenticated())
        {
            SendResponse(header.token(), RPC_STATUS_DENIED, nullptr);
            return;
        }

        switch (header.method_id())
        {
            case GAME_UTILITIES_PROCESS_CLIENT_REQUEST:
            {
                bgs::protocol::game_utilities::v1::ClientRequest request;
                if (!request.ParseFromArray(body, int(size)) || request.attribute_size() == 0)
                {
                    SendResponse(header.token(), RPC_STATUS_BAD_REQUEST, nullptr);
                    return;
                }

                // The command is always the first attribute (FUN_142575b80).
                std::string const& command = request.attribute(0).name();
                bgs::protocol::game_utilities::v1::ClientResponse response;
                uint32 status = RPC_STATUS_NOT_IMPLEMENTED;
                if (IsCommand(command, "Command_RealmListTicketRequest_v1"))
                    status = HandleRealmListTicketRequest(request, response);
                else if (IsCommand(command, "Command_RealmListRequest_v1"))
                    status = HandleRealmListRequest(request, response);
                else if (IsCommand(command, "Command_RealmJoinRequest_v1"))
                    status = HandleRealmJoinRequest(request, response);
                // Command_LastCharPlayedRequest_v1: an error is not fatal and makes the client ask
                // for the realm list (FUN_1414e0280).

                LOG_DEBUG("server.battlenet", "{} {} -> status {}", _remote, command, status);
                SendResponse(header.token(), status, status == RPC_STATUS_OK ? &response : nullptr);
                return;
            }
            case GAME_UTILITIES_GET_ALL_VALUES_FOR_ATTRIBUTE:
            {
                bgs::protocol::game_utilities::v1::GetAllValuesForAttributeRequest request;
                request.ParseFromArray(body, int(size));

                // The sub-region list: one string_value per sub-region (FUN_1414e0820).
                bgs::protocol::game_utilities::v1::GetAllValuesForAttributeResponse response;
                if (IsCommand(request.attribute_key(), "Command_RealmListRequest_v1"))
                    response.add_attribute_value()->set_string_value(SUB_REGION);
                SendResponse(header.token(), RPC_STATUS_OK, &response);
                return;
            }
            default:
                LOG_DEBUG("server.battlenet", "{} called unhandled GameUtilities method {}", _remote, header.method_id());
                SendResponse(header.token(), RPC_STATUS_NOT_IMPLEMENTED, nullptr);
                return;
        }
    }

    uint32 Session::HandleRealmListTicketRequest(bgs::protocol::game_utilities::v1::ClientRequest const& request,
        bgs::protocol::game_utilities::v1::ClientResponse& response)
    {
        // Param_ClientInfo carries the client's 32-byte secret, the second half of the key
        // material the realm join builds (bnet-login-flow.ANALYSIS.md 5.2, 5.5).
        if (bgs::protocol::Attribute const* clientInfo = FindAttribute(request.attribute(), "Param_ClientInfo"))
        {
            boost::json::value json = ParseClientJson(clientInfo->value().blob_value(), "JSONRealmListTicketClientInformation");
            boost::json::value const* secret = json.is_object() ? json.as_object().if_contains("info") : nullptr;
            secret = secret && secret->is_object() ? secret->as_object().if_contains("secret") : nullptr;
            if (secret && secret->is_array() && secret->as_array().size() == 32)
            {
                _clientSecret.clear();
                for (boost::json::value const& byte : secret->as_array())
                    _clientSecret.push_back(uint8(byte.to_number<int64>()));
            }
        }

        if (_clientSecret.size() != 32)
            LOG_INFO("server.battlenet", "{} sent no client secret with the realm list ticket request", _remote);

        bgs::protocol::Attribute* ticket = response.add_attribute();
        ticket->set_name("Param_RealmListTicket");
        ticket->mutable_value()->set_blob_value("AuthRealmListTicket");
        return RPC_STATUS_OK;
    }

    uint32 Session::HandleRealmListRequest(bgs::protocol::game_utilities::v1::ClientRequest const& /*request*/,
        bgs::protocol::game_utilities::v1::ClientResponse& response)
    {
        boost::json::array updates, counts;
        for (auto const& [handle, realm] : sRealmList->GetRealms())
        {
            boost::json::object update;
            update["wowRealmAddress"] = RealmAddress(realm.Id.Realm);
            update["update"] = RealmEntry(realm);
            update["deleting"] = false;
            updates.push_back(std::move(update));

            LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_NUM_CHARS_ON_REALM);
            stmt->SetData(0, realm.Id.Realm);
            stmt->SetData(1, _accountId);
            uint32 count = 0;
            if (PreparedQueryResult result = LoginDatabase.Query(stmt))
                count = (*result)[0].Get<uint8>();
            counts.push_back(boost::json::object{ { "wowRealmAddress", RealmAddress(realm.Id.Realm) }, { "count", count } });
        }

        // Both are required (0x80000135 otherwise); an empty realm list fails the login.
        bgs::protocol::Attribute* realmList = response.add_attribute();
        realmList->set_name("Param_RealmList");
        realmList->mutable_value()->set_blob_value(MakeServerJson("JSONRealmListUpdates", boost::json::object{ { "updates", updates } }));

        bgs::protocol::Attribute* characterCounts = response.add_attribute();
        characterCounts->set_name("Param_CharacterCountList");
        characterCounts->mutable_value()->set_blob_value(MakeServerJson("JSONRealmCharacterCountList", boost::json::object{ { "counts", counts } }));
        return RPC_STATUS_OK;
    }

    uint32 Session::HandleRealmJoinRequest(bgs::protocol::game_utilities::v1::ClientRequest const& request,
        bgs::protocol::game_utilities::v1::ClientResponse& response)
    {
        bgs::protocol::Attribute const* address = FindAttribute(request.attribute(), "Param_RealmAddress");
        if (!address)
            return RPC_STATUS_BAD_REQUEST;

        Realm const* joined = nullptr;
        for (auto const& [handle, realm] : sRealmList->GetRealms())
            if (RealmAddress(realm.Id.Realm) == address->value().uint_value())
                joined = &realm;

        if (!joined || (joined->Flags & REALM_FLAG_OFFLINE) || !joined->ExternalAddress)
        {
            LOG_INFO("server.battlenet", "{} asked to join unknown or offline realm {:08X}", _remote, address->value().uint_value());
            return RPC_STATUS_DENIED;
        }

        // The key material is [JoinSecret][client secret] (FUN_1414833f0); the world server derives
        // the session from it (world-handshake.ANALYSIS.md 3.3), so it is stored before the reply.
        if (_clientSecret.size() != 32)
            return RPC_STATUS_DENIED;

        std::array<uint8, 32> joinSecret = Acore::Crypto::GetRandomBytes<32>();
        std::array<uint8, 64> joinKey;
        std::copy(joinSecret.begin(), joinSecret.end(), joinKey.begin());
        std::copy(_clientSecret.begin(), _clientSecret.end(), joinKey.begin() + 32);

        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_BNET_JOIN_KEY);
        stmt->SetData(0, joinKey);
        stmt->SetData(1, _accountId);
        LoginDatabase.DirectExecute(stmt);

        boost::json::object family;
        family["family"] = 1;
        family["addresses"] = boost::json::array{
            boost::json::object{ { "ip", joined->ExternalAddress->to_string() }, { "port", joined->Port } }
        };

        bgs::protocol::Attribute* ticket = response.add_attribute();
        ticket->set_name("Param_RealmJoinTicket");
        ticket->mutable_value()->set_blob_value(_accountName);

        bgs::protocol::Attribute* addresses = response.add_attribute();
        addresses->set_name("Param_ServerAddresses");
        addresses->mutable_value()->set_blob_value(MakeServerJson("JSONRealmListServerIPAddresses",
            boost::json::object{ { "families", boost::json::array{ family } } }));

        bgs::protocol::Attribute* secret = response.add_attribute();
        secret->set_name("Param_JoinSecret");
        secret->mutable_value()->set_blob_value(joinSecret.data(), joinSecret.size());

        LOG_INFO("server.battlenet", "{} ({}) joins realm {} at {}:{}", _remote, _accountName, joined->Name,
            joined->ExternalAddress->to_string(), joined->Port);
        return RPC_STATUS_OK;
    }

    void Session::SendRequest(uint32 serviceHash, uint32 methodId, google::protobuf::Message const& body)
    {
        bgs::protocol::Header header;
        header.set_service_id(0);
        header.set_service_hash(serviceHash);
        header.set_method_id(methodId);
        header.set_token(++_requestToken);
        SendFrame(header, &body);
    }

    void Session::SendResponse(uint32 token, uint32 status, google::protobuf::Message const* body)
    {
        bgs::protocol::Header header;
        header.set_service_id(RESPONSE_SERVICE_ID);
        header.set_token(token);
        if (status != RPC_STATUS_OK)
            header.set_status(status);
        SendFrame(header, body);
    }

    void Session::SendFrame(bgs::protocol::Header const& header, google::protobuf::Message const* body)
    {
        bgs::protocol::Header framed = header;
        std::size_t bodySize = body ? body->ByteSizeLong() : 0;
        framed.set_size(uint32(bodySize));

        std::size_t headerSize = framed.ByteSizeLong();
        std::vector<uint8> frame(2 + headerSize + bodySize);
        frame[0] = uint8(headerSize >> 8);
        frame[1] = uint8(headerSize);
        framed.SerializeToArray(frame.data() + 2, int(headerSize));
        if (body)
            body->SerializeToArray(frame.data() + 2 + headerSize, int(bodySize));

        bool idle = _writeQueue.empty();
        _writeQueue.push_back(std::move(frame));
        if (idle)
            WriteNext();
    }

    void Session::WriteNext()
    {
        if (_closed || _writeQueue.empty())
            return;

        boost::asio::async_write(_socket, boost::asio::buffer(_writeQueue.front()),
            [self = shared_from_this()](boost::system::error_code const& error, std::size_t /*transferred*/)
            {
                if (error)
                {
                    LOG_DEBUG("server.battlenet", "{} write failed: {}", self->_remote, error.message());
                    self->Close();
                    return;
                }

                self->_writeQueue.pop_front();
                self->WriteNext();
            });
    }

    void Session::Close()
    {
        if (_closed)
            return;

        _closed = true;
        boost::system::error_code error;
        _socket.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
        _socket.next_layer().close(error);
    }
}
