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

#include "PetPackets.h"
#include "PacketGuid128.h"

void WorldPackets::Pet::DismissCritter::Read()
{
    CritterGUID = Guid128::Read(_worldPacket);              // 3.4.3: PackedGuid128 (FUN_14075d340)
}

void WorldPackets::Pet::PetAbandon::Read()
{
    PetGUID = Guid128::Read(_worldPacket);
}

void WorldPackets::Pet::PetStopAttack::Read()
{
    PetGUID = Guid128::Read(_worldPacket);                  // 3.4.3: PackedGuid128 (FUN_14075f1f0)
}

void WorldPackets::Pet::PetSpellAutocast::Read()
{
    // 3.4.3 (writer FUN_14075f180): PackedGuid128 PetGUID, u32 SpellID, bit AutocastEnabled, flush
    PetGUID = Guid128::Read(_worldPacket);
    _worldPacket >> SpellID;
    AutocastEnabled = _worldPacket.ReadBit();
}

WorldPacket const* WorldPackets::Pet::PetLearnedSpell::Write()
{
    _worldPacket << uint32(SpellID);
    return &_worldPacket;
}

WorldPacket const* WorldPackets::Pet::PetUnlearnedSpell::Write()
{
    _worldPacket << uint32(SpellID);
    return &_worldPacket;
}

WorldPacket const* WorldPackets::Pet::PetActionSound::Write()
{
    // 3.4.3 (FUN_1406b5650): PackedGuid128 UnitGUID, u32 Action
    Guid128::Write(_worldPacket, UnitGUID);
    _worldPacket << int32(Action);
    return &_worldPacket;
}

WorldPacket const* WorldPackets::Pet::PetDismissSound::Write()
{
    _worldPacket << int32(ModelId);
    _worldPacket << float(ModelPosition.x);
    _worldPacket << float(ModelPosition.y);
    _worldPacket << float(ModelPosition.z);
    return &_worldPacket;
}
