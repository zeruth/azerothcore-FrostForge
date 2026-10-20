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

#include "MovementPacketBuilder.h"
#include "ByteBuffer.h"
#include "MoveSpline.h"
#include "PacketGuid128.h"
#include <vector>

namespace Movement
{
    namespace
    {
        void WriteVector(ByteBuffer& b, Vector3 const& v)
        {
            b << float(v.x) << float(v.y) << float(v.z);
        }

        /// The MovementSpline's Face (TrinityCore MonsterMoveType): 0 none, 1 spot, 2 target, 3 angle.
        uint8 FaceType(MoveSplineFlag const& flags)
        {
            switch (flags & MoveSplineFlag::Mask_Final_Facing)
            {
                case MoveSplineFlag::Final_Point:  return 1;
                case MoveSplineFlag::Final_Target: return 2;
                case MoveSplineFlag::Final_Angle:  return 3;
                default:                           return 0;
            }
        }

        // 3.4.3 spline flags [TrinityCore 3.4.3 MoveSplineFlag.h]
        constexpr uint32 CLIENT_SPLINE_FALLING           = 0x00000040;
        constexpr uint32 CLIENT_SPLINE_NO_SPLINE         = 0x00000080;
        constexpr uint32 CLIENT_SPLINE_FLYING            = 0x00000200;
        constexpr uint32 CLIENT_SPLINE_ORIENTATION_FIXED = 0x00000400;
        constexpr uint32 CLIENT_SPLINE_CATMULLROM        = 0x00000800;
        constexpr uint32 CLIENT_SPLINE_CYCLIC            = 0x00001000;
        constexpr uint32 CLIENT_SPLINE_ENTER_CYCLE       = 0x00002000;
        constexpr uint32 CLIENT_SPLINE_FROZEN            = 0x00004000;
        constexpr uint32 CLIENT_SPLINE_TRANSPORT_ENTER   = 0x00008000;
        constexpr uint32 CLIENT_SPLINE_TRANSPORT_EXIT    = 0x00010000;
        constexpr uint32 CLIENT_SPLINE_BACKWARD          = 0x00080000;
        constexpr uint32 CLIENT_SPLINE_CAN_SWIM          = 0x00200000;
        constexpr uint32 CLIENT_SPLINE_UNCOMPRESSED_PATH = 0x00400000;
        constexpr uint32 CLIENT_SPLINE_PARABOLIC         = 0x04000000;

        /// The path as the client takes it: a smooth (catmull-rom) path whole, a linear one as its
        /// destination and the packed offsets of the points between from the middle (3.3.5's
        /// WriteLinearPath / WriteCatmullRomPath, now in counted lists).
        void BuildPath(MoveSpline::MySpline const& spline, MoveSplineFlag const& flags, std::vector<Vector3>& points, std::vector<Vector3>& deltas,
            bool& uncompressed, bool& enterCycle)
        {
            enterCycle = false;
            uncompressed = flags.isSmooth();
            if (spline.getPointCount() < 3)
                return;

            if (uncompressed)
            {
                uint32 count = spline.getPointCount() - 3;
                if (flags.cyclic && (flags & MoveSplineFlag::Flying))
                {
                    // a fake first point, which the client erases after the first cycle (Enter_Cycle)
                    enterCycle = true;
                    points.push_back(spline.getPoint(1));
                }
                for (uint32 i = 0; i < count; ++i)
                    points.push_back(spline.getPoint(2 + i));
                if (flags.cyclic && !(flags & MoveSplineFlag::Flying))
                    points.push_back(Vector3::zero());
                return;
            }

            uint32 lastIdx = spline.getPointCount() - 3;
            Vector3 const* realPath = &spline.getPoint(1);
            points.push_back(realPath[lastIdx]);
            if (lastIdx > 1)
            {
                Vector3 middle = (realPath[0] + realPath[lastIdx]) / 2.f;
                for (uint32 i = 1; i < lastIdx; ++i)
                    deltas.push_back(middle - realPath[i]);
            }
        }
    }

