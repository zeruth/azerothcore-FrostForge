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

#ifndef MovementPackets_h__
#define MovementPackets_h__

#include "Define.h"
#include "ObjectGuid.h"
#include "Optional.h"

class ByteBuffer;
class Unit;
struct MovementInfo;
struct Position;

/// The 3.4.3 movement status ("MovementInfo", FrostLite docs/updateobject.md 5): written by the
/// client's FUN_14074c3e0 into every CMSG_MOVE_*, read by FUN_14070ceb0 from SMSG_MOVE_UPDATE and
/// the object create block. AC keeps its 3.3.5 MovementInfo and movement flag values in game logic;
/// these convert at the wire.
namespace WorldPackets::Movement
{
    /// AC's MovementFlags (3.3.5 values) to the client's, and back. 3.4.3 has no ONTRANSPORT or
    /// SPLINE_ENABLED flag (the status carries HasTransport / HasSpline bits instead), so the flags
    /// above them moved down: DISABLE_GRAVITY..SPLINE_ELEVATION by one bit, WATERWALKING, FALLING_SLOW
    /// and HOVER by two. [values inferred: TrinityCore 3.4.3 MovementFlags]
    uint32 ToClientMovementFlags(uint32 flags);
    uint32 FromClientMovementFlags(uint32 flags);

    /// MovementFlags2: NO_STRAFE and NO_JUMPING keep their bits; FULL_SPEED_TURNING,
    /// FULL_SPEED_PITCHING and ALWAYS_ALLOW_PITCHING moved down one bit; the rest have no 3.4.3
    /// counterpart AC sets.
    uint32 ToClientMovementFlags2(uint16 flags2);
    uint16 FromClientMovementFlags2(uint32 flags2);

    /// TransportLocation (updateobject.md 3.3): guid, x, y, z, o, seat, move time, optional previous
    /// move time and vehicle record id.
    void WriteTransportInfo(ByteBuffer& data, ObjectGuid const& guid, Position const& offset, int8 seat,
        uint32 moveTime, Optional<uint32> prevMoveTime, Optional<uint32> vehicleRecId);

    /// The status of `mi` (its guid is the mover). `vehicleRecId` is set for a vehicle passenger.
    void WriteMovementInfo(ByteBuffer& data, MovementInfo const& mi, Optional<uint32> vehicleRecId = {});

    /// A unit's current status, as SMSG_MOVE_UPDATE and its create block send it.
    void WriteUnitMovementInfo(ByteBuffer& data, Unit const* unit);

    /// A status from the client into AC's MovementInfo (flags converted; transport and spline become
    /// AC's ONTRANSPORT / SPLINE_ENABLED flags). Returns false if the status is malformed.
    bool ReadMovementInfo(ByteBuffer& data, MovementInfo& mi);
}

#endif // MovementPackets_h__
