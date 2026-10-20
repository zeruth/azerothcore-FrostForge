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

#ifndef __UPDATEDATA_H
#define __UPDATEDATA_H

#include "ByteBuffer.h"
#include "ObjectGuid.h"

class WorldPacket;

/// The update block types of SMSG_UPDATE_OBJECT (FrostLite docs/updateobject.md 2). 3.4.3 has no
/// movement, out-of-range or near-objects block: removals travel in the message header.
enum OBJECT_UPDATE_TYPE
{
    UPDATETYPE_VALUES               = 0,
    UPDATETYPE_CREATE_OBJECT        = 1,    // an object coming into view
    UPDATETYPE_CREATE_OBJECT2       = 2,    // an object just spawned (the client may play its birth)
};

/// One SMSG_UPDATE_OBJECT (0x27CB): u32 NumObjUpdates, u16 MapID, bit HasRemovedObjects, [u16
/// DestroyCount, u32 RemovedCount, PackedGuid128 x RemovedCount: the destroyed first, then those
/// going out of range], u32 DataSize, the blocks. The client drops the whole message if MapID is not
/// its current map, so it is the receiver's map.
class UpdateData
{
public:
    static constexpr uint32 NO_MAP = 0xFFFFFFFF;

    explicit UpdateData(uint32 mapId = NO_MAP);

    void SetMapId(uint32 mapId) { m_map = mapId; }
    [[nodiscard]] uint32 GetMapId() const { return m_map; }

    void AddOutOfRangeGUID(ObjectGuid guid);
    void AddDestroyObject(ObjectGuid guid);
    void AddUpdateBlock(ByteBuffer const& block);
    void AddUpdateBlock(UpdateData const& block);
    bool BuildPacket(WorldPacket& packet);
    [[nodiscard]] bool HasData() const { return m_blockCount > 0 || !m_outOfRangeGUIDs.empty() || !m_destroyGUIDs.empty(); }
    void Clear();

protected:
    uint32 m_map;
    uint32 m_blockCount;
    GuidVector m_outOfRangeGUIDs;
    GuidVector m_destroyGUIDs;
    ByteBuffer m_data;
};
#endif
