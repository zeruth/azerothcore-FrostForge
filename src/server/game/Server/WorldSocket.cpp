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

#include "TC9Sidecar.h"
#include "WorldSocket.h"
#include "AccountMgr.h"
#include "Config.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "Ed25519.h"
#include "GameTime.h"
#include "HMAC.h"
#include "IPLocation.h"
#include "Opcodes.h"
#include "PacketLog.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "SessionKeyGenerator.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "RBAC.h"
#include <memory>
#include <mutex>

using boost::asio::ip::tcp;

// The 3.4.3.54261 world connection (.agents/plans/frostforge-server/world-handshake.ANALYSIS.md):
// a line each way, then SMSG_AUTH_CHALLENGE, CMSG_AUTH_SESSION checked against the Battle.net join
// key, an Ed25519ctx-signed SMSG_ENTER_ENCRYPTED_MODE, and AES-128-GCM from its acknowledgement on.
namespace
{
    std::string const ServerConnectionInitialize("WORLD OF WARCRAFT CONNECTION - SERVER TO CLIENT - V2\n");
    std::string const ClientConnectionInitialize("WORLD OF WARCRAFT CONNECTION - CLIENT TO SERVER - V2\n");

    // The client's constants (0x1427b4348..0x1427b43d8, and the Win64 auth seed FUN_1402271a0 builds).
    std::array<uint8, 16> const AuthCheckSeed = { 0xC5, 0xC6, 0x98, 0x95, 0x76, 0x3F, 0x1D, 0xCD, 0xB6, 0xA1, 0x37, 0x28, 0xB3, 0x12, 0xFF, 0x8A };
    std::array<uint8, 16> const SessionKeySeed = { 0x58, 0xCB, 0xCF, 0x40, 0xFE, 0x2E, 0xCE, 0xA6, 0x5A, 0x90, 0xB8, 0x01, 0x68, 0x6C, 0x28, 0x0B };
    std::array<uint8, 16> const EncryptionKeySeed = { 0xE9, 0x75, 0x3C, 0x50, 0x90, 0x93, 0x61, 0xDA, 0x3B, 0x07, 0xEE, 0xFA, 0xFF, 0x9D, 0x41, 0xB8 };
    std::array<uint8, 16> const EnableEncryptionSeed = { 0x90, 0x9C, 0xD0, 0x50, 0x5A, 0x2C, 0x14, 0xDD, 0x5C, 0x2C, 0xC0, 0x64, 0x14, 0xF3, 0xFE, 0xC9 };
    std::array<uint8, 16> const EnableEncryptionContext = { 0xA7, 0x1F, 0xB6, 0x9B, 0xC9, 0x7C, 0xDD, 0x96, 0xE9, 0xBB, 0xB8, 0x21, 0x39, 0x8D, 0x5A, 0xD4 };
    std::array<uint8, 16> const Win64AuthSeed = { 0x25, 0xFD, 0x81, 0x24, 0x75, 0xDC, 0xF2, 0x6F, 0x9F, 0x13, 0x83, 0xAE, 0xD3, 0x7F, 0xC9, 0x9E };

    constexpr uint32 IV_MAGIC_CLIENT = 0x544E4C43; // "CLNT"
    constexpr uint32 IV_MAGIC_SERVER = 0x52565253; // "SRVR"

    Acore::Crypto::AES::IV MakeIV(uint64 counter, uint32 magic)
    {
        Acore::Crypto::AES::IV iv;
        for (std::size_t i = 0; i < 8; ++i)
            iv[i] = uint8(counter >> (8 * i));
        for (std::size_t i = 0; i < 4; ++i)
            iv[8 + i] = uint8(magic >> (8 * i));
        return iv;
    }

    // The key SMSG_ENTER_ENCRYPTED_MODE is signed with; the client carries its public half.
    Acore::Crypto::Ed25519 const* EnterEncryptedModeKey()
    {
        static Acore::Crypto::Ed25519 key;
        static std::once_flag loaded;
        std::call_once(loaded, []
        {
            std::string file = sConfigMgr->GetOption<std::string>("EnterEncryptedModePrivateKeyFile", "worldserver-ed25519.key.pem");
            if (!key.LoadPrivateKeyFromFile(file))
                LOG_ERROR("network", "WorldSocket: cannot load the Ed25519 key {}; no client can enter encrypted mode", file);
        });
        return key.IsLoaded() ? &key : nullptr;
    }
}

