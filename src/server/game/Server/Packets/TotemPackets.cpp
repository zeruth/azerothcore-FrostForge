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

#include "TotemPackets.h"
#include "PacketGuid128.h"

void WorldPackets::Totem::TotemDestroyed::Read()
{
    // 3.4.3 (writer FUN_140761260): u8 Slot, PackedGuid128 TotemGUID
    _worldPacket >> Slot;
    Guid128::Skip(_worldPacket);
}

WorldPacket const* WorldPackets::Totem::TotemCreated::Write()
{
    // 3.4.3 (FUN_1406cc290): u8 Slot, PackedGuid128 Totem, u32 Duration, u32 SpellID, f32 TimeMod,
    // bit CannotDismiss, flush
    _worldPacket << uint8(Slot);
    Guid128::Write(_worldPacket, Totem);
    _worldPacket << uint32(Duration);
    _worldPacket << uint32(SpellID);
    _worldPacket << float(1.0f);
    _worldPacket.WriteBit(false);
    _worldPacket.FlushBits();

    return &_worldPacket;
}
