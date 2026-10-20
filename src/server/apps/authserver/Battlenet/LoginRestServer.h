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

#ifndef LoginRestServer_h__
#define LoginRestServer_h__

#include "Define.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace Battlenet
{
    // The account a login ticket stands for, from the web login until VerifyWebCredentials.
    struct LoginTicket
    {
        uint32 AccountId = 0;
        std::string AccountName;
        std::chrono::steady_clock::time_point Expires;
    };

    // The web login the 3.4.3 client is sent to by ChallengeNotify.OnExternalChallenge
    // (bnet-login-flow.ANALYSIS.md section 2): a GET of the form, then a POST of the account and
    // password to the same URL, answered with a login ticket. HTTPS with the Battle.net
    // certificate; the client does not verify the peer on this path.
    class LoginRestServer
    {
    public:
        static LoginRestServer& Instance();

        bool Start(boost::asio::io_context& ioContext, std::string const& bindIp, uint16 port,
            std::string const& externalAddress, std::string const& certificateFile, std::string const& privateKeyFile);
        void Stop();

        // The URL the challenge sends the client to.
        std::string const& GetFormUrl() const { return _formUrl; }

        // Takes a ticket out of the store; a ticket is good once, for a few minutes.
        std::optional<LoginTicket> RedeemTicket(std::string const& ticket);

        // Checks an account name and password; on success returns a fresh ticket, otherwise sets
        // the error code and message the client shows.
        std::optional<std::string> Login(std::string const& accountName, std::string const& password,
            std::string& errorCode, std::string& errorMessage);

    private:
        LoginRestServer();

        void Accept();

        boost::asio::ssl::context _sslContext;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> _acceptor;
        std::string _formUrl;
        std::unordered_map<std::string, LoginTicket> _tickets;
    };
}

#define sLoginRestServer Battlenet::LoginRestServer::Instance()

#endif // LoginRestServer_h__
