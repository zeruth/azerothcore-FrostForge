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
#include "Log.h"
#include "bgs/low/pb/client/connection_service.pb.h"
#include "bgs/low/pb/client/rpc_types.pb.h"
#include <boost/asio/write.hpp>
#include <chrono>

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
}

namespace Battlenet
{
    Session::Session(boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context& sslContext)
        : _socket(std::move(socket), sslContext), _readSize(0), _closed(false)
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
