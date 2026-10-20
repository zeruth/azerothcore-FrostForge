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

#include "UpdateFieldsWriter.h"
#include "ByteBuffer.h"
#include "Errors.h"
#include "Log.h"
#include "PacketGuid128.h"

namespace UF
{
    namespace
    {
        constexpr UFStructInfo const* StructInfos[STRUCT_COUNT] =
        {
            &ObjectDataInfo, &ItemDataInfo, &ContainerDataInfo, &UnitDataInfo, &PlayerDataInfo,
            &ActivePlayerDataInfo, &GameObjectDataInfo, &DynamicObjectDataInfo, &CorpseDataInfo
        };

        constexpr StructId ItemStructs[] = { STRUCT_OBJECT, STRUCT_ITEM };
        constexpr StructId ContainerStructs[] = { STRUCT_OBJECT, STRUCT_ITEM, STRUCT_CONTAINER };
        constexpr StructId UnitStructs[] = { STRUCT_OBJECT, STRUCT_UNIT };
        constexpr StructId PlayerStructs[] = { STRUCT_OBJECT, STRUCT_UNIT, STRUCT_PLAYER };
        constexpr StructId ActivePlayerStructs[] = { STRUCT_OBJECT, STRUCT_UNIT, STRUCT_PLAYER, STRUCT_ACTIVE_PLAYER };
        constexpr StructId GameObjectStructs[] = { STRUCT_OBJECT, STRUCT_GAMEOBJECT };
        constexpr StructId DynamicObjectStructs[] = { STRUCT_OBJECT, STRUCT_DYNAMICOBJECT };
        constexpr StructId CorpseStructs[] = { STRUCT_OBJECT, STRUCT_CORPSE };
        constexpr StructId ObjectStructs[] = { STRUCT_OBJECT };

        bool Visible(uint8 vis, uint8 fieldFlags)
        {
            return !vis || (vis & fieldFlags);
        }

        /// A bit set sized for a mask.
        class Mask
        {
        public:
            explicit Mask(uint32 bits) : _words((bits + 31) / 32, 0) { }

            void Set(uint32 bit) { _words[bit / 32] |= 1u << (bit % 32); }
            bool Get(uint32 bit) const { return bit / 32 < _words.size() && (_words[bit / 32] & (1u << (bit % 32))); }
            uint32 Word(uint32 i) const { return _words[i]; }
            std::size_t Words() const { return _words.size(); }
            bool Any() const
            {
                for (uint32 w : _words)
                    if (w)
                        return true;
                return false;
            }

        private:
            std::vector<uint32> _words;
        };

        void WriteValue(ByteBuffer& data, uint8 const* at, uint8 width)
        {
            data.FlushBits();
            if (width == 16)
            {
                uint64 low, high;
                std::memcpy(&low, at, 8);
                std::memcpy(&high, at + 8, 8);
                Guid128::WritePacked(data, low, high);
                return;
            }
            data.append(at, width);
        }

        void WriteZero(ByteBuffer& data, uint8 width)
        {
            data.FlushBits();
            if (width == 16)
            {
                data << uint8(0) << uint8(0);
                return;
            }
            for (uint8 i = 0; i < width; ++i)
                data << uint8(0);
        }

