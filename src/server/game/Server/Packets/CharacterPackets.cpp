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

#include "CharacterPackets.h"

// 3.4.3 (writer FUN_140761180): bit ShowCloak, flush
void WorldPackets::Character::ShowingCloak::Read()
{
    ShowCloak = _worldPacket.ReadBit();
}

// 3.4.3 (writer FUN_1407611e0): bit ShowHelm, flush
void WorldPackets::Character::ShowingHelm::Read()
{
    ShowHelm = _worldPacket.ReadBit();
}

WorldPacket const* WorldPackets::Character::LogoutResponse::Write()
{
    // 3.4.3 (parser FUN_1406b89a0, handler FUN_14156ca30): i32 LogoutResult, bit Instant, flush
    _worldPacket << int32(LogoutResult);
    _worldPacket.WriteBit(Instant);
    _worldPacket.FlushBits();
    return &_worldPacket;
}

void WorldPackets::Character::PlayedTimeClient::Read()
{
    // 3.4.3: bit TriggerScriptEvent, flush (writer FUN_14075ff50)
    TriggerScriptEvent = _worldPacket.ReadBit();
}

WorldPacket const* WorldPackets::Character::PlayedTime::Write()
{
    // 3.4.3: u32 TotalTime, u32 LevelTime, bit TriggerScriptEvent, flush (FUN_1406c7bb0)
    _worldPacket << uint32(TotalTime);
    _worldPacket << uint32(LevelTime);
    _worldPacket.WriteBit(TriggerScriptEvent);
    _worldPacket.FlushBits();

    return &_worldPacket;
}
