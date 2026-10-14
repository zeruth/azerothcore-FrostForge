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

#include "DBCStores.h"
#include "BattlegroundMgr.h"
#include "DB2FileLoader.h"
#include "DBCFileLoader.h"
#include "DBCSources.h"
#include "DBCfmt.h"
#include "Errors.h"
#include "LFGMgr.h"
#include "Log.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "TransportMgr.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

typedef std::tuple<int16, int8, int32> WMOAreaTableKey;
typedef std::map<WMOAreaTableKey, WMOAreaTableEntry const*> WMOAreaInfoByTripple;

DBCStorage <AreaTableEntry> sAreaTableStore(AreaTableEntryfmt);
DBCStorage <AreaGroupEntry> sAreaGroupStore(AreaGroupEntryfmt);
DBCStorage <AreaPOIEntry> sAreaPOIStore(AreaPOIEntryfmt);

static WMOAreaInfoByTripple sWMOAreaInfoByTripple;

DBCStorage <AchievementEntry> sAchievementStore(Achievementfmt);
DBCStorage <AchievementCategoryEntry> sAchievementCategoryStore(AchievementCategoryfmt);
DBCStorage <AchievementCriteriaEntry> sAchievementCriteriaStore(AchievementCriteriafmt);
DBCStorage <AuctionHouseEntry> sAuctionHouseStore(AuctionHouseEntryfmt);
DBCStorage <BankBagSlotPricesEntry> sBankBagSlotPricesStore(BankBagSlotPricesEntryfmt);
DBCStorage <BattlemasterListEntry> sBattlemasterListStore(BattlemasterListEntryfmt);
DBCStorage <BarberShopStyleEntry> sBarberShopStyleStore(BarberShopStyleEntryfmt);
DBCStorage <CharStartOutfitEntry> sCharStartOutfitStore(CharStartOutfitEntryfmt);
std::map<uint32, CharStartOutfitEntry const*> sCharStartOutfitMap;

DBCStorage <CharSectionsEntry> sCharSectionsStore(CharSectionsEntryfmt);

DBCStorage <CharTitlesEntry> sCharTitlesStore(CharTitlesEntryfmt);
DBCStorage <ChatChannelsEntry> sChatChannelsStore(ChatChannelsEntryfmt);
DBCStorage <ChrClassesEntry> sChrClassesStore(ChrClassesEntryfmt);
DBCStorage <ChrRacesEntry> sChrRacesStore(ChrRacesEntryfmt);
DBCStorage <CinematicCameraEntry> sCinematicCameraStore(CinematicCameraEntryfmt);
DBCStorage <CinematicSequencesEntry> sCinematicSequencesStore(CinematicSequencesEntryfmt);
DBCStorage <CreatureDisplayInfoEntry> sCreatureDisplayInfoStore(CreatureDisplayInfofmt);
DBCStorage <CreatureDisplayInfoExtraEntry> sCreatureDisplayInfoExtraStore(CreatureDisplayInfoExtrafmt);
DBCStorage <CreatureFamilyEntry> sCreatureFamilyStore(CreatureFamilyfmt);
DBCStorage <CreatureModelDataEntry> sCreatureModelDataStore(CreatureModelDatafmt);
DBCStorage <CreatureSpellDataEntry> sCreatureSpellDataStore(CreatureSpellDatafmt);
DBCStorage <CreatureTypeEntry> sCreatureTypeStore(CreatureTypefmt);
DBCStorage <CurrencyTypesEntry> sCurrencyTypesStore(CurrencyTypesfmt);

DBCStorage <DestructibleModelDataEntry> sDestructibleModelDataStore(DestructibleModelDatafmt);
DBCStorage <DungeonEncounterEntry> sDungeonEncounterStore(DungeonEncounterfmt);
DBCStorage <DurabilityQualityEntry> sDurabilityQualityStore(DurabilityQualityfmt);
DBCStorage <DurabilityCostsEntry> sDurabilityCostsStore(DurabilityCostsfmt);

DBCStorage <EmotesEntry> sEmotesStore(EmotesEntryfmt);
DBCStorage <EmotesTextEntry> sEmotesTextStore(EmotesTextEntryfmt);

typedef std::tuple<uint32, uint32, uint32> EmotesTextSoundKey;
static std::map<EmotesTextSoundKey, EmotesTextSoundEntry const*> sEmotesTextSoundMap;
DBCStorage <EmotesTextSoundEntry> sEmotesTextSoundStore(EmotesTextSoundEntryfmt);

typedef std::map<uint32, SimpleFactionsList> FactionTeamMap;
static FactionTeamMap sFactionTeamMap;
DBCStorage <FactionEntry> sFactionStore(FactionEntryfmt);
DBCStorage <FactionTemplateEntry> sFactionTemplateStore(FactionTemplateEntryfmt);

DBCStorage <GameObjectArtKitEntry> sGameObjectArtKitStore(GameObjectArtKitfmt);

DBCStorage <GameObjectDisplayInfoEntry> sGameObjectDisplayInfoStore(GameObjectDisplayInfofmt);
DBCStorage <GemPropertiesEntry> sGemPropertiesStore(GemPropertiesEntryfmt);
DBCStorage <GlyphPropertiesEntry> sGlyphPropertiesStore(GlyphPropertiesfmt);
DBCStorage <GlyphSlotEntry> sGlyphSlotStore(GlyphSlotfmt);

DBCStorage <GtBarberShopCostBaseEntry>    sGtBarberShopCostBaseStore(GtBarberShopCostBasefmt);
DBCStorage <GtCombatRatingsEntry>         sGtCombatRatingsStore(GtCombatRatingsfmt);
DBCStorage <GtChanceToMeleeCritBaseEntry> sGtChanceToMeleeCritBaseStore(GtChanceToMeleeCritBasefmt);
DBCStorage <GtChanceToMeleeCritEntry>     sGtChanceToMeleeCritStore(GtChanceToMeleeCritfmt);
DBCStorage <GtChanceToSpellCritBaseEntry> sGtChanceToSpellCritBaseStore(GtChanceToSpellCritBasefmt);
DBCStorage <GtChanceToSpellCritEntry>     sGtChanceToSpellCritStore(GtChanceToSpellCritfmt);
DBCStorage <GtNPCManaCostScalerEntry>     sGtNPCManaCostScalerStore(GtNPCManaCostScalerfmt);
DBCStorage <GtOCTClassCombatRatingScalarEntry> sGtOCTClassCombatRatingScalarStore(GtOCTClassCombatRatingScalarfmt);
DBCStorage <GtOCTRegenHPEntry>            sGtOCTRegenHPStore(GtOCTRegenHPfmt);
//DBCStorage <GtOCTRegenMPEntry>            sGtOCTRegenMPStore(GtOCTRegenMPfmt);  -- not used currently
DBCStorage <GtRegenHPPerSptEntry>         sGtRegenHPPerSptStore(GtRegenHPPerSptfmt);
DBCStorage <GtRegenMPPerSptEntry>         sGtRegenMPPerSptStore(GtRegenMPPerSptfmt);

DBCStorage <HolidaysEntry>                sHolidaysStore(Holidaysfmt);

DBCStorage <ItemEntry>                    sItemStore(Itemfmt);
DBCStorage <ItemBagFamilyEntry>           sItemBagFamilyStore(ItemBagFamilyfmt);
//DBCStorage <ItemCondExtCostsEntry> sItemCondExtCostsStore(ItemCondExtCostsEntryfmt);
DBCStorage <ItemDisplayInfoEntry> sItemDisplayInfoStore(ItemDisplayTemplateEntryfmt);
DBCStorage <ItemExtendedCostEntry> sItemExtendedCostStore(ItemExtendedCostEntryfmt);
DBCStorage <ItemLimitCategoryEntry> sItemLimitCategoryStore(ItemLimitCategoryEntryfmt);
DBCStorage <ItemRandomPropertiesEntry> sItemRandomPropertiesStore(ItemRandomPropertiesfmt);
DBCStorage <ItemRandomSuffixEntry> sItemRandomSuffixStore(ItemRandomSuffixfmt);
DBCStorage <ItemSetEntry> sItemSetStore(ItemSetEntryfmt);

DBCStorage <LFGDungeonEntry> sLFGDungeonStore(LFGDungeonEntryfmt);
DBCStorage <LightEntry> sLightStore(LightEntryfmt);
DBCStorage <LiquidTypeEntry> sLiquidTypeStore(LiquidTypefmt);
DBCStorage <LockEntry> sLockStore(LockEntryfmt);

DBCStorage <MailTemplateEntry> sMailTemplateStore(MailTemplateEntryfmt);
DBCStorage <MapEntry> sMapStore(MapEntryfmt);

// DBC used only for initialization sMapDifficultyMap at startup.
DBCStorage <MapDifficultyEntry> sMapDifficultyStore(MapDifficultyEntryfmt); // only for loading
MapDifficultyMap sMapDifficultyMap;

DBCStorage <MovieEntry> sMovieStore(MovieEntryfmt);

DBCStorage <NamesReservedEntry> sNamesReservedStore(NamesReservedfmt);
DBCStorage <NamesProfanityEntry> sNamesProfanityStore(NamesProfanityfmt);

DBCStorage <OverrideSpellDataEntry> sOverrideSpellDataStore(OverrideSpellDatafmt);

DBCStorage <PowerDisplayEntry> sPowerDisplayStore(PowerDisplayfmt);
DBCStorage <PvPDifficultyEntry> sPvPDifficultyStore(PvPDifficultyfmt);

DBCStorage <QuestSortEntry> sQuestSortStore(QuestSortEntryfmt);
DBCStorage <QuestXPEntry>   sQuestXPStore(QuestXPfmt);
DBCStorage <QuestFactionRewEntry>  sQuestFactionRewardStore(QuestFactionRewardfmt);
DBCStorage <RandomPropertiesPointsEntry> sRandomPropertiesPointsStore(RandomPropertiesPointsfmt);
DBCStorage <ScalingStatDistributionEntry> sScalingStatDistributionStore(ScalingStatDistributionfmt);
DBCStorage <ScalingStatValuesEntry> sScalingStatValuesStore(ScalingStatValuesfmt);

