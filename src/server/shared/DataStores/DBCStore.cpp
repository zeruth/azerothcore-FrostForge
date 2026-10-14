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

#include "DBCStore.h"
#include "DB2FileLoader.h"
#include "DBCDatabaseLoader.h"
#include "DBCFileLoader.h"
#include "StringFormat.h"
#include <algorithm>
#include <bit>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

DBCStorageBase::DBCStorageBase(char const* fmt) : _fieldCount(0), _fileFormat(fmt), _dataTable(nullptr), _indexTableSize(0)
{
}

DBCStorageBase::~DBCStorageBase()
{
    delete[] _dataTable;
    for (char* strings : _stringPool)
        delete[] strings;
}

bool DBCStorageBase::Load(char const* path, char**& indexTable)
{
    indexTable = nullptr;

    DBCFileLoader dbc;

    // Check if load was sucessful, only then continue
    if (!dbc.Load(path, _fileFormat))
        return false;

    _fieldCount = dbc.GetCols();

    // load raw non-string data
    _dataTable = dbc.AutoProduceData(_fileFormat, _indexTableSize, indexTable);

    // load strings from dbc data
    if (char* stringBlock = dbc.AutoProduceStrings(_fileFormat, _dataTable))
        _stringPool.push_back(stringBlock);

    // error in dbc file at loading if nullptr
    return indexTable != nullptr;
}

