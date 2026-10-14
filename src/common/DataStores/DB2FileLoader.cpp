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

#include "DB2FileLoader.h"
#include "Errors.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace
{
    constexpr uint32 DB2_SIGNATURE = 0x34434457; // 'WDC4'

    // the header's flags
    constexpr uint16 DB2_FLAG_SPARSE = 0x1;
    constexpr uint16 DB2_FLAG_ID_LIST = 0x4;

#pragma pack(push, 1)
    struct DB2Header
    {
        uint32 Signature;
        uint32 RecordCount;
        uint32 FieldCount;
        uint32 RecordSize;
        uint32 StringTableSize;
        uint32 TableHash;
        uint32 LayoutHash;
        uint32 MinId;
        uint32 MaxId;
        uint32 Locale;
        uint16 Flags;
        uint16 IdIndex;
        uint32 TotalFieldCount;
        uint32 PackedDataOffset;
        uint32 LookupColumnCount;
        uint32 FieldStorageInfoSize;
        uint32 CommonDataSize;
        uint32 PalletDataSize;
        uint32 SectionCount;
    };

    struct DB2SectionHeader
    {
        uint64 TactKey;             // non-zero: the section is encrypted with this key
        uint32 FileOffset;
        uint32 RecordCount;
        uint32 StringTableSize;
        uint32 RecordsEnd;          // sparse files: where the variable-length records end
        uint32 IdListSize;
        uint32 RelationshipSize;
        uint32 OffsetMapCount;
        uint32 CopyTableCount;
    };

    struct DB2FieldStructure
    {
        int16 SizeBits;             // the column's width in bits is 32 - SizeBits
        uint16 Offset;
    };

    struct DB2OffsetMapEntry
    {
        uint32 Offset;
        uint16 Size;
    };
#pragma pack(pop)

    static_assert(sizeof(DB2Header) == 0x48);
    static_assert(sizeof(DB2SectionHeader) == 0x28);

    /// Reads a little-endian value at pos, advancing it
    template<typename T>
    bool Read(std::vector<uint8> const& file, std::size_t& pos, T& value)
    {
        if (pos + sizeof(T) > file.size())
            return false;

        memcpy(&value, file.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    uint64 ReadBits(uint8 const* base, uint32 offsetBits, uint32 sizeBits)
    {
        uint64 word;
        memcpy(&word, base + (offsetBits >> 3), sizeof(word));
        uint32 top = 64 - sizeBits;
        return (word << ((top - (offsetBits & 7)) & 63)) >> (top & 63);
    }

    uint32 FieldTypeWidth(DB2FieldType type)
    {
        switch (type)
        {
            case DB2FieldType::Int8:
                return 1;
            case DB2FieldType::Int16:
                return 2;
            case DB2FieldType::Int64:
                return 8;
            default:
                return 4;
        }
    }
}

bool DB2FileLoader::Fail(std::string error)
{
    _error = std::move(error);
    _records.clear();
    return false;
}

uint32 DB2FileLoader::GetFieldSize(uint32 field) const
{
    return uint32(32 - _fieldSizeBits[field]) / 8;
}

bool DB2FileLoader::Load(char const* filename, DB2Meta const* meta)
{
    static_assert(sizeof(FieldInfo) == 0x18, "a column storage entry is 0x18 bytes");
    static_assert(sizeof(RecordCopy) == 0x8, "a copy table entry is 8 bytes");

    _meta = meta;
    _error.clear();

    std::ifstream stream(filename, std::ios::binary);
    if (!stream)
        return Fail(Acore::StringFormat("Unable to open {}", filename));

    _file.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());

    std::size_t pos = 0;
    DB2Header header;
    if (!Read(_file, pos, header))
        return Fail(Acore::StringFormat("{}: too small for a header", filename));

    if (header.Signature != DB2_SIGNATURE)
        return Fail(Acore::StringFormat("{}: signature 0x{:08X}, expected WDC4", filename, header.Signature));

    if (header.TableHash != meta->TableHash)
        return Fail(Acore::StringFormat("{}: table hash 0x{:08X}, expected 0x{:08X} for {}",
            filename, header.TableHash, meta->TableHash, meta->Name));

    if (header.LayoutHash != meta->LayoutHash)
        return Fail(Acore::StringFormat("{}: layout hash 0x{:08X}, expected 0x{:08X} (wrong client build?)",
            filename, header.LayoutHash, meta->LayoutHash));

    if (header.FieldCount != meta->FileFieldCount || header.TotalFieldCount != meta->FileFieldCount)
        return Fail(Acore::StringFormat("{}: {} columns, expected {}",
            filename, header.FieldCount, meta->FileFieldCount));

    bool sparse = (header.Flags & DB2_FLAG_SPARSE) != 0;
    if (sparse != meta->Sparse || ((header.Flags & DB2_FLAG_ID_LIST) != 0) != !meta->HasIndexFieldInData())
        return Fail(Acore::StringFormat("{}: flags 0x{:X} do not match the table's metadata", filename, header.Flags));

    if (header.LookupColumnCount > 1)
        return Fail(Acore::StringFormat("{}: {} relationship columns, at most 1 is supported",
            filename, header.LookupColumnCount));

    std::vector<DB2SectionHeader> sections(header.SectionCount);
    for (DB2SectionHeader& section : sections)
        if (!Read(_file, pos, section))
            return Fail(Acore::StringFormat("{}: truncated section headers", filename));

    _fieldSizeBits.resize(header.FieldCount);
    for (int16& sizeBits : _fieldSizeBits)
    {
        DB2FieldStructure structure;
        if (!Read(_file, pos, structure))
            return Fail(Acore::StringFormat("{}: truncated field structures", filename));

        sizeBits = structure.SizeBits;
    }

    if (header.RecordCount && !sparse && !header.FieldStorageInfoSize)
        return Fail(Acore::StringFormat("{}: records without column storage information", filename));

    _fieldInfo.resize(header.FieldStorageInfoSize / sizeof(FieldInfo));
    for (FieldInfo& info : _fieldInfo)
        if (!Read(_file, pos, info))
            return Fail(Acore::StringFormat("{}: truncated column storage information", filename));

    // pallets, then pallet arrays, then common data, each in column order
    _pallets.assign(_fieldInfo.size(), {});
    for (FieldStorage storage : { FieldStorage::Pallet, FieldStorage::PalletArray })
    {
        for (std::size_t field = 0; field < _fieldInfo.size(); ++field)
        {
            if (_fieldInfo[field].Storage != storage)
                continue;

            _pallets[field].resize(_fieldInfo[field].AdditionalSize / sizeof(uint32));
            for (uint32& value : _pallets[field])
                if (!Read(_file, pos, value))
                    return Fail(Acore::StringFormat("{}: truncated pallet of column {}", filename, field));
        }
    }

    _commonData.assign(_fieldInfo.size(), {});
    for (std::size_t field = 0; field < _fieldInfo.size(); ++field)
    {
        if (_fieldInfo[field].Storage != FieldStorage::CommonData)
            continue;

        for (uint32 i = 0; i < _fieldInfo[field].AdditionalSize / 8; ++i)
        {
            uint32 id;
            uint32 value;
            if (!Read(_file, pos, id) || !Read(_file, pos, value))
                return Fail(Acore::StringFormat("{}: truncated common data of column {}", filename, field));

            _commonData[field][id] = value;
        }
    }

    // each encrypted section lists the ids it holds; they are read with the section itself
    for (DB2SectionHeader const& section : sections)
    {
        if (!section.TactKey)
            continue;

        uint32 count;
        if (!Read(_file, pos, count))
            return Fail(Acore::StringFormat("{}: truncated encrypted id list", filename));

        pos += std::size_t(count) * sizeof(uint32);
    }

    if (!sparse)
    {
        std::size_t expected = pos + std::size_t(header.RecordSize) * header.RecordCount + header.StringTableSize
            + (meta->HasIndexFieldInData() ? 0 : std::size_t(header.RecordCount) * sizeof(uint32));
        for (DB2SectionHeader const& section : sections)
            expected += section.RelationshipSize + std::size_t(section.CopyTableCount) * sizeof(RecordCopy);

        if (expected != _file.size())
            return Fail(Acore::StringFormat("{}: {} bytes, the header accounts for {}",
                filename, _file.size(), expected));
    }

    _arrayStart.resize(meta->FieldCount);
    _arrayElementCount = 0;
    for (uint32 field = 0; field < meta->FieldCount; ++field)
    {
        _arrayStart[field] = _arrayElementCount;
        _arrayElementCount += meta->Fields[field].ArraySize;
    }

    _recordSize = header.RecordSize;
    _packedDataOffset = header.PackedDataOffset;
    _minId = header.MinId;
    _maxId = header.MaxId;

    // regular files: records of every section, then strings of every section, 8 bytes of
    // padding for packed reads at the end
    std::size_t stringsStart = std::size_t(header.RecordSize) * header.RecordCount;
    if (!sparse)
        _data.assign(stringsStart + header.StringTableSize + 8, 0);

    _records.clear();
    _records.reserve(header.RecordCount);
    _copies.clear();

    std::size_t stringsOffset = stringsStart;
    for (std::size_t s = 0; s < sections.size(); ++s)
    {
        DB2SectionHeader const& section = sections[s];
        std::size_t sectionFirstRecord = _records.size();
        pos = section.FileOffset;

        if (!sparse)
        {
            std::size_t recordBytes = std::size_t(header.RecordSize) * section.RecordCount;
            if (pos + recordBytes + section.StringTableSize > _file.size())
                return Fail(Acore::StringFormat("{}: section {} runs past the end of the file", filename, s));

            uint8* records = _data.data() + sectionFirstRecord * header.RecordSize;
            memcpy(records, _file.data() + pos, recordBytes);
            memcpy(_data.data() + stringsOffset, _file.data() + pos + recordBytes, section.StringTableSize);
            pos += recordBytes + section.StringTableSize;
            stringsOffset += section.StringTableSize;

            for (uint32 i = 0; i < section.RecordCount; ++i)
                _records.push_back({ 0, 0, records + std::size_t(i) * header.RecordSize, header.RecordSize });
        }
        else
            pos = section.RecordsEnd;

        std::vector<uint32> ids(section.IdListSize / sizeof(uint32));
        for (uint32& id : ids)
            if (!Read(_file, pos, id))
                return Fail(Acore::StringFormat("{}: truncated id list in section {}", filename, s));

        for (uint32 i = 0; i < section.CopyTableCount; ++i)
        {
            RecordCopy copy;
            if (!Read(_file, pos, copy))
                return Fail(Acore::StringFormat("{}: truncated copy table in section {}", filename, s));

            _copies.push_back(copy);
        }

        std::vector<DB2OffsetMapEntry> offsetMap(section.OffsetMapCount);
        for (DB2OffsetMapEntry& entry : offsetMap)
            if (!Read(_file, pos, entry))
                return Fail(Acore::StringFormat("{}: truncated offset map in section {}", filename, s));

        // relationship data: (parent id, record index in the section) pairs
        std::vector<std::pair<uint32, uint32>> parents;
        if (section.RelationshipSize)
        {
            uint32 count;
            uint32 minParent;
            uint32 maxParent;
            if (!Read(_file, pos, count) || !Read(_file, pos, minParent) || !Read(_file, pos, maxParent))
                return Fail(Acore::StringFormat("{}: truncated relationship data in section {}", filename, s));

            parents.resize(count);
            for (std::pair<uint32, uint32>& parent : parents)
                if (!Read(_file, pos, parent.first) || !Read(_file, pos, parent.second))
                    return Fail(Acore::StringFormat("{}: truncated relationship data in section {}", filename, s));
        }

        if (sparse)
        {
            std::vector<uint32> offsetMapIds(section.OffsetMapCount);
            for (uint32& id : offsetMapIds)
                if (!Read(_file, pos, id))
                    return Fail(Acore::StringFormat("{}: truncated offset map ids in section {}", filename, s));

            if (ids.empty())
                ids = offsetMapIds;

            for (DB2OffsetMapEntry const& entry : offsetMap)
            {
                if (std::size_t(entry.Offset) + entry.Size > _file.size())
                    return Fail(Acore::StringFormat("{}: record past the end of the file in section {}", filename, s));

                _records.push_back({ 0, 0, _file.data() + entry.Offset, entry.Size });
            }
        }

        std::size_t sectionRecords = _records.size() - sectionFirstRecord;
        if (!meta->HasIndexFieldInData() && ids.size() != sectionRecords)
            return Fail(Acore::StringFormat("{}: section {} has {} records but {} ids", filename, s, sectionRecords, ids.size()));

        for (std::size_t i = 0; i < ids.size() && i < sectionRecords; ++i)
            _records[sectionFirstRecord + i].Id = ids[i];

        for (std::pair<uint32, uint32> const& parent : parents)
        {
            if (parent.second >= sectionRecords)
                return Fail(Acore::StringFormat("{}: relationship of section {} points at record {} of {}",
                    filename, s, parent.second, sectionRecords));

            _records[sectionFirstRecord + parent.second].ParentId = parent.first;
        }
    }

    if (_records.size() != header.RecordCount)
        return Fail(Acore::StringFormat("{}: {} records read, the header has {}", filename, _records.size(), header.RecordCount));

    if (meta->HasIndexFieldInData())
        for (uint32 i = 0; i < _records.size(); ++i)
            _records[i].Id = getRecord(i).getUInt(uint32(meta->IndexField));

    if (meta->HasParentIndexFieldInData())
        for (uint32 i = 0; i < _records.size(); ++i)
            _records[i].ParentId = getRecord(i).getUInt(uint32(meta->ParentIndexField));

    return true;
}

DB2FileLoader::Record::Record(DB2FileLoader const& file, uint32 index) : _file(file), _index(index), _data(nullptr)
{
    ASSERT(index < file._records.size(), "record {} of {} in {}", index, file._records.size(), file._meta->Name);
    _data = file._records[index].Data;

    if (!file._meta->Sparse)
        return;

    // sparse records keep their columns at their natural sizes, strings inline
    DB2Meta const* meta = file._meta;
    _sparseOffsets.resize(file._arrayElementCount);
    uint32 offset = 0;
    for (uint32 field = 0; field < meta->FileFieldCount; ++field)
    {
        for (uint32 i = 0; i < meta->Fields[field].ArraySize; ++i)
        {
            _sparseOffsets[file._arrayStart[field] + i] = offset;
            switch (meta->Fields[field].Type)
            {
                case DB2FieldType::String:
                    offset += uint32(strnlen(reinterpret_cast<char const*>(_data) + offset,
                        file._records[index].Size - offset)) + 1;
                    break;
                case DB2FieldType::Float:
                    offset += sizeof(float);
                    break;
                default:
                    offset += file.GetFieldSize(field);
                    break;
            }
        }
    }

    ASSERT(offset <= file._records[index].Size, "{} record {} reads {} bytes of {}",
        meta->Name, index, offset, file._records[index].Size);
}

uint32 DB2FileLoader::Record::getId() const
{
    return _file._records[_index].Id;
}

uint32 DB2FileLoader::Record::getParentId() const
{
    return _file._records[_index].ParentId;
}

uint8 const* DB2FileLoader::Record::GetSparseFieldAddress(uint32 field, uint32 arrayIndex) const
{
    ASSERT(field < _file._meta->FileFieldCount && arrayIndex < _file._meta->Fields[field].ArraySize);
    return _data + _sparseOffsets[_file._arrayStart[field] + arrayIndex];
}

uint32 DB2FileLoader::Record::GetRawValue(uint32 field, uint32 arrayIndex) const
{
    DB2Meta const* meta = _file._meta;
    ASSERT(field < meta->FileFieldCount && arrayIndex < meta->Fields[field].ArraySize,
        "{} column {}[{}] out of range", meta->Name, field, arrayIndex);

    if (meta->Sparse)
    {
        uint32 value = 0;
        memcpy(&value, GetSparseFieldAddress(field, arrayIndex), std::min<uint32>(_file.GetFieldSize(field), sizeof(value)));
        return value;
    }

    FieldInfo const& info = _file._fieldInfo[field];
    uint8 const* packed = _data + _file._packedDataOffset;
    switch (info.Storage)
    {
        case FieldStorage::None:
        {
            uint32 width = FieldTypeWidth(meta->Fields[field].Type);
            uint32 value = 0;
            memcpy(&value, _data + (info.OffsetBits >> 3) + std::size_t(arrayIndex) * width, std::min<uint32>(width, 4));
            return value;
        }
        case FieldStorage::Packed:
            return uint32(ReadBits(packed, info.Value1, info.Value2));
        case FieldStorage::CommonData:
        {
            std::unordered_map<uint32, uint32> const& values = _file._commonData[field];
            auto itr = values.find(getId());
            return itr != values.end() ? itr->second : info.Value1;
        }
        case FieldStorage::Pallet:
            return _file._pallets[field][ReadBits(packed, info.Value1, info.Value2)];
        case FieldStorage::PalletArray:
            return _file._pallets[field][ReadBits(packed, info.Value1, info.Value2) * info.Value3 + arrayIndex];
        case FieldStorage::PackedSigned:
        {
            uint32 bits = uint32(ReadBits(packed, info.Value1, info.Value2));
            uint32 sign = 1u << ((info.Value2 - 1) & 31);
            return (bits ^ sign) - sign;
        }
        default:
            ABORT("{} column {} has unknown storage {}", meta->Name, field, uint32(info.Storage));
    }

    return 0;
}

uint32 DB2FileLoader::Record::getUInt(uint32 field, uint32 arrayIndex) const
{
    DB2MetaField const& meta = _file._meta->Fields[field];
    uint32 raw = GetRawValue(field, arrayIndex);
    switch (meta.Type)
    {
        case DB2FieldType::Int8:
            return meta.IsSigned ? uint32(int32(int8(raw))) : uint8(raw);
        case DB2FieldType::Int16:
            return meta.IsSigned ? uint32(int32(int16(raw))) : uint16(raw);
        default:
            return raw;
    }
}

uint64 DB2FileLoader::Record::getUInt64(uint32 field, uint32 arrayIndex) const
{
    DB2Meta const* meta = _file._meta;
    if (meta->Fields[field].Type != DB2FieldType::Int64)
        return getUInt(field, arrayIndex);

    uint8 const* address = nullptr;
    if (meta->Sparse)
        address = GetSparseFieldAddress(field, arrayIndex);
    else if (_file._fieldInfo[field].Storage == FieldStorage::None)
        address = _data + (_file._fieldInfo[field].OffsetBits >> 3) + std::size_t(arrayIndex) * sizeof(uint64);
    else
        return GetRawValue(field, arrayIndex);

    uint64 value;
    memcpy(&value, address, sizeof(value));
    return value;
}

float DB2FileLoader::Record::getFloat(uint32 field, uint32 arrayIndex) const
{
    ASSERT(_file._meta->Fields[field].Type == DB2FieldType::Float, "{} column {} is not a float", _file._meta->Name, field);
    uint32 raw = GetRawValue(field, arrayIndex);
    float value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

char const* DB2FileLoader::Record::getString(uint32 field, uint32 arrayIndex) const
{
    DB2Meta const* meta = _file._meta;
    ASSERT(meta->Fields[field].Type == DB2FieldType::String, "{} column {} is not a string", meta->Name, field);

    if (meta->Sparse)
        return reinterpret_cast<char const*>(GetSparseFieldAddress(field, arrayIndex));

    // the column holds the string's offset from the column itself
    uint8 const* column = _data + (_file._fieldInfo[field].OffsetBits >> 3) + std::size_t(arrayIndex) * sizeof(uint32);
    uint32 offset;
    memcpy(&offset, column, sizeof(offset));
    if (!offset)
        return "";

    std::size_t position = std::size_t(column - _file._data.data()) + offset;
    ASSERT(position < _file._data.size(), "{} column {} of record {} points outside the strings", meta->Name, field, _index);
    return reinterpret_cast<char const*>(_file._data.data() + position);
}
