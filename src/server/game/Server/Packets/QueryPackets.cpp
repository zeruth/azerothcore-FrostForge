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

#include "QueryPackets.h"
#include "PacketGuid128.h"

void WorldPackets::Query::NameQuery::Read()
{
    uint32 count = _worldPacket.read<uint32>();
    if (count > 1000)                                   // the client asks in batches of at most a few hundred
        count = 0;
    Players.reserve(count);
    for (uint32 i = 0; i < count; ++i)
        Players.push_back(Guid128::Read(_worldPacket));
}

WorldPacket const* WorldPackets::Query::NameQueryResponse::Write()
{
    // 3.4.3 (FUN_1406c8310): u32 count, then per player u8 Result, PackedGuid128 Player, bit HasData,
    // bit HasUnused920, flush, [PlayerGuidLookupData], [{u32, PackedGuid128, 7-bit length, string}]
    _worldPacket << uint32(Players.size());
    for (NameCacheLookupResult const& p : Players)
    {
        _worldPacket << uint8(p.Result);
        Guid128::Write(_worldPacket, p.Player);
        _worldPacket.WriteBit(p.HasData);
        _worldPacket.WriteBit(false);
        _worldPacket.FlushBits();
        if (!p.HasData)
            continue;

        // PlayerGuidLookupData (FUN_140717ea0): bit IsDeleted, 6-bit name length, five 7-bit declined
        // name lengths, flush, the declined names, PackedGuid128 AccountID, BnetAccountID, GuidActual,
        // u64 GuildClubMemberID, u32 VirtualRealmAddress, u8 Race, Sex, ClassID, Level, u8 Unused915,
        // the name
        _worldPacket.WriteBit(p.IsDeleted);
        _worldPacket.WriteBits(p.Name.size(), 6);
        for (uint8 i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
            _worldPacket.WriteBits(p.DeclinedNames ? p.DeclinedNames->name[i].size() : 0, 7);
        _worldPacket.FlushBits();
        if (p.DeclinedNames)
            for (uint8 i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
                _worldPacket.WriteString(p.DeclinedNames->name[i]);

        Guid128::WritePacked(_worldPacket, 0, 0);       // AccountID [TODO: the WowAccount GUID, if a use turns up]
        Guid128::WritePacked(_worldPacket, 0, 0);       // BnetAccountID
        Guid128::Write(_worldPacket, p.Player);         // GuidActual
        _worldPacket << uint64(0);                      // GuildClubMemberID
        _worldPacket << uint32(Guid128::VirtualRealmAddress());
        _worldPacket << uint8(p.Race);
        _worldPacket << uint8(p.Sex);
        _worldPacket << uint8(p.Class);
        _worldPacket << uint8(p.Level);
        _worldPacket << uint8(0);                       // Unused915
        _worldPacket.WriteString(p.Name);
    }

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Query::TimeQueryResponse::Write()
{
    _worldPacket << int64(CurrentTime);

    return &_worldPacket;
}

void WorldPackets::Query::CorpseMapPositionQuery::Read()
{
    // 3.4.3 (writer FUN_1407688e0): PackedGuid128 Player, PackedGuid128 Transport
    Guid128::Skip(_worldPacket);
    Guid128::Skip(_worldPacket);
}
