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

#include "LoginRestServer.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"
#include "SRP6.h"
#include "StringFormat.h"
#include "Util.h"
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

namespace http = boost::beast::http;

namespace
{
    // The path the client is sent to; the GET and the POST both use it.
    constexpr char const* LOGIN_PATH = "/bnetserver/login/";
    constexpr std::chrono::minutes TICKET_LIFETIME(5);

    // FUN_14149a3e0 wants exactly "Content-Type:", and a value with a ';' in it: a bare
    // "application/json" counts as no content type at all.
    constexpr char const* JSON_CONTENT_TYPE = "application/json;charset=utf-8";

    std::string FormJson()
    {
        boost::json::object form;
        form["type"] = "LOGIN_FORM";
        form["inputs"] = boost::json::array{
            boost::json::object{ { "input_id", "account_name" }, { "type", "text" }, { "label", "Account" }, { "max_length", 320 } },
            boost::json::object{ { "input_id", "password" }, { "type", "password" }, { "label", "Password" }, { "max_length", 128 } },
            boost::json::object{ { "input_id", "log_in_submit" }, { "type", "submit" }, { "label", "Log In" } }
        };
        return boost::json::serialize(form);
    }

    // One HTTPS connection: reads requests until the client closes it.
    class RestSession : public std::enable_shared_from_this<RestSession>
    {
    public:
        RestSession(boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context& sslContext)
            : _stream(std::move(socket), sslContext) { }

        void Start()
        {
            _stream.async_handshake(boost::asio::ssl::stream_base::server,
                [self = shared_from_this()](boost::system::error_code const& error)
                {
                    if (error)
                    {
                        LOG_DEBUG("server.battlenet", "Web login TLS handshake failed: {}", error.message());
                        return;
                    }

                    self->Read();
                });
        }

    private:
        void Read()
        {
            _request = {};
            http::async_read(_stream, _buffer, _request,
                [self = shared_from_this()](boost::system::error_code const& error, std::size_t /*transferred*/)
                {
                    if (!error)
                        self->Handle();
                });
        }

        void Handle()
        {
            http::status status = http::status::ok;
            std::string body;

            std::string_view target(_request.target().data(), _request.target().size());
            LOG_DEBUG("server.battlenet", "Web login {} {}", std::string(_request.method_string()), std::string(target));
            if (target.substr(0, target.find('?')) != LOGIN_PATH)
            {
                status = http::status::not_found;
                body = R"({"error_code":"NOT_FOUND","error_message":"Unknown path"})";
            }
            else if (_request.method() == http::verb::get)
                body = FormJson();
            else if (_request.method() == http::verb::post)
                body = HandleLogin(status);
            else
            {
                status = http::status::method_not_allowed;
                body = R"({"error_code":"METHOD_NOT_ALLOWED","error_message":"GET or POST"})";
            }

            auto response = std::make_shared<http::response<http::string_body>>(status, _request.version());
            response->set(http::field::server, "FrostForge");
            response->set(http::field::content_type, JSON_CONTENT_TYPE);
            response->keep_alive(_request.keep_alive());
            response->body() = std::move(body);
            response->prepare_payload();

            http::async_write(_stream, *response,
                [self = shared_from_this(), response](boost::system::error_code const& error, std::size_t /*transferred*/)
                {
                    if (!error && response->keep_alive())
                        self->Read();
                });
        }

        // The POST body (FUN_1414982e0): {"version","program_id","platform_id",
        // "inputs":[{"input_id":"account_name","value"},{"input_id":"password","value"}]}.
        std::string HandleLogin(http::status& status)
        {
            std::string accountName, password;
            boost::system::error_code error;
            boost::json::value json = boost::json::parse(_request.body(), error);
            if (!error && json.is_object())
            {
                if (boost::json::value const* inputs = json.as_object().if_contains("inputs"); inputs && inputs->is_array())
                {
                    for (boost::json::value const& input : inputs->as_array())
                    {
                        if (!input.is_object())
                            continue;

                        boost::json::value const* id = input.as_object().if_contains("input_id");
                        boost::json::value const* value = input.as_object().if_contains("value");
                        if (!id || !value || !id->is_string() || !value->is_string())
                            continue;

                        if (id->as_string() == "account_name")
                            accountName = value->as_string().c_str();
                        else if (id->as_string() == "password")
                            password = value->as_string().c_str();
                    }
                }
            }

            std::string errorCode, errorMessage;
            if (std::optional<std::string> ticket = sLoginRestServer.Login(accountName, password, errorCode, errorMessage))
            {
                boost::json::object done;
                done["authentication_state"] = "DONE";
                done["login_ticket"] = *ticket;
                return boost::json::serialize(done);
            }

            // A status of 300 or more makes the client read error_code / error_message
            // (FUN_141495050); anything but CAPTCHA_REQUIRED fails the login with the message.
            status = http::status::bad_request;
            boost::json::object failure;
            failure["error_code"] = errorCode;
            failure["error_message"] = errorMessage;
            return boost::json::serialize(failure);
        }

        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> _stream;
        boost::beast::flat_buffer _buffer;
        http::request<http::string_body> _request;
    };
}

namespace Battlenet
{
    LoginRestServer& LoginRestServer::Instance()
    {
        static LoginRestServer instance;
        return instance;
    }

