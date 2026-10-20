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

#include "LFGPackets.h"
#include "PacketGuid128.h"

void WorldPackets::LFG::LFGJoin::Read()
{
    // 3.4.3 (writer FUN_140766930, Lua JoinLFG / JoinSingleLFG): bits QueueAsGroup, HasPartyIndex, Unknown,
    // flush, u8 Roles, u32 slot count, [u8 PartyIndex], the slots (LFGDungeons ID | type << 24). No comment.
    QueueAsGroup = _worldPacket.ReadBit();
    bool hasPartyIndex = _worldPacket.ReadBit();
    _worldPacket.ReadBit();
    _worldPacket.ResetBitReader();
    Roles = _worldPacket.read<uint8>();
    uint32 count = _worldPacket.read<uint32>();
    if (hasPartyIndex)
        _worldPacket.read_skip<uint8>();
    Slots.resize(count);
    for (uint32& slot : Slots)
        _worldPacket >> slot;
}

void WorldPackets::LFG::ReadRideTicket(ByteBuffer& data, RideTicket& ticket)
{
    // RideTicket, 3.4.3 (FUN_14070f950): PackedGuid128 RequesterGuid, u32 Id, u32 Type, i64 Time, bit, flush
    ticket.RequesterGuid = Guid128::Read(data);
    ticket.Id = data.read<uint32>();
    ticket.Type = data.read<uint32>();
    ticket.Time = data.read<int64>();
    data.ReadBit();
    data.ResetBitReader();
}

void WorldPackets::LFG::LFGLeave::Read()
{
    // 3.4.3 (writer FUN_140766a60, Lua LeaveLFG): the RideTicket of the queue
    ReadRideTicket(_worldPacket, Ticket);
}
