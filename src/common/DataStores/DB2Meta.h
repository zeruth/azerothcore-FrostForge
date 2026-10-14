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

#ifndef DB2_META_H
#define DB2_META_H

#include "Define.h"

/// Column types, numbered as the client numbers them in its table metadata.
enum class DB2FieldType : uint8
{
    Int32   = 0,
    Int64   = 1,
    String  = 2,
    Float   = 3,
    Int8    = 4,
    Int16   = 5
};

struct DB2MetaField
{
    DB2FieldType Type;
    uint8 ArraySize;
    bool IsSigned;
    bool IsLocalized;
};

/// Describes one client table file, as the client's own table metadata does. Fields are the
/// file's columns in file order, followed by the parent id field when the file keeps it in its
/// relationship data instead of in the records.
struct DB2Meta
{
    [[nodiscard]] constexpr bool HasIndexFieldInData() const { return IndexField != -1; }
    [[nodiscard]] constexpr bool HasParentIndexField() const { return ParentIndexField != -1; }
    [[nodiscard]] constexpr bool HasParentIndexFieldInData() const
    {
        return ParentIndexField != -1 && uint32(ParentIndexField) < FileFieldCount;
    }

    char const* Name;
    uint32 FileDataId;
    int32 IndexField;
    int32 ParentIndexField;
    uint32 FieldCount;
    uint32 FileFieldCount;
    uint32 TableHash;
    uint32 LayoutHash;
    bool Sparse;
    DB2MetaField const* Fields;
};

#endif
