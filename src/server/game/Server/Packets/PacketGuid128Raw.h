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

#ifndef PacketGuid128Raw_h__
#define PacketGuid128Raw_h__

#include "Define.h"

namespace Guid128
{
    /// A 3.4.3 GUID kept as the client sent it, for GUIDs AC has no object for (a spell cast's id).
    /// Read/written by PacketGuid128.h's ReadRaw/WriteRaw.
    struct Raw
    {
        uint64 Low = 0;
        uint64 High = 0;

        [[nodiscard]] bool IsEmpty() const { return !Low && !High; }
        bool operator==(Raw const& other) const { return Low == other.Low && High == other.High; }
    };
}

#endif // PacketGuid128Raw_h__