DBCStorage <SkillLineEntry> sSkillLineStore(SkillLinefmt);
DBCStorage <SkillLineAbilityEntry> sSkillLineAbilityStore(SkillLineAbilityfmt);
SkillLineAbilityIndexBySkillLine sSkillLineAbilityIndexBySkillLine;
DBCStorage <SkillRaceClassInfoEntry> sSkillRaceClassInfoStore(SkillRaceClassInfofmt);
SkillRaceClassInfoMap SkillRaceClassInfoBySkill;
DBCStorage <SkillTiersEntry> sSkillTiersStore(SkillTiersfmt);

DBCStorage <SoundEntriesEntry> sSoundEntriesStore(SoundEntriesfmt);

DBCStorage <SpellItemEnchantmentEntry> sSpellItemEnchantmentStore(SpellItemEnchantmentfmt);
DBCStorage <SpellItemEnchantmentConditionEntry> sSpellItemEnchantmentConditionStore(SpellItemEnchantmentConditionfmt);
DBCStorage <SpellEntry> sSpellStore(SpellEntryfmt);
SpellCategoryStore sSpellsByCategoryStore;
PetFamilySpellsStore sPetFamilySpellsStore;

DBCStorage <SpellCastTimesEntry> sSpellCastTimesStore(SpellCastTimefmt);
DBCStorage <SpellCategoryEntry> sSpellCategoryStore(SpellCategoryfmt);
DBCStorage <SpellDifficultyEntry> sSpellDifficultyStore(SpellDifficultyfmt);
DBCStorage <SpellDurationEntry> sSpellDurationStore(SpellDurationfmt);
DBCStorage <SpellFocusObjectEntry> sSpellFocusObjectStore(SpellFocusObjectfmt);
DBCStorage <SpellRadiusEntry> sSpellRadiusStore(SpellRadiusfmt);
DBCStorage <SpellRangeEntry> sSpellRangeStore(SpellRangefmt);
DBCStorage <SpellRuneCostEntry> sSpellRuneCostStore(SpellRuneCostfmt);
DBCStorage <SpellShapeshiftFormEntry> sSpellShapeshiftFormStore(SpellShapeshiftFormEntryfmt);
DBCStorage <SpellVisualEntry> sSpellVisualStore(SpellVisualfmt);
DBCStorage <StableSlotPricesEntry> sStableSlotPricesStore(StableSlotPricesfmt);
DBCStorage <SummonPropertiesEntry> sSummonPropertiesStore(SummonPropertiesfmt);
DBCStorage <TalentEntry> sTalentStore(TalentEntryfmt);
TalentSpellPosMap sTalentSpellPosMap;
std::unordered_set<uint32> sPetTalentSpells;
DBCStorage <TalentTabEntry> sTalentTabStore(TalentTabEntryfmt);

// store absolute bit position for first rank for talent inspect
static uint32 sTalentTabPages[MAX_CLASSES][3];

DBCStorage <TaxiNodesEntry> sTaxiNodesStore(TaxiNodesEntryfmt);
TaxiMask sTaxiNodesMask;
TaxiMask sOldContinentsNodesMask;
TaxiMask sHordeTaxiNodesMask;
TaxiMask sAllianceTaxiNodesMask;
TaxiMask sDeathKnightTaxiNodesMask;

// DBC used only for initialization sTaxiPathSetBySource at startup.
TaxiPathSetBySource sTaxiPathSetBySource;
DBCStorage <TaxiPathEntry> sTaxiPathStore(TaxiPathEntryfmt);

// DBC used only for initialization sTaxiPathNodeStore at startup.
TaxiPathNodesByPath sTaxiPathNodesByPath;
static DBCStorage <TaxiPathNodeEntry> sTaxiPathNodeStore(TaxiPathNodeEntryfmt);

DBCStorage <TeamContributionPointsEntry> sTeamContributionPointsStore(TeamContributionPointsfmt);
DBCStorage <TotemCategoryEntry> sTotemCategoryStore(TotemCategoryEntryfmt);
DBCStorage <TransportAnimationEntry> sTransportAnimationStore(TransportAnimationfmt);
DBCStorage <TransportRotationEntry> sTransportRotationStore(TransportRotationfmt);
DBCStorage <VehicleEntry> sVehicleStore(VehicleEntryfmt);
DBCStorage <VehicleSeatEntry> sVehicleSeatStore(VehicleSeatEntryfmt);
DBCStorage <WMOAreaTableEntry> sWMOAreaTableStore(WMOAreaTableEntryfmt);
DBCStorage <WorldMapAreaEntry> sWorldMapAreaStore(WorldMapAreaEntryfmt);
DBCStorage <WorldMapOverlayEntry> sWorldMapOverlayStore(WorldMapOverlayEntryfmt);

typedef std::list<std::string> StoreProblemList;

uint32 DBCFileCount = 0;

static bool LoadDBC_assert_print(uint32 fsize, uint32 rsize, std::string const& filename)
{
    LOG_ERROR("dbc", "Size of '{}' set by format string ({}) not equal size of C++ structure ({}).", filename, fsize, rsize);

    // ASSERT must fail after function call
    return false;
}

template<class T>
inline void LoadDBC(uint32& availableDbcLocales, StoreProblemList& errors, DBCStorage<T>& storage, std::string const& dbcPath, std::string const& filename, char const* dbTable = nullptr)
{
    // compatibility format and C++ structure sizes
    ASSERT(DBCFileLoader::GetFormatRecordSize(storage.GetFormat()) == sizeof(T) || LoadDBC_assert_print(DBCFileLoader::GetFormatRecordSize(storage.GetFormat()), sizeof(T), filename));

    ++DBCFileCount;
    std::string dbcFilename = dbcPath + filename;
    bool existDBData = false;

    if (storage.Load(dbcFilename.c_str()))
    {
        for (uint8 i = 0; i < TOTAL_LOCALES; ++i)
        {
            if (!(availableDbcLocales & (1 << i)))
                continue;

            std::string localizedName(dbcPath);
            localizedName.append(localeNames[i]);
            localizedName.push_back('/');
            localizedName.append(filename);

            if (!storage.LoadStringsFrom(localizedName.c_str()))
                availableDbcLocales &= ~(1 << i);             // mark as not available for speedup next checks
        }
    }

    if (dbTable)
        storage.LoadFromDB(dbTable, storage.GetFormat());

    if (storage.GetNumRows())
        existDBData = true;

    if (!existDBData)
    {
        // sort problematic dbc to (1) non compatible and (2) non-existed
        if (FILE* f = fopen(dbcFilename.c_str(), "rb"))
        {
            std::ostringstream stream;
            stream << dbcFilename << " exists, and has " << storage.GetFieldCount() << " field(s) (expected " << strlen(storage.GetFormat()) << "). Extracted file might be from wrong client version or a database-update has been forgotten.";
            std::string buf = stream.str();
            errors.push_back(buf);
            fclose(f);
        }
        else
            errors.push_back(dbcFilename);
    }
}

/// Loads a store from its 3.4.3.54261 client table through the store's sources (DBCSources.h),
/// converts what the client table keeps in another form (fixup), then applies the world
/// database's overrides as LoadDBC does. The overrides are already in AC's form, so the fixup
/// runs before them.
template<class T>
inline void LoadDB2(StoreProblemList& errors, DBCStorage<T>& storage, std::string const& dbcPath, DBCSourceInfo const& sources,
    char const* dbTable = nullptr, std::type_identity_t<std::function<void(DBCStorage<T>&)>> const& fixup = nullptr)
{
    ASSERT(DBCFileLoader::GetFormatRecordSize(storage.GetFormat()) == sizeof(T) || LoadDBC_assert_print(DBCFileLoader::GetFormatRecordSize(storage.GetFormat()), sizeof(T), sources.Meta->Name));

    ++DBCFileCount;
    std::string db2Filename = dbcPath + sources.Meta->Name + ".db2";
    std::string error;
    if (!storage.LoadDB2(db2Filename.c_str(), sources, error))
        errors.push_back(error);
    else if (fixup)
        fixup(storage);

    if (dbTable)
        storage.LoadFromDB(dbTable, storage.GetFormat());

    if (!storage.GetNumRows() && error.empty())
        errors.push_back(db2Filename + " holds no records");
}

/// Loads a store from the client's GameTable (GameTables/<name>.txt, extracted beside the client
/// tables), then the world database's overrides as LoadDBC does.
template<class T>
inline void LoadGameTable(StoreProblemList& errors, DBCStorage<T>& storage, std::string const& gtPath, char const* name, char const* dbTable)
{
    ASSERT(DBCFileLoader::GetFormatRecordSize(storage.GetFormat()) == sizeof(T) || LoadDBC_assert_print(DBCFileLoader::GetFormatRecordSize(storage.GetFormat()), sizeof(T), name));

    ++DBCFileCount;
    std::string error;
    if (!storage.LoadGameTable((gtPath + name + ".txt").c_str(), error))
        errors.push_back(error);

    if (dbTable)
        storage.LoadFromDB(dbTable, storage.GetFormat());
}

// Fixups: values a 54261 table keeps in another form than the store's. Each is named in
// apps/db2meta/dbcsources.json beside the evidence for it.
namespace
{
    /// Reads one client table outside the stores, for a fixup that needs columns no store holds.
    std::unordered_map<uint32, DB2FileLoader::Record> ReadDB2(DB2FileLoader& db2, std::string const& dbcPath, DB2Meta const& meta, StoreProblemList& errors)
    {
        std::unordered_map<uint32, DB2FileLoader::Record> records;
        if (!db2.Load((dbcPath + meta.Name + ".db2").c_str(), &meta))
        {
            errors.push_back(db2.GetError());
            return records;
        }

        for (uint32 i = 0; i < db2.GetNumRows(); ++i)
            records.emplace(db2.getRecord(i).getId(), db2.getRecord(i));

        return records;
    }