bool DBCStorageBase::LoadDB2(char const* path, DBCSourceInfo const& sources, std::string& error, char**& indexTable)
{
    indexTable = nullptr;

    // the tables read: a single-table store is its own table, keyed by the record id
    DBCJoinTable const single{ sources.Meta, DBCJoinKey::Id };
    DBCJoinTable const* joins = sources.Joins ? sources.Joins : &single;
    std::size_t joinCount = sources.Joins ? sources.JoinCount : 1;

    // the joined tables sit beside the store's own
    std::string directory(path);
    directory.resize(directory.find_last_of("/\\") + 1);

    struct Table
    {
        DB2FileLoader File;
        // key (and effect index in the high bits) -> row; the lowest row id where several share one
        std::unordered_map<uint64, uint32> Rows;
    };

    std::vector<std::unique_ptr<Table>> tables;
    for (std::size_t t = 0; t < joinCount; ++t)
    {
        DBCJoinTable const& join = joins[t];
        std::unique_ptr<Table> table = std::make_unique<Table>();
        std::string file = sources.Joins ? directory + join.Meta->Name + ".db2" : std::string(path);
        if (!table->File.Load(file.c_str(), join.Meta))
        {
            error = table->File.GetError();
            return false;
        }

        std::vector<std::pair<uint32, uint32>> rows; // (row id, row)
        for (uint32 i = 0; i < table->File.GetNumRows(); ++i)
            rows.emplace_back(table->File.getRecord(i).getId(), i);
        std::sort(rows.begin(), rows.end());

        for (auto const& [id, row] : rows)
        {
            DB2FileLoader::Record record = table->File.getRecord(row);
            uint32 key = join.Key == DBCJoinKey::Id ? id : join.Key == DBCJoinKey::Parent ? record.getParentId() : record.getUInt(join.KeyField);
            uint64 effect = join.EffectField >= 0 ? record.getUInt(uint32(join.EffectField)) : 0;
            table->Rows.try_emplace(key | (effect << 32), row);
        }

        // a single table's copies are its source records under new ids
        if (!sources.Joins)
            for (DB2FileLoader::RecordCopy const& copy : table->File.GetCopies())
                if (auto itr = table->Rows.find(copy.SourceId); itr != table->Rows.end())
                    table->Rows.try_emplace(copy.NewId, itr->second);

        tables.push_back(std::move(table));
    }

    auto inStruct = [](char c) { return c != FT_NA && c != FT_NA_BYTE && c != FT_SORT; };

    // one source per column the struct holds, each of a type the column can take
    std::size_t source = 0;
    for (char const* format = _fileFormat; *format; ++format)
    {
        if (!inStruct(*format))
            continue;

        if (source >= sources.SourceCount)
        {
            error = Acore::StringFormat("{}: the format holds more columns than the {} sources", sources.Meta->Name, sources.SourceCount);
            return false;
        }

        DBCSource const& from = sources.Sources[source++];
        if (from.Table >= joinCount)
        {
            error = Acore::StringFormat("{}: a source reads table {} of {}", sources.Meta->Name, from.Table, joinCount);
            return false;
        }

        if (from.Kind != DBCSourceKind::Column)
            continue;

        DB2Meta const* meta = joins[from.Table].Meta;
        if (from.Field >= meta->FileFieldCount || from.ArrayIndex >= meta->Fields[from.Field].ArraySize)
        {
            error = Acore::StringFormat("{}: no column {}[{}]", meta->Name, from.Field, from.ArrayIndex);
            return false;
        }

        DB2FieldType type = meta->Fields[from.Field].Type;
        bool fits = *format == FT_STRING ? type == DB2FieldType::String
            : *format == FT_FLOAT ? type == DB2FieldType::Float
            : type != DB2FieldType::String && type != DB2FieldType::Float;
        if (!fits)
        {
            error = Acore::StringFormat("{}: format column '{}' cannot load column {} of type {}",
                meta->Name, *format, from.Field, uint32(type));
            return false;
        }
    }

    if (source != sources.SourceCount)
    {
        error = Acore::StringFormat("{}: {} sources for {} columns", sources.Meta->Name, sources.SourceCount, source);
        return false;
    }

    _fieldCount = uint32(strlen(_fileFormat));
    uint32 recordSize = DBCFileLoader::GetFormatRecordSize(_fileFormat);

    // the records: every key any table holds (10 spells have a SpellName and effects but no SpellMisc)
    std::vector<uint32> ids;
    for (std::unique_ptr<Table> const& table : tables)
        for (auto const& [key, row] : table->Rows)
            ids.push_back(uint32(key));
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    // the row a source reads for a record, if its table has one
    auto rowOf = [&](DBCSource const& from, uint32 id) -> std::optional<DB2FileLoader::Record>
    {
        Table const& table = *tables[from.Table];
        uint64 effect = from.Effect != DBC_SOURCE_NO_EFFECT ? from.Effect : 0;
        auto itr = table.Rows.find(id | (effect << 32));
        if (itr == table.Rows.end())
            return std::nullopt;

        return table.File.getRecord(itr->second);
    };

    auto isStringColumn = [&](DBCSource const& from)
    {
        return from.Kind == DBCSourceKind::Column && joins[from.Table].Meta->Fields[from.Field].Type == DB2FieldType::String;
    };

    // strings: one pool, with the empty string every column without text points at in front
    std::size_t stringSize = 1;
    for (uint32 id : ids)
        for (std::size_t s = 0; s < sources.SourceCount; ++s)
            if (isStringColumn(sources.Sources[s]))
                if (std::optional<DB2FileLoader::Record> record = rowOf(sources.Sources[s], id))
                    stringSize += strlen(record->getString(sources.Sources[s].Field, sources.Sources[s].ArrayIndex)) + 1;

    char* strings = new char[stringSize];
    strings[0] = '\0';
    std::size_t stringOffset = 1;
    _stringPool.push_back(strings);

    _indexTableSize = ids.empty() ? 0 : ids.back() + 1;
    indexTable = new char*[_indexTableSize];
    memset(indexTable, 0, _indexTableSize * sizeof(char*));
    _dataTable = new char[ids.size() * recordSize];

    char* data = _dataTable;
    for (uint32 id : ids)
    {
        indexTable[id] = data;
        source = 0;
        for (char const* format = _fileFormat; *format; ++format)
        {
            if (!inStruct(*format))
                continue;

            DBCSource const& from = sources.Sources[source++];
            // (Record holds a reference: constructed, never assigned)
            std::optional<DB2FileLoader::Record> record = from.Kind == DBCSourceKind::Column || from.Kind == DBCSourceKind::ParentId
                ? rowOf(from, id) : std::nullopt;

            if (*format == FT_STRING)
            {
                char* string = strings;
                if (isStringColumn(from) && record)
                {
                    char const* text = record->getString(from.Field, from.ArrayIndex);
                    if (std::size_t length = strlen(text))
                    {
                        string = strings + stringOffset;
                        memcpy(string, text, length + 1);
                        stringOffset += length + 1;
                    }
                }

                memcpy(data, &string, sizeof(char*));
                data += sizeof(char*);
                continue;
            }

            // a record without a row in a joined table has the source's default
            uint32 value = 0;
            switch (from.Kind)
            {
                case DBCSourceKind::Column:
                    if (record)
                        value = *format == FT_FLOAT ? std::bit_cast<uint32>(record->getFloat(from.Field, from.ArrayIndex))
                            : record->getUInt(from.Field, from.ArrayIndex);
                    else
                        value = from.Default;
                    break;
                case DBCSourceKind::Id:
                    value = id;
                    break;
                case DBCSourceKind::ParentId:
                    if (record)
                        value = record->getParentId();
                    break;
                case DBCSourceKind::Zero:
                    break;
            }

            if (*format == FT_BYTE)
            {
                *reinterpret_cast<uint8*>(data) = uint8(value);
                data += sizeof(uint8);
            }
            else
            {
                ASSERT(*format != FT_LOGIC, "{}: logical columns are not loaded", sources.Meta->Name);
                memcpy(data, &value, sizeof(value));
                data += sizeof(value);
            }
        }
    }

    ASSERT(std::size_t(data - _dataTable) == ids.size() * recordSize);
    return true;
}

