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

#ifndef BattlenetSession_h__
#define BattlenetSession_h__

#include "Define.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace google::protobuf
{
    class Message;
}

namespace bgs::protocol
{
    class Header;
}

namespace bgs::protocol::game_utilities::v1
{
    class ClientRequest;
    class ClientResponse;
}

namespace Battlenet
{
    // Service hashes: FNV-1a over the service's descriptor_name, as the 3.4.3 client computes them.
    enum ServiceHash : uint32
    {
        SERVICE_CONNECTION          = 0x65446991, // bnet.protocol.connection.ConnectionService
        SERVICE_AUTHENTICATION      = 0x0DECFC01, // bnet.protocol.authentication.AuthenticationServer
        SERVICE_AUTHENTICATION_LISTENER = 0x71240E35, // bnet.protocol.authentication.AuthenticationClient
        SERVICE_GAME_UTILITIES      = 0x3FC1274D, // bnet.protocol.game_utilities.GameUtilities
        SERVICE_ACCOUNT             = 0x62DA0891, // bnet.protocol.account.AccountService
        SERVICE_CHALLENGE_LISTENER  = 0xBBDA171F  // bnet.protocol.challenge.ChallengeNotify
    };

    // A Header with this service_id is a response, matched to its call by token
    // (rpc_connection.cpp FUN_141f05010).
    constexpr uint32 RESPONSE_SERVICE_ID = 0xFE;

    // Status codes the client itself answers unknown calls with (FUN_141f03910).
    enum RpcStatus : uint32
    {
        RPC_STATUS_OK                  = 0,
        RPC_STATUS_DENIED              = 0x03,
        RPC_STATUS_NOT_IMPLEMENTED     = 0x0D,
        RPC_STATUS_BAD_REQUEST         = 0x0B,
        RPC_STATUS_INVALID_SERVICE     = 0xBC2
    };

    using SslSocket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    // One client connection on the Battle.net port: TLS, then RPC frames of
    // [u16 big-endian header length][bgs.protocol.Header][body of Header.size bytes]
    // (rpc_frame_transport_default.cpp, FUN_141f25650 / FUN_141f25ec0).
    class Session : public std::enable_shared_from_this<Session>
    {
    public:
        Session(boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context& sslContext);

        void Start();

    private:
        void ReadMore();
        void OnRead(boost::system::error_code const& error, std::size_t transferred);
        bool ParseFrames();

        void HandleRequest(bgs::protocol::Header const& header, uint8 const* body, std::size_t size);
        void HandleResponse(bgs::protocol::Header const& header);

        void HandleConnectionService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size);
        void HandleAuthenticationService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size);
        void HandleAccountService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size);
        void HandleGameUtilitiesService(bgs::protocol::Header const& header, uint8 const* body, std::size_t size);

        // GameUtilities.ProcessClientRequest commands (bnet-login-flow.ANALYSIS.md section 5).
        uint32 HandleRealmListTicketRequest(bgs::protocol::game_utilities::v1::ClientRequest const& request,
            bgs::protocol::game_utilities::v1::ClientResponse& response);
        uint32 HandleRealmListRequest(bgs::protocol::game_utilities::v1::ClientRequest const& request,
            bgs::protocol::game_utilities::v1::ClientResponse& response);
        uint32 HandleRealmJoinRequest(bgs::protocol::game_utilities::v1::ClientRequest const& request,
            bgs::protocol::game_utilities::v1::ClientResponse& response);

        void SendResponse(uint32 token, uint32 status, google::protobuf::Message const* body);
        // A call the server makes into a service the client exports, named by hash (bindless).
        void SendRequest(uint32 serviceHash, uint32 methodId, google::protobuf::Message const& body);
        void SendFrame(bgs::protocol::Header const& header, google::protobuf::Message const* body);
        void WriteNext();
        void Close();

        bool IsAuthenticated() const { return _accountId != 0; }

        SslSocket _socket;
        std::string _remote;
        std::vector<uint8> _readBuffer;
        std::size_t _readSize;
        std::deque<std::vector<uint8>> _writeQueue;
        bool _closed;

        uint32 _requestToken;
        uint32 _accountId;
        std::string _accountName;
        std::string _locale;
        std::vector<uint8> _sessionKey;   // LogonResult.session_key, echoed as Param_BnetSessionKey
        std::vector<uint8> _clientSecret; // the 32 random bytes of Param_ClientInfo's secret
    };
}

#endif // BattlenetSession_h__
