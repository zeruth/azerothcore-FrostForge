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

#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

void WorldSession::HandleDuelAcceptedOpcode(WorldPacket& recvPacket)
{
    // CMSG_DUEL_RESPONSE, 3.4.3 (writer FUN_14075d3d0): PackedGuid128 ArbiterGUID, bits Accepted, Forfeited,
    // flush. 3.3.5 had an opcode for accepting and one for cancelling.
    ObjectGuid guid = Guid128::Read(recvPacket);
    bool accepted = recvPacket.ReadBit();
    recvPacket.ReadBit();                                   // Forfeited
    recvPacket.ResetBitReader();

    if (!accepted)
    {
        HandleDuelCancelled(guid);
        return;
    }

    Player* player = GetPlayer();
    // the requester's client accepts by itself: only the challenged one starts the countdown
    if (!player->duel || player == player->duel->Initiator || player->duel->State != DUEL_STATE_CHALLENGED)
        return;

    Player* target = player->duel->Opponent;
    if (target->GetGuidValue(PLAYER_DUEL_ARBITER) != guid)
        return;

    LOG_DEBUG("network.opcode", "Player 1 is: {} ({})", player->GetGUID().ToString(), player->GetName());
    LOG_DEBUG("network.opcode", "Player 2 is: {} ({})", target->GetGUID().ToString(), target->GetName());

    time_t now = GameTime::GetGameTime().count();
    player->duel->StartTime = now + 3;
    target->duel->StartTime = now + 3;

    player->duel->State = DUEL_STATE_COUNTDOWN;
    target->duel->State = DUEL_STATE_COUNTDOWN;

    player->SendDuelCountdown(3000);
    target->SendDuelCountdown(3000);
}

void WorldSession::HandleDuelCancelledOpcode(WorldPacket& recvPacket)
{
    HandleDuelCancelled(Guid128::Read(recvPacket));
}

void WorldSession::HandleCanDuel(WorldPacket& recvPacket)
{
    // CMSG_CAN_DUEL, 3.4.3 (writer FUN_140765770): PackedGuid128 TargetGUID, bit ToTheDeath, flush. The
    // answer, SMSG_CAN_DUEL_RESULT (PackedGuid128 target, bits Result, ToTheDeath), makes the client cast the
    // duel spell at the target.
    ObjectGuid target = Guid128::Read(recvPacket);
    recvPacket.ReadBit();
    recvPacket.ResetBitReader();

    Player* other = ObjectAccessor::FindPlayer(target);
    bool result = other && !GetPlayer()->duel && !other->duel;

    WorldPacket data(SMSG_CAN_DUEL_RESULT, 18 + 1);
    Guid128::Write(data, target);
    data.WriteBit(result);
    data.WriteBit(false);
    data.FlushBits();
    SendPacket(&data);
}

void WorldSession::HandleDuelCancelled(ObjectGuid /*arbiter*/)
{
    Player* player = GetPlayer();

    // no duel requested
    if (!player->duel || player->duel->State == DUEL_STATE_COMPLETED)
        return;

    // player surrendered in a duel using /forfeit
    if (GetPlayer()->duel->State == DUEL_STATE_IN_PROGRESS)
    {
        GetPlayer()->CombatStopWithPets(true);
        GetPlayer()->duel->Opponent->CombatStopWithPets(true);

        GetPlayer()->CastSpell(GetPlayer(), 7267, true);    // beg
        GetPlayer()->DuelComplete(DUEL_WON);
        return;
    }

    GetPlayer()->DuelComplete(DUEL_INTERRUPTED);
}