WorldSocket::WorldSocket(IoContextTcpSocket&& socket)
    : Socket(std::move(socket)), _decrypt(false), _encrypt(true), _recvCounter(0), _sendCounter(0), _decryptEnabled(false),
    _encryptEnabled(false), _connectionStringReceived(false), _OverSpeedPings(0), _worldSession(nullptr), _authed(false),
    _sendBufferSize(4096), _loggingPackets(false)
{
    Acore::Crypto::GetRandomBytes(_dosChallenge);
    Acore::Crypto::GetRandomBytes(_serverChallenge);
    _sessionKey.fill(0);
    _headerBuffer.Resize(sizeof(WorldPacketHeader));
}

WorldSocket::~WorldSocket() = default;

void WorldSocket::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_IP_INFO);
    stmt->SetData(0, ip_address);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&WorldSocket::CheckIpCallback, this, std::placeholders::_1)));
}

void WorldSocket::CheckIpCallback(PreparedQueryResult result)
{
    if (result)
    {
        bool banned = false;
        do
        {
            Field* fields = result->Fetch();
            if (fields[0].Get<uint64>() != 0)
                banned = true;

        } while (result->NextRow());

        if (banned)
        {
            LOG_ERROR("network", "WorldSocket::CheckIpCallback: IP {} is banned, closing.", GetRemoteIpAddress().to_string());
            CloseSocket();
            return;
        }
    }

    // The server speaks first: its line, raw, before any header (FUN_141235030).
    MessageBuffer initializer;
    initializer.Write(ServerConnectionInitialize.data(), ServerConnectionInitialize.size());
    QueuePacket(std::move(initializer));

    AsyncRead();
}

bool WorldSocket::Update()
{
    EncryptableAndCompressiblePacket* queued;
    if (_bufferQueue.Dequeue(queued))
    {
        // Allocate buffer only when it's needed but not on every Update() call.
        MessageBuffer buffer(_sendBufferSize);
        do
        {
            uint16 opcode = queued->GetOpcode();
            std::vector<uint8> payload(sizeof(opcode) + queued->size());
            payload[0] = uint8(opcode);
            payload[1] = uint8(opcode >> 8);
            if (!queued->empty())
                std::memcpy(payload.data() + sizeof(opcode), queued->contents(), queued->size());

            WorldPacketHeader header;
            header.Size = uint32(payload.size());
            std::memset(header.Tag, 0, sizeof(header.Tag));
            if (queued->NeedsEncryption())
                EncryptSend(payload.data(), payload.size(), header.Tag);
            else
                ++_sendCounter;
            EndianConvert(header.Size);

            std::size_t packetSize = sizeof(header) + payload.size();
            if (buffer.GetRemainingSpace() < packetSize)
            {
                if (buffer.GetActiveSize() > 0)
                    QueuePacket(std::move(buffer));
                buffer.Resize(std::max(_sendBufferSize, packetSize));
            }

            buffer.Write(&header, sizeof(header));
            buffer.Write(payload.data(), payload.size());

            delete queued;
        } while (_bufferQueue.Dequeue(queued));

        if (buffer.GetActiveSize() > 0)
            QueuePacket(std::move(buffer));
    }

    if (!BaseSocket::Update())
        return false;

    _queryProcessor.ProcessReadyCallbacks();

    return true;
}

void WorldSocket::SendAuthChallenge()
{
    // DosZeroBits 0: the client's proof of work is then 0 and is not checked (FUN_1412323a0)
    WorldPacket packet(SMSG_AUTH_CHALLENGE, 32 + 16 + 1);
    packet.append(_dosChallenge);
    packet.append(_serverChallenge);
    packet << uint8(0);

    SendPacketAndLogOpcode(packet);
}

void WorldSocket::OnClose()
{
    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);
        _worldSession = nullptr;
    }
}

bool WorldSocket::ReadConnectionString(MessageBuffer& packet)
{
    std::size_t wanted = ClientConnectionInitialize.size() - _connectionString.size();
    std::size_t taken = std::min(packet.GetActiveSize(), wanted);
    _connectionString.append(reinterpret_cast<char const*>(packet.GetReadPointer()), taken);
    packet.ReadCompleted(taken);

    if (_connectionString.size() < ClientConnectionInitialize.size())
        return true;

    if (_connectionString != ClientConnectionInitialize)
    {
        LOG_ERROR("network", "WorldSocket: {} sent a wrong connection string", GetRemoteIpAddress().to_string());
        return false;
    }

    _connectionStringReceived = true;
    SendAuthChallenge();
    return true;
}