    /// A Difficulty.db2 id as AC's difficulty index. 3.3.5 numbered each map's own modes from 0
    /// (a 25-player-only raid's one mode is 0), so the index is the rank of the mode's old enum
    /// value (Difficulty column 3: 1 Normal and 3 10 Player -> 0, 2 Heroic and 4 25 Player -> 1,
    /// 5 -> 2, 6 -> 3; none -> 0) among the modes MapDifficulty gives the entry's map (parent
    /// the map, column 3 the DifficultyID). Matches 3.3.5's MapDifficulty on 147 of 149 rows
    /// (the two others are battlegrounds). A map without MapDifficulty rows keeps the old enum.
    template<class T, class M, class N>
    std::function<void(DBCStorage<T>&)> DifficultyIndex(std::string const& dbcPath, StoreProblemList& errors, M T::* member, N T::* map)
    {
        return [&dbcPath, &errors, member, map](DBCStorage<T>& storage)
        {
            DB2FileLoader db2;
            std::unordered_map<uint32, DB2FileLoader::Record> difficulties = ReadDB2(db2, dbcPath, DB2Metadata::Difficulty, errors);
            auto oldEnum = [&](uint32 difficultyId) -> int32
            {
                auto itr = difficulties.find(difficultyId);
                int32 value = itr != difficulties.end() ? itr->second.getInt(3) : 0;
                return value < 0 ? 0 : value;
            };

            DB2FileLoader mapDb2;
            std::unordered_map<uint32, DB2FileLoader::Record> mapDifficulties = ReadDB2(mapDb2, dbcPath, DB2Metadata::MapDifficulty, errors);
            std::unordered_map<uint32, std::vector<int32>> modes;
            for (auto const& [id, record] : mapDifficulties)
                modes[record.getParentId()].push_back(oldEnum(record.getUInt(3)));

            for (auto& [mapId, values] : modes)
            {
                std::sort(values.begin(), values.end());
                values.erase(std::unique(values.begin(), values.end()), values.end());
            }

            for (T const* entry : storage)
            {
                int32 value = oldEnum(uint32(entry->*member));
                auto itr = modes.find(uint32(entry->*map));
                if (itr != modes.end())
                {
                    auto rank = std::find(itr->second.begin(), itr->second.end(), value);
                    if (rank != itr->second.end())
                        value = int32(rank - itr->second.begin());
                }

                const_cast<T*>(entry)->*member = M(value);
            }
        };
    }

    /// MapDifficulty's reset interval (0 none, 1 daily, 2 weekly) as the seconds AC keeps.
    void ResetIntervalSeconds(DBCStorage<MapDifficultyEntry>& storage)
    {
        for (MapDifficultyEntry const* entry : storage)
        {
            uint32& resetTime = const_cast<MapDifficultyEntry*>(entry)->resetTime;
            resetTime = resetTime == 1 ? DAY : resetTime == 2 ? WEEK : 0;
        }
    }

    /// Honor and arena points are currencies in 54261 (1901 and 1900): ItemExtendedCost keeps
    /// up to five currency ids (column 8) and counts (column 9).
    std::function<void(DBCStorage<ItemExtendedCostEntry>&)> CurrencyCosts(std::string const& dbcPath, StoreProblemList& errors)
    {
        return [&dbcPath, &errors](DBCStorage<ItemExtendedCostEntry>& storage)
        {
            constexpr uint32 CURRENCY_ARENA_POINTS = 1900;
            constexpr uint32 CURRENCY_HONOR_POINTS = 1901;

            DB2FileLoader db2;
            std::unordered_map<uint32, DB2FileLoader::Record> costs = ReadDB2(db2, dbcPath, DB2Metadata::ItemExtendedCost, errors);
            for (ItemExtendedCostEntry const* entry : storage)
            {
                ItemExtendedCostEntry* cost = const_cast<ItemExtendedCostEntry*>(entry);
                cost->reqhonorpoints = 0;
                cost->reqarenapoints = 0;
                auto itr = costs.find(entry->ID);
                if (itr == costs.end())
                    continue;

                for (uint32 i = 0; i < 5; ++i)
                {
                    uint32 currency = itr->second.getUInt(8, i);
                    if (currency == CURRENCY_HONOR_POINTS)
                        cost->reqhonorpoints += itr->second.getUInt(9, i);
                    else if (currency == CURRENCY_ARENA_POINTS)
                        cost->reqarenapoints += itr->second.getUInt(9, i);
                }
            }
        };
    }

    /// An item's display is no longer an Item column: its base appearance (ItemModifiedAppearance
    /// with the lowest appearance modifier, then order, then row id; column 1 the item, 2 the
    /// modifier, 3 the ItemAppearance, 4 the order) names an ItemAppearance, whose column 1 is
    /// the ItemDisplayInfo id.
    std::function<void(DBCStorage<ItemEntry>&)> ItemAppearanceDisplay(std::string const& dbcPath, StoreProblemList& errors)
    {
        return [&dbcPath, &errors](DBCStorage<ItemEntry>& storage)
        {
            DB2FileLoader appearanceDb2;
            std::unordered_map<uint32, DB2FileLoader::Record> appearances = ReadDB2(appearanceDb2, dbcPath, DB2Metadata::ItemAppearance, errors);

            DB2FileLoader modified;
            if (!modified.Load((dbcPath + DB2Metadata::ItemModifiedAppearance.Name + ".db2").c_str(), &DB2Metadata::ItemModifiedAppearance))
            {
                errors.push_back(modified.GetError());
                return;
            }

            // per item: (modifier, order, row id) of its base appearance, and that appearance
            std::unordered_map<uint32, std::pair<std::tuple<uint32, uint32, uint32>, uint32>> base;
            for (uint32 i = 0; i < modified.GetNumRows(); ++i)
            {
                DB2FileLoader::Record record = modified.getRecord(i);
                std::tuple<uint32, uint32, uint32> key(record.getUInt(2), record.getUInt(4), record.getId());
                auto [itr, inserted] = base.try_emplace(record.getUInt(1), key, record.getUInt(3));
                if (!inserted && key < itr->second.first)
                    itr->second = { key, record.getUInt(3) };
            }

            for (ItemEntry const* entry : storage)
            {
                uint32 display = 0;
                if (auto itr = base.find(entry->ID); itr != base.end())
                    if (auto appearance = appearances.find(itr->second.second); appearance != appearances.end())
                        display = appearance->second.getUInt(1);

                const_cast<ItemEntry*>(entry)->DisplayInfoID = display;
            }
        };
    }

    /// The set bonuses are ItemSetSpell rows in 54261 (parent the set, column 1 the spell,
    /// column 2 the number of items), in the order 3.3.5 kept them.
    std::function<void(DBCStorage<ItemSetEntry>&)> ItemSetSpells(std::string const& dbcPath, StoreProblemList& errors)
    {
        return [&dbcPath, &errors](DBCStorage<ItemSetEntry>& storage)
        {
            DB2FileLoader db2;
            if (!db2.Load((dbcPath + DB2Metadata::ItemSetSpell.Name + ".db2").c_str(), &DB2Metadata::ItemSetSpell))
            {
                errors.push_back(db2.GetError());
                return;
            }

            std::vector<std::pair<uint32, uint32>> rows; // (id, row)
            for (uint32 i = 0; i < db2.GetNumRows(); ++i)
                rows.emplace_back(db2.getRecord(i).getId(), i);
            std::sort(rows.begin(), rows.end());

            std::unordered_map<uint32, uint32> filled;
            for (auto const& [id, row] : rows)
            {
                DB2FileLoader::Record record = db2.getRecord(row);
                ItemSetEntry const* set = storage.LookupEntry(record.getParentId());
                if (!set)
                    continue;

                uint32& slot = filled[record.getParentId()];
                if (slot >= MAX_ITEM_SET_SPELLS)
                {
                    LOG_ERROR("dbc", "ItemSetSpell: set {} has more than {} bonuses, {} dropped", record.getParentId(), MAX_ITEM_SET_SPELLS, id);
                    continue;
                }

                ItemSetEntry* entry = const_cast<ItemSetEntry*>(set);
                entry->spells[slot] = record.getUInt(1);
                entry->items_to_triggerspell[slot] = record.getUInt(2);
                ++slot;
            }
        };
    }

