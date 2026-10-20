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

#include "Log.h"
#include "PacketGuid128.h"
#include "Opcodes.h"
#include "Player.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

void WorldSession::HandleLearnTalentOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075e5d0): i32 TalentID, u16 RequestedRank (the 0-based rank being learned)
    uint32 talent_id = recvData.read<uint32>();
    uint32 requested_rank = recvData.read<uint16>();

    _player->LearnTalent(talent_id, requested_rank);
    _player->SendTalentsInfoData(false);
}

void WorldSession::HandleLearnPreviewTalents(WorldPacket& recvPacket)
{
    LOG_DEBUG("network", "CMSG_LEARN_PREVIEW_TALENTS");

    // 3.4.3 (writer FUN_14075e620): u32 Count, Count x {i32 TalentID, u8 Rank}; no talent group
    uint32 talentsCount;
    recvPacket >> talentsCount;

    // Client has max 44 talents for tree for 3 trees, rounded up : 150
    uint32 const MaxTalentsCount = 150;

    for (uint32 i = 0; i < talentsCount && i < MaxTalentsCount; ++i)
    {
        uint32 talentId = recvPacket.read<uint32>();
        uint32 talentRank = recvPacket.read<uint8>();

        _player->LearnTalent(talentId, talentRank);
    }

    _player->SendTalentsInfoData(false);

    recvPacket.rfinish();
}

void WorldSession::HandleTalentWipeConfirmOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "CMSG_CONFIRM_RESPEC_WIPE");
    // 3.4.3 (writer FUN_14075d030): PackedGuid128 RespecMaster, u8 RespecType (0 talents, as asked)
    ObjectGuid guid = Guid128::Read(recvData);
    recvData.read_skip<uint8>();

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_TRAINER);
    if (!unit)
    {
        LOG_DEBUG("network", "WORLD: HandleTalentWipeConfirmOpcode - Unit ({}) not found or you can't interact with him.", guid.ToString());
        return;
    }

    if (!unit->CanResetTalents(_player))
        return;

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    if (!(_player->resetTalents()))
    {
        // no talents to reset: an empty master makes the client show ERR_TALENT_WIPE_ERROR
        WorldPacket data(SMSG_RESPEC_WIPE_CONFIRM, 1 + 4 + 2);
        data << int8(0);
        data << uint32(0);
        Guid128::Write(data, ObjectGuid::Empty);
        SendPacket(&data);
        return;
    }

    _player->SendTalentsInfoData(false);
    unit->CastSpell(_player, 14867, true);                  //spell: "Untalent Visual Effect"
}

void WorldSession::HandleUnlearnSkillOpcode(WorldPacket& recvData)
{
    uint32 skillId;
    recvData >> skillId;

    if (!IsPrimaryProfessionSkill(skillId))
        return;

    GetPlayer()->SetSkill(skillId, 0, 0, 0);
}