SocketReadCallbackResult WorldSocket::ReadHandler()
{
    if (!IsOpen())
        return SocketReadCallbackResult::Stop;

    MessageBuffer& packet = GetReadBuffer();
    while (packet.GetActiveSize() > 0)
    {
        if (!_connectionStringReceived)
        {
            if (!ReadConnectionString(packet))
            {
                CloseSocket();
                return SocketReadCallbackResult::Stop;
            }

            continue;
        }

        if (_headerBuffer.GetRemainingSpace() > 0)
        {
            // need to receive the header
            std::size_t readHeaderSize = std::min(packet.GetActiveSize(), _headerBuffer.GetRemainingSpace());
            _headerBuffer.Write(packet.GetReadPointer(), readHeaderSize);
            packet.ReadCompleted(readHeaderSize);

            if (_headerBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole header this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }

            // We just received nice new header
            if (!ReadHeaderHandler())
            {
                CloseSocket();
                return SocketReadCallbackResult::Stop;
            }
        }

        // We have full read header, now check the data payload
        if (_packetBuffer.GetRemainingSpace() > 0)
        {
            // need more data in the payload
            std::size_t readDataSize = std::min(packet.GetActiveSize(), _packetBuffer.GetRemainingSpace());
            _packetBuffer.Write(packet.GetReadPointer(), readDataSize);
            packet.ReadCompleted(readDataSize);

            if (_packetBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole data this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }
        }

        // just received fresh new payload
        ReadDataHandlerResult result = ReadDataHandler();
        _headerBuffer.Reset();

        if (result != ReadDataHandlerResult::Ok)
        {
            if (result != ReadDataHandlerResult::WaitingForQuery)
            {
                CloseSocket();
            }

            return SocketReadCallbackResult::Stop;
        }
    }

    return SocketReadCallbackResult::KeepReading;
}

bool WorldSocket::ReadHeaderHandler()
{
    ASSERT(_headerBuffer.GetActiveSize() == sizeof(WorldPacketHeader));

    WorldPacketHeader* header = reinterpret_cast<WorldPacketHeader*>(_headerBuffer.GetReadPointer());
    EndianConvert(header->Size);

    if (!header->IsValidSize())
    {
        LOG_ERROR("network", "WorldSocket::ReadHeaderHandler(): client {} sent malformed packet (size: {})",
            GetRemoteIpAddress().to_string(), header->Size);

        return false;
    }

    _packetBuffer.Resize(header->Size);
    return true;
}

bool WorldSocket::DecryptRecv(uint8* data, std::size_t length, uint8 const (&tag)[12])
{
    Acore::Crypto::AES::Tag copy;
    std::memcpy(copy, tag, sizeof(copy));
    bool ok = _decrypt.Process(MakeIV(_recvCounter, IV_MAGIC_CLIENT), data, length, copy);
    ++_recvCounter;
    return ok;
}

void WorldSocket::EncryptSend(uint8* data, std::size_t length, uint8 (&tag)[12])
{
    _encrypt.Process(MakeIV(_sendCounter, IV_MAGIC_SERVER), data, length, tag);
    ++_sendCounter;
}

struct ClientAuthSession
{
    uint64 DosResponse = 0;
    uint32 RegionID = 0;
    uint32 BattlegroupID = 0;
    uint32 RealmID = 0;
    std::array<uint8, 16> LocalChallenge = {};
    std::array<uint8, 24> Digest = {};
    bool UseIPv6 = false;
    std::string RealmJoinTicket;
};

struct AccountInfo
{
    uint32 Id;
    std::array<uint8, 64> JoinKey;
    std::string LastIP;
    bool IsLockedToIP;
    std::string LockCountry;
    uint8 Expansion;
    uint32 Flags;
    int64 MuteTime;
    LocaleConstant Locale;
    uint32 Recruiter;
    std::string OS;
    bool IsRectuiter;
    AccountTypes Security;
    bool IsBanned;
    uint32 TotalTime;