    /// A spell's costs are SpellPower rows in 54261, one per cost (column 1 the order, 10 the power
    /// type, 2 the amount, 3 per level, 4 per second, 7 the percentage as a float). Rune costs are
    /// rows of power types 20, 22 and 21 (AC's rune slots 0, 1, 2), and a runic power row with a
    /// negative amount is the runic power the spell gives; they make the spell's SpellRuneCost
    /// entry, under its own id, as 3.3.5 kept them in SpellRuneCost.dbc. The first other row is
    /// AC's power and cost.
    std::function<void(DBCStorage<SpellEntry>&)> SpellPowerCosts(std::string const& dbcPath, StoreProblemList& errors)
    {
        return [&dbcPath, &errors](DBCStorage<SpellEntry>& storage)
        {
            constexpr int32 POWER_RUNE_BLOOD = 20;
            constexpr int32 POWER_RUNE_FROST = 21;
            constexpr int32 POWER_RUNE_UNHOLY = 22;

            DB2FileLoader db2;
            if (!db2.Load((dbcPath + DB2Metadata::SpellPower.Name + ".db2").c_str(), &DB2Metadata::SpellPower))
            {
                errors.push_back(db2.GetError());
                return;
            }

            // per spell: its rows by (order, row id)
            std::unordered_map<uint32, std::vector<std::tuple<uint32, uint32, uint32>>> costs;
            for (uint32 i = 0; i < db2.GetNumRows(); ++i)
            {
                DB2FileLoader::Record record = db2.getRecord(i);
                costs[record.getParentId()].emplace_back(record.getUInt(1), record.getId(), i);
            }

            for (SpellEntry const* spell : storage)
            {
                SpellEntry* entry = const_cast<SpellEntry*>(spell);
                entry->PowerType = POWER_MANA;
                entry->ManaCost = 0;
                entry->ManaCostPerlevel = 0;
                entry->ManaPerSecond = 0;
                entry->ManaCostPercentage = 0;
                entry->RuneCostID = 0;

                auto itr = costs.find(entry->Id);
                if (itr == costs.end())
                    continue;

                std::sort(itr->second.begin(), itr->second.end());
                std::array<uint32, 3> runes = {};
                uint32 runicPowerGain = 0;
                bool hasMain = false;
                for (auto const& [order, id, row] : itr->second)
                {
                    DB2FileLoader::Record record = db2.getRecord(row);
                    int32 type = record.getInt(10);
                    int32 amount = record.getInt(2);
                    if (type == POWER_RUNE_BLOOD)
                        runes[0] += amount;
                    else if (type == POWER_RUNE_UNHOLY)
                        runes[1] += amount;
                    else if (type == POWER_RUNE_FROST)
                        runes[2] += amount;
                    else if (type == POWER_RUNIC_POWER && amount < 0)
                        runicPowerGain += uint32(-amount);
                    else if (!hasMain)
                    {
                        hasMain = true;
                        entry->PowerType = uint32(type);
                        entry->ManaCost = uint32(amount);
                        entry->ManaCostPerlevel = record.getUInt(3);
                        entry->ManaPerSecond = record.getUInt(4);
                        entry->ManaCostPercentage = uint32(record.getFloat(7));
                    }
                }

                if (runes == std::array<uint32, 3>{} && !runicPowerGain)
                    continue;

                if (!hasMain)
                    entry->PowerType = POWER_RUNE;

                SpellRuneCostEntry* runeCost = new SpellRuneCostEntry();
                runeCost->ID = entry->Id;
                for (uint32 slot = 0; slot < 3; ++slot)
                    runeCost->RuneCost[slot] = runes[slot];
                runeCost->runePowerGain = runicPowerGain;
                sSpellRuneCostStore.SetEntry(entry->Id, runeCost);
                entry->RuneCostID = entry->Id;
            }
        };
    }

    /// AreaGroup is AreaGroupMember rows in 54261 (parent the group, column 0 an area). AC's
    /// entry holds six areas and the id of the group that continues it, so a larger group is
    /// chained through entries numbered after the highest group id.
    void LoadAreaGroups(std::string const& dbcPath, StoreProblemList& errors)
    {
        DB2FileLoader db2;
        if (!db2.Load((dbcPath + DB2Metadata::AreaGroupMember.Name + ".db2").c_str(), &DB2Metadata::AreaGroupMember))
        {
            errors.push_back(db2.GetError());
            return;
        }

        std::map<uint32, std::vector<uint32>> groups;
        std::vector<std::pair<uint32, uint32>> rows; // (row id, row)
        for (uint32 i = 0; i < db2.GetNumRows(); ++i)
            rows.emplace_back(db2.getRecord(i).getId(), i);
        std::sort(rows.begin(), rows.end());
        for (auto const& [id, row] : rows)
            groups[db2.getRecord(row).getParentId()].push_back(db2.getRecord(row).getUInt(0));

        uint32 nextId = groups.empty() ? 1 : groups.rbegin()->first + 1;
        for (auto const& [groupId, areas] : groups)
        {
            uint32 id = groupId;
            for (std::size_t first = 0; first < areas.size(); first += MAX_GROUP_AREA_IDS)
            {
                AreaGroupEntry* entry = new AreaGroupEntry();
                entry->AreaGroupId = id;
                for (std::size_t i = 0; i < MAX_GROUP_AREA_IDS && first + i < areas.size(); ++i)
                    entry->AreaId[i] = areas[first + i];

                entry->nextGroup = first + MAX_GROUP_AREA_IDS < areas.size() ? nextId : 0;
                sAreaGroupStore.SetEntry(id, entry);
                id = entry->nextGroup;
                if (id)
                    ++nextId;
            }
        }
    }

    /// WorldMapArea is the UI map tables in 54261. A zone's UiMapAssignment (column 7 the area,
    /// 6 the map, 5 the order, 2 the region: min x, y, z, then max x, y, z) gives its map and its
    /// corners (AC's x1/y1 are the maxima); its virtual map is the map of the continent its UiMap
    /// sits under (UiMap column 2 the parent; a continent's own assignment has area 0), -1 when
    /// that is its own map. Checked against 3.3.5: map and corners agree on all 86 zones still
    /// assigned, the virtual map on every zone of maps 530 and 571 (the only ones AC reads it for).
    void LoadWorldMapAreas(std::string const& dbcPath, StoreProblemList& errors)
    {
        DB2FileLoader uiMapDb2;
        std::unordered_map<uint32, DB2FileLoader::Record> uiMaps = ReadDB2(uiMapDb2, dbcPath, DB2Metadata::UiMap, errors);

        DB2FileLoader db2;
        if (!db2.Load((dbcPath + DB2Metadata::UiMapAssignment.Name + ".db2").c_str(), &DB2Metadata::UiMapAssignment))
        {
            errors.push_back(db2.GetError());
            return;
        }

        std::vector<std::pair<uint32, uint32>> rows; // (row id, row)
        for (uint32 i = 0; i < db2.GetNumRows(); ++i)
            rows.emplace_back(db2.getRecord(i).getId(), i);
        std::sort(rows.begin(), rows.end());

        std::unordered_map<uint32, uint32> continentMap;   // UiMap -> its own map
        std::unordered_map<uint32, uint32> zoneRow;        // area -> its first order-0 assignment
        for (auto const& [id, row] : rows)
        {
            DB2FileLoader::Record record = db2.getRecord(row);
            if (!record.getUInt(7))
                continentMap.try_emplace(record.getParentId(), record.getUInt(6));
            else if (!record.getUInt(5))
                zoneRow.try_emplace(record.getUInt(7), row);
        }

        for (auto const& [area, row] : zoneRow)
        {
            DB2FileLoader::Record record = db2.getRecord(row);
            WorldMapAreaEntry* entry = new WorldMapAreaEntry();
            entry->map_id = record.getUInt(6);
            entry->area_id = area;
            entry->x1 = record.getFloat(2, 3);
            entry->x2 = record.getFloat(2, 0);
            entry->y1 = record.getFloat(2, 4);
            entry->y2 = record.getFloat(2, 1);
            entry->virtual_map_id = -1;

            // up the UI map tree to the first continent
            uint32 uiMap = record.getParentId();
            for (uint32 depth = 0; depth < 16; ++depth)
            {
                auto parent = uiMaps.find(uiMap);
                if (parent == uiMaps.end() || !(uiMap = parent->second.getUInt(2)))
                    break;

                if (auto continent = continentMap.find(uiMap); continent != continentMap.end())
                {
                    if (continent->second != entry->map_id)
                        entry->virtual_map_id = int32(continent->second);
                    break;
                }
            }

            sWorldMapAreaStore.SetEntry(area, entry);
        }
    }

    /// A criteria's achievement, count and flags are CriteriaTree's in 54261: a node (column 4 the
    /// criteria, 1 the parent node, 2 the amount, 6 the flags) under a tree whose root an
    /// achievement names (Achievement column 13). A criteria can sit under several achievements'
    /// trees; AC's criteria holds one, the first node's (by id) that reaches an achievement.
    std::function<void(DBCStorage<AchievementCriteriaEntry>&)> CriteriaTreeLinks(std::string const& dbcPath, StoreProblemList& errors)
    {
        return [&dbcPath, &errors](DBCStorage<AchievementCriteriaEntry>& storage)
        {
            DB2FileLoader treeDb2;
            std::unordered_map<uint32, DB2FileLoader::Record> tree = ReadDB2(treeDb2, dbcPath, DB2Metadata::CriteriaTree, errors);
            DB2FileLoader achievementDb2;
            std::unordered_map<uint32, DB2FileLoader::Record> achievements = ReadDB2(achievementDb2, dbcPath, DB2Metadata::Achievement, errors);

            std::unordered_map<uint32, uint32> achievementOfRoot;
            for (auto const& [id, record] : achievements)
                achievementOfRoot.try_emplace(record.getUInt(13), id);

            auto achievementOf = [&](uint32 node) -> uint32
            {
                for (uint32 depth = 0; depth < 16 && node; ++depth)
                {
                    if (auto itr = achievementOfRoot.find(node); itr != achievementOfRoot.end())
                        return itr->second;

                    auto itr = tree.find(node);
                    node = itr != tree.end() ? itr->second.getUInt(1) : 0;
                }

                return 0;
            };

            std::vector<uint32> nodes;
            for (auto const& [id, record] : tree)
                if (record.getUInt(4))
                    nodes.push_back(id);
            std::sort(nodes.begin(), nodes.end());

            // per criteria: its first node, and the first node reaching an achievement
            std::unordered_map<uint32, std::pair<uint32, uint32>> links;
            for (uint32 node : nodes)
            {
                uint32 criteria = tree.at(node).getUInt(4);
                auto [itr, inserted] = links.try_emplace(criteria, node, 0);
                if (!itr->second.second)
                    if (uint32 achievement = achievementOf(node))
                        itr->second = { node, achievement };
            }

            for (AchievementCriteriaEntry const* criteria : storage)
            {
                AchievementCriteriaEntry* entry = const_cast<AchievementCriteriaEntry*>(criteria);
                auto itr = links.find(entry->ID);
                if (itr == links.end())
                    continue;

                DB2FileLoader::Record const& node = tree.at(itr->second.first);
                entry->referredAchievement = itr->second.second;
                entry->raw.count = node.getUInt(2);
                entry->flags = node.getUInt(6);
            }
        };
    }

