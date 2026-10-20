/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "NPCPackets.h"
#include "PacketGuid128.h"

void WorldPackets::NPC::Hello::Read()
{
    // 3.4.3: PackedGuid128 Unit (CMSG_TRAINER_LIST: writer FUN_140761300)
    Unit = Guid128::Read(_worldPacket);
}

WorldPacket const* WorldPackets::NPC::TrainerList::Write()
{
    // 3.4.3 (FUN_1406cc4b0, FUN_140706a90; the handler FUN_141a545d0 opens the trainer frame):
    // PackedGuid128 TrainerGUID, u32 TrainerType, u32 TrainerID (echoed in CMSG_TRAINER_BUY_SPELL), u32
    // count, per spell {u32 SpellID, u32 MoneyCost, u32 ReqSkillLine, u32 ReqSkillRank, u32 ReqAbility[3],
    // u8 Usable (0 available, 1 unavailable, 2 known), u8 ReqLevel}, 11-bit greeting length, flush, the
    // greeting. 3.4.3 has no talent point cost.
    std::string_view greeting = std::string_view(Greeting).substr(0, std::min<std::size_t>(Greeting.size(), 0x7FF));
    Guid128::Write(_worldPacket, TrainerGUID);
    _worldPacket << int32(TrainerType);
    _worldPacket << uint32(TrainerID);

    _worldPacket << uint32(Spells.size());
    for (TrainerListSpell const& spell : Spells)
    {
        _worldPacket << int32(spell.SpellID);
        _worldPacket << int32(spell.MoneyCost);
        _worldPacket << int32(spell.ReqSkillLine);
        _worldPacket << int32(spell.ReqSkillRank);
        for (int32 ability : spell.ReqAbility)
            _worldPacket << int32(ability);
        _worldPacket << uint8(spell.Usable);
        _worldPacket << uint8(spell.ReqLevel);
    }

    _worldPacket.WriteBits(greeting.size(), 11);
    _worldPacket.FlushBits();
    _worldPacket.WriteString(greeting);

    return &_worldPacket;
}

void WorldPackets::NPC::TrainerBuySpell::Read()
{
    // 3.4.3 (writer FUN_1407612b0): PackedGuid128 TrainerGUID, u32 TrainerID, u32 SpellID
    TrainerGUID = Guid128::Read(_worldPacket);
    _worldPacket >> TrainerID;
    _worldPacket >> SpellID;
}

WorldPacket const* WorldPackets::NPC::TrainerBuyFailed::Write()
{
    // 3.4.3 (FUN_1406b6fc0): PackedGuid128 TrainerGUID, u32 SpellID, u32 TrainerFailedReason (0
    // unavailable, 1 not enough money; the handler FUN_1417386c0 only logs it)
    Guid128::Write(_worldPacket, TrainerGUID);
    _worldPacket << int32(SpellID);
    _worldPacket << int32(TrainerFailedReason);

    return &_worldPacket;
}
