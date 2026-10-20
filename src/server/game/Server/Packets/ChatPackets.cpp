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

#include "ChatPackets.h"
#include "PacketGuid128.h"

WorldPacket const* WorldPackets::Chat::Emote::Write()
{
    // 3.4.3: PackedGuid128 Guid, u32 EmoteID, u32 SpellVisualKitIDs count,
    // u32 SequenceVariation, the kit ids
    Guid128::Write(_worldPacket, Guid);
    _worldPacket << uint32(EmoteID);
    _worldPacket << uint32(0);
    _worldPacket << uint32(0);

    return &_worldPacket;
}

void WorldPackets::Chat::EmoteClient::Read()
{
    // 3.4.3: empty
}

WorldPacket const* WorldPackets::Chat::ChatServerMessage::Write()
{
    // 3.4.3 (inline in the chat switch, handler FUN_141906bd0): i32 MessageID (ServerMessages.db2),
    // 11-bit parameter length, flush, the parameter
    std::string_view param = std::string_view(StringParam).substr(0, std::min<std::size_t>(StringParam.size(), 0x7FF));
    _worldPacket << int32(MessageID);
    _worldPacket.WriteBits(param.size(), 11);
    _worldPacket.FlushBits();
    _worldPacket.WriteString(param);

    return &_worldPacket;
}