    /// 54261 records which spell a rank supersedes; AC keeps which spell supersedes it.
    void InvertSupersedes(DBCStorage<SkillLineAbilityEntry>& storage)
    {
        std::unordered_map<uint32, uint32> supersededBy;
        for (SkillLineAbilityEntry const* entry : storage)
            if (entry->SupercededBySpell)
                supersededBy[entry->SupercededBySpell] = entry->Spell;

        for (SkillLineAbilityEntry const* entry : storage)
        {
            auto itr = supersededBy.find(entry->Spell);
            const_cast<SkillLineAbilityEntry*>(entry)->SupercededBySpell = itr != supersededBy.end() ? itr->second : 0;
        }
    }
}

void LoadDBCStores(std::string const& dataPath)
{
    uint32 oldMSTime = getMSTime();

    std::string dbcPath = dataPath + "dbc/";
    std::string gtPath = dataPath + "gt/GameTables/";

    StoreProblemList bad_dbc_files;
    uint32 availableDbcLocales = 0xFFFFFFFF;

#define LOAD_DBC(store, file, dbtable) LoadDBC(availableDbcLocales, bad_dbc_files, store, dbcPath, file, dbtable)
#define LOAD_GT(store, name, dbtable) LoadGameTable(bad_dbc_files, store, gtPath, name, dbtable)
#define LOAD_DB2(store, sources, dbtable) LoadDB2(bad_dbc_files, store, dbcPath, sources, dbtable)
#define LOAD_DB2_FIXUP(store, sources, dbtable, fixup) LoadDB2(bad_dbc_files, store, dbcPath, sources, dbtable, fixup)

    LOAD_DB2(sAreaTableStore,                        DBCSources::AreaTableInfo,                              "areatable_dbc");
    LOAD_DB2(sAchievementStore,                      DBCSources::AchievementInfo,                            "achievement_dbc");
    LOAD_DB2(sAchievementCategoryStore,              DBCSources::Achievement_CategoryInfo,                   "achievement_category_dbc");
    LOAD_DB2_FIXUP(sAchievementCriteriaStore,             DBCSources::CriteriaInfo,                               "achievement_criteria_dbc", CriteriaTreeLinks(dbcPath, bad_dbc_files));
    LoadAreaGroups(dbcPath, bad_dbc_files);                                 // AreaGroupMember
    sAreaGroupStore.LoadFromDB("areagroup_dbc", sAreaGroupStore.GetFormat());
    LOAD_DB2(sAreaPOIStore,                          DBCSources::AreaPOIInfo,                                "areapoi_dbc");
    LOAD_DB2(sAuctionHouseStore,                     DBCSources::AuctionHouseInfo,                           "auctionhouse_dbc");
    LOAD_DB2(sBankBagSlotPricesStore,                DBCSources::BankBagSlotPricesInfo,                      "bankbagslotprices_dbc");
    LOAD_DB2(sBattlemasterListStore,                 DBCSources::BattlemasterListInfo,                       "battlemasterlist_dbc");
    LOAD_DB2(sBarberShopStyleStore,                  DBCSources::BarberShopStyleInfo,                        "barbershopstyle_dbc");
    LOAD_DB2(sCharStartOutfitStore,                  DBCSources::CharStartOutfitInfo,                        "charstartoutfit_dbc");
    LOAD_DB2(sCharSectionsStore,                     DBCSources::CharSectionsInfo,                           "charsections_dbc");
    LOAD_DB2(sCharTitlesStore,                       DBCSources::CharTitlesInfo,                             "chartitles_dbc");
    LOAD_DB2(sChatChannelsStore,                     DBCSources::ChatChannelsInfo,                           "chatchannels_dbc");
    LOAD_DB2(sChrClassesStore,                       DBCSources::ChrClassesInfo,                             "chrclasses_dbc");
    LOAD_DB2(sChrRacesStore,                         DBCSources::ChrRacesInfo,                               "chrraces_dbc");
    LOAD_DB2(sCinematicCameraStore,                  DBCSources::CinematicCameraInfo,                        "cinematiccamera_dbc");
    LOAD_DB2(sCinematicSequencesStore,               DBCSources::CinematicSequencesInfo,                     "cinematicsequences_dbc");
    LOAD_DB2(sCreatureDisplayInfoStore,              DBCSources::CreatureDisplayInfoInfo,                    "creaturedisplayinfo_dbc");
    LOAD_DB2(sCreatureDisplayInfoExtraStore,         DBCSources::CreatureDisplayInfoExtraInfo,               "creaturedisplayinfoextra_dbc");
    LOAD_DB2(sCreatureFamilyStore,                   DBCSources::CreatureFamilyInfo,                         "creaturefamily_dbc");
    LOAD_DB2(sCreatureModelDataStore,                DBCSources::CreatureModelDataInfo,                      "creaturemodeldata_dbc");
    LOAD_DB2(sCreatureSpellDataStore,                DBCSources::CreatureSpellDataInfo,                      "creaturespelldata_dbc");
    LOAD_DB2(sCreatureTypeStore,                     DBCSources::CreatureTypeInfo,                           "creaturetype_dbc");
    LOAD_DB2(sCurrencyTypesStore,                    DBCSources::CurrencyTypesInfo,                          "currencytypes_dbc");
    LOAD_DB2(sDestructibleModelDataStore,            DBCSources::DestructibleModelDataInfo,                  "destructiblemodeldata_dbc");
    LOAD_DB2_FIXUP(sDungeonEncounterStore,                 DBCSources::DungeonEncounterInfo,                       "dungeonencounter_dbc", DifficultyIndex(dbcPath, bad_dbc_files, &DungeonEncounterEntry::difficulty, &DungeonEncounterEntry::mapId));
    LOAD_DB2(sDurabilityCostsStore,                  DBCSources::DurabilityCostsInfo,                        "durabilitycosts_dbc");
    LOAD_DB2(sDurabilityQualityStore,                DBCSources::DurabilityQualityInfo,                      "durabilityquality_dbc");
    LOAD_DB2(sEmotesStore,                           DBCSources::EmotesInfo,                                 "emotes_dbc");
    LOAD_DB2(sEmotesTextStore,                       DBCSources::EmotesTextInfo,                             "emotestext_dbc");
    LOAD_DB2(sEmotesTextSoundStore,                  DBCSources::EmotesTextSoundInfo,                        "emotestextsound_dbc");
    LOAD_DB2(sFactionStore,                          DBCSources::FactionInfo,                                "faction_dbc");
    LOAD_DB2(sFactionTemplateStore,                  DBCSources::FactionTemplateInfo,                        "factiontemplate_dbc");
    LOAD_DB2(sGameObjectArtKitStore,                 DBCSources::GameObjectArtKitInfo,                       "gameobjectartkit_dbc");
    LOAD_DB2(sGameObjectDisplayInfoStore,            DBCSources::GameObjectDisplayInfoInfo,                  "gameobjectdisplayinfo_dbc");
    LOAD_DB2(sGemPropertiesStore,                    DBCSources::GemPropertiesInfo,                          "gemproperties_dbc");
    LOAD_DB2(sGlyphPropertiesStore,                  DBCSources::GlyphPropertiesInfo,                        "glyphproperties_dbc");
    LOAD_DB2(sGlyphSlotStore,                        DBCSources::GlyphSlotInfo,                              "glyphslot_dbc");
    LOAD_GT(sGtBarberShopCostBaseStore,             "BarberShopCostBase",                   "gtbarbershopcostbase_dbc");
    LOAD_GT(sGtCombatRatingsStore,                  "CombatRatings",                        "gtcombatratings_dbc");
    LOAD_GT(sGtChanceToMeleeCritBaseStore,          "ChanceToMeleeCritBase",                "gtchancetomeleecritbase_dbc");
    LOAD_GT(sGtChanceToMeleeCritStore,              "ChanceToMeleeCrit",                    "gtchancetomeleecrit_dbc");
    LOAD_GT(sGtChanceToSpellCritBaseStore,          "ChanceToSpellCritBase",                "gtchancetospellcritbase_dbc");
    LOAD_GT(sGtChanceToSpellCritStore,              "ChanceToSpellCrit",                    "gtchancetospellcrit_dbc");
    LOAD_GT(sGtNPCManaCostScalerStore,              "NPCManaCostScaler",                    "gtnpcmanacostscaler_dbc");
    LOAD_GT(sGtOCTClassCombatRatingScalarStore,     "OCTClassCombatRatingScalar",           "gtoctclasscombatratingscalar_dbc");
    LOAD_GT(sGtOCTRegenHPStore,                     "OCTRegenHP",                           "gtoctregenhp_dbc");
    //LOAD_DBC(sGtOCTRegenMPStore,                  "gtOCTRegenMP.dbc",                     "gtoctregenmp_dbc");       -- not used currently
    LOAD_GT(sGtRegenHPPerSptStore,                  "RegenHPPerSpt",                        "gtregenhpperspt_dbc");
    LOAD_GT(sGtRegenMPPerSptStore,                  "RegenMPPerSpt",                        "gtregenmpperspt_dbc");
    LOAD_DB2(sHolidaysStore,                         DBCSources::HolidaysInfo,                               "holidays_dbc");
    LOAD_DB2_FIXUP(sItemStore,                            DBCSources::ItemInfo,                                   "item_dbc", ItemAppearanceDisplay(dbcPath, bad_dbc_files));
    LOAD_DB2(sItemBagFamilyStore,                    DBCSources::ItemBagFamilyInfo,                          "itembagfamily_dbc");
    LOAD_DB2(sItemDisplayInfoStore,                  DBCSources::ItemDisplayInfoInfo,                        "itemdisplayinfo_dbc");
    //LOAD_DBC(sItemCondExtCostsStore,              "ItemCondExtCosts.dbc",                 "itemcondextcosts_dbc");
    LOAD_DB2_FIXUP(sItemExtendedCostStore,                 DBCSources::ItemExtendedCostInfo,                       "itemextendedcost_dbc", CurrencyCosts(dbcPath, bad_dbc_files));
    LOAD_DB2(sItemLimitCategoryStore,                DBCSources::ItemLimitCategoryInfo,                      "itemlimitcategory_dbc");
    LOAD_DB2(sItemRandomPropertiesStore,             DBCSources::ItemRandomPropertiesInfo,                   "itemrandomproperties_dbc");
    LOAD_DB2(sItemRandomSuffixStore,                 DBCSources::ItemRandomSuffixInfo,                       "itemrandomsuffix_dbc");
    LOAD_DB2_FIXUP(sItemSetStore,                         DBCSources::ItemSetInfo,                                "itemset_dbc", ItemSetSpells(dbcPath, bad_dbc_files));
    LOAD_DB2_FIXUP(sLFGDungeonStore,                       DBCSources::LFGDungeonsInfo,                            "lfgdungeons_dbc", DifficultyIndex(dbcPath, bad_dbc_files, &LFGDungeonEntry::Difficulty, &LFGDungeonEntry::MapID));
    LOAD_DB2(sLightStore,                            DBCSources::LightInfo,                                  "light_dbc");
    LOAD_DB2(sLiquidTypeStore,                       DBCSources::LiquidTypeInfo,                             "liquidtype_dbc");
    LOAD_DB2(sLockStore,                             DBCSources::LockInfo,                                   "lock_dbc");
    LOAD_DB2(sMailTemplateStore,                     DBCSources::MailTemplateInfo,                           "mailtemplate_dbc");
    LOAD_DB2(sMapStore,                              DBCSources::MapInfo,                                    "map_dbc");
    LOAD_DB2_FIXUP(sMapDifficultyStore,                    DBCSources::MapDifficultyInfo,                          "mapdifficulty_dbc", [&](DBCStorage<MapDifficultyEntry>& storage) { DifficultyIndex(dbcPath, bad_dbc_files, &MapDifficultyEntry::Difficulty, &MapDifficultyEntry::MapId)(storage); ResetIntervalSeconds(storage); });
    LOAD_DB2(sMovieStore,                            DBCSources::MovieInfo,                                  "movie_dbc");
    LOAD_DB2(sNamesReservedStore,                    DBCSources::NamesReservedInfo,                          "namesreserved_dbc");
    LOAD_DB2(sNamesProfanityStore,                   DBCSources::NamesProfanityInfo,                         "namesprofanity_dbc");
    LOAD_DB2(sOverrideSpellDataStore,                DBCSources::OverrideSpellDataInfo,                      "overridespelldata_dbc");
    LOAD_DB2(sPowerDisplayStore,                     DBCSources::PowerDisplayInfo,                           "powerdisplay_dbc");
    LOAD_DB2(sPvPDifficultyStore,                   DBCSources::PVPDifficultyInfo,                          "pvpdifficulty_dbc");
    LOAD_DB2(sQuestXPStore,                          DBCSources::QuestXPInfo,                                "questxp_dbc");
    LOAD_DB2(sQuestFactionRewardStore,               DBCSources::QuestFactionRewardInfo,                     "questfactionreward_dbc");
    LOAD_DB2(sQuestSortStore,                        DBCSources::QuestSortInfo,                              "questsort_dbc");
    LOAD_DB2(sRandomPropertiesPointsStore,           DBCSources::RandPropPointsInfo,                         "randproppoints_dbc");
    LOAD_DB2(sScalingStatDistributionStore,          DBCSources::ScalingStatDistributionInfo,                "scalingstatdistribution_dbc");
    LOAD_DB2(sScalingStatValuesStore,                DBCSources::ScalingStatValuesInfo,                      "scalingstatvalues_dbc");
    LOAD_DB2(sSkillLineStore,                        DBCSources::SkillLineInfo,                              "skillline_dbc");
    LOAD_DB2_FIXUP(sSkillLineAbilityStore,                 DBCSources::SkillLineAbilityInfo,                       "skilllineability_dbc", InvertSupersedes);
    LOAD_DB2(sSkillRaceClassInfoStore,               DBCSources::SkillRaceClassInfoInfo,                     "skillraceclassinfo_dbc");
    sSkillTiersStore.LoadFromDB("skilltiers_dbc", sSkillTiersStore.GetFormat());           // server-side: no client table
    ++DBCFileCount;
    LOAD_DB2(sSoundEntriesStore,                    DBCSources::SoundKitInfo,                               "soundentries_dbc");
    LOAD_DB2_FIXUP(sSpellStore,                            DBCSources::SpellInfo,                                  "spell_dbc", SpellPowerCosts(dbcPath, bad_dbc_files));
    sSpellRuneCostStore.LoadFromDB("spellrunecost_dbc", sSpellRuneCostStore.GetFormat());
    LOAD_DB2(sSpellCastTimesStore,                   DBCSources::SpellCastTimesInfo,                         "spellcasttimes_dbc");
    LOAD_DB2(sSpellCategoryStore,                    DBCSources::SpellCategoryInfo,                          "spellcategory_dbc");
    sSpellDifficultyStore.LoadFromDB("spelldifficulty_dbc", sSpellDifficultyStore.GetFormat()); // server-side: no client table
    ++DBCFileCount;
    LOAD_DB2(sSpellDurationStore,                    DBCSources::SpellDurationInfo,                          "spellduration_dbc");
    LOAD_DB2(sSpellFocusObjectStore,                 DBCSources::SpellFocusObjectInfo,                       "spellfocusobject_dbc");
    LOAD_DB2(sSpellItemEnchantmentStore,             DBCSources::SpellItemEnchantmentInfo,                   "spellitemenchantment_dbc");
    LOAD_DB2(sSpellItemEnchantmentConditionStore,    DBCSources::SpellItemEnchantmentConditionInfo,          "spellitemenchantmentcondition_dbc");
    LOAD_DB2(sSpellRadiusStore,                      DBCSources::SpellRadiusInfo,                            "spellradius_dbc");
    LOAD_DB2(sSpellRangeStore,                       DBCSources::SpellRangeInfo,                             "spellrange_dbc");
    // sSpellRuneCostStore: built from SpellPower with the spells (SpellPowerCosts), overrides below
    LOAD_DB2(sSpellShapeshiftFormStore,              DBCSources::SpellShapeshiftFormInfo,                    "spellshapeshiftform_dbc");
    LOAD_DB2(sSpellVisualStore,                      DBCSources::SpellVisualInfo,                            "spellvisual_dbc");
    LOAD_DB2(sStableSlotPricesStore,                 DBCSources::StableSlotPricesInfo,                       "stableslotprices_dbc");
    LOAD_DB2(sSummonPropertiesStore,                 DBCSources::SummonPropertiesInfo,                       "summonproperties_dbc");
    LOAD_DB2(sTalentStore,                           DBCSources::TalentInfo,                                 "talent_dbc");
    LOAD_DB2(sTalentTabStore,                        DBCSources::TalentTabInfo,                              "talenttab_dbc");
    LOAD_DB2(sTaxiNodesStore,                        DBCSources::TaxiNodesInfo,                              "taxinodes_dbc");
    LOAD_DB2(sTaxiPathStore,                         DBCSources::TaxiPathInfo,                               "taxipath_dbc");
    LOAD_DB2(sTaxiPathNodeStore,                     DBCSources::TaxiPathNodeInfo,                           "taxipathnode_dbc");
    LOAD_GT(sTeamContributionPointsStore,           "TeamContributionPoints",               "teamcontributionpoints_dbc");
    LOAD_DB2(sTotemCategoryStore,                    DBCSources::TotemCategoryInfo,                          "totemcategory_dbc");
    LOAD_DB2(sTransportAnimationStore,               DBCSources::TransportAnimationInfo,                     "transportanimation_dbc");
    LOAD_DB2(sTransportRotationStore,                DBCSources::TransportRotationInfo,                      "transportrotation_dbc");
    LOAD_DB2(sVehicleStore,                          DBCSources::VehicleInfo,                                "vehicle_dbc");
    LOAD_DB2(sVehicleSeatStore,                      DBCSources::VehicleSeatInfo,                            "vehicleseat_dbc");
    LOAD_DB2(sWMOAreaTableStore,                     DBCSources::WMOAreaTableInfo,                           "wmoareatable_dbc");
    LoadWorldMapAreas(dbcPath, bad_dbc_files);                              // UiMapAssignment, UiMap
    sWorldMapAreaStore.LoadFromDB("worldmaparea_dbc", sWorldMapAreaStore.GetFormat());
    LOAD_DB2(sWorldMapOverlayStore,                  DBCSources::WorldMapOverlayInfo,                        "worldmapoverlay_dbc");

#undef LOAD_DBC
#undef LOAD_DB2
#undef LOAD_DB2_FIXUP
#undef LOAD_GT

    for (CharStartOutfitEntry const* outfit : sCharStartOutfitStore)
        sCharStartOutfitMap[outfit->Race | (outfit->Class << 8) | (outfit->Gender << 16)] = outfit;

    for (FactionEntry const* faction : sFactionStore)
    {
        if (faction->team)
        {
            SimpleFactionsList& flist = sFactionTeamMap[faction->team];
            flist.push_back(faction->ID);
        }
    }

    for (GameObjectDisplayInfoEntry const* info : sGameObjectDisplayInfoStore)
    {
        if (info->maxX < info->minX)
            std::swap(*(float*)(&info->maxX), *(float*)(&info->minX));

        if (info->maxY < info->minY)
            std::swap(*(float*)(&info->maxY), *(float*)(&info->minY));

        if (info->maxZ < info->minZ)
            std::swap(*(float*)(&info->maxZ), *(float*)(&info->minZ));
    }

    for (EmotesTextSoundEntry const* emoteTextSound : sEmotesTextSoundStore)
    {
        EmotesTextSoundKey key(emoteTextSound->EmotesTextId, emoteTextSound->RaceId, emoteTextSound->SexId);
        sEmotesTextSoundMap[key] = emoteTextSound;
    }

    // fill data
    for (MapDifficultyEntry const* entry : sMapDifficultyStore)
        sMapDifficultyMap[MAKE_PAIR32(entry->MapId, entry->Difficulty)] = MapDifficulty(entry->resetTime, entry->maxPlayers, entry->areaTriggerText[0] != '\0');

    for (PvPDifficultyEntry const* entry : sPvPDifficultyStore)
        if (entry->bracketId > MAX_BATTLEGROUND_BRACKETS)
            ASSERT(false && "Need update MAX_BATTLEGROUND_BRACKETS by DBC data");

    for (auto i : sSpellStore)
        if (i->Category)
            sSpellsByCategoryStore[i->Category].emplace(false, i->Id);

    for (SkillRaceClassInfoEntry const* entry : sSkillRaceClassInfoStore)
    {
        if (sSkillLineStore.LookupEntry(entry->SkillID))
        {
            SkillRaceClassInfoBySkill.emplace(entry->SkillID, entry);
        }
    }

    for (SkillLineAbilityEntry const* skillLine : sSkillLineAbilityStore)
    {
        SpellEntry const* spellEntry = sSpellStore.LookupEntry(skillLine->Spell);
        if (spellEntry && spellEntry->Attributes & SPELL_ATTR0_PASSIVE)
        {
            for (CreatureFamilyEntry const* cFamily : sCreatureFamilyStore)
            {
                if (skillLine->SkillLine != cFamily->skillLine[0] && skillLine->SkillLine != cFamily->skillLine[1])
                {
                    continue;
                }

                if (spellEntry->SpellLevel)
                {
                    continue;
                }

                if (skillLine->AcquireMethod != SKILL_LINE_ABILITY_LEARNED_ON_SKILL_LEARN)
                {
                    continue;
                }

                sPetFamilySpellsStore[cFamily->ID].insert(spellEntry->Id);
            }
        }
    }

    for (SkillLineAbilityEntry const* skillLine : sSkillLineAbilityStore)
        sSkillLineAbilityIndexBySkillLine[skillLine->SkillLine].push_back(skillLine);

    // Create Spelldifficulty searcher
    for (SpellDifficultyEntry const* spellDiff : sSpellDifficultyStore)
    {
        SpellDifficultyEntry newEntry;

        memset(newEntry.SpellID, 0, 4 * sizeof(uint32));

        for (uint8 x = 0; x < MAX_DIFFICULTY; ++x)
        {
            if (spellDiff->SpellID[x] <= 0 || !sSpellStore.LookupEntry(spellDiff->SpellID[x]))
            {
                if (spellDiff->SpellID[x] > 0) //don't show error if spell is <= 0, not all modes have spells and there are unknown negative values
                    LOG_ERROR("sql.sql", "spelldifficulty_dbc: spell {} at field id: {} at spellid {} does not exist in SpellStore (spell.dbc), loaded as 0", spellDiff->SpellID[x], spellDiff->ID, x);

                newEntry.SpellID[x] = 0; // spell was <= 0 or invalid, set to 0
            }
            else
                newEntry.SpellID[x] = spellDiff->SpellID[x];
        }

        if (newEntry.SpellID[0] <= 0 || newEntry.SpellID[1] <= 0) // id0-1 must be always set!
            continue;

        for (uint8 x = 0; x < MAX_DIFFICULTY; ++x)
            if (newEntry.SpellID[x])
                sSpellMgr->SetSpellDifficultyId(uint32(newEntry.SpellID[x]), spellDiff->ID);
    }

    // create talent spells set
    for (TalentEntry const* talentInfo : sTalentStore)
    {
        TalentTabEntry const* talentTab = sTalentTabStore.LookupEntry(talentInfo->TalentTab);

        for (uint8 j = 0; j < MAX_TALENT_RANK; ++j)
        {
            if (talentInfo->RankID[j])
            {
                sTalentSpellPosMap[talentInfo->RankID[j]] = TalentSpellPos(talentInfo->TalentID, j);

                if (talentTab && talentTab->petTalentMask)
                {
                    sPetTalentSpells.insert(talentInfo->RankID[j]);
                }
            }
        }
    }

    // prepare fast data access to bit pos of talent ranks for use at inspecting
    {
        // now have all max ranks (and then bit amount used for store talent ranks in inspect)
        for (uint32 talentTabId = 1; talentTabId < sTalentTabStore.GetNumRows(); ++talentTabId)
        {
            TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentTabId);
            if (!talentTabInfo)
                continue;

            // prevent memory corruption; otherwise cls will become 12 below
            if ((talentTabInfo->ClassMask & CLASSMASK_ALL_PLAYABLE) == 0)
                continue;

            // store class talent tab pages
            for (uint32 cls = 1; cls < MAX_CLASSES; ++cls)
                if (talentTabInfo->ClassMask & (1 << (cls - 1)))
                    sTalentTabPages[cls][talentTabInfo->tabpage] = talentTabId;
        }
    }

    for (uint32 i = 1; i < sTaxiPathStore.GetNumRows(); ++i)
        if (TaxiPathEntry const* entry = sTaxiPathStore.LookupEntry(i))
            sTaxiPathSetBySource[entry->from][entry->to] = entry;

    // Calculate path nodes count
    uint32 pathCount = sTaxiPathStore.GetNumRows();
    std::vector<uint32> pathLength;
    pathLength.resize(pathCount);                           // 0 and some other indexes not used

    for (uint32 i = 1; i < sTaxiPathNodeStore.GetNumRows(); ++i)
        if (TaxiPathNodeEntry const* entry = sTaxiPathNodeStore.LookupEntry(i))
            if (pathLength[entry->path] < entry->index + 1)
                pathLength[entry->path] = entry->index + 1;

    // Set path length
    sTaxiPathNodesByPath.resize(pathCount);                 // 0 and some other indexes not used
    for (uint32 i = 1; i < sTaxiPathNodesByPath.size(); ++i)
        sTaxiPathNodesByPath[i].resize(pathLength[i]);

    // fill data
    for (TaxiPathNodeEntry const* entry : sTaxiPathNodeStore)
        sTaxiPathNodesByPath[entry->path][entry->index] = entry;

    // Initialize global taxinodes mask
    // include existed nodes that have at least single not spell base (scripted) path
    {
        std::set<uint32> spellPaths;
        for (SpellEntry const* sInfo : sSpellStore)
            for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
                if (sInfo->Effect[j] == SPELL_EFFECT_SEND_TAXI)
                    spellPaths.insert(sInfo->EffectMiscValue[j]);

        sTaxiNodesMask.fill(0);
        sOldContinentsNodesMask.fill(0);
        sHordeTaxiNodesMask.fill(0);
        sAllianceTaxiNodesMask.fill(0);
        sDeathKnightTaxiNodesMask.fill(0);

        for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
        {
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
            if (!node)
                continue;

            TaxiPathSetBySource::const_iterator src_i = sTaxiPathSetBySource.find(i);
            if (src_i != sTaxiPathSetBySource.end() && !src_i->second.empty())
            {
                bool ok = false;
                for (TaxiPathSetForSource::const_iterator dest_i = src_i->second.begin(); dest_i != src_i->second.end(); ++dest_i)
                {
                    // not spell path
                    if (dest_i->second->price || spellPaths.find(dest_i->second->ID) == spellPaths.end())
                    {
                        ok = true;
                        break;
                    }
                }

                if (!ok)
                    continue;
            }

            // valid taxi network node
            uint8  field   = (uint8)((i - 1) / 32);
            uint32 submask = 1 << ((i - 1) % 32);
            sTaxiNodesMask[field] |= submask;

            if (node->MountCreatureID[0] && node->MountCreatureID[0] != 32981)
                sHordeTaxiNodesMask[field] |= submask;

            if (node->MountCreatureID[1] && node->MountCreatureID[1] != 32981)
                sAllianceTaxiNodesMask[field] |= submask;

            if (node->MountCreatureID[0] == 32981 || node->MountCreatureID[1] == 32981)
                sDeathKnightTaxiNodesMask[field] |= submask;

            // old continent node (+ nodes virtually at old continents, check explicitly to avoid loading map files for zone info)
            if (node->map_id < 2 || i == 82 || i == 83 || i == 93 || i == 94)
                sOldContinentsNodesMask[field] |= submask;

            // fix DK node at Ebon Hold and Shadow Vault flight master
            if (i == 315 || i == 333)
                ((TaxiNodesEntry*)node)->MountCreatureID[1] = 32981;
        }
    }

    for (TransportAnimationEntry const* anim : sTransportAnimationStore)
        sTransportMgr->AddPathNodeToTransport(anim->TransportEntry, anim->TimeSeg, anim);

    for (TransportRotationEntry const* rot : sTransportRotationStore)
        sTransportMgr->AddPathRotationToTransport(rot->TransportEntry, rot->TimeSeg, rot);

    for (WMOAreaTableEntry const* entry : sWMOAreaTableStore)
        sWMOAreaInfoByTripple[WMOAreaTableKey(entry->rootId, entry->adtId, entry->groupId)] = entry;

    // error checks
    if (bad_dbc_files.size() >= DBCFileCount)
    {
        LOG_ERROR("dbc", "Incorrect DataDir value in worldserver.conf or ALL required *.dbc files ({}) not found by path: {}dbc", DBCFileCount, dataPath);
        exit(1);
    }
    else if (!bad_dbc_files.empty())
    {
        std::string str;
        for (StoreProblemList::iterator i = bad_dbc_files.begin(); i != bad_dbc_files.end(); ++i)
            str += *i + "\n";

        LOG_ERROR("dbc", "Some required *.dbc files ({} from {}) not found or not compatible:\n{}", (uint32)bad_dbc_files.size(), DBCFileCount, str);
        exit(1);
    }

    // Check loaded DBC files proper version
    if (!sAreaTableStore.LookupEntry(4987)             ||       // last area added in 3.3.5a
            !sCharTitlesStore.LookupEntry(177)         ||       // last char title added in 3.3.5a
            !sGemPropertiesStore.LookupEntry(1629)     ||       // last added spell in 3.3.5a
            !sItemStore.LookupEntry(56806)             ||       // last client known item added in 3.3.5a
            !sItemExtendedCostStore.LookupEntry(2997)  ||       // last item extended cost added in 3.3.5a
            !sMapStore.LookupEntry(724)                ||       // last map added in 3.3.5a
            !sSpellStore.LookupEntry(80864)            )        // last client known item added in 3.3.5a
    {
        LOG_ERROR("dbc", "You have _outdated_ DBC data. Please extract correct versions from current using client.");
        exit(1);
    }

    LOG_INFO("server.loading", ">> Initialized {} Data Stores in {} ms", DBCFileCount, GetMSTimeDiffToNow(oldMSTime));
    LOG_INFO("server.loading", " ");
}