    explicit AccountInfo(Field* fields)
    {
        //           0                1          2         3               4            5        6          7         8            9    10           11
        // SELECT a.id, a.bnet_join_key, a.last_ip, a.locked, a.lock_country, a.expansion, a.Flags a.mutetime, a.locale, a.recruiter, a.os, a.totaltime,
        //            12                                                   13    14
        // aa.gmLevel, ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate, r.id
        // FROM account a
        // LEFT JOIN account_access aa ON a.id = aa.AccountID AND aa.RealmID IN (-1, ?)
        // LEFT JOIN account_banned ab ON a.id = ab.id
        // LEFT JOIN account r ON a.id = r.recruiter
        // WHERE a.username = ? ORDER BY aa.RealmID DESC LIMIT 1
        Id = fields[0].Get<uint32>();
        JoinKey = fields[1].Get<Binary, 64>();
        LastIP = fields[2].Get<std::string>();
        IsLockedToIP = fields[3].Get<bool>();
        LockCountry = fields[4].Get<std::string>();
        Expansion = fields[5].Get<uint8>();
        Flags = fields[6].Get<uint32>();
        MuteTime = fields[7].Get<int64>();
        Locale = LocaleConstant(fields[8].Get<uint8>());
        Recruiter = fields[9].Get<uint32>();
        OS = fields[10].Get<std::string>();
        TotalTime = fields[11].Get<uint32>();
        Security = AccountTypes(fields[12].Get<uint8>());
        IsBanned = fields[13].Get<uint64>() != 0;
        IsRectuiter = fields[14].Get<uint32>() != 0;

        uint32 world_expansion = sWorld->getIntConfig(CONFIG_EXPANSION);
        if (Expansion > world_expansion)
            Expansion = world_expansion;

        if (Locale >= TOTAL_LOCALES)
            Locale = LOCALE_enUS;
    }
};

