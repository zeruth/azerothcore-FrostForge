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

#ifndef __WORLDSOCKET_H__
#define __WORLDSOCKET_H__

#include "AES.h"
#include "Common.h"
#include "MPSCQueue.h"
#include "Socket.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <boost/asio/ip/tcp.hpp>

using boost::asio::ip::tcp;

class EncryptableAndCompressiblePacket : public WorldPacket
{
public:
    EncryptableAndCompressiblePacket(WorldPacket const& packet, bool encrypt) : WorldPacket(packet), _encrypt(encrypt)
    {
        SocketQueueLink.store(nullptr, std::memory_order_relaxed);
    }

    bool NeedsEncryption() const { return _encrypt; }

    std::atomic<EncryptableAndCompressiblePacket*> SocketQueueLink;

private:
    bool _encrypt;
};

namespace WorldPackets
{
    class ServerPacket;
}

// The 3.4.3 packet header, both directions (world-handshake.ANALYSIS.md 2.1): the length of
// what follows (u16 opcode + body), then the first 12 bytes of its AES-GCM tag (zero while the
// connection is not encrypted).
#pragma pack(push, 1)
struct WorldPacketHeader
{
    uint32 Size;
    uint8 Tag[12];

    bool IsValidSize() const { return Size >= 2 && Size < 0x40000; }
};
#pragma pack(pop)

struct ClientAuthSession;
struct AccountInfo;

class AC_GAME_API WorldSocket final : public Socket<WorldSocket>
{
    typedef Socket<WorldSocket> BaseSocket;

public:
    WorldSocket(IoContextTcpSocket&& socket);
    ~WorldSocket();

    WorldSocket(WorldSocket const& right) = delete;
    WorldSocket& operator=(WorldSocket const& right) = delete;

    void Start() override;
    bool Update() final;

    void SendPacket(WorldPacket const& packet);

    void SetSendBufferSize(std::size_t sendBufferSize) { _sendBufferSize = sendBufferSize; }

    bool IsLoggingPackets() const { return _loggingPackets; }
    void SetPacketLogging(bool state) { _loggingPackets = state; }

protected:
    void OnClose() override;
    SocketReadCallbackResult ReadHandler() final;
    bool ReadHeaderHandler();

    enum class ReadDataHandlerResult
    {
        Ok = 0,
        Error = 1,
        WaitingForQuery = 2
    };

    ReadDataHandlerResult ReadDataHandler();

private:
    void CheckIpCallback(PreparedQueryResult result);
    bool ReadConnectionString(MessageBuffer& packet);

    /// writes network.opcode log
    /// accessing WorldSession is not threadsafe, only do it when holding _worldSessionLock
    void LogOpcodeText(OpcodeClient opcode, std::unique_lock<std::mutex> const& guard) const;

    /// sends and logs network.opcode without accessing WorldSession
    void SendPacketAndLogOpcode(WorldPacket const& packet);
    void SendAuthChallenge();
    void HandleAuthSession(WorldPacket& recvPacket);
    void HandleAuthSessionCallback(std::shared_ptr<ClientAuthSession> authSession, PreparedQueryResult result);
    bool HandleEnterEncryptedModeAck();
    void SendAuthResponseError(uint32 code);

    bool HandlePing(WorldPacket& recvPacket);

    // AES-128-GCM of the payload; the IV is the direction's packet counter and "CLNT" / "SRVR"
    // (world-handshake.ANALYSIS.md 2.2). The counters count every packet, encrypted or not.
    bool DecryptRecv(uint8* data, std::size_t length, uint8 const (&tag)[12]);
    void EncryptSend(uint8* data, std::size_t length, uint8 (&tag)[12]);

    std::array<uint8, 32> _dosChallenge;
    std::array<uint8, 16> _serverChallenge;
    std::array<uint8, 40> _sessionKey;
    Acore::Crypto::AES _decrypt;
    Acore::Crypto::AES _encrypt;
    uint64 _recvCounter;
    uint64 _sendCounter;
    bool _decryptEnabled;
    std::atomic<bool> _encryptEnabled;

    std::string _connectionString;
    bool _connectionStringReceived;
    std::shared_ptr<AccountInfo> _pendingAccount;
    std::string _pendingAccountName;

    TimePoint _LastPingTime;
    uint32 _OverSpeedPings;

    std::mutex _worldSessionLock;
    WorldSession* _worldSession;
    bool _authed;

    MessageBuffer _headerBuffer;
    MessageBuffer _packetBuffer;
    MPSCQueue<EncryptableAndCompressiblePacket, &EncryptableAndCompressiblePacket::SocketQueueLink> _bufferQueue;
    std::size_t _sendBufferSize;

    QueryCallbackProcessor _queryProcessor;
    std::string _ipCountry;

    bool _loggingPackets;
};

#endif
