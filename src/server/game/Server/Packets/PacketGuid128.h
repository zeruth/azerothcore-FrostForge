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
#include "Optional.h"
#include "Realm.h"
#include "World.h"
#include <limits>
#include <utility>

/// The 3.4.3 client's 128-bit GUIDs on the wire.
///
/// The client packs one (FUN_141550b20 reads, FUN_141550bf0 writes) as a mask of the low half's
/// non-zero bytes, a mask of the high half's, then those bytes in order.
///
/// The high half carries the GUID type in its top 6 bits (the client takes `high >> 58` everywhere
/// it asks what an object is, and rejects types >= 0x37 in object updates). Under it:
///   * Player (2), Item (3): the realm id at bit 42; the counter is the low half
///     (players: char-create.ANALYSIS.md 3; items: TrinityCore's layout).
///   * Creature (8), Vehicle (9), Pet (10), GameObject (11), DynamicObject (12), Corpse (14): the
///     realm id at bit 42, the map id at bit 29 (13 bits), the entry at bit 6 (23 bits); the low half
///     holds the counter. The client reads the entry back as `(high >> 6) & 0x7fffff` for types
///     8/9/10/11 (e.g. FUN_1419d9520's callers) and the map as `(high >> 29) & 0x1fff`.
///   * Transport (6), AC's moving transports: the counter at bit 38 of the high half, low half 0.
///
/// AC's 64-bit GUIDs carry no map id, and every packet must name an object the same way, so the map
/// field is left 0 [TODO: the client reads it only for display (FUN_1415620c0); fill it if a use turns
/// up]. The mapping is a pure function of AC's GUID and back (Read), so the client's GUIDs in requests
/// resolve to AC's objects.
namespace Guid128
{
    enum Type : uint8
    {
        TYPE_NULL           = 0,
        TYPE_PLAYER         = 2,
        TYPE_ITEM           = 3,
        TYPE_TRANSPORT      = 6,
        TYPE_CREATURE       = 8,
        TYPE_VEHICLE        = 9,
        TYPE_PET            = 10,
        TYPE_GAMEOBJECT     = 11,
        TYPE_DYNAMICOBJECT  = 12,
        TYPE_CORPSE         = 14,
        TYPE_PARTY          = 27,
    };

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

    inline uint64 RealmBits()
    {
        return uint64(realm.Id.Realm & 0x1FFF) << 42;
    }

    /// The high half of this realm's player GUIDs.
    inline uint64 PlayerHigh()
    {
        return (uint64(TYPE_PLAYER) << 58) | RealmBits();
    }

    /// The 3.4.3 GUID of an AC object, as {low, high}.
    inline std::pair<uint64, uint64> Encode(ObjectGuid const& guid)
    {
        if (guid.IsEmpty())
            return { 0, 0 };

        auto world = [&guid](Type type) -> std::pair<uint64, uint64>
        {
            uint64 high = (uint64(type) << 58) | RealmBits() | (uint64(guid.GetEntry() & 0x7FFFFF) << 6);
            return { uint64(guid.GetCounter()), high };
        };

        switch (guid.GetHigh())
        {
            case HighGuid::Player:        return { uint64(guid.GetCounter()), PlayerHigh() };
            case HighGuid::Item:          return { uint64(guid.GetCounter()), (uint64(TYPE_ITEM) << 58) | RealmBits() };
            case HighGuid::Unit:          return world(TYPE_CREATURE);
            case HighGuid::Vehicle:       return world(TYPE_VEHICLE);
            case HighGuid::Pet:           return world(TYPE_PET);
            case HighGuid::GameObject:
            case HighGuid::Transport:     return world(TYPE_GAMEOBJECT);
            case HighGuid::DynamicObject: return world(TYPE_DYNAMICOBJECT);
            case HighGuid::Corpse:        return world(TYPE_CORPSE);
            case HighGuid::Mo_Transport:  return { 0, (uint64(TYPE_TRANSPORT) << 58) | (uint64(guid.GetCounter()) << 38) };
            case HighGuid::Group:         return { uint64(guid.GetCounter()), uint64(TYPE_PARTY) << 58 };
            default:                      return { uint64(guid.GetCounter()), 0 };  // Instance: never sent as a GUID
        }
    }