WorldSocket::ReadDataHandlerResult WorldSocket::ReadDataHandler()
{
    WorldPacketHeader* header = reinterpret_cast<WorldPacketHeader*>(_headerBuffer.GetReadPointer());

    if (_decryptEnabled)
    {
        if (!DecryptRecv(_packetBuffer.GetReadPointer(), _packetBuffer.GetActiveSize(), header->Tag))
        {
            LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client {} failed to decrypt packet (size: {})",
                GetRemoteIpAddress().to_string(), header->Size);
            return ReadDataHandlerResult::Error;
        }
    }
    else
        ++_recvCounter;

    uint8 const* payload = _packetBuffer.GetReadPointer();
    OpcodeClient opcode = static_cast<OpcodeClient>(uint16(payload[0] | (payload[1] << 8)));

    WorldPacket packet(opcode, header->Size - 2);
    if (header->Size > 2)
        packet.append(payload + 2, header->Size - 2);
    _packetBuffer.Reset();

    WorldPacket* packetToQueue;

    if (sPacketLog->CanLogPacket() && IsLoggingPackets())
        sPacketLog->LogPacket(packet, CLIENT_TO_SERVER, GetRemoteIpAddress(), GetRemotePort());

    std::unique_lock<std::mutex> sessionGuard(_worldSessionLock, std::defer_lock);

    switch (opcode)
    {
        case CMSG_PING:
        {
            LogOpcodeText(opcode, sessionGuard);
            try
            {
                return HandlePing(packet) ? ReadDataHandlerResult::Ok : ReadDataHandlerResult::Error;
            }
            catch (ByteBufferException const&)
            {
            }
            LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client {} sent malformed CMSG_PING", GetRemoteIpAddress().to_string());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_AUTH_SESSION:
        {
            LogOpcodeText(opcode, sessionGuard);
            if (_authed || _pendingAccount)
            {
                LOG_ERROR("network", "WorldSocket::ProcessIncoming: received duplicate CMSG_AUTH_SESSION from {}", GetRemoteIpAddress().to_string());
                return ReadDataHandlerResult::Error;
            }

            try
            {
                HandleAuthSession(packet);
                return ReadDataHandlerResult::WaitingForQuery;
            }
            catch (ByteBufferException const&) { }

            LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client {} sent malformed CMSG_AUTH_SESSION", GetRemoteIpAddress().to_string());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_ENTER_ENCRYPTED_MODE_ACK:
            LogOpcodeText(opcode, sessionGuard);
            return HandleEnterEncryptedModeAck() ? ReadDataHandlerResult::Ok : ReadDataHandlerResult::Error;
        case CMSG_ENABLE_NAGLE:
            // empty (FUN_14076a5d0); may come right after CMSG_AUTH_SESSION. The socket keeps no-delay.
            LogOpcodeText(opcode, sessionGuard);
            return ReadDataHandlerResult::Ok;
        case CMSG_KEEP_ALIVE:
            // empty (FUN_1407675d0); sent while a loading screen is up ("Loading Screen Ping SEND")
            LogOpcodeText(opcode, sessionGuard);
            return ReadDataHandlerResult::Ok;
        case CMSG_QUEUED_MESSAGES_END:
            // u32 Timestamp; nothing is queued across connections here
            LogOpcodeText(opcode, sessionGuard);
            return ReadDataHandlerResult::Ok;
        case CMSG_LOG_DISCONNECT:
        {
            LogOpcodeText(opcode, sessionGuard);
            uint32 reason = packet.size() >= 4 ? packet.read<uint32>() : 0;
            LOG_INFO("network", "WorldSocket: client {} reports disconnect reason {}", GetRemoteIpAddress().to_string(), reason);
            return ReadDataHandlerResult::Ok;
        }
        case CMSG_TIME_SYNC_RESPONSE:
            packetToQueue = new WorldPacket(std::move(packet), GameTime::Now());
            break;
        default:
            packetToQueue = new WorldPacket(std::move(packet));
            break;
    }

    sessionGuard.lock();

    LogOpcodeText(opcode, sessionGuard);

    if (!_worldSession)
    {
        LOG_ERROR("network.opcode", "ProcessIncoming: Client not authed opcode = {}", uint32(opcode));
        delete packetToQueue;
        return ReadDataHandlerResult::Error;
    }

    OpcodeHandler const* handler = uint32(opcode) < NUM_OPCODE_HANDLERS ? opcodeTable[opcode] : nullptr;
    if (!handler)
    {
        LOG_DEBUG("network.opcode", "No defined handler for opcode {} sent by {}", GetOpcodeNameForLogging(static_cast<OpcodeClient>(packetToQueue->GetOpcode())), _worldSession->GetPlayerInfo());
        delete packetToQueue;
        return ReadDataHandlerResult::Ok;
    }

    // a handler still reading the 3.3.5 layout would misread the 3.4.3 one: ignore the packet
    // rather than drop the client
    if (!handler->IsConverted())
    {
        if (handler->FirstRefusal())
            LOG_DEBUG("network.opcode", "C->S: {} ignored, its handler is not converted to the 3.4.3 layout yet (logged once)", GetOpcodeNameForLogging(opcode));
        delete packetToQueue;
        return ReadDataHandlerResult::Ok;
    }

    // Our Idle timer will reset on any non PING opcodes on login screen, allowing us to catch people idling.
    if (packetToQueue->GetOpcode() != CMSG_WARDEN3_DATA)
    {
        _worldSession->ResetTimeOutTime(false);
    }

    // Copy the packet to the heap before enqueuing
    _worldSession->QueuePacket(packetToQueue);

    return ReadDataHandlerResult::Ok;
}

void WorldSocket::LogOpcodeText(OpcodeClient opcode, std::unique_lock<std::mutex> const& guard) const
{
    if (!guard)
    {
        LOG_TRACE("network.opcode", "C->S: {} {}", GetRemoteIpAddress().to_string(), GetOpcodeNameForLogging(opcode));
    }
    else
    {
        LOG_TRACE("network.opcode", "C->S: {} {}", (_worldSession ? _worldSession->GetPlayerInfo() : GetRemoteIpAddress().to_string()),
            GetOpcodeNameForLogging(opcode));
    }
}

void WorldSocket::SendPacketAndLogOpcode(WorldPacket const& packet)
{
    LOG_TRACE("network.opcode", "S->C: {} {}", GetRemoteIpAddress().to_string(), GetOpcodeNameForLogging(static_cast<OpcodeServer>(packet.GetOpcode())));
    SendPacket(packet);
}

void WorldSocket::SendPacket(WorldPacket const& packet)
{
    if (!IsOpen())
        return;

    // Only opcodes whose layout has been converted go out: anything else is still built in its 3.3.5
    // layout, which the 3.4.3 client would misread.
    OpcodeServer opcode = static_cast<OpcodeServer>(packet.GetOpcode());
    OpcodeHandler const* handler = uint32(opcode) < NUM_OPCODE_HANDLERS ? opcodeTable[opcode] : nullptr;
    if (!handler || !handler->IsConverted())
    {
        if (!handler)
            LOG_TRACE("network.opcode", "S->C: {} not sent, the 3.4.3 client has no such opcode", GetOpcodeNameForLogging(opcode));
        else if (handler->FirstRefusal())
            LOG_DEBUG("network.opcode", "S->C: {} not sent, not converted to the 3.4.3 layout yet (logged once)", GetOpcodeNameForLogging(opcode));
        return;
    }

    if (sPacketLog->CanLogPacket() && IsLoggingPackets())
        sPacketLog->LogPacket(packet, SERVER_TO_CLIENT, GetRemoteIpAddress(), GetRemotePort());

    _bufferQueue.Enqueue(new EncryptableAndCompressiblePacket(packet, _encryptEnabled.load()));
}

void WorldSocket::HandleAuthSession(WorldPacket& recvPacket)
{
    std::shared_ptr<ClientAuthSession> authSession = std::make_shared<ClientAuthSession>();

    // CMSG_AUTH_SESSION (FUN_14076a4d0)
    recvPacket >> authSession->DosResponse;
    recvPacket >> authSession->RegionID;
    recvPacket >> authSession->BattlegroupID;
    recvPacket >> authSession->RealmID;
    recvPacket.read(authSession->LocalChallenge);
    recvPacket.read(authSession->Digest);
    authSession->UseIPv6 = recvPacket.ReadBit();
    uint32 ticketLength;
    recvPacket >> ticketLength;
    if (ticketLength > 0x400)
        throw ByteBufferPositionException(false, recvPacket.rpos(), ticketLength, recvPacket.size());
    authSession->RealmJoinTicket.resize(ticketLength);
    if (ticketLength)
        recvPacket.read(reinterpret_cast<uint8*>(authSession->RealmJoinTicket.data()), ticketLength);

    // The join ticket is the account name the Battle.net service handed out at RealmJoin.
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_BY_NAME);
    stmt->SetData(0, int32(realm.Id.Realm));
    stmt->SetData(1, authSession->RealmJoinTicket);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(std::bind(&WorldSocket::HandleAuthSessionCallback, this, authSession, std::placeholders::_1)));
}