bool DBCStorageBase::LoadGameTable(char const* path, std::string& error, char**& indexTable)
{
    indexTable = nullptr;

    // a client GameTable (GameTables/*.txt): a header line, then one line per row, tab separated,
    // the first column the row's label. AC's gt stores keep the values column by column
    // (entry = column * rows + row): apps/db2meta/gtmatch.py reproduces every 3.3.5 store so
    std::ifstream file(path);
    if (!file)
    {
        error = Acore::StringFormat("Unable to open {}", path);
        return false;
    }

    std::string line;
    std::getline(file, line);
    std::vector<std::vector<float>> rows;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty())
            continue;

        std::vector<float> row;
        std::istringstream cells(line);
        std::string cell;
        std::getline(cells, cell, '\t');    // the label
        while (std::getline(cells, cell, '\t'))
            row.push_back(std::stof(cell));

        if (!rows.empty() && row.size() != rows.front().size())
        {
            error = Acore::StringFormat("{}: row {} has {} columns, the first {}", path, rows.size() + 1, row.size(), rows.front().size());
            return false;
        }

        rows.push_back(std::move(row));
    }

    if (rows.empty())
    {
        error = Acore::StringFormat("{}: no rows", path);
        return false;
    }

    std::vector<float> values;
    for (std::size_t column = 0; column < rows.front().size(); ++column)
        for (std::vector<float> const& row : rows)
            values.push_back(row[column]);

    _fieldCount = uint32(strlen(_fileFormat));
    uint32 recordSize = DBCFileLoader::GetFormatRecordSize(_fileFormat);
    _indexTableSize = uint32(values.size());
    indexTable = new char*[_indexTableSize];
    _dataTable = new char[values.size() * recordSize];

    char* data = _dataTable;
    for (uint32 i = 0; i < values.size(); ++i)
    {
        indexTable[i] = data;
        for (char const* format = _fileFormat; *format; ++format)
        {
            switch (*format)
            {
                case FT_FLOAT:
                    memcpy(data, &values[i], sizeof(float));
                    data += sizeof(float);
                    break;
                case FT_IND:
                case FT_INT:
                    memcpy(data, &i, sizeof(uint32));
                    data += sizeof(uint32);
                    break;
                case FT_SORT:
                case FT_NA:
                    break;
                default:
                    error = Acore::StringFormat("{}: a game table has no column for format '{}'", path, *format);
                    return false;
            }
        }
    }

    return true;
}

bool DBCStorageBase::LoadStringsFrom(char const* path, char** indexTable)
{
    // DBC must be already loaded using Load
    if (!indexTable)
        return false;

    DBCFileLoader dbc;

    // Check if load was successful, only then continue
    if (!dbc.Load(path, _fileFormat))
        return false;

    // load strings from another locale dbc data
    if (char* stringBlock = dbc.AutoProduceStrings(_fileFormat, _dataTable))
        _stringPool.push_back(stringBlock);

    return true;
}

void DBCStorageBase::LoadFromDB(char const* table, char const* format, char**& indexTable)
{
    _stringPool.push_back(DBCDatabaseLoader(table, format, _stringPool).Load(_indexTableSize, indexTable));
}
