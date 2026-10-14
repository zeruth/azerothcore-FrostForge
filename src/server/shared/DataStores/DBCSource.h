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

#ifndef DBC_SOURCE_H
#define DBC_SOURCE_H

#include "DB2Meta.h"
#include "Define.h"
#include <cstddef>

enum class DBCSourceKind : uint8
{
    Column,     // a column (and array element) of the client table
    Id,         // the record id
    ParentId,   // the parent id, from the file's relationship data or its parent column
    Zero        // no 54261 source: 0, 0.0f or an empty string (dbcsources.json says why)
};

constexpr uint8 DBC_SOURCE_NO_EFFECT = 0xFF;

/// Where one loaded column of a store's format comes from in its 3.4.3.54261 client table(s).
struct DBCSource
{
    DBCSourceKind Kind;
    uint8 Field = 0;
    uint8 ArrayIndex = 0;
    uint8 Table = 0;                        // a joined store: the index of the table in Joins
    uint8 Effect = DBC_SOURCE_NO_EFFECT;    // a joined store: the row of this effect index
    uint32 Default = 0;                     // a joined store: the value when the table has no row
};

/// How a joined table's rows name the store's record.
enum class DBCJoinKey : uint8
{
    Id,         // the row id is the record id
    Parent,     // the parent id is
    Column      // a column holds it (KeyField)
};

/// A table a joined store reads (3.3.5's Spell.dbc is a dozen 54261 tables). Where a table holds
/// several rows for one record (one per difficulty) the lowest row id is taken; a table with
/// an EffectField holds one row per effect index, and its sources name the effect.
struct DBCJoinTable
{
    DB2Meta const* Meta;
    DBCJoinKey Key;
    uint8 KeyField = 0;
    int16 EffectField = -1;
};

/// A store's client table and one source per format column the struct holds (all but 'x', 'X'
/// and 'd'). The store is indexed by the record id, whether its format marks the index 'n' or 'd'.
/// A joined store names its tables in Joins; its records are every key they hold.
struct DBCSourceInfo
{
    DB2Meta const* Meta;
    DBCSource const* Sources;
    std::size_t SourceCount;
    DBCJoinTable const* Joins = nullptr;
    std::size_t JoinCount = 0;
};

#endif