SimpleFactionsList const* GetFactionTeamList(uint32 faction)
{
    FactionTeamMap::const_iterator itr = sFactionTeamMap.find(faction);
    if (itr != sFactionTeamMap.end())
        return &itr->second;

    return nullptr;
}

char const* GetPetName(uint32 petfamily, uint32 dbclang)
{
    if (!petfamily)
        return nullptr;

    CreatureFamilyEntry const* pet_family = sCreatureFamilyStore.LookupEntry(petfamily);
    if (!pet_family)
        return nullptr;

    return pet_family->Name[dbclang];
}

TalentSpellPos const* GetTalentSpellPos(uint32 spellId)
{
    TalentSpellPosMap::const_iterator itr = sTalentSpellPosMap.find(spellId);
    if (itr == sTalentSpellPosMap.end())
        return nullptr;

    return &itr->second;
}

uint32 GetTalentSpellCost(uint32 spellId)
{
    if (TalentSpellPos const* pos = GetTalentSpellPos(spellId))
        return pos->rank + 1;

    return 0;
}

WMOAreaTableEntry const* GetWMOAreaTableEntryByTripple(int32 rootid, int32 adtid, int32 groupid)
{
    auto i = sWMOAreaInfoByTripple.find(WMOAreaTableKey(int16(rootid), int8(adtid), groupid));
    if (i != sWMOAreaInfoByTripple.end())
        return i->second;

    return nullptr;
}

