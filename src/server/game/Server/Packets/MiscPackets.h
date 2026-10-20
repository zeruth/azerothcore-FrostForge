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

#ifndef MiscPackets_h__
#define MiscPackets_h__

#include "ObjectGuid.h"
#include "Packet.h"
#include "Weather.h"

enum WeatherState : uint32;

namespace WorldPackets
{
    namespace Misc
    {
        class AC_GAME_API Weather final : public ServerPacket
        {
        public:
            Weather();
            Weather(WeatherState weatherID, float intensity = 0.0f, bool abrupt = false);

            WorldPacket const* Write() override;

            bool Abrupt = false;
            float Intensity = 0.0f;
            WeatherState WeatherID = WeatherState(0);
        };

        class LevelUpInfo final : public ServerPacket
        {
        public:
            LevelUpInfo() : ServerPacket(SMSG_LEVEL_UP_INFO, 56) { }

            WorldPacket const* Write() override;

            uint32 Level = 0;
            uint32 HealthDelta = 0;
            std::array<uint32, MAX_POWERS> PowerDelta = { };
            std::array<uint32, MAX_STATS> StatDelta = { };
        };

        class AC_GAME_API PlayMusic final : public ServerPacket
        {
        public:
            // cppcheck-suppress missingReturn
            PlayMusic() : ServerPacket(SMSG_PLAY_MUSIC, 4) { }
            PlayMusic(uint32 soundKitID) : ServerPacket(SMSG_PLAY_MUSIC, 4), SoundKitID(soundKitID) { }

            WorldPacket const* Write() override;

            uint32 SoundKitID = 0;
        };

        class AC_GAME_API PlayObjectSound final : public ServerPacket
        {
        public:
            // cppcheck-suppress missingReturn
            PlayObjectSound() : ServerPacket(SMSG_PLAY_OBJECT_SOUND, 4 + 8) { }
            PlayObjectSound(ObjectGuid const& sourceObjectGUID, uint32 soundKitID)
                : ServerPacket(SMSG_PLAY_OBJECT_SOUND, 4 + 8), SourceObjectGUID(sourceObjectGUID), SoundKitID(soundKitID) { }

            WorldPacket const* Write() override;

            ObjectGuid SourceObjectGUID;
            uint32 SoundKitID = 0;
            ::Position Position;
        };

        class AC_GAME_API Playsound final : public ServerPacket
        {
        public:
            // cppcheck-suppress missingReturn
            Playsound() : ServerPacket(SMSG_PLAY_SOUND, 4) { }
            Playsound(uint32 soundKitID) : ServerPacket(SMSG_PLAY_SOUND, 4), SoundKitID(soundKitID) { }
            Playsound(uint32 soundKitID, ObjectGuid source) : ServerPacket(SMSG_PLAY_SOUND, 4 + 18 + 4), SoundKitID(soundKitID), SourceObjectGUID(source) { }

            WorldPacket const* Write() override;

            uint32 SoundKitID = 0;
            ObjectGuid SourceObjectGUID;
        };

        class MinimapPingClient final : public ClientPacket
        {
        public:
            MinimapPingClient(WorldPacket&& packet) : ClientPacket(CMSG_MINIMAP_PING, std::move(packet)) {}

            void Read() override;

            float MapX = 0.0f; // Raw position coordinates
            float MapY = 0.0f;
        };

        class MinimapPing final : public ServerPacket
        {
        public:
            MinimapPing() : ServerPacket(SMSG_MINIMAP_PING, 8 + 4 + 4) { }

            WorldPacket const* Write() override;

            ObjectGuid SourceGuid;
            float MapX = 0.0f;
            float MapY = 0.0f;
        };

        class RandomRollClient final : public ClientPacket
        {
        public:
            RandomRollClient(WorldPacket&& packet) : ClientPacket(CMSG_RANDOM_ROLL, std::move(packet)) { }

            void Read() override;

            uint32 Min = 0;
            uint32 Max = 0;
        };