    /// The AC object a 3.4.3 GUID names; Empty for one this server never hands out.
    inline ObjectGuid Decode(uint64 low, uint64 high)
    {
        if (!low && !high)
            return ObjectGuid::Empty;

        uint8 type = uint8(high >> 58);
        uint32 entry = uint32(high >> 6) & 0x7FFFFF;
        bool thisRealm = ((high >> 42) & 0x1FFF) == (realm.Id.Realm & 0x1FFF);
        auto counter = [low](HighGuid hi) -> Optional<ObjectGuid::LowType>
        {
            if (!low || low > ObjectGuid::GetMaxCounter(hi))
                return {};
            return ObjectGuid::LowType(low);
        };

        switch (type)
        {
            case TYPE_PLAYER:
                if (thisRealm && high == PlayerHigh())
                    if (auto c = counter(HighGuid::Player))
                        return ObjectGuid(HighGuid::Player, *c);
                break;
            case TYPE_ITEM:
                if (thisRealm)
                    if (auto c = counter(HighGuid::Item))
                        return ObjectGuid(HighGuid::Item, *c);
                break;
            case TYPE_CREATURE:      if (auto c = counter(HighGuid::Unit)) return ObjectGuid(HighGuid::Unit, entry, *c); break;
            case TYPE_VEHICLE:       if (auto c = counter(HighGuid::Vehicle)) return ObjectGuid(HighGuid::Vehicle, entry, *c); break;
            case TYPE_PET:           if (auto c = counter(HighGuid::Pet)) return ObjectGuid(HighGuid::Pet, entry, *c); break;
            case TYPE_GAMEOBJECT:    if (auto c = counter(HighGuid::GameObject)) return ObjectGuid(HighGuid::GameObject, entry, *c); break;
            case TYPE_DYNAMICOBJECT: if (auto c = counter(HighGuid::DynamicObject)) return ObjectGuid(HighGuid::DynamicObject, *c); break;
            case TYPE_CORPSE:        if (auto c = counter(HighGuid::Corpse)) return ObjectGuid(HighGuid::Corpse, *c); break;
            case TYPE_TRANSPORT:
                if (!low)
                    if (ObjectGuid::LowType c = ObjectGuid::LowType((high >> 38) & 0xFFFFF))
                        return ObjectGuid(HighGuid::Mo_Transport, c);
                break;
            case TYPE_PARTY:         if (auto c = counter(HighGuid::Group)) return ObjectGuid(HighGuid::Group, *c); break;
            default:
                break;
        }
        return ObjectGuid::Empty;
    }

    /// Any object's GUID (or the empty GUID) as the 3.4.3 client knows it.
    inline void Write(ByteBuffer& data, ObjectGuid const& guid)
    {
        auto [low, high] = Encode(guid);
        WritePacked(data, low, high);
    }

    /// A GUID from the client, as the AC object it names (Empty if none).
    inline ObjectGuid Read(ByteBuffer& data)
    {
        uint64 low, high;
        ReadPacked(data, low, high);
        return Decode(low, high);
    }

    /// A player GUID (or the empty GUID).
    inline void WritePlayer(ByteBuffer& data, ObjectGuid const& guid)
    {
        Write(data, guid);
    }

    /// A character GUID from the client: HighGuid Player, this realm, a 32-bit counter. Empty otherwise.
    inline ObjectGuid ReadPlayer(ByteBuffer& data)
    {
        ObjectGuid guid = Read(data);
        return guid.IsPlayer() ? guid : ObjectGuid::Empty;
    }

    /// An item GUID (or the empty GUID).
    inline void WriteItem(ByteBuffer& data, ObjectGuid const& guid)
    {
        Write(data, guid);
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