uint32 GetVirtualMapForMapAndZone(uint32 mapid, uint32 zoneId)
{
    if (mapid != MAP_OUTLAND && mapid != MAP_NORTHREND)                        // speed for most cases
        return mapid;

    if (WorldMapAreaEntry const* wma = sWorldMapAreaStore.LookupEntry(zoneId))
        return wma->virtual_map_id >= 0 ? wma->virtual_map_id : wma->map_id;

    return mapid;
}

ContentLevels GetContentLevelsForMapAndZone(uint32 mapid, uint32 zoneId)
{
    mapid = GetVirtualMapForMapAndZone(mapid, zoneId);
    if (mapid < 2)
        return CONTENT_1_60;

    MapEntry const* mapEntry = sMapStore.LookupEntry(mapid);
    if (!mapEntry)
        return CONTENT_1_60;

    switch (mapEntry->Expansion())
    {
        default:
            return CONTENT_1_60;
        case 1:
            return CONTENT_61_70;
        case 2:
            return CONTENT_71_80;
    }
}

void Zone2MapCoordinates(float& x, float& y, uint32 zone)
{
    WorldMapAreaEntry const* maEntry = sWorldMapAreaStore.LookupEntry(zone);

    // if not listed then map coordinates (instance)
    if (!maEntry)
        return;

    std::swap(x, y);                                         // at client map coords swapped
    x = x * ((maEntry->x2 - maEntry->x1) / 100) + maEntry->x1;
    y = y * ((maEntry->y2 - maEntry->y1) / 100) + maEntry->y1; // client y coord from top to down
}

