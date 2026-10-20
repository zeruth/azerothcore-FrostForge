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

#ifndef LFGPackets_h__
#define LFGPackets_h__

#include "Packet.h"
#include "PacketUtilities.h"

namespace WorldPackets::LFG
{
    /// A queue as the 3.4.3 client names it (FUN_14070f950): it keys its queue records on all four fields
    struct RideTicket
    {
        ObjectGuid RequesterGuid;
        uint32 Id = 0;
        uint32 Type = 0;
        int64 Time = 0;
    };

    void ReadRideTicket(ByteBuffer& data, RideTicket& ticket);

    class LFGJoin final : public ClientPacket
    {
    public:
        LFGJoin(WorldPacket&& packet) : ClientPacket(CMSG_DF_JOIN, std::move(packet)) { }

        void Read() override;

        uint32 Roles = 0;
        Array<uint32, 50> Slots;
        std::string Comment;                                // not in 3.4.3: always empty
        bool QueueAsGroup = false;
    };

    class LFGLeave final : public ClientPacket
    {
    public:
        LFGLeave(WorldPacket&& packet) : ClientPacket(CMSG_DF_LEAVE, std::move(packet)) { }

        void Read() override;

        RideTicket Ticket;
    };
}

#endif // LFGPackets_h__
