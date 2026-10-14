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

#ifndef DB2_FILE_LOADER_H
#define DB2_FILE_LOADER_H

#include "DB2Meta.h"
#include "Define.h"
#include <string>
#include <unordered_map>
#include <vector>

/// Reads a WDC4 client table file (3.4.3.54261) as the client does, described by its DB2Meta.
///
/// Regular files keep every section's records together followed by every section's strings,
/// the layout string offsets are relative to. Sparse files keep variable-length records with
/// their strings inline. Packed, common-data and pallet columns are decoded on read.
///
/// Encrypted sections are read like the others: the extraction fails any file it cannot fully
/// decrypt, so their data is present in the files we load.
class DB2FileLoader
{
public:
    struct RecordCopy
    {
        uint32 NewId;
        uint32 SourceId;
    };

    DB2FileLoader() = default;

    bool Load(char const* filename, DB2Meta const* meta);

    class Record
    {
    public:
        [[nodiscard]] uint32 getId() const;
        /// The parent id from the file's relationship data, or from the parent field when the
        /// records hold it; 0 for tables without a parent
        [[nodiscard]] uint32 getParentId() const;

        /// Integer columns of any width, zero- or sign-extended as the column is declared
        [[nodiscard]] uint32 getUInt(uint32 field, uint32 arrayIndex = 0) const;
        [[nodiscard]] int32 getInt(uint32 field, uint32 arrayIndex = 0) const { return int32(getUInt(field, arrayIndex)); }
        [[nodiscard]] uint64 getUInt64(uint32 field, uint32 arrayIndex = 0) const;
        [[nodiscard]] float getFloat(uint32 field, uint32 arrayIndex = 0) const;
        /// Never null; an empty string when the column has none
        [[nodiscard]] char const* getString(uint32 field, uint32 arrayIndex = 0) const;

    private:
        Record(DB2FileLoader const& file, uint32 index);

        [[nodiscard]] uint32 GetRawValue(uint32 field, uint32 arrayIndex) const;
        [[nodiscard]] uint8 const* GetSparseFieldAddress(uint32 field, uint32 arrayIndex) const;

        DB2FileLoader const& _file;
        uint32 _index;
        uint8 const* _data;
        // sparse records: where each column (and array element) starts
        std::vector<uint32> _sparseOffsets;

        friend class DB2FileLoader;
    };

    [[nodiscard]] Record getRecord(uint32 index) const { return Record(*this, index); }

    [[nodiscard]] uint32 GetNumRows() const { return uint32(_records.size()); }
    [[nodiscard]] std::vector<RecordCopy> const& GetCopies() const { return _copies; }
    [[nodiscard]] uint32 GetMinId() const { return _minId; }
    [[nodiscard]] uint32 GetMaxId() const { return _maxId; }
    [[nodiscard]] DB2Meta const* GetMeta() const { return _meta; }
    [[nodiscard]] std::string const& GetError() const { return _error; }

private:
    // how a column is stored in regular files
    enum class FieldStorage : uint32
    {
        None            = 0,
        Packed          = 1,
        CommonData      = 2,
        Pallet          = 3,
        PalletArray     = 4,
        PackedSigned    = 5
    };

    struct FieldInfo
    {
        uint16 OffsetBits;
        uint16 SizeBits;
        uint32 AdditionalSize;
        FieldStorage Storage;
        uint32 Value1;  // packed bit offset, or the common data default
        uint32 Value2;  // packed bit width
        uint32 Value3;  // pallet array size
    };

    struct RecordEntry
    {
        uint32 Id;
        uint32 ParentId;
        uint8 const* Data;
        uint32 Size;
    };

    bool Fail(std::string error);
    [[nodiscard]] uint32 GetFieldSize(uint32 field) const;

    DB2Meta const* _meta = nullptr;
    std::string _error;
    std::vector<uint8> _file;
    // regular files: every section's records, then every section's strings
    std::vector<uint8> _data;
    uint32 _recordSize = 0;
    uint32 _packedDataOffset = 0;
    uint32 _minId = 0;
    uint32 _maxId = 0;
    // the file's field structures: per column, its size in bits as 32 - value
    std::vector<int16> _fieldSizeBits;
    std::vector<FieldInfo> _fieldInfo;
    // per column, the index of its first array element among all columns' elements
    std::vector<uint32> _arrayStart;
    uint32 _arrayElementCount = 0;
    std::vector<std::vector<uint32>> _pallets;
    std::vector<std::unordered_map<uint32, uint32>> _commonData;
    std::vector<RecordEntry> _records;
    std::vector<RecordCopy> _copies;
};

#endif
