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

#include "BattlenetServer.h"
#include "BattlenetSession.h"
#include "Log.h"
#include <boost/asio/ip/address.hpp>

namespace Battlenet
{
    Server& Server::Instance()
    {
        static Server instance;
        return instance;
    }

    // The client offers TLS 1.2 through SChannel; anything older is refused.
    Server::Server() : _sslContext(boost::asio::ssl::context::tls_server)
    {
        _sslContext.set_options(boost::asio::ssl::context::default_workarounds
            | boost::asio::ssl::context::no_sslv2
            | boost::asio::ssl::context::no_sslv3
            | boost::asio::ssl::context::no_tlsv1
            | boost::asio::ssl::context::no_tlsv1_1);
    }

    bool Server::Start(boost::asio::io_context& ioContext, std::string const& bindIp, uint16 port,
        std::string const& certificateFile, std::string const& privateKeyFile)
    {
        boost::system::error_code error;
        _sslContext.use_certificate_chain_file(certificateFile, error);
        if (error)
        {
            LOG_ERROR("server.battlenet", "Cannot load the Battle.net certificate chain {}: {}", certificateFile, error.message());
            return false;
        }

        _sslContext.use_private_key_file(privateKeyFile, boost::asio::ssl::context::pem, error);
        if (error)
        {
            LOG_ERROR("server.battlenet", "Cannot load the Battle.net private key {}: {}", privateKeyFile, error.message());
            return false;
        }

        boost::asio::ip::address address = boost::asio::ip::make_address(bindIp, error);
        if (error)
        {
            LOG_ERROR("server.battlenet", "Invalid BattlenetBindIP {}: {}", bindIp, error.message());
            return false;
        }

        boost::asio::ip::tcp::endpoint endpoint(address, port);
        _acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(ioContext);
        _acceptor->open(endpoint.protocol(), error);
        if (!error)
            _acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), error);
        if (!error)
            _acceptor->bind(endpoint, error);
        if (!error)
            _acceptor->listen(boost::asio::socket_base::max_listen_connections, error);
        if (error)
        {
            LOG_ERROR("server.battlenet", "Cannot listen for Battle.net on {}:{}: {}", bindIp, port, error.message());
            _acceptor.reset();
            return false;
        }

        LOG_INFO("server.battlenet", "Battle.net listening on {}:{}", bindIp, port);
        Accept();
        return true;
    }

    void Server::Stop()
    {
        if (!_acceptor)
            return;

        boost::system::error_code error;
        _acceptor->close(error);
        _acceptor.reset();
    }

    void Server::Accept()
    {
        _acceptor->async_accept([this](boost::system::error_code const& error, boost::asio::ip::tcp::socket socket)
        {
            if (error == boost::asio::error::operation_aborted || !_acceptor)
                return;

            if (!error)
                std::make_shared<Session>(std::move(socket), _sslContext)->Start();
            else
                LOG_DEBUG("server.battlenet", "Battle.net accept failed: {}", error.message());

            Accept();
        });
    }
}
