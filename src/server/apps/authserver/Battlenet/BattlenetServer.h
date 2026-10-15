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

#ifndef BattlenetServer_h__
#define BattlenetServer_h__

#include "Define.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <string>

namespace Battlenet
{
    // The TLS listener the 3.4.3 client connects to for Battle.net RPC (port 1119 by default).
    class Server
    {
    public:
        static Server& Instance();

        bool Start(boost::asio::io_context& ioContext, std::string const& bindIp, uint16 port,
            std::string const& certificateFile, std::string const& privateKeyFile);
        void Stop();

    private:
        Server();

        void Accept();

        boost::asio::ssl::context _sslContext;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> _acceptor;
    };
}

#define sBattlenetServer Battlenet::Server::Instance()

#endif // BattlenetServer_h__
