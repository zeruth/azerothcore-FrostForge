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

#include "MovementPackets.h"
#include "ByteBuffer.h"
#include "GameTime.h"
#include "Object.h"
#include "PacketGuid128.h"
#include "Transport.h"
#include "Unit.h"
#include "Vehicle.h"
#include <cmath>

namespace WorldPackets::Movement
{
    namespace
    {
        // 3.4.3 flag values [TrinityCore 3.4.3 UnitDefines.h]
        constexpr uint32 CLIENT_MOVEMENTFLAG_MASK_LOW = 0x000001FF;   // FORWARD..WALKING: unchanged
        constexpr uint32 AC_SHIFTED_ONE = 0x07FFFC00;                // DISABLE_GRAVITY..SPLINE_ELEVATION
        constexpr uint32 AC_SHIFTED_TWO = 0x70000000;                // WATERWALKING, FALLING_SLOW, HOVER

        constexpr uint32 CLIENT_SHIFTED_ONE = AC_SHIFTED_ONE >> 1;   // 0x03FFFE00
        constexpr uint32 CLIENT_SHIFTED_TWO = AC_SHIFTED_TWO >> 2;   // 0x1C000000

        bool Finite(float v) { return std::isfinite(v); }
    }

    uint32 ToClientMovementFlags(uint32 flags)
    {
        return (flags & CLIENT_MOVEMENTFLAG_MASK_LOW) | ((flags & AC_SHIFTED_ONE) >> 1) | ((flags & AC_SHIFTED_TWO) >> 2);
    }

    uint32 FromClientMovementFlags(uint32 flags)
    {
        return (flags & CLIENT_MOVEMENTFLAG_MASK_LOW) | ((flags & CLIENT_SHIFTED_ONE) << 1) | ((flags & CLIENT_SHIFTED_TWO) << 2);
    }

    uint32 ToClientMovementFlags2(uint16 flags2)
    {
        uint32 out = flags2 & (MOVEMENTFLAG2_NO_STRAFE | MOVEMENTFLAG2_NO_JUMPING);
        if (flags2 & MOVEMENTFLAG2_FULL_SPEED_TURNING)
            out |= 0x04;
        if (flags2 & MOVEMENTFLAG2_FULL_SPEED_PITCHING)
            out |= 0x08;
        if (flags2 & MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING)
            out |= 0x10;
        return out;
    }

    uint16 FromClientMovementFlags2(uint32 flags2)
    {
        uint16 out = uint16(flags2 & (MOVEMENTFLAG2_NO_STRAFE | MOVEMENTFLAG2_NO_JUMPING));
        if (flags2 & 0x04)
            out |= MOVEMENTFLAG2_FULL_SPEED_TURNING;
        if (flags2 & 0x08)
            out |= MOVEMENTFLAG2_FULL_SPEED_PITCHING;
        if (flags2 & 0x10)
            out |= MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING;
        return out;
    }

    void WriteTransportInfo(ByteBuffer& data, ObjectGuid const& guid, Position const& offset, int8 seat,
        uint32 moveTime, Optional<uint32> prevMoveTime, Optional<uint32> vehicleRecId)
    {
        data.FlushBits();
        Guid128::Write(data, guid);
        data << float(offset.GetPositionX());
        data << float(offset.GetPositionY());
        data << float(offset.GetPositionZ());
        data << float(Position::NormalizeOrientation(offset.GetOrientation()));
        data << int8(seat);
        data << uint32(moveTime);
        data.WriteBit(prevMoveTime.has_value());
        data.WriteBit(vehicleRecId.has_value());
        data.FlushBits();
        if (prevMoveTime)
            data << uint32(*prevMoveTime);
        if (vehicleRecId)
            data << uint32(*vehicleRecId);
    }

    void WriteMovementInfo(ByteBuffer& data, MovementInfo const& mi, Optional<uint32> vehicleRecId)
    {
        bool hasTransport = mi.HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && !mi.transport.guid.IsEmpty();
        bool hasFallDirection = mi.HasMovementFlag(MOVEMENTFLAG_FALLING);
        bool hasFall = hasFallDirection || mi.fallTime != 0;

        data.FlushBits();
        Guid128::Write(data, mi.guid);                               // MoverGUID
        data << uint32(ToClientMovementFlags(mi.GetMovementFlags()));
        data << uint32(ToClientMovementFlags2(mi.GetExtraMovementFlags()));
        data << uint32(0);                                          // MovementFlags3
        data << uint32(mi.time);                                    // MoveTime
        data << float(mi.pos.GetPositionX());
        data << float(mi.pos.GetPositionY());
        data << float(mi.pos.GetPositionZ());
        data << float(Position::NormalizeOrientation(mi.pos.GetOrientation()));
        data << float(Finite(mi.pitch) ? mi.pitch : 0.0f);         // Pitch
        data << float(Finite(mi.splineElevation) ? mi.splineElevation : 0.0f);  // StepUpStartElevation
        data << uint32(0);                                          // RemoveForcesIDs
        data << uint32(0);                                          // MoveIndex

        data.WriteBit(false);                                       // HasStandingOnGameObjectGUID
        data.WriteBit(hasTransport);
        data.WriteBit(hasFall);
        data.WriteBit(false);                                       // HasSpline: TODO, creates send no spline yet
        data.WriteBit(false);                                       // HeightChangeFailed
        data.WriteBit(false);                                       // RemoteTimeValid
        data.WriteBit(false);                                       // HasInertia
        data.WriteBit(false);                                       // HasAdvFlying
        data.FlushBits();

        if (hasTransport)
        {
            Optional<uint32> prevTime;
            if (mi.HasExtraMovementFlag(MOVEMENTFLAG2_INTERPOLATED_MOVEMENT))
                prevTime = mi.transport.time2;
            WriteTransportInfo(data, mi.transport.guid, mi.transport.pos, mi.transport.seat, mi.transport.time, prevTime, vehicleRecId);
        }

        if (hasFall)
        {
            data << uint32(mi.fallTime);
            data << float(mi.jump.zspeed);                          // JumpVelocity
            data.WriteBit(hasFallDirection);
            data.FlushBits();
            if (hasFallDirection)
            {
                data << float(mi.jump.sinAngle);
                data << float(mi.jump.cosAngle);
                data << float(mi.jump.xyspeed);
            }
        }
    }