void Map2ZoneCoordinates(float& x, float& y, uint32 zone)
{
    WorldMapAreaEntry const* maEntry = sWorldMapAreaStore.LookupEntry(zone);

    // if not listed then map coordinates (instance)
    if (!maEntry)
        return;

    x = (x - maEntry->x1) / ((maEntry->x2 - maEntry->x1) / 100);
    y = (y - maEntry->y1) / ((maEntry->y2 - maEntry->y1) / 100); // client y coord from top to down
    std::swap(x, y);                                         // client have map coords swapped
}

MapDifficulty const* GetMapDifficultyData(uint32 mapId, Difficulty difficulty)
{
    MapDifficultyMap::const_iterator itr = sMapDifficultyMap.find(MAKE_PAIR32(mapId, difficulty));
    return itr != sMapDifficultyMap.end() ? &itr->second : nullptr;
}

MapDifficulty const* GetDownscaledMapDifficultyData(uint32 mapId, Difficulty& difficulty)
{
    uint32 tmpDiff = difficulty;

    MapDifficulty const* mapDiff = GetMapDifficultyData(mapId, Difficulty(tmpDiff));
    if (!mapDiff)
    {
        if (tmpDiff > RAID_DIFFICULTY_25MAN_NORMAL) // heroic, downscale to normal
            tmpDiff -= 2;
        else
            tmpDiff -= 1;   // any non-normal mode for raids like tbc (only one mode)

        // pull new data
        mapDiff = GetMapDifficultyData(mapId, Difficulty(tmpDiff)); // we are 10 normal or 25 normal
        if (!mapDiff)
        {
            tmpDiff -= 1;
            mapDiff = GetMapDifficultyData(mapId, Difficulty(tmpDiff)); // 10 normal
        }
    }

    difficulty = Difficulty(tmpDiff);

    return mapDiff;
}

PvPDifficultyEntry const* GetBattlegroundBracketByLevel(uint32 mapid, uint32 level)
{
    PvPDifficultyEntry const* maxEntry = nullptr;              // used for level > max listed level case

    for (PvPDifficultyEntry const* entry : sPvPDifficultyStore)
    {
        // skip unrelated and too-high brackets
        if (entry->mapId != mapid || entry->minLevel > level)
            continue;

        // exactly fit
        if (entry->maxLevel >= level)
            return entry;

        // remember for possible out-of-range case (search higher from existed)
        if (!maxEntry || maxEntry->maxLevel < entry->maxLevel)
            maxEntry = entry;
    }

    return maxEntry;
}

PvPDifficultyEntry const* GetBattlegroundBracketById(uint32 mapid, BattlegroundBracketId id)
{
    for (PvPDifficultyEntry const* entry : sPvPDifficultyStore)
        if (entry->mapId == mapid && entry->GetBracketId() == id)
            return entry;

    return nullptr;
}

uint32 const* GetTalentTabPages(uint8 cls)
{
    return sTalentTabPages[cls];
}

bool IsSharedDifficultyMap(uint32 mapid)
{
    return sWorld->getBoolConfig(CONFIG_INSTANCE_SHARED_ID) && (mapid == 631 || mapid == 724);
}

uint32 GetLiquidFlags(uint32 liquidType)
{
    if (LiquidTypeEntry const* liq = sLiquidTypeStore.LookupEntry(liquidType))
        return 1 << liq->Type;

    return 0;
}

CharStartOutfitEntry const* GetCharStartOutfitEntry(uint8 race, uint8 class_, uint8 gender)
{
    std::map<uint32, CharStartOutfitEntry const*>::const_iterator itr = sCharStartOutfitMap.find(race | (class_ << 8) | (gender << 16));
    if (itr == sCharStartOutfitMap.end())
        return nullptr;

    return itr->second;
}

/// Returns LFGDungeonEntry for a specific map and difficulty. Will return first found entry if multiple dungeons use the same map (such as Scarlet Monastery)
LFGDungeonEntry const* GetLFGDungeon(uint32 mapId, Difficulty difficulty)
{
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i);
        if (!dungeon)
            continue;

        if (dungeon->MapID == uint32(mapId) && Difficulty(dungeon->Difficulty) == difficulty)
            return dungeon;
    }

    return nullptr;
}

LFGDungeonEntry const* GetZoneLFGDungeonEntry(std::string const& zoneName, LocaleConstant locale)
{
    for (LFGDungeonEntry const* dungeon : sLFGDungeonStore)
    {
        if (dungeon->TypeID == lfg::LFG_TYPE_ZONE && zoneName.find(dungeon->Name[locale]) != std::string::npos)
        {
            return dungeon;
        }
    }

    return nullptr;
}

uint32 GetDefaultMapLight(uint32 mapId)
{
    for (int32 i = sLightStore.GetNumRows(); i >= 0; --i)
    {
        LightEntry const* light = sLightStore.LookupEntry(uint32(i));
        if (!light)
            continue;

        if (light->MapId == mapId && light->X == 0.0f && light->Y == 0.0f && light->Z == 0.0f)
            return light->Id;
    }

    return 0;
}

SkillRaceClassInfoEntry const* GetSkillRaceClassInfo(uint32 skill, uint8 race, uint8 class_)
{
    SkillRaceClassInfoBounds bounds = SkillRaceClassInfoBySkill.equal_range(skill);
    for (SkillRaceClassInfoMap::iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        if (itr->second->RaceMask && !(itr->second->RaceMask & (1 << (race - 1))))
        {
            continue;
        }

        if (itr->second->ClassMask && !(itr->second->ClassMask & (1 << (class_ - 1))))
        {
            continue;
        }

        return itr->second;
    }

    return nullptr;
}

EmotesTextSoundEntry const* FindTextSoundEmoteFor(uint32 emote, uint32 race, uint32 gender)
{
    auto itr = sEmotesTextSoundMap.find(EmotesTextSoundKey(emote, race, gender));
    return itr != sEmotesTextSoundMap.end() ? itr->second : nullptr;
}

std::vector<SkillLineAbilityEntry const*> const& GetSkillLineAbilitiesBySkillLine(uint32 skillLine)
{
    auto it = sSkillLineAbilityIndexBySkillLine.find(skillLine);
    if (it == sSkillLineAbilityIndexBySkillLine.end())
    {
        static const std::vector<SkillLineAbilityEntry const*> emptyVector;
        return emptyVector;
    }
    return it->second;
}