    uint32 PacketBuilder::ClientFlags(MoveSplineFlag const& flags)
    {
        uint32 out = 0;
        if (flags.falling)             out |= CLIENT_SPLINE_FALLING;
        if (flags.no_spline)           out |= CLIENT_SPLINE_NO_SPLINE;
        if (flags.parabolic)           out |= CLIENT_SPLINE_PARABOLIC;
        if (flags.canSwim)             out |= CLIENT_SPLINE_CAN_SWIM;
        if (flags.flying)              out |= CLIENT_SPLINE_FLYING;
        if (flags.orientationFixed)    out |= CLIENT_SPLINE_ORIENTATION_FIXED;
        if (flags.catmullrom)          out |= CLIENT_SPLINE_CATMULLROM;
        if (flags.cyclic)              out |= CLIENT_SPLINE_CYCLIC;
        if (flags.enter_cycle)         out |= CLIENT_SPLINE_ENTER_CYCLE;
        if (flags.frozen)              out |= CLIENT_SPLINE_FROZEN;
        if (flags.transportEnter)      out |= CLIENT_SPLINE_TRANSPORT_ENTER;
        if (flags.transportExit)       out |= CLIENT_SPLINE_TRANSPORT_EXIT;
        if (flags.orientationInversed) out |= CLIENT_SPLINE_BACKWARD;
        // 3.3.5's Animation (an AnimType in the low byte) is an anim tier transition in 3.4.3: not sent yet (TODO)
        return out;
    }

    // MovementSpline (the client's reader FUN_140715140): u32 Flags, i32 Elapsed, u32 MoveTime, u32
    // FadeObjectTime, u8 Mode, PackedGuid128 TransportGUID, i8 VehicleSeat, 40 bits (Face:2,
    // PointsCount:16, VehicleExitVoluntary, Interpolate, PackedDeltasCount:16, HasSplineFilter,
    // HasSpellEffectExtraData, HasJumpExtraData, HasAnimTierTransition), the facing, the points, the
    // packed deltas, the jump data.
    void PacketBuilder::WriteMovementSpline(MoveSpline const& mov, ByteBuffer& data, ObjectGuid const& transport, int8 seat)
    {
        MoveSplineFlag splineflags = mov.splineflags;
        std::vector<Vector3> points, deltas;
        bool uncompressed, enterCycle;
        BuildPath(mov._Spline(), mov.splineflags, points, deltas, uncompressed, enterCycle);
        if (enterCycle)
            splineflags.enter_cycle = true;

        uint32 flags = ClientFlags(splineflags);
        if (uncompressed)
            flags |= CLIENT_SPLINE_UNCOMPRESSED_PATH;
        uint8 face = FaceType(splineflags);
        bool hasJump = splineflags.parabolic;

        data << uint32(flags);
        data << int32(0);                                   // Elapsed
        data << uint32(mov.Duration());                     // MoveTime
        data << uint32(0);                                  // FadeObjectTime
        data << uint8(0);                                   // Mode
        Guid128::Write(data, transport);
        data << int8(seat);
        data.WriteBits(face, 2);
        data.WriteBits(points.size(), 16);
        data.WriteBit(false);                               // VehicleExitVoluntary
        data.WriteBit(false);                               // Interpolate
        data.WriteBits(deltas.size(), 16);
        data.WriteBit(false);                               // HasSplineFilter
        data.WriteBit(false);                               // HasSpellEffectExtraData
        data.WriteBit(hasJump);
        data.WriteBit(false);                               // HasAnimTierTransition
        data.FlushBits();

        switch (face)
        {
            case 1:
                data << float(mov.facing.f.x) << float(mov.facing.f.y) << float(mov.facing.f.z);
                break;
            case 2:
                data << float(0.0f);                        // FaceDirection
                Guid128::Write(data, ObjectGuid(mov.facing.target));
                break;
            case 3:
                data << float(mov.facing.angle);
                break;
            default:
                break;
        }

        for (Vector3 const& point : points)
            WriteVector(data, point);
        for (Vector3 const& delta : deltas)
            data.appendPackXYZ(delta.x, delta.y, delta.z);

        if (hasJump)
        {
            data << float(mov.vertical_acceleration);       // JumpGravity
            data << uint32(mov.effect_start_time);          // StartTime
            data << uint32(0);                              // Duration
        }
    }

