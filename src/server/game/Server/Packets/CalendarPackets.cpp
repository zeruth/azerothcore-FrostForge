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

#include "CalendarPackets.h"
#include "Packet.h"
#include "PacketGuid128.h"

void WorldPackets::Calendar::GetEvent::Read()
{
    _worldPacket >> EventId;
}

/// CMSG_CALENDAR_COMPLAIN, 3.4.3 (FUN_140765110): PackedGuid128 InvitedByGUID, u64 EventID, u64 InviteID
void WorldPackets::Calendar::CalendarComplain::Read()
{
    ComplainGuid = Guid128::Read(_worldPacket);
    _worldPacket >> EventId;
    _worldPacket.read_skip<uint64>();                       // InviteID
}
