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

#ifndef UPDATE_FIELDS_WRITER_H
#define UPDATE_FIELDS_WRITER_H

#include "ObjectGuid.h"
#include "UpdateFieldsData.h"
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

class ByteBuffer;

/// The 3.4.3 update-field values of an object and their create / update serialization.
///
/// AC keeps its game state where it always has (Object's values array, the entities' members);
/// before an object is sent, its 3.4.3 values are filled into one byte image per data struct
/// (UF::Values, filled by each entity's FillUpdateFields), and the streams are generated from
/// those images by the client-derived programs in UpdateFieldsData.h. A values update is the
/// difference between what was last sent and the fresh fill, so no field needs change tracking of
/// its own.
namespace UF
{
    /// The data structs, in the order a type lists them.
    enum StructId : uint8
    {
        STRUCT_OBJECT,
        STRUCT_ITEM,
        STRUCT_CONTAINER,
        STRUCT_UNIT,
        STRUCT_PLAYER,
        STRUCT_ACTIVE_PLAYER,
        STRUCT_GAMEOBJECT,
        STRUCT_DYNAMICOBJECT,
        STRUCT_CORPSE,
        STRUCT_COUNT
    };

    /// The client's object type ids (SMSG_UPDATE_OBJECT's ObjectTypeID, updateobject.md 2).
    enum ObjectTypeId : uint8
    {
        OBJECT_TYPE_OBJECT          = 0,
        OBJECT_TYPE_ITEM            = 1,
        OBJECT_TYPE_CONTAINER       = 2,
        OBJECT_TYPE_UNIT            = 5,
        OBJECT_TYPE_PLAYER          = 6,
        OBJECT_TYPE_ACTIVE_PLAYER   = 7,
        OBJECT_TYPE_GAMEOBJECT      = 8,
        OBJECT_TYPE_DYNAMICOBJECT   = 9,
        OBJECT_TYPE_CORPSE          = 10,
    };

    UFStructInfo const& GetStructInfo(StructId id);

    /// The structs an object type is made of, in wire order.
    std::span<StructId const> GetTypeStructs(uint8 objectTypeId);

    /// One data struct's values: an image of the client's struct, plus its dynamic arrays.
    class Data
    {
    public:
        explicit Data(UFStructInfo const& info) : _info(&info), _image(info.ImageSize, 0), _dyn(info.DynCount) { }

        UFStructInfo const& Info() const { return *_info; }
        uint8 const* Image() const { return _image.data(); }
        std::vector<uint8> const& DynRaw(uint8 dyn) const { return _dyn[dyn]; }
        uint32 DynSize(uint8 dyn) const { return uint32(_dyn[dyn].size() / _info->Dyns[dyn].ElemSize); }

        void Set(UFField const& f, uint64 value, uint32 i = 0, uint32 j = 0)
        {
            Put(_image.data() + f.At(i, j), f.Width, value);
        }

        void SetFloat(UFField const& f, float value, uint32 i = 0, uint32 j = 0)
        {
            uint32 raw;
            std::memcpy(&raw, &value, sizeof(raw));
            Set(f, raw, i, j);
        }

        void SetGuid(UFField const& f, ObjectGuid const& guid, uint32 i = 0);

        /// A 128-bit GUID given as its two halves (GUID types AC has no ObjectGuid for: guilds, accounts).
        void SetGuid128(UFField const& f, uint64 low, uint64 high, uint32 i = 0)
        {
            uint8* at = _image.data() + f.At(i);
            Put(at, 8, low);
            Put(at + 8, 8, high);
        }

        uint64 Get(UFField const& f, uint32 i = 0, uint32 j = 0) const
        {
            uint64 value = 0;
            std::memcpy(&value, _image.data() + f.At(i, j), std::min<std::size_t>(f.Width, sizeof(value)));
            return value;
        }

        /// Appends a zeroed element to dynamic array `dyn`; returns it for PutElem.
        uint8* DynAppend(uint8 dyn)
        {
            std::vector<uint8>& v = _dyn[dyn];
            std::size_t at = v.size();
            v.resize(at + _info->Dyns[dyn].ElemSize, 0);
            return v.data() + at;
        }

        void DynClear(uint8 dyn) { _dyn[dyn].clear(); }

        static void Put(uint8* at, uint8 width, uint64 value)
        {
            for (uint8 b = 0; b < width && b < 8; ++b)
                at[b] = uint8(value >> (8 * b));
        }

        static void PutElem(uint8* elem, UFField const& f, uint64 value, uint32 j = 0) { Put(elem + f.At(0, j), f.Width, value); }

        bool operator==(Data const& other) const { return _image == other._image && _dyn == other._dyn; }

    private:
        UFStructInfo const* _info;
        std::vector<uint8> _image;
        std::vector<std::vector<uint8>> _dyn;
    };

    /// Fields a values update sends whatever their change: viewer-dependent values (AC's
    /// UF_FLAG_DYNAMIC fields), and every field visible to `ForceVis` (AC's field notify flags,
    /// e.g. party member fields when a member joins).
    struct ForcedField
    {
        StructId Struct;
        uint16 Offset;
    };

    /// An object's values: the structs of its type (an ActivePlayer's include ActivePlayerData).
    class Values
    {
    public:
        Data& Add(StructId id)
        {
            _structs[id].emplace(GetStructInfo(id));
            return *_structs[id];
        }

        Data* Get(StructId id) { return _structs[id] ? &*_structs[id] : nullptr; }
        Data const* Get(StructId id) const { return _structs[id] ? &*_structs[id] : nullptr; }

    private:
        std::array<std::optional<Data>, STRUCT_COUNT> _structs;
    };

    /// A struct's create stream: every field the receiver's FieldFlags let it see, in order.
    void WriteCreate(ByteBuffer& data, Data const& values, uint8 fieldFlags);

    /// A struct's update stream: the change mask (fields that differ from `old`, are forced, and are
    /// visible) and those values. Writes nothing and returns false if no field is in it. Without
    /// `old`, every field is sent.
    bool WriteUpdate(ByteBuffer& data, Data const& values, Data const* old, uint8 fieldFlags, uint8 forceVis,
        std::span<ForcedField const> forced = {}, StructId id = STRUCT_COUNT);

    /// The values of a create block: u32 size, u8 FieldFlags, then each struct of the type.
    void WriteValuesCreate(ByteBuffer& data, Values const& values, uint8 objectTypeId, uint8 fieldFlags);

    /// The values of a values update block: u32 size, u32 changed-type mask, then each changed
    /// struct's update. Writes nothing and returns false when nothing changed.
    bool WriteValuesUpdate(ByteBuffer& data, Values const& values, Values const* old, uint8 objectTypeId,
        uint8 fieldFlags, uint8 forceVis, std::span<ForcedField const> forced = {});
}

#endif // UPDATE_FIELDS_WRITER_H