    // SMSG_ON_MONSTER_MOVE (0x2DD4), after the mover's GUID: f32 x, y, z (the start), u32 ID, f32
    // Destination x, y, z, bit CrzTeleport, 3 bits StopDistanceTolerance, flush, the MovementSpline
    // (the movement switch's case at 0x14073f3f0; TrinityCore 3.4.3 has no Destination, the client reads one).
    void PacketBuilder::WriteMonsterMove(MoveSpline const& mov, ByteBuffer& data, ObjectGuid const& transport, int8 seat)
    {
        MoveSpline::MySpline const& spline = mov._Spline();
        WriteVector(data, spline.getPoint(spline.first()));
        data << uint32(mov.GetId());
        WriteVector(data, mov.isCyclic() ? Vector3::zero() : mov.FinalDestination());
        data.WriteBit(false);                               // CrzTeleport
        data.WriteBits(0, 3);                               // StopDistanceTolerance
        data.FlushBits();
        WriteMovementSpline(mov, data, transport, seat);
    }

    void PacketBuilder::WriteStopMovement(Vector3 const& loc, uint32 splineId, ByteBuffer& data, ObjectGuid const& transport, int8 seat)
    {
        WriteVector(data, loc);
        data << uint32(splineId);
        WriteVector(data, loc);                             // Destination
        data.WriteBit(false);                               // CrzTeleport
        data.WriteBits(2, 3);                               // StopDistanceTolerance [TrinityCore's stop]
        data.FlushBits();

        // an empty MovementSpline
        data << uint32(0) << int32(0) << uint32(0) << uint32(0) << uint8(0);
        Guid128::Write(data, transport);
        data << int8(seat);
        data.WriteBits(0, 40);
        data.FlushBits();
    }

    // The create block's spline: u32 ID, f32 Destination x, y, z, bit HasSplineMove, then
    // MovementSpline (updateobject.md 3.4): u32 Flags, i32 Elapsed, u32 Duration, f32
    // DurationModifier, f32 NextDurationModifier, 24 bits (Face:2, HasFadeObjectTime,
    // PointsCount:16, HasSplineFilter, HasSpellEffectExtraData, HasJumpExtraData,
    // HasAnimTierTransition, unused), the facing (spot xyz / target GUID / angle), [FadeObjectTime],
    // every path point, [jump: f32 gravity, u32 start, u32 duration].
    void PacketBuilder::WriteCreate(MoveSpline const& mov, ByteBuffer& data)
    {
        MoveSpline::MySpline const& spline = mov._Spline();
        data << uint32(mov.GetId());

        Vector3 dest;
        if (!mov.isCyclic())
            dest = mov.FinalDestination();
        else if (spline.getPointCount() > 1)
            dest = spline.getPoint(spline.last() - 1);
        WriteVector(data, dest);

        bool hasSplineMove = !mov.Finalized();
        data.WriteBit(hasSplineMove);
        data.FlushBits();
        if (!hasSplineMove)
            return;

        MoveSplineFlag const& splineflags = mov.splineflags;
        uint8 face = FaceType(splineflags);
        bool hasJump = splineflags.parabolic;
        MoveSpline::MySpline::ControlArray const& path = spline.getPoints();

        data << uint32(ClientFlags(splineflags));
        data << int32(mov.timePassed());                    // Elapsed
        data << uint32(mov.Duration());
        data << float(1.0f);                                // DurationModifier
        data << float(1.0f);                                // NextDurationModifier
        data.WriteBits(face, 2);
        data.WriteBit(false);                               // HasFadeObjectTime
        data.WriteBits(path.size(), 16);
        data.WriteBit(false);                               // HasSplineFilter
        data.WriteBit(false);                               // HasSpellEffectExtraData
        data.WriteBit(hasJump);
        data.WriteBit(false);                               // HasAnimTierTransition
        data.WriteBit(false);
        data.FlushBits();

        switch (face)
        {
            case 1:
                data << float(mov.facing.f.x) << float(mov.facing.f.y) << float(mov.facing.f.z);
                break;
            case 2:
                Guid128::Write(data, ObjectGuid(mov.facing.target));
                break;
            case 3:
                data << float(mov.facing.angle);
                break;
            default:
                break;
        }

        for (Vector3 const& point : path)
            WriteVector(data, point);

        if (hasJump)
        {
            data << float(mov.vertical_acceleration);
            data << uint32(mov.effect_start_time);
            data << uint32(0);
        }
    }
}
