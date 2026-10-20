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

#include "MiscPackets.h"
#include "PacketGuid128.h"

WorldPackets::Misc::Weather::Weather() : ServerPacket(SMSG_WEATHER, 4 + 4 + 1) { }

WorldPackets::Misc::Weather::Weather(WeatherState weatherID, float intensity /*= 0.0f*/, bool abrupt /*= false*/)
    : ServerPacket(SMSG_WEATHER, 4 + 4 + 1), Abrupt(abrupt), Intensity(intensity), WeatherID(weatherID) { }

WorldPacket const* WorldPackets::Misc::Weather::Write()
{
    // 3.4.3: u32 WeatherID, f32 Intensity, bit Abrupt, flush
    _worldPacket << uint32(WeatherID);
    _worldPacket << float(Intensity);
    _worldPacket.WriteBit(Abrupt);
    _worldPacket.FlushBits();

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::LevelUpInfo::Write()
{
    // 3.4.3: the handler (FUN_14170e5d0) reads 76 bytes unchecked: i32 Level, i32 HealthDelta,
    // i32 PowerDelta[10], i32 StatDelta[5], i32 NumNewTalents, i32 NumNewPvpTalentSlots
    _worldPacket << uint32(Level);
    _worldPacket << uint32(HealthDelta);

    for (std::size_t i = 0; i < 10; ++i)
        _worldPacket << uint32(i < PowerDelta.size() ? PowerDelta[i] : 0);

    for (uint32 stat : StatDelta)
        _worldPacket << stat;

    _worldPacket << int32(0);                               // NumNewTalents
    _worldPacket << int32(0);                               // NumNewPvpTalentSlots

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::PlayMusic::Write()
{
    _worldPacket << SoundKitID;

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::PlayObjectSound::Write()
{
    _worldPacket << SoundKitID;
    _worldPacket << SourceObjectGUID;

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::Playsound::Write()
{
    _worldPacket << SoundKitID;

    return &_worldPacket;
}

void WorldPackets::Misc::MinimapPingClient::Read()
{
    // 3.4.3 (writer FUN_140768230): bit HasPartyIndex, flush, f32 PositionX, f32 PositionY, [u8 PartyIndex]
    _worldPacket.ReadBit();
    _worldPacket.ResetBitReader();
    _worldPacket >> MapX;
    _worldPacket >> MapY;
    _worldPacket.rfinish();
}

WorldPacket const* WorldPackets::Misc::MinimapPing::Write()
{
    // 3.4.3 (FUN_1406c50d0): PackedGuid128 Sender, f32 PositionX, f32 PositionY
    Guid128::Write(_worldPacket, SourceGuid);
    _worldPacket << float(MapX);
    _worldPacket << float(MapY);

    return &_worldPacket;
}

void WorldPackets::Misc::RandomRollClient::Read()
{
    // 3.4.3 (writer FUN_140768a40): bit HasPartyIndex, flush, i32 Min, i32 Max, [u8 PartyIndex]
    _worldPacket.ReadBit();
    _worldPacket.ResetBitReader();
    _worldPacket >> Min;
    _worldPacket >> Max;
    _worldPacket.rfinish();
}

WorldPacket const* WorldPackets::Misc::RandomRoll::Write()
{
    // 3.4.3 (FUN_1406c8dd0): PackedGuid128 Roller, PackedGuid128 RollerWowAccount, i32 Min, i32 Max, i32 Result
    Guid128::Write(_worldPacket, Roller);
    Guid128::Write(_worldPacket, ObjectGuid::Empty);
    _worldPacket << int32(Min);
    _worldPacket << int32(Max);
    _worldPacket << int32(Result);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::StartMirrorTimer::Write()
{
    _worldPacket << uint32(Timer);
    _worldPacket << uint32(Value);
    _worldPacket << uint32(MaxValue);
    _worldPacket << int32(Scale);
    _worldPacket << uint8(Paused);
    _worldPacket << uint32(SpellID);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::PauseMirrorTimer::Write()
{
    _worldPacket << uint32(Timer);
    _worldPacket << uint8(Paused);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::StopMirrorTimer::Write()
{
    _worldPacket << uint32(Timer);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Misc::CrossedInebriationThreshold::Write()
{
    _worldPacket << Guid;
    _worldPacket << uint32(Threshold);
    _worldPacket << uint32(ItemID);

    return &_worldPacket;
}

void WorldPackets::Misc::Complain::Read()
{
    _worldPacket >> SpamType; // 0 - mail, 1 - chat
    _worldPacket >> SpammerGuid;
    switch (SpamType)
    {
    case 0:
        _worldPacket >> Unk1; // const 0
        _worldPacket >> MailIdOrMessageType; // probably mail id
        _worldPacket >> ChannelId; // const 0
        break;
    case 1:
        _worldPacket >> Unk1; // probably language
        _worldPacket >> MailIdOrMessageType;
        _worldPacket >> ChannelId;
        _worldPacket >> SecondsSinceMessage;
        _worldPacket >> Description; // spam description string (messagetype, channel name, player name, message)
        break;
    }
}

WorldPacket const* WorldPackets::Misc::ComplainResult::Write()
{
    _worldPacket << Unk;

    return &_worldPacket;
}
