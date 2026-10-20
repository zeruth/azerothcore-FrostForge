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

#include "InstancePackets.h"

WorldPacket const* WorldPackets::Instance::InstanceReset::Write()
{
    _worldPacket << MapId;

    return &_worldPacket;
}

// SMSG_INSTANCE_RESET_FAILED, 3.4.3 (FUN_1406c2a10, handler FUN_141906f50): u32 MapID, 2-bit Reason (AC's
// InstanceResetFailureReason), flush
WorldPacket const* WorldPackets::Instance::InstanceResetFailed::Write()
{
    _worldPacket << uint32(MapId);
    _worldPacket.WriteBits(uint32(Reason), 2);
    _worldPacket.FlushBits();

    return &_worldPacket;
}

// SMSG_SET_DUNGEON_DIFFICULTY, 3.4.3: i32 DifficultyID, a
// Difficulty.db2 id: AC's 5-man normal / heroic (0 / 1) are 1 / 2.
WorldPacket const* WorldPackets::Instance::SetDungeonDifficulty::Write()
{
    _worldPacket << int32(Difficulty + 1);

    return &_worldPacket;
}

// CMSG_SET_DUNGEON_DIFFICULTY, 3.4.3 (writer FUN_140769130, Lua SetDungeonDifficultyID): u32 DifficultyID, a
// Difficulty.db2 id; the inverse of SMSG_SET_DUNGEON_DIFFICULTY's (AC's 0 / 1 are 1 / 2). Anything else is
// out of AC's range and refused by the handler.
void WorldPackets::Instance::SetDungeonDifficultyClient::Read()
{
    uint32 difficultyId = _worldPacket.read<uint32>();
    Mode = difficultyId - 1;
}

// SMSG_RESET_FAILED_NOTIFY, 3.4.3 (handler FUN_141907330): empty, the client reads nothing
WorldPacket const* WorldPackets::Instance::ResetFailedNotify::Write()
{

    return &_worldPacket;
}

// SMSG_RAID_DIFFICULTY_SET, 3.4.3: i32 DifficultyID, u8 Legacy
// (which of the two raid difficulty slots); AC's 10/25 normal/heroic (0-3) are Difficulty.db2's 3-6.
WorldPacket const* WorldPackets::Instance::SetRaidDifficulty::Write()
{
    _worldPacket << int32(Difficulty + 3);
    _worldPacket << uint8(0);

    return &_worldPacket;
}

// CMSG_SET_RAID_DIFFICULTY, 3.4.3 (writer FUN_140769570, Lua SetRaidDifficultyID / SetLegacyRaidDifficultyID):
// u32 DifficultyID (Difficulty.db2: AC's 0-3 are 3-6, the inverse of SMSG_RAID_DIFFICULTY_SET), u8 Legacy
void WorldPackets::Instance::SetRaidDifficultyClient::Read()
{
    uint32 difficultyId = _worldPacket.read<uint32>();
    _worldPacket.read_skip<uint8>();
    Mode = difficultyId - 3;
}

// CMSG_INSTANCE_LOCK_RESPONSE, 3.4.3 (writer FUN_14075e220, Lua RespondInstanceLock): bit AcceptLock, flush
void WorldPackets::Instance::InstanceLockResponse::Read()
{
    Accept = _worldPacket.ReadBit();
}
