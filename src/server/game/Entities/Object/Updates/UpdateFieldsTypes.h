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

#ifndef UPDATE_FIELDS_TYPES_H
#define UPDATE_FIELDS_TYPES_H

#include "Define.h"
#include <cstddef>

/// The building blocks of the 3.4.3 update-field tables (UpdateFieldsData.h, generated).
///
/// Each data struct's values are a byte image laid out like the client's own struct: a field
/// lives at the client's offset, with the client's width. The create and update streams are
/// programs over that image, read off the client's readers (FrostLite docs/updatefields.json).
namespace UF
{
    /// Field visibility, the u8 FieldFlags of a values create (TrinityCore's UpdateFieldFlag).
    enum FieldFlag : uint8
    {
        FIELD_FLAG_NONE         = 0x00,
        FIELD_FLAG_OWNER        = 0x01,
        FIELD_FLAG_PARTY_MEMBER = 0x02,
        FIELD_FLAG_UNIT_ALL     = 0x04,
        FIELD_FLAG_EMPATH       = 0x08,
    };

    enum class UFOp : uint8
    {
        // create and update
        Value,          // Width-byte values at Offset (+ k * Stride for Count > 1); update: value k on bit Bit + k
        Zero,           // a value the client reads that the server does not store: written as zero
        Group,          // Count elements of A members (the following Member/Value instructions), interleaved
                        // per element; member values at member Offset + i * Stride
        // create only
        DynSize,        // u32 size of dynamic array A
        DynElems,       // the elements of dynamic array A
        // update only
        Member,         // a Group/Nested/Skill member: Bit = its bit for element 0 (Group: + i;
                        // Nested: an element mask bit, + j for its Count inner values at Stride)
        Nested,         // Count elements (Stride apart), element i on bit Bit + i; each an element change
                        // mask (Mask bits, or 0x8000 | n: a block mask and n 32-bit blocks), then the
                        // A members on that mask
        Skill,          // SkillInfo: its own 1793-bit mask (u32 + 25-bit block mask, 32-bit blocks), then
                        // Count skills of A u16 members, member m of skill i on sub-bit Bit_m + i
        Unsupported,    // an update entry the server cannot produce yet: bit Bit is never set
    };

    struct UFInstr
    {
        UFOp Op;
        uint8 Width;        // bytes; 16 = a packed ObjectGuid
        uint8 Vis;          // FieldFlag mask that may see it (0 = everyone)
        uint8 A;            // members / dynamic array index
        uint16 Offset;
        uint16 Count;
        uint16 Stride;
        uint16 Bit;
        uint16 Mask;
    };

    /// A field of a struct image: element i, inner element j at Offset + i * Stride + j * InnerStride.
    struct UFField
    {
        uint16 Offset;
        uint8 Width;
        uint16 Count;
        uint16 Stride;
        uint16 InnerCount;
        uint16 InnerStride;

        constexpr uint32 At(uint32 i = 0, uint32 j = 0) const { return Offset + i * Stride + j * InnerStride; }
    };

    struct UFDynInfo
    {
        uint16 ElemSize;
        UFInstr const* Members;
        uint8 MemberCount;
    };

    struct UFStructInfo
    {
        char const* Name;
        uint16 TypeBit;             // the struct's bit in a values update's changed-type mask
        uint16 ImageSize;
        uint16 MaskBits;            // bits in the struct's change mask
        uint16 MaskBlocks;          // 0: a single mask of MaskBits; n: a block mask of n bits and 32-bit blocks
        UFInstr const* Create;
        std::size_t CreateCount;
        UFInstr const* Update;
        std::size_t UpdateCount;
        UFDynInfo const* Dyns;
        uint8 DynCount;
        int16 const* Parents;       // each mask bit's group bit, -1 for none
    };
}

#endif // UPDATE_FIELDS_TYPES_H
