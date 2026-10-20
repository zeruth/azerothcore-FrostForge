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

#ifndef QueryPackets_h__
#define QueryPackets_h__

#include "Packet.h"
#include "Unit.h"

namespace WorldPackets
{
    namespace Query
    {
        /// 3.4.3 CMSG_QUERY_PLAYER_NAMES: u32 count, PackedGuid128 per player (writer FUN_14076a850).
        class NameQuery final : public ClientPacket
        {
        public:
            NameQuery(WorldPacket&& packet) : ClientPacket(CMSG_QUERY_PLAYER_NAMES, std::move(packet)) {}

            void Read() override;

            std::vector<ObjectGuid> Players;
        };

        /// One player of SMSG_QUERY_PLAYER_NAMES_RESPONSE (PlayerGuidLookupData, FUN_140717ea0).
        struct NameCacheLookupResult
        {
            ObjectGuid Player;
            uint8 Result = 1;               // the handler (FUN_140351180): 0 found, 1 unknown
            bool HasData = false;
            bool IsDeleted = false;
            std::string_view Name;
            DeclinedName const* DeclinedNames = nullptr;
            uint8 Race = RACE_NONE;
            uint8 Sex = GENDER_MALE;
            uint8 Class = CLASS_NONE;
            uint8 Level = 0;
        };

        class NameQueryResponse final : public ServerPacket
        {
        public:
            NameQueryResponse() : ServerPacket(SMSG_QUERY_PLAYER_NAMES_RESPONSE, 4 + 60) {}

            WorldPacket const* Write() override;

            std::vector<NameCacheLookupResult> Players;
        };

        class TimeQuery final : public ClientPacket
        {
        public:
            TimeQuery(WorldPacket&& packet) : ClientPacket(CMSG_QUERY_TIME, std::move(packet)) {}

            void Read() override {};
        };

        /// 3.4.3 SMSG_QUERY_TIME_RESPONSE: i64 CurrentTime only (handler FUN_141a6c040 reads 8 bytes).
        class TimeQueryResponse final : public ServerPacket
        {
        public:
            TimeQueryResponse() : ServerPacket(SMSG_QUERY_TIME_RESPONSE, 8) {}

            WorldPacket const* Write() override;

            int64 CurrentTime = 0;
        };

        class CorpseMapPositionQuery final : public ClientPacket
        {
        public:
            CorpseMapPositionQuery(WorldPacket&& packet) : ClientPacket(CMSG_QUERY_CORPSE_TRANSPORT, std::move(packet)) {}

            void Read() override;

            uint32 unk;
        };
    }
}

#endif // QueryPackets_h__
