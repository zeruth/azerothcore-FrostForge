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

#ifndef PacketGuid128_h__
#define PacketGuid128_h__

#include "ByteBuffer.h"
#include "ObjectGuid.h"
#include "Realm.h"
#include "World.h"
#include <limits>

/// The 3.4.3 client's 128-bit GUIDs on the wire.
///
/// The client packs one (FUN_141550b20 reads, FUN_141550bf0 writes) as a mask of the low half's
/// non-zero bytes, a mask of the high half's, then those bytes in order. A player's GUID carries
/// HighGuid Player (2) in the top 6 bits of the high half and the realm id under it, with the
/// character counter in the low half (char-create.ANALYSIS.md 3).
///
/// Only player GUIDs are mapped from AC's 64-bit ObjectGuid so far. World objects need the map id
/// and entry in the high half; that mapping belongs with the object update conversion
/// (SMSG_UPDATE_OBJECT), which should add it here so every packet shares one.
namespace Guid128
{
    inline void WritePacked(ByteBuffer& data, uint64 low, uint64 high)
    {
        uint8 lowMask = 0, highMask = 0;
        for (uint8 i = 0; i < 8; ++i)
        {
            if ((low >> (8 * i)) & 0xFF)
                lowMask |= uint8(1 << i);
            if ((high >> (8 * i)) & 0xFF)
                highMask |= uint8(1 << i);
        }

        data << uint8(lowMask) << uint8(highMask);
        for (uint8 i = 0; i < 8; ++i)
            if (lowMask & (1 << i))
                data << uint8(low >> (8 * i));
        for (uint8 i = 0; i < 8; ++i)
            if (highMask & (1 << i))
                data << uint8(high >> (8 * i));
    }

    inline void ReadPacked(ByteBuffer& data, uint64& low, uint64& high)
    {
        uint8 lowMask = data.read<uint8>();
        uint8 highMask = data.read<uint8>();
        low = high = 0;
        for (uint8 i = 0; i < 8; ++i)
            if (lowMask & (1 << i))
                low |= uint64(data.read<uint8>()) << (8 * i);
        for (uint8 i = 0; i < 8; ++i)
            if (highMask & (1 << i))
                high |= uint64(data.read<uint8>()) << (8 * i);
    }

    /// The high half of this realm's player GUIDs.
    inline uint64 PlayerHigh()
    {
        return (uint64(2) << 58) | (uint64(realm.Id.Realm & 0x1FFF) << 42);
    }

    /// A player GUID (or the empty GUID) as the 3.4.3 client knows it.
    inline void WritePlayer(ByteBuffer& data, ObjectGuid const& guid)
    {
        if (guid.IsEmpty())
            WritePacked(data, 0, 0);
        else
            WritePacked(data, guid.GetCounter(), PlayerHigh());
    }

    /// A character GUID from the client: HighGuid Player, this realm, a 32-bit counter. Empty otherwise.
    inline ObjectGuid ReadPlayer(ByteBuffer& data)
    {
        uint64 low, high;
        ReadPacked(data, low, high);
        if (high != PlayerHigh() || !low || low > std::numeric_limits<ObjectGuid::LowType>::max())
            return ObjectGuid::Empty;
        return ObjectGuid::Create<HighGuid::Player>(ObjectGuid::LowType(low));
    }

    /// An item GUID: HighGuid Item (3) and the realm id in the high half, the item counter in the
    /// low half. [inferred: TrinityCore's item GUID layout; the client reads items' GUIDs only from
    /// the object update, whose conversion should confirm this or replace it.]
    inline void WriteItem(ByteBuffer& data, ObjectGuid const& guid)
    {
        if (guid.IsEmpty())
            WritePacked(data, 0, 0);
        else
            WritePacked(data, guid.GetCounter(), (uint64(3) << 58) | (uint64(realm.Id.Realm & 0x1FFF) << 42));
    }

    /// The Battle.net address of this realm, region << 24 | site << 16 | realm id, as the Battle.net
    /// service advertises it (BattlenetSession.cpp RealmAddress). Packets name a player's realm by it.
    inline uint32 VirtualRealmAddress()
    {
        return (1u << 24) | (1u << 16) | (realm.Id.Realm & 0xFFFF);
    }

    /// Skips a packed GUID the server has no use for.
    inline void Skip(ByteBuffer& data)
    {
        uint64 low, high;
        ReadPacked(data, low, high);
    }
}

#endif // PacketGuid128_h__