void WorldSocket::HandleAuthSessionCallback(std::shared_ptr<ClientAuthSession> authSession, PreparedQueryResult result)
{
    // Stop if the account is not found
    if (!result)
    {
        // We can not log here, as we do not know the account. Thus, no accountId.
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (unknown account '{}' or no join key).", authSession->RealmJoinTicket);
        DelayedCloseSocket();
        return;
    }

    std::shared_ptr<AccountInfo> account = std::make_shared<AccountInfo>(result->Fetch());

    // For hook purposes, we get Remoteaddress at this point.
    std::string address = sConfigMgr->GetOption<bool>("AllowLoggingIPAddressesInDatabase", true, true) ? GetRemoteIpAddress().to_string() : "0.0.0.0";

    LoginDatabasePreparedStatement* stmt = nullptr;

    // As we don't know if attempted login process by ip works, we update last_attempt_ip right away
    stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_ATTEMPT_IP);
    stmt->SetData(0, address);
    stmt->SetData(1, authSession->RealmJoinTicket);
    LoginDatabase.Execute(stmt);
    // This also allows to check for possible "hack" attempts on account

    // First reject the connection if packet contains invalid data or realm state doesn't allow logging in
    if (sWorld->IsClosed())
    {
        SendAuthResponseError(AUTH_REJECT);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: World closed, denying client ({}).", GetRemoteIpAddress().to_string());
        DelayedCloseSocket();
        return;
    }

    if (authSession->RealmID != realm.Id.Realm)
    {
        SendAuthResponseError(REALM_LIST_REALM_NOT_FOUND);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Client {} requested connecting with realm id {} but this realm has id {} set in config.",
            GetRemoteIpAddress().to_string(), authSession->RealmID, realm.Id.Realm);
        DelayedCloseSocket();
        return;
    }

    // Digest = HMAC-SHA256(SHA256(joinKey || Win64AuthSeed), LocalChallenge || ServerChallenge || AuthCheckSeed)[0:24]
    Acore::Crypto::SHA256 digestKeyHash;
    digestKeyHash.UpdateData(account->JoinKey.data(), account->JoinKey.size());
    digestKeyHash.UpdateData(Win64AuthSeed.data(), Win64AuthSeed.size());
    digestKeyHash.Finalize();

    Acore::Crypto::HMAC_SHA256 digest(digestKeyHash.GetDigest());
    digest.UpdateData(authSession->LocalChallenge);
    digest.UpdateData(_serverChallenge);
    digest.UpdateData(AuthCheckSeed);
    digest.Finalize();

    if (std::memcmp(digest.GetDigest().data(), authSession->Digest.data(), authSession->Digest.size()) != 0)
    {
        SendAuthResponseError(AUTH_FAILED);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Authentication failed for account: {} ('{}') address: {}", account->Id, authSession->RealmJoinTicket, address);
        DelayedCloseSocket();
        return;
    }

    // SessionKey: HMAC-SHA256(SHA256(joinKey), ServerChallenge || LocalChallenge || SessionKeySeed),
    // stretched to 40 bytes by the SHA-256 session key generator
    Acore::Crypto::SHA256 keyHash;
    keyHash.UpdateData(account->JoinKey.data(), account->JoinKey.size());
    keyHash.Finalize();

    Acore::Crypto::HMAC_SHA256 sessionKeyHmac(keyHash.GetDigest());
    sessionKeyHmac.UpdateData(_serverChallenge);
    sessionKeyHmac.UpdateData(authSession->LocalChallenge);
    sessionKeyHmac.UpdateData(SessionKeySeed);
    sessionKeyHmac.Finalize();

    SessionKeyGenerator<Acore::Crypto::SHA256> sessionKeyGenerator(sessionKeyHmac.GetDigest());
    sessionKeyGenerator.Generate(_sessionKey.data(), uint32(_sessionKey.size()));

    // EncryptKey: HMAC-SHA256(SessionKey, LocalChallenge || ServerChallenge || EncryptionKeySeed)[0:16]
    Acore::Crypto::HMAC_SHA256 encryptKeyGenerator(_sessionKey);
    encryptKeyGenerator.UpdateData(authSession->LocalChallenge);
    encryptKeyGenerator.UpdateData(_serverChallenge);
    encryptKeyGenerator.UpdateData(EncryptionKeySeed);
    encryptKeyGenerator.Finalize();

    Acore::Crypto::AES::Key encryptKey;
    std::memcpy(encryptKey.data(), encryptKeyGenerator.GetDigest().data(), encryptKey.size());

    if (IpLocationRecord const* location = sIPLocation->GetLocationRecord(address))
        _ipCountry = location->CountryCode;

    ///- Re-check ip locking (same check as in auth).
    if (account->IsLockedToIP)
    {
        if (account->LastIP != address)
        {
            SendAuthResponseError(AUTH_FAILED);
            LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs. Original IP: {}, new IP: {}).", account->LastIP, address);
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account->Id);
            DelayedCloseSocket();
            return;
        }
    }
    else if (!account->LockCountry.empty() && account->LockCountry != "00" && !_ipCountry.empty())
    {
        if (account->LockCountry != _ipCountry)
        {
            SendAuthResponseError(AUTH_FAILED);
            LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account country differs. Original country: {}, new country: {}).", account->LockCountry, _ipCountry);
            // We could log on hook only instead of an additional db log, however action logger is config based. Better keep DB logging as well
            sScriptMgr->OnFailedAccountLogin(account->Id);
            DelayedCloseSocket();
            return;
        }
    }

    //! Negative mutetime indicates amount of minutes to be muted effective on next login - which is now.
    if (account->MuteTime < 0)
    {
        account->MuteTime = GameTime::GetGameTime().count() + std::llabs(account->MuteTime);

        auto* muteStmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_MUTE_TIME_LOGIN);
        muteStmt->SetData(0, account->MuteTime);
        muteStmt->SetData(1, account->Id);
        LoginDatabase.Execute(muteStmt);
    }

    if (account->IsBanned)
    {
        SendAuthResponseError(AUTH_BANNED);
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        sScriptMgr->OnFailedAccountLogin(account->Id);
        DelayedCloseSocket();
        return;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = sWorld->GetPlayerSecurityLimit();
    LOG_DEBUG("network", "Allowed Level: {} Player Level {}", allowedAccountType, account->Security);
    if (allowedAccountType > SEC_PLAYER && account->Security < allowedAccountType)
    {
        SendAuthResponseError(AUTH_UNAVAILABLE);
        LOG_DEBUG("network", "WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
        sScriptMgr->OnFailedAccountLogin(account->Id);
        DelayedCloseSocket();
        return;
    }

    // SMSG_ENTER_ENCRYPTED_MODE: Ed25519ctx over HMAC-SHA256(EncryptKey, Enabled || EnableEncryptionSeed)
    // (FUN_141561000); the session starts when the client acknowledges it.
    Acore::Crypto::Ed25519 const* signer = EnterEncryptedModeKey();
    if (!signer)
    {
        SendAuthResponseError(AUTH_SYSTEM_ERROR);
        DelayedCloseSocket();
        return;
    }

    uint8 const enabled = 1;
    Acore::Crypto::HMAC_SHA256 toSign(encryptKey);
    toSign.UpdateData(&enabled, 1);
    toSign.UpdateData(EnableEncryptionSeed);
    toSign.Finalize();

    Acore::Crypto::Ed25519::Signature signature;
    if (!signer->Sign(toSign.GetDigest().data(), toSign.GetDigest().size(), EnableEncryptionContext.data(),
        EnableEncryptionContext.size(), signature))
    {
        LOG_ERROR("network", "WorldSocket::HandleAuthSession: signing SMSG_ENTER_ENCRYPTED_MODE failed");
        SendAuthResponseError(AUTH_SYSTEM_ERROR);
        DelayedCloseSocket();
        return;
    }

    _decrypt.Init(encryptKey);
    _encrypt.Init(encryptKey);

    WorldPacket enterEncryptedMode(SMSG_ENTER_ENCRYPTED_MODE, 64 + 1);
    enterEncryptedMode.append(signature);
    enterEncryptedMode.WriteBit(enabled != 0);
    enterEncryptedMode.FlushBits();
    SendPacketAndLogOpcode(enterEncryptedMode);

    _pendingAccount = std::move(account);
    _pendingAccountName = authSession->RealmJoinTicket;

    LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Client '{}' authenticated from {}, entering encrypted mode.", _pendingAccountName, address);

    // Update the last_ip in the database as it was successful for login
    stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_IP);
    stmt->SetData(0, address);
    stmt->SetData(1, _pendingAccountName);
    LoginDatabase.Execute(stmt);

    AsyncRead();
}