        class RandomRoll final : public ServerPacket
        {
        public:
            RandomRoll() : ServerPacket(SMSG_RANDOM_ROLL, 4 + 4 + 4 + 8) { }

            WorldPacket const* Write() override;

            uint32 Min = 0;
            uint32 Max = 0;
            uint32 Result = 0;
            ObjectGuid Roller;
        };

        class StartMirrorTimer final : public ServerPacket
        {
        public:
            StartMirrorTimer() : ServerPacket(SMSG_START_MIRROR_TIMER, 21) { }
            StartMirrorTimer(uint32 timer, uint32 value, uint32 maxValue, int32 scale, bool paused, uint32 spellID) :
                    ServerPacket(SMSG_START_MIRROR_TIMER, 21), Timer(timer), Value(value), MaxValue(maxValue), Scale(scale), Paused(paused), SpellID(spellID) { }

            WorldPacket const* Write() override;

            uint32 Timer = 0;
            uint32 Value = 0;
            uint32 MaxValue = 0;
            int32 Scale = 0;
            bool Paused = false;
            uint32 SpellID = 0;
        };

        class PauseMirrorTimer final : public ServerPacket
        {
        public:
            PauseMirrorTimer() : ServerPacket(SMSG_PAUSE_MIRROR_TIMER, 5) { }
            PauseMirrorTimer(uint32 timer, bool paused) : ServerPacket(SMSG_PAUSE_MIRROR_TIMER, 5), Timer(timer), Paused(paused) { }

            WorldPacket const* Write() override;

            uint32 Timer = 0;
            bool Paused = true;
        };

        class StopMirrorTimer final : public ServerPacket
        {
        public:
            StopMirrorTimer() : ServerPacket(SMSG_STOP_MIRROR_TIMER, 4) { }
            StopMirrorTimer(uint32 timer) : ServerPacket(SMSG_STOP_MIRROR_TIMER, 4), Timer(timer) { }

            WorldPacket const* Write() override;

            uint32 Timer = 0;
        };

        class DurabilityDamageDeath final : public ServerPacket
        {
        public:
            DurabilityDamageDeath() : ServerPacket(SMSG_DURABILITY_DAMAGE_DEATH, 4) { }

            // 3.4.3: read raw (FUN_1406b55f0); the handler (FUN_141910c40) formats DURABILITYDAMAGE_DEATH
            // with the first u32
            WorldPacket const* Write() override { _worldPacket << uint32(Percent); return &_worldPacket; }

            uint32 Percent = 10;
        };

        class CrossedInebriationThreshold final : public ServerPacket
        {
        public:
            CrossedInebriationThreshold() : ServerPacket(SMSG_CROSSED_INEBRIATION_THRESHOLD, 8 + 4 + 4) { }

            WorldPacket const* Write() override;

            ObjectGuid Guid;
            uint32 Threshold = 0;
            uint32 ItemID = 0;

        };

        class Complain final : public ClientPacket
        {
        public:
            Complain(WorldPacket&& packet) : ClientPacket(CMSG_COMPLAINT, std::move(packet)) {}

            void Read() override;

            uint8 SpamType              = 0; // ComplaintType: 0 - mail, 1 - chat, 2 - calendar
            ObjectGuid SpammerGuid;          // Offender
            uint32 Unk1                 = 0; // chat: Command
            uint64 MailIdOrMessageType  = 0; // mail: MailID; calendar: EventID
            uint32 ChannelId            = 0;
            uint32 SecondsSinceMessage  = 0; // TimeSinceOffence
            std::string Description     = ""; // chat: MessageLog
        };

        class ComplainResult final : public ServerPacket
        {
        public:
            ComplainResult() : ServerPacket(SMSG_COMPLAINT_RESULT, 5) {}

            WorldPacket const* Write() override;

            uint32 ComplaintType = 0;
            uint8 Unk = 0;                   // Result
        };

    }
}

#endif // MiscPackets_h__
