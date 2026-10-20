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

#ifndef AC_PACKET_BUILDER_H
#define AC_PACKET_BUILDER_H

#include "Define.h"
#include "ObjectGuid.h"

class ByteBuffer;
namespace G3D
{
    class Vector3;
}

namespace Movement
{
    using G3D::Vector3;

    class MoveSpline;
    class MoveSplineFlag;

    /// Splines in the 3.4.3 client's layouts.
    class PacketBuilder
    {
        static void WriteMovementSpline(MoveSpline const& mov, ByteBuffer& data, ObjectGuid const& transport, int8 seat);
    public:
        /// SMSG_ON_MONSTER_MOVE after the mover's GUID: start position, spline id, destination,
        /// CrzTeleport / StopDistanceTolerance bits, the MovementSpline (FUN_140715140).
        static void WriteMonsterMove(MoveSpline const& mov, ByteBuffer& data, ObjectGuid const& transport = ObjectGuid::Empty, int8 seat = 0);
        static void WriteStopMovement(Vector3 const& loc, uint32 splineId, ByteBuffer& data, ObjectGuid const& transport = ObjectGuid::Empty, int8 seat = 0);
        /// The create block's spline (updateobject.md 3.1 #32-35 and 3.4).
        static void WriteCreate(MoveSpline const& mov, ByteBuffer& data);
        /// AC's (3.3.5) spline flags as the client's.
        static uint32 ClientFlags(MoveSplineFlag const& flags);
    };
}
#endif // AC_PACKET_BUILDER_H