    void WriteUnitMovementInfo(ByteBuffer& data, Unit const* unit)
    {
        MovementInfo mi = unit->m_movementInfo;
        mi.guid = unit->GetGUID();
        mi.time = GameTime::GetGameTimeMS().count();
        mi.pos.Relocate(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetOrientation());
        mi.RemoveMovementFlag(MOVEMENTFLAG_SPLINE_ENABLED);

        Optional<uint32> vehicleRecId;
        if (mi.HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
        {
            if (Vehicle* vehicle = unit->GetVehicle())
            {
                mi.transport.guid = vehicle->GetBase()->GetGUID();
                if (vehicle->GetVehicleInfo())
                    vehicleRecId = vehicle->GetVehicleInfo()->m_ID;
            }
            else if (Transport* transport = unit->GetTransport())
                mi.transport.guid = transport->GetGUID();
        }

        WriteMovementInfo(data, mi, vehicleRecId);
    }

    bool ReadMovementInfo(ByteBuffer& data, MovementInfo& mi)
    {
        mi.guid = Guid128::Read(data);

        uint32 flags, flags2, flags3;
        data >> flags >> flags2 >> flags3;
        data >> mi.time;
        float x, y, z, o;
        data >> x >> y >> z >> o;
        mi.pos.Relocate(x, y, z, o);
        data >> mi.pitch;
        data >> mi.splineElevation;                                // StepUpStartElevation

        uint32 removeForcesCount, moveIndex;
        data >> removeForcesCount >> moveIndex;
        if (removeForcesCount > 64)
            return false;
        for (uint32 i = 0; i < removeForcesCount; ++i)
            Guid128::Skip(data);

        data.ResetBitReader();
        bool hasStandingOnGameObject = data.ReadBit();
        bool hasTransport = data.ReadBit();
        bool hasFall = data.ReadBit();
        bool hasSpline = data.ReadBit();
        data.ReadBit();                                             // HeightChangeFailed
        data.ReadBit();                                             // RemoteTimeValid
        bool hasInertia = data.ReadBit();
        bool hasAdvFlying = data.ReadBit();

        mi.transport.Reset();
        if (hasTransport)
        {
            mi.transport.guid = Guid128::Read(data);
            float tx, ty, tz, to;
            data >> tx >> ty >> tz >> to;
            mi.transport.pos.Relocate(tx, ty, tz, to);
            mi.transport.seat = data.read<int8>();
            data >> mi.transport.time;
            data.ResetBitReader();
            bool hasPrevMoveTime = data.ReadBit();
            bool hasVehicleRecId = data.ReadBit();
            if (hasPrevMoveTime)
                data >> mi.transport.time2;
            if (hasVehicleRecId)
                data.read_skip<uint32>();
        }

        if (hasStandingOnGameObject)
            Guid128::Skip(data);

        if (hasInertia)
        {
            data.read_skip<uint32>();                               // ID
            data.read_skip<float>();
            data.read_skip<float>();
            data.read_skip<float>();
            data.read_skip<uint32>();                               // Lifetime
        }

        if (hasAdvFlying)
        {
            data.read_skip<float>();                                // ForwardVelocity
            data.read_skip<float>();                                // UpVelocity
        }

        mi.fallTime = 0;
        mi.jump.Reset();
        if (hasFall)
        {
            data >> mi.fallTime;
            data >> mi.jump.zspeed;
            data.ResetBitReader();
            if (data.ReadBit())
            {
                data >> mi.jump.sinAngle;
                data >> mi.jump.cosAngle;
                data >> mi.jump.xyspeed;
            }
        }

        flags = FromClientMovementFlags(flags);
        if (hasTransport)
            flags |= MOVEMENTFLAG_ONTRANSPORT;
        if (hasSpline)
            flags |= MOVEMENTFLAG_SPLINE_ENABLED;
        mi.SetMovementFlags(flags);
        mi.flags2 = FromClientMovementFlags2(flags2);

        return Finite(x) && Finite(y) && Finite(z) && Finite(o) && Finite(mi.pitch);
    }
}
