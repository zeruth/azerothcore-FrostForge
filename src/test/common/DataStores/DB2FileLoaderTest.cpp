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
#include "DB2Metadata.h"
#include "gtest/gtest.h"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

// These tests read the 3.4.3.54261 client tables extracted to var/data/dbc (see
// .agents/plans/frostforge-server/data-track.PLAN.md), or the directory in AC_DB2_DIR.
// They are skipped when the tables are not there.

namespace
{
    std::filesystem::path GetDB2Dir()
    {
        if (char const* dir = std::getenv("AC_DB2_DIR"))
            return dir;

        return std::filesystem::path(AC_SOURCE_DIR) / "var" / "data" / "dbc";
    }

    std::string GetDB2Path(DB2Meta const& meta)
    {
        return (GetDB2Dir() / (std::string(meta.Name) + ".db2")).string();
    }

    class DB2FileLoaderTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!std::filesystem::exists(GetDB2Path(DB2Metadata::Map)))
                GTEST_SKIP() << "no client tables in " << GetDB2Dir().string();
        }

        // record index by id
        static std::unordered_map<uint32, uint32> IndexById(DB2FileLoader const& db2)
        {
            std::unordered_map<uint32, uint32> index;
            for (uint32 i = 0; i < db2.GetNumRows(); ++i)
                index[db2.getRecord(i).getId()] = i;

            return index;
        }
    };
}

// Every table opens, its ids are unique and within the header's range, every column of every
// record decodes, and every copy points at a record that exists.
TEST_F(DB2FileLoaderTest, LoadsEveryTable)
{
    for (DB2Meta const* meta : DB2Metadata::All)
    {
        SCOPED_TRACE(meta->Name);

        DB2FileLoader db2;
        ASSERT_TRUE(db2.Load(GetDB2Path(*meta).c_str(), meta)) << db2.GetError();

        std::unordered_set<uint32> ids;
        for (uint32 i = 0; i < db2.GetNumRows(); ++i)
        {
            DB2FileLoader::Record record = db2.getRecord(i);
            uint32 id = record.getId();
            EXPECT_GE(id, db2.GetMinId());
            EXPECT_LE(id, db2.GetMaxId());
            EXPECT_TRUE(ids.insert(id).second) << "duplicate id " << id;

            for (uint32 field = 0; field < meta->FileFieldCount; ++field)
            {
                for (uint32 a = 0; a < meta->Fields[field].ArraySize; ++a)
                {
                    switch (meta->Fields[field].Type)
                    {
                        case DB2FieldType::String:
                            EXPECT_NE(record.getString(field, a), nullptr);
                            break;
                        case DB2FieldType::Float:
                            (void)record.getFloat(field, a);
                            break;
                        case DB2FieldType::Int64:
                            (void)record.getUInt64(field, a);
                            break;
                        default:
                            (void)record.getUInt(field, a);
                            break;
                    }
                }
            }

            (void)record.getParentId();
        }

        for (DB2FileLoader::RecordCopy const& copy : db2.GetCopies())
            EXPECT_TRUE(ids.contains(copy.SourceId)) << "copy " << copy.NewId << " of missing " << copy.SourceId;
    }
}

TEST_F(DB2FileLoaderTest, Map)
{
    DB2FileLoader db2;
    ASSERT_TRUE(db2.Load(GetDB2Path(DB2Metadata::Map).c_str(), &DB2Metadata::Map)) << db2.GetError();

    // columns: 0 Directory, 7 InstanceType (pallet-free packed), 8 ExpansionID (packed, 2 bits).
    // This build tags the continents expansion 0, Outland and Northrend included; the expansion
    // shows on their instances.
    std::unordered_map<uint32, uint32> index = IndexById(db2);
    struct Expected { uint32 Id; char const* Directory; uint32 InstanceType; uint32 Expansion; };
    for (Expected const& expected : { Expected{ 0, "Azeroth", 0, 0 }, Expected{ 1, "Kalimdor", 0, 0 },
        Expected{ 33, "Shadowfang", 1, 0 }, Expected{ 30, "PVPZone01", 3, 0 },
        Expected{ 530, "Expansion01", 0, 0 }, Expected{ 532, "Karazahn", 2, 1 },
        Expected{ 571, "Northrend", 0, 0 }, Expected{ 575, "UtgardePinnacle", 1, 2 } })
    {
        SCOPED_TRACE(expected.Id);
        ASSERT_TRUE(index.contains(expected.Id));
        DB2FileLoader::Record record = db2.getRecord(index[expected.Id]);
        EXPECT_STREQ(record.getString(0), expected.Directory);
        EXPECT_EQ(record.getUInt(7), expected.InstanceType);
        EXPECT_EQ(record.getUInt(8), expected.Expansion);
    }
}

// a sparse table: variable-length records, strings inline
TEST_F(DB2FileLoaderTest, ItemSparse)
{
    DB2FileLoader db2;
    ASSERT_TRUE(db2.Load(GetDB2Path(DB2Metadata::ItemSparse).c_str(), &DB2Metadata::ItemSparse)) << db2.GetError();

    // column 5: Display_lang, the item's name
    std::unordered_map<uint32, uint32> index = IndexById(db2);
    ASSERT_TRUE(index.contains(25));
    EXPECT_STREQ(db2.getRecord(index[25]).getString(5), "Worn Shortsword");
}

// a record from one of the file's encrypted sections, extracted with its key
TEST_F(DB2FileLoaderTest, EncryptedSection)
{
    DB2FileLoader db2;
    ASSERT_TRUE(db2.Load(GetDB2Path(DB2Metadata::Spell).c_str(), &DB2Metadata::Spell)) << db2.GetError();

    // column 1: Description_lang
    std::unordered_map<uint32, uint32> index = IndexById(db2);
    ASSERT_TRUE(index.contains(423330));
    EXPECT_STREQ(db2.getRecord(index[423330]).getString(1), "Restores $s1 health.");
}

// SpellEffect keeps its SpellID in the file's relationship data
TEST_F(DB2FileLoaderTest, ParentIds)
{
    DB2FileLoader spellNames;
    ASSERT_TRUE(spellNames.Load(GetDB2Path(DB2Metadata::SpellName).c_str(), &DB2Metadata::SpellName))
        << spellNames.GetError();

    std::unordered_map<uint32, uint32> names = IndexById(spellNames);
    ASSERT_TRUE(names.contains(133));
    EXPECT_STREQ(spellNames.getRecord(names[133]).getString(0), "Fireball");

    DB2FileLoader effects;
    ASSERT_TRUE(effects.Load(GetDB2Path(DB2Metadata::SpellEffect).c_str(), &DB2Metadata::SpellEffect))
        << effects.GetError();

    // This build's SpellName has 27272 spells, and SpellEffect also holds effects of spells it
    // does not name (early ranks among them, e.g. 143), so a parent need not be in SpellName.
    uint32 fireballEffects = 0;
    uint32 named = 0;
    for (uint32 i = 0; i < effects.GetNumRows(); ++i)
    {
        uint32 spellId = effects.getRecord(i).getParentId();
        EXPECT_NE(spellId, 0u) << "effect " << effects.getRecord(i).getId() << " without a spell";
        EXPECT_LE(spellId, spellNames.GetMaxId()) << "effect " << effects.getRecord(i).getId();
        if (names.contains(spellId))
            ++named;
        if (spellId == 133)
            ++fireballEffects;
    }

    EXPECT_GT(fireballEffects, 0u);
    EXPECT_GT(named, effects.GetNumRows() / 2);
}
