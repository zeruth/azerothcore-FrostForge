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

#include "UpdateData.h"
#include "ByteBuffer.h"
#include "Errors.h"
#include "Log.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "World.h"
#include "WorldPacket.h"

UpdateData::UpdateData(uint32 mapId) : m_map(mapId), m_blockCount(0)
{
    m_outOfRangeGUIDs.reserve(15);
}

void UpdateData::AddOutOfRangeGUID(ObjectGuid guid)
{
    m_outOfRangeGUIDs.push_back(guid);
}

void UpdateData::AddDestroyObject(ObjectGuid guid)
{
    m_destroyGUIDs.push_back(guid);
}

void UpdateData::AddUpdateBlock(ByteBuffer const& block)
{
    m_data.append(block);
    ++m_blockCount;
}

void UpdateData::AddUpdateBlock(UpdateData const& block)
{
    m_data.append(block.m_data);
    m_blockCount += block.m_blockCount;
    m_outOfRangeGUIDs.insert(m_outOfRangeGUIDs.end(), block.m_outOfRangeGUIDs.begin(), block.m_outOfRangeGUIDs.end());
    m_destroyGUIDs.insert(m_destroyGUIDs.end(), block.m_destroyGUIDs.begin(), block.m_destroyGUIDs.end());
    if (m_map == NO_MAP)
        m_map = block.m_map;
}

bool UpdateData::BuildPacket(WorldPacket& packet)
{
    ASSERT(packet.empty());

    if (m_map == NO_MAP)
    {
        // the client drops a message for another map: every block is built for a receiver, whose map it is
        LOG_ERROR("network", "UpdateData::BuildPacket: SMSG_UPDATE_OBJECT with {} blocks has no map id", m_blockCount);
        m_map = 0;
    }

    bool hasRemoved = !m_outOfRangeGUIDs.empty() || !m_destroyGUIDs.empty();
    packet.reserve(4 + 2 + 1 + (hasRemoved ? 6 + 18 * (m_outOfRangeGUIDs.size() + m_destroyGUIDs.size()) : 0) + 4 + m_data.size());

    packet << uint32(m_blockCount);
    packet << uint16(m_map);
    packet.WriteBit(hasRemoved);
    packet.FlushBits();

    if (hasRemoved)
    {
        packet << uint16(m_destroyGUIDs.size());
        packet << uint32(m_destroyGUIDs.size() + m_outOfRangeGUIDs.size());

        for (ObjectGuid const& guid : m_destroyGUIDs)
            Guid128::Write(packet, guid);

        for (ObjectGuid const& guid : m_outOfRangeGUIDs)
            Guid128::Write(packet, guid);
    }

    packet << uint32(m_data.size());
    packet.append(m_data);
    packet.SetOpcode(SMSG_UPDATE_OBJECT);

    return true;
}

void UpdateData::Clear()
{
    m_data.clear();
    m_outOfRangeGUIDs.clear();
    m_destroyGUIDs.clear();
    m_blockCount = 0;
}