        /// A mask of `bits` bits the way the client's FUN_1425071f0 reads it: past 32 bits, whole
        /// words are raw little-endian u32 if the bit reader is at a byte boundary; the rest as bits.
        void WriteMaskWords(ByteBuffer& data, std::vector<uint32> const& words, uint32 bits)
        {
            uint32 w = 0;
            if (bits > 32)
            {
                bool raw = !data.HasUnfinishedBitPack();
                while (bits > 32)
                {
                    if (raw)
                        data << uint32(words[w]);
                    else
                        data.WriteBits(words[w], 32);
                    ++w;
                    bits -= 32;
                }
            }
            if (bits)
                data.WriteBits(words[w] & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1)), bits);
        }

        /// A struct (or SkillInfo) mask in blocks form: the block mask, then each non-zero block.
        void WriteBlocks(ByteBuffer& data, Mask const& mask, uint32 blocks)
        {
            std::vector<uint32> blockMask((blocks + 31) / 32, 0);
            for (uint32 b = 0; b < blocks; ++b)
                if (mask.Word(b))
                    blockMask[b / 32] |= 1u << (b % 32);
            WriteMaskWords(data, blockMask, blocks);
            for (uint32 b = 0; b < blocks; ++b)
                if (mask.Word(b))
                    data.WriteBits(mask.Word(b), 32);
        }

        struct Diff
        {
            Data const& Cur;
            Data const* Old;
            uint8 FieldFlags;
            uint8 ForceVis;
            std::span<ForcedField const> Forced;
            StructId Id;

            bool Changed(uint32 offset, uint8 width, uint8 vis) const
            {
                if (!Visible(vis, FieldFlags))
                    return false;
                if (!Old || (ForceVis && (vis & ForceVis)))
                    return true;
                for (ForcedField const& f : Forced)
                    if (f.Struct == Id && f.Offset == offset)
                        return true;
                return std::memcmp(Cur.Image() + offset, Old->Image() + offset, width) != 0;
            }
        };

        /// Nested element `i`'s change mask (bit 0: any member; an inner array also sets the bit
        /// before its first, its group bit, as TrinityCore's QuestLog does for ObjectiveProgress).
        Mask NestedMask(Diff const& diff, UFInstr const& nested, UFInstr const* members, uint32 i)
        {
            uint32 bits = (nested.Mask & 0x8000) ? (nested.Mask & 0xFF) * 32 : nested.Mask;
            Mask mask(std::max<uint32>(bits, 1));
            for (uint8 m = 0; m < nested.A; ++m)
            {
                UFInstr const& member = members[m];
                uint32 count = std::max<uint32>(member.Count, 1);
                for (uint32 j = 0; j < count; ++j)
                {
                    if (!diff.Changed(member.Offset + i * nested.Stride + j * member.Stride, member.Width, member.Vis))
                        continue;
                    mask.Set(0);
                    mask.Set(member.Bit + j);
                    if (count > 1)
                        mask.Set(member.Bit - 1);
                }
            }
            return mask;
        }

        Mask SkillMask(Diff const& diff, UFInstr const& skill, UFInstr const* members)
        {
            Mask mask(1 + 256 * 7);
            for (uint32 i = 0; i < skill.Count; ++i)
                for (uint8 m = 0; m < skill.A; ++m)
                    if (diff.Changed(members[m].Offset + i * skill.Stride, members[m].Width, members[m].Vis))
                    {
                        mask.Set(0);
                        mask.Set(members[m].Bit + i);
                    }
            return mask;
        }
    }

    UFStructInfo const& GetStructInfo(StructId id)
    {
        return *StructInfos[id];
    }

    std::span<StructId const> GetTypeStructs(uint8 objectTypeId)
    {
        switch (objectTypeId)
        {
            case OBJECT_TYPE_ITEM:          return ItemStructs;
            case OBJECT_TYPE_CONTAINER:     return ContainerStructs;
            case OBJECT_TYPE_UNIT:          return UnitStructs;
            case OBJECT_TYPE_PLAYER:        return PlayerStructs;
            case OBJECT_TYPE_ACTIVE_PLAYER: return ActivePlayerStructs;
            case OBJECT_TYPE_GAMEOBJECT:    return GameObjectStructs;
            case OBJECT_TYPE_DYNAMICOBJECT: return DynamicObjectStructs;
            case OBJECT_TYPE_CORPSE:        return CorpseStructs;
            default:                        return ObjectStructs;
        }
    }

    void Data::SetGuid(UFField const& f, ObjectGuid const& guid, uint32 i)
    {
        auto [low, high] = Guid128::Encode(guid);
        uint8* at = _image.data() + f.At(i);
        Put(at, 8, low);
        Put(at + 8, 8, high);
    }

    void WriteCreate(ByteBuffer& data, Data const& values, uint8 fieldFlags)
    {
        UFStructInfo const& info = values.Info();
        uint8 const* image = values.Image();
        for (std::size_t idx = 0; idx < info.CreateCount; ++idx)
        {
            UFInstr const& ins = info.Create[idx];
            switch (ins.Op)
            {
                case UFOp::Value:
                    if (Visible(ins.Vis, fieldFlags))
                        for (uint32 k = 0; k < std::max<uint32>(ins.Count, 1); ++k)
                            WriteValue(data, image + ins.Offset + k * ins.Stride, ins.Width);
                    break;
                case UFOp::Zero:
                    if (Visible(ins.Vis, fieldFlags))
                        WriteZero(data, ins.Width);
                    break;
                case UFOp::Group:
                {
                    UFInstr const* members = &info.Create[idx + 1];
                    for (uint32 i = 0; i < ins.Count; ++i)
                        for (uint8 m = 0; m < ins.A; ++m)
                            if (Visible(members[m].Vis, fieldFlags))
                                for (uint32 j = 0; j < std::max<uint32>(members[m].Count, 1); ++j)
                                    WriteValue(data, image + members[m].Offset + i * ins.Stride + j * members[m].Stride, members[m].Width);
                    idx += ins.A;
                    break;
                }
                case UFOp::DynSize:
                    data.FlushBits();
                    data << uint32(values.DynSize(ins.A));
                    break;
                case UFOp::DynElems:
                {
                    UFDynInfo const& dyn = info.Dyns[ins.A];
                    std::vector<uint8> const& raw = values.DynRaw(ins.A);
                    for (std::size_t at = 0; at + dyn.ElemSize <= raw.size(); at += dyn.ElemSize)
                        for (uint8 m = 0; m < dyn.MemberCount; ++m)
                            for (uint32 j = 0; j < std::max<uint32>(dyn.Members[m].Count, 1); ++j)
                                WriteValue(data, raw.data() + at + dyn.Members[m].Offset + j * dyn.Members[m].Stride, dyn.Members[m].Width);
                    break;
                }
                default:
                    ABORT("UF::WriteCreate: {} create program has update instruction {}", info.Name, uint32(ins.Op));
            }
        }
        data.FlushBits();
    }

    bool WriteUpdate(ByteBuffer& data, Data const& values, Data const* old, uint8 fieldFlags, uint8 forceVis,
        std::span<ForcedField const> forced, StructId id)
    {
        UFStructInfo const& info = values.Info();
        uint8 const* image = values.Image();
        Diff diff{ values, old, fieldFlags, forceVis, forced, id };
        Mask mask(info.MaskBits);

        auto mark = [&](uint32 bit)
        {
            for (int32 b = int32(bit); b >= 0; b = info.Parents[b])
                mask.Set(uint32(b));
        };

        // pass 1: the change mask
        for (std::size_t idx = 0; idx < info.UpdateCount; ++idx)
        {
            UFInstr const& ins = info.Update[idx];
            switch (ins.Op)
            {
                case UFOp::Value:
                    for (uint32 k = 0; k < std::max<uint32>(ins.Count, 1); ++k)
                        if (diff.Changed(ins.Offset + k * ins.Stride, ins.Width, ins.Vis))
                            mark(ins.Bit + k);
                    break;
                case UFOp::Group:
                {
                    UFInstr const* members = &info.Update[idx + 1];
                    for (uint32 i = 0; i < ins.Count; ++i)
                        for (uint8 m = 0; m < ins.A; ++m)
                            if (diff.Changed(members[m].Offset + i * ins.Stride, members[m].Width, members[m].Vis))
                                mark(members[m].Bit + i);
                    idx += ins.A;
                    break;
                }
                case UFOp::Nested:
                    for (uint32 i = 0; i < ins.Count; ++i)
                        if (NestedMask(diff, ins, &info.Update[idx + 1], i).Any())
                            mark(ins.Bit + i);
                    idx += ins.A;
                    break;
                case UFOp::Skill:
                    if (SkillMask(diff, ins, &info.Update[idx + 1]).Any())
                        mark(ins.Bit);
                    idx += ins.A;
                    break;
                case UFOp::Zero:
                case UFOp::Unsupported:
                    break;
                default:
                    ABORT("UF::WriteUpdate: {} update program has create instruction {}", info.Name, uint32(ins.Op));
            }
        }

        if (!mask.Any())
            return false;

        // pass 2: the stream
        if (!info.MaskBlocks)
            data.WriteBits(mask.Word(0), info.MaskBits);
        else
            WriteBlocks(data, mask, info.MaskBlocks);

        for (std::size_t idx = 0; idx < info.UpdateCount; ++idx)
        {
            UFInstr const& ins = info.Update[idx];
            switch (ins.Op)
            {
                case UFOp::Value:
                    for (uint32 k = 0; k < std::max<uint32>(ins.Count, 1); ++k)
                        if (mask.Get(ins.Bit + k))
                            WriteValue(data, image + ins.Offset + k * ins.Stride, ins.Width);
                    break;
                case UFOp::Zero:
                    if (mask.Get(ins.Bit))
                        WriteZero(data, ins.Width);
                    break;
                case UFOp::Group:
                {
                    UFInstr const* members = &info.Update[idx + 1];
                    for (uint32 i = 0; i < ins.Count; ++i)
                        for (uint8 m = 0; m < ins.A; ++m)
                            if (mask.Get(members[m].Bit + i))
                                WriteValue(data, image + members[m].Offset + i * ins.Stride, members[m].Width);
                    idx += ins.A;
                    break;
                }
                case UFOp::Nested:
                {
                    UFInstr const* members = &info.Update[idx + 1];
                    for (uint32 i = 0; i < ins.Count; ++i)
                    {
                        if (!mask.Get(ins.Bit + i))
                            continue;
                        Mask elem = NestedMask(diff, ins, members, i);
                        if (ins.Mask & 0x8000)
                            WriteBlocks(data, elem, ins.Mask & 0xFF);
                        else
                            data.WriteBits(elem.Word(0), ins.Mask);
                        data.FlushBits();
                        for (uint8 m = 0; m < ins.A; ++m)
                        {
                            UFInstr const& member = members[m];
                            uint32 count = std::max<uint32>(member.Count, 1);
                            if (count == 1)
                            {
                                if (elem.Get(0) && elem.Get(member.Bit))
                                    WriteValue(data, image + member.Offset + i * ins.Stride, member.Width);
                                continue;
                            }
                            if (!elem.Get(member.Bit - 1))
                                continue;
                            for (uint32 j = 0; j < count; ++j)
                                if (elem.Get(member.Bit + j))
                                    WriteValue(data, image + member.Offset + i * ins.Stride + j * member.Stride, member.Width);
                        }
                    }
                    idx += ins.A;
                    break;
                }
                case UFOp::Skill:
                {
                    UFInstr const* members = &info.Update[idx + 1];
                    if (mask.Get(ins.Bit))
                    {
                        Mask skill = SkillMask(diff, ins, members);
                        // 57 blocks: the block mask's first word is a plain u32, the rest 25 bits
                        uint32 blockMask[2] = { 0, 0 };
                        for (uint32 b = 0; b < skill.Words(); ++b)
                            if (skill.Word(b))
                                blockMask[b / 32] |= 1u << (b % 32);
                        data.FlushBits();
                        data << uint32(blockMask[0]);
                        data.WriteBits(blockMask[1], 25);
                        for (uint32 b = 0; b < skill.Words(); ++b)
                            if (skill.Word(b))
                                data.WriteBits(skill.Word(b), 32);
                        data.FlushBits();
                        if (skill.Get(0))
                            for (uint32 i = 0; i < ins.Count; ++i)
                                for (uint8 m = 0; m < ins.A; ++m)
                                    if (skill.Get(members[m].Bit + i))
                                        WriteValue(data, image + members[m].Offset + i * ins.Stride, members[m].Width);
                    }
                    idx += ins.A;
                    break;
                }
                case UFOp::Unsupported:
                    ASSERT(!mask.Get(ins.Bit), "UF::WriteUpdate: {} bit {} set but not producible", info.Name, uint32(ins.Bit));
                    break;
                default:
                    break;
            }
        }
        data.FlushBits();
        return true;
    }

    void WriteValuesCreate(ByteBuffer& data, Values const& values, uint8 objectTypeId, uint8 fieldFlags)
    {
        data.FlushBits();
        std::size_t sizePos = data.wpos();
        data << uint32(0);
        data << uint8(fieldFlags);
        for (StructId id : GetTypeStructs(objectTypeId))
        {
            Data const* d = values.Get(id);
            ASSERT(d, "UF::WriteValuesCreate: type {} lacks struct {}", uint32(objectTypeId), GetStructInfo(id).Name);
            WriteCreate(data, *d, fieldFlags);
        }
        data.put<uint32>(sizePos, uint32(data.wpos() - sizePos - 4));
    }

    bool WriteValuesUpdate(ByteBuffer& data, Values const& values, Values const* old, uint8 objectTypeId,
        uint8 fieldFlags, uint8 forceVis, std::span<ForcedField const> forced)
    {
        ByteBuffer body(256);
        uint32 typeMask = 0;
        for (StructId id : GetTypeStructs(objectTypeId))
        {
            Data const* d = values.Get(id);
            ASSERT(d, "UF::WriteValuesUpdate: type {} lacks struct {}", uint32(objectTypeId), GetStructInfo(id).Name);
            if (WriteUpdate(body, *d, old ? old->Get(id) : nullptr, fieldFlags, forceVis, forced, id))
                typeMask |= d->Info().TypeBit;
        }

        if (!typeMask)
            return false;

        data.FlushBits();
        data << uint32(body.size() + 4);
        data << uint32(typeMask);
        data.append(body);
        return true;
    }
}