    LoginRestServer::LoginRestServer() : _sslContext(boost::asio::ssl::context::tls_server)
    {
        _sslContext.set_options(boost::asio::ssl::context::default_workarounds
            | boost::asio::ssl::context::no_sslv2
            | boost::asio::ssl::context::no_sslv3
            | boost::asio::ssl::context::no_tlsv1
            | boost::asio::ssl::context::no_tlsv1_1);
    }

    bool LoginRestServer::Start(boost::asio::io_context& ioContext, std::string const& bindIp, uint16 port,
        std::string const& externalAddress, std::string const& certificateFile, std::string const& privateKeyFile)
    {
        boost::system::error_code error;
        _sslContext.use_certificate_chain_file(certificateFile, error);
        if (!error)
            _sslContext.use_private_key_file(privateKeyFile, boost::asio::ssl::context::pem, error);
        if (error)
        {
            LOG_ERROR("server.battlenet", "Cannot load the web login certificate: {}", error.message());
            return false;
        }

        boost::asio::ip::address address = boost::asio::ip::make_address(bindIp, error);
        if (error)
        {
            LOG_ERROR("server.battlenet", "Invalid web login bind address {}: {}", bindIp, error.message());
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
            LOG_ERROR("server.battlenet", "Cannot listen for the web login on {}:{}: {}", bindIp, port, error.message());
            _acceptor.reset();
            return false;
        }

        _formUrl = Acore::StringFormat("https://{}:{}{}", externalAddress, port, LOGIN_PATH);
        LOG_INFO("server.battlenet", "Web login listening on {}:{} ({})", bindIp, port, _formUrl);
        Accept();
        return true;
    }

    void LoginRestServer::Stop()
    {
        if (!_acceptor)
            return;

        boost::system::error_code error;
        _acceptor->close(error);
        _acceptor.reset();
    }

    void LoginRestServer::Accept()
    {
        _acceptor->async_accept([this](boost::system::error_code const& error, boost::asio::ip::tcp::socket socket)
        {
            if (error == boost::asio::error::operation_aborted || !_acceptor)
                return;

            if (!error)
            {
                boost::system::error_code endpointError;
                auto endpoint = socket.remote_endpoint(endpointError);
                LOG_DEBUG("server.battlenet", "Web login connection from {}", endpointError ? std::string("?") : endpoint.address().to_string());
                std::make_shared<RestSession>(std::move(socket), _sslContext)->Start();
            }

            Accept();
        });
    }

    std::optional<std::string> LoginRestServer::Login(std::string const& accountName, std::string const& password,
        std::string& errorCode, std::string& errorMessage)
    {
        // AzerothCore keeps account names and passwords upper case (AccountMgr::CreateAccount).
        std::string name = accountName;
        std::string pass = password;
        if (name.empty() || pass.empty() || !Utf8ToUpperOnlyLatin(name) || !Utf8ToUpperOnlyLatin(pass))
        {
            errorCode = "INVALID_CREDENTIALS";
            errorMessage = "Enter an account name and a password.";
            return std::nullopt;
        }

        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_BNET_ACCOUNT_LOGIN);
        stmt->SetData(0, name);
        PreparedQueryResult result = LoginDatabase.Query(stmt);
        if (!result)
        {
            errorCode = "INVALID_CREDENTIALS";
            errorMessage = "The account name or password is incorrect.";
            LOG_INFO("server.battlenet", "Web login for unknown account {}", name);
            return std::nullopt;
        }

        Field* fields = result->Fetch();
        uint32 accountId = fields[0].Get<uint32>();
        std::string username = fields[1].Get<std::string>();
        bool verified = Acore::Crypto::SRP6::CheckLogin(username, pass,
            fields[2].Get<Binary, Acore::Crypto::SRP6::SALT_LENGTH>(),
            fields[3].Get<Binary, Acore::Crypto::SRP6::VERIFIER_LENGTH>());
        if (!verified)
        {
            errorCode = "INVALID_CREDENTIALS";
            errorMessage = "The account name or password is incorrect.";
            LOG_INFO("server.battlenet", "Web login for {} with a wrong password", username);
            return std::nullopt;
        }

        if (fields[5].Get<uint64>() != 0)
        {
            errorCode = "ACCOUNT_BANNED";
            errorMessage = "This account has been closed.";
            return std::nullopt;
        }

        std::string ticket = "TC-" + ByteArrayToHexStr(Acore::Crypto::GetRandomBytes<20>());
        _tickets[ticket] = { accountId, username, std::chrono::steady_clock::now() + TICKET_LIFETIME };
        LOG_INFO("server.battlenet", "Web login for {} (account {}) accepted", username, accountId);
        return ticket;
    }

    std::optional<LoginTicket> LoginRestServer::RedeemTicket(std::string const& ticket)
    {
        auto now = std::chrono::steady_clock::now();
        std::erase_if(_tickets, [now](auto const& entry) { return entry.second.Expires < now; });

        auto itr = _tickets.find(ticket);
        if (itr == _tickets.end())
            return std::nullopt;

        LoginTicket redeemed = std::move(itr->second);
        _tickets.erase(itr);
        return redeemed;
    }
}