bool WorldSocket::HandleEnterEncryptedModeAck()
{
    if (!_pendingAccount || _authed)
    {
        LOG_ERROR("network", "WorldSocket: client {} acknowledged encrypted mode out of order", GetRemoteIpAddress().to_string());
        return false;
    }

    // the client encrypts everything after the acknowledgement, in both directions
    _decryptEnabled = true;
    _encryptEnabled = true;

    std::shared_ptr<AccountInfo> account = std::move(_pendingAccount);
    std::string address = GetRemoteIpAddress().to_string();

    // At this point, we can safely hook a successful login
    sScriptMgr->OnAccountLogin(account->Id);

    _authed = true;

    sScriptMgr->OnLastIpUpdate(account->Id, address);

    std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);
    _worldSession = new WorldSession(account->Id, std::move(_pendingAccountName), account->Flags, shared_from_this(), account->Security,
        account->Expansion, account->MuteTime, account->Locale, account->Recruiter, account->IsRectuiter, account->Security ? true : false, account->TotalTime);

    // Warden is the 3.3.5 module set; the 3.4.3 client's is not ported, so none is started.
    if (sWorld->getBoolConfig(CONFIG_WARDEN_ENABLED))
        LOG_DEBUG("network", "WorldSocket: Warden is not started for 3.4.3 clients");

    _worldSession->ValidateAccountFlags();

    sWorldSessionMgr->AddSession(_worldSession);
    return true;
}

void WorldSocket::SendAuthResponseError(uint32 code)
{
    // the short failure the client accepts before encryption (FUN_1414f77b0)
    WorldPacket packet(SMSG_AUTH_FAILED, 4);
    packet << uint32(code);

    SendPacketAndLogOpcode(packet);
}

bool WorldSocket::HandlePing(WorldPacket& recvPacket)
{
    using namespace std::chrono;

    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> ping;
    recvPacket >> latency;

    if (_LastPingTime == steady_clock::time_point())
    {
        _LastPingTime = steady_clock::now();
    }
    else
    {
        steady_clock::time_point now = steady_clock::now();
        steady_clock::duration diff = now - _LastPingTime;

        _LastPingTime = now;

        if (diff < seconds(27))
        {
            ++_OverSpeedPings;

            uint32 maxAllowed = sWorld->getIntConfig(CONFIG_MAX_OVERSPEED_PINGS);

            if (maxAllowed && _OverSpeedPings > maxAllowed)
            {
                std::unique_lock<std::mutex> sessionGuard(_worldSessionLock);

                if (_worldSession && !_worldSession->HasPermission(rbac::RBAC_PERM_SKIP_CHECK_OVERSPEED_PING))
                {
                    LOG_ERROR("network", "WorldSocket::HandlePing: {} kicked for over-speed pings (address: {})",
                        _worldSession->GetPlayerInfo(), GetRemoteIpAddress().to_string());

                    return false;
                }
            }
        }
        else
        {
            _OverSpeedPings = 0;
        }
    }

    {
        std::lock_guard<std::mutex> sessionGuard(_worldSessionLock);

        if (_worldSession)
            _worldSession->SetLatency(latency);
        else
        {
            LOG_ERROR("network", "WorldSocket::HandlePing: peer sent CMSG_PING, but is not authenticated or got recently kicked, address = {}", GetRemoteIpAddress().to_string());
            return false;
        }
    }

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    SendPacketAndLogOpcode(packet);

    return true;
}
