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

#include "BattlegroundMgr.h"
#include "PacketGuid128.h"
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "BattlegroundAB.h"
#include "BattlegroundAV.h"
#include "BattlegroundBE.h"
#include "BattlegroundDS.h"
#include "BattlegroundEY.h"
#include "BattlegroundIC.h"
#include "BattlegroundNA.h"
#include "BattlegroundQueue.h"
#include "BattlegroundRL.h"
#include "BattlegroundRV.h"
#include "BattlegroundSA.h"
#include "BattlegroundWS.h"
#include "Chat.h"
#include "Common.h"
#include "DisableMgr.h"
#include "Formulas.h"
#include "GameEventMgr.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "Map.h"
#include "MapMgr.h"
#include "MiscPackets.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldState.h"
#include "WorldStateDefines.h"
#include <unordered_map>

bool BattlegroundTemplate::IsArena() const
{
    return BattlemasterEntry->type == MAP_ARENA;
}

/*********************************************************/
/***            BATTLEGROUND MANAGER                   ***/
/*********************************************************/

BattlegroundMgr::BattlegroundMgr() :
    m_ArenaTesting(sWorld->getBoolConfig(CONFIG_DEBUG_ARENA)),
    m_Testing(sWorld->getBoolConfig(CONFIG_DEBUG_BATTLEGROUND)),
    m_NextAutoDistributionTime(0),
    m_AutoDistributionTimeChecker(0),
    m_NextPeriodicQueueUpdateTime(5 * IN_MILLISECONDS)
{
}

BattlegroundMgr::~BattlegroundMgr()
{
    DeleteAllBattlegrounds();
}

BattlegroundMgr* BattlegroundMgr::instance()
{
    static BattlegroundMgr instance;
    return &instance;
}

void BattlegroundMgr::DeleteAllBattlegrounds()
{
    for (auto& [_, data] : bgDataStore)
    {
        while (!data._Battlegrounds.empty())
            delete data._Battlegrounds.begin()->second;

        data._Battlegrounds.clear();

        while (!data.BGFreeSlotQueue.empty())
            delete data.BGFreeSlotQueue.front();
    }

    bgDataStore.clear();
}

// used to update running battlegrounds, and delete finished ones
void BattlegroundMgr::Update(uint32 diff)
{
    // update all battlegrounds and delete if needed
    for (auto& [_, bgData] : bgDataStore)
    {
        auto& bgList = bgData._Battlegrounds;
        auto itrDelete = bgList.begin();

        // first one is template and should not be deleted
        for (BattlegroundContainer::iterator itr = ++itrDelete; itr != bgList.end();)
        {
            itrDelete = itr++;
            Battleground* bg = itrDelete->second;

            bg->Update(diff);
            if (bg->ToBeDeleted())
            {
                itrDelete->second = nullptr;
                bgList.erase(itrDelete);

                BattlegroundClientIdsContainer& clients = bgData._ClientBattlegroundIds[bg->GetBracketId()];
                if (!clients.empty())
                    clients.erase(bg->GetClientInstanceID());

                delete bg;
            }
        }
    }

    // update events
    for (uint8 qtype = BATTLEGROUND_QUEUE_NONE; qtype < MAX_BATTLEGROUND_QUEUE_TYPES; ++qtype)
        m_BattlegroundQueues[qtype].UpdateEvents(diff);

    // update using scheduled tasks (used only for rated arenas, initial opponent search works differently than periodic queue update)
    if (!m_QueueUpdateScheduler.empty())
    {
        std::vector<uint64> scheduled;
        std::swap(scheduled, m_QueueUpdateScheduler);

        for (uint8 i = 0; i < scheduled.size(); i++)
        {
            uint32 arenaMMRating = scheduled[i] >> 32;
            uint8 arenaType = scheduled[i] >> 24 & 255;
            BattlegroundQueueTypeId bgQueueTypeId = BattlegroundQueueTypeId(scheduled[i] >> 16 & 255);
            BattlegroundTypeId bgTypeId = BattlegroundTypeId((scheduled[i] >> 8) & 255);
            BattlegroundBracketId bracket_id = BattlegroundBracketId(scheduled[i] & 255);
            m_BattlegroundQueues[bgQueueTypeId].BattlegroundQueueUpdate(diff, bgTypeId, bracket_id, arenaType, arenaMMRating > 0, arenaMMRating);
            m_BattlegroundQueues[bgQueueTypeId].BattlegroundQueueAnnouncerUpdate(diff, bgQueueTypeId, bracket_id);
        }
    }

    // periodic queue update
    if (m_NextPeriodicQueueUpdateTime < diff)
    {
        m_NextPeriodicQueueUpdateTime = 5 * IN_MILLISECONDS;

        LOG_TRACE("bg.arena", "BattlegroundMgr: UPDATING ARENA QUEUES");

        // for rated arenas
        for (uint32 qtype = BATTLEGROUND_QUEUE_2v2; qtype < MAX_BATTLEGROUND_QUEUE_TYPES; ++qtype)
        {
            for (uint32 bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
            {
                m_BattlegroundQueues[qtype].BattlegroundQueueUpdate(m_NextPeriodicQueueUpdateTime, BATTLEGROUND_AA, BattlegroundBracketId(bracket), BattlegroundMgr::BGArenaType(BattlegroundQueueTypeId(qtype)), true, 0);
            }
        }

        for (uint32 qtype = BATTLEGROUND_QUEUE_AV; qtype < MAX_BATTLEGROUND_QUEUE_TYPES; ++qtype)
        {
            for (uint32 bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
            {
                m_BattlegroundQueues[qtype].BattlegroundQueueAnnouncerUpdate(m_NextPeriodicQueueUpdateTime, BattlegroundQueueTypeId(qtype), BattlegroundBracketId(bracket));
            }
        }
    }
    else
        m_NextPeriodicQueueUpdateTime -= diff;

    // arena points auto-distribution
    if (sWorld->getBoolConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_POINTS))
    {
        if (m_AutoDistributionTimeChecker < diff)
        {
            if (GameTime::GetGameTime() > m_NextAutoDistributionTime)
            {
                sArenaTeamMgr->DistributeArenaPoints();
                m_NextAutoDistributionTime = GameTime::GetGameTime() + Seconds(DAY * sWorld->getIntConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_INTERVAL_DAYS));
                sWorldState->setWorldState(WORLD_STATE_CUSTOM_ARENA_DISTRIBUTION_TIME, m_NextAutoDistributionTime.count());
            }
            m_AutoDistributionTimeChecker = 600000; // 10 minutes check
        }
        else
            m_AutoDistributionTimeChecker -= diff;
    }
}

/// RideTicket of a battleground queue slot, 3.4.3 (FUN_14070f950): PackedGuid128 RequesterGuid, u32 Id, u32 Type, i64
/// Time, bit, flush. The client keys its queue slots on the whole ticket, so every status of one slot (the final NONE
/// included) carries the same one: Id = the player's queue slot, Type 1 (battlegrounds), no requester, no time.
static void WriteBattlegroundTicket(ByteBuffer& data, uint8 queueSlot)
{
    Guid128::Write(data, ObjectGuid::Empty);
    data << uint32(queueSlot);
    data << uint32(1);
    data << int64(0);
    data.WriteBit(false);
    data.FlushBits();
}

/// The u64 queue id, 3.4.3 (Lua GetBattlefieldStatus): BattlemasterListID, Type << 16 (0 battleground, 1 arena), Rated
/// << 20, TeamSize << 24, 0x1F10000000000000
static uint64 BattlegroundQueueId(uint32 listId, uint8 arenaType, bool rated)
{
    return uint64(listId & 0xFFFF) | (uint64(arenaType ? 1 : 0) << 16) | (uint64(rated ? 1 : 0) << 20) | (uint64(arenaType) << 24) | 0x1F10000000000000ULL;
}

/// SMSG_BATTLEFIELD_STATUS_*, 3.4.3: 3.3.5's one message is one opcode per status. NONE (handler FUN_1419b41b0): the
/// RideTicket. The others start with BattlefieldStatusHeader (FUN_1406f8ac0): RideTicket, u32 queue id count (1), u8
/// RangeMin, u8 RangeMax, u8 TeamSize, u32 InstanceID, the u64 queue ids, bits RegisteredMatch, TournamentRules, flush.
/// QUEUED (FUN_1419b41c0): u32 AverageWaitTime, u32 WaitTime (ms), u32, bits AsGroup, EligibleForMatchmaking,
/// SuspendedQueue, flush. NEED_CONFIRMATION (FUN_1419b4110): u32 Mapid, u32 Timeout (ms), u8 Role. ACTIVE
/// (FUN_1419b3760): u32 Mapid, u32 ShutdownTimer, u32 StartTimer (ms), bits ArenaFaction, LeftEarly, IsBrawl, flush.
void BattlegroundMgr::BuildBattlegroundStatusPacket(WorldPacket* data, Battleground* bg, uint8 QueueSlot, uint8 StatusID, uint32 Time1, uint32 Time2, uint8 arenatype, TeamId teamId, bool isRated, BattlegroundTypeId forceBgTypeId)
{
    if (StatusID == STATUS_NONE || !bg)
    {
        data->Initialize(SMSG_BATTLEFIELD_STATUS_NONE, 20);
        WriteBattlegroundTicket(*data, QueueSlot);
        return;
    }

    Opcodes opcode = SMSG_BATTLEFIELD_STATUS_ACTIVE;
    switch (StatusID)
    {
        case STATUS_WAIT_QUEUE: opcode = SMSG_BATTLEFIELD_STATUS_QUEUED; break;
        case STATUS_WAIT_JOIN: opcode = SMSG_BATTLEFIELD_STATUS_NEED_CONFIRMATION; break;
        default: break;                                     // in progress, ending
    }

    bool rated = bg->isRated() || isRated;
    uint32 listId = forceBgTypeId != BATTLEGROUND_TYPE_NONE ? forceBgTypeId : bg->GetBgTypeID();
    data->Initialize(opcode, 64);
    WriteBattlegroundTicket(*data, QueueSlot);
    *data << uint32(1);                                     // queue ids
    *data << uint8(bg->GetMinLevel());
    *data << uint8(bg->GetMaxLevel());
    *data << uint8(arenatype);                              // TeamSize
    *data << uint32(bg->GetClientInstanceID());
    *data << uint64(BattlegroundQueueId(listId, arenatype, rated));
    data->WriteBit(rated);                                  // RegisteredMatch
    data->WriteBit(false);                                  // TournamentRules
    data->FlushBits();

    switch (StatusID)
    {
        case STATUS_WAIT_QUEUE:
            *data << uint32(Time1);                         // average wait time, milliseconds
            *data << uint32(Time2);                         // time in queue, milliseconds
            *data << uint32(0);
            data->WriteBit(false);                          // AsGroup
            data->WriteBit(true);                           // EligibleForMatchmaking
            data->WriteBit(false);                          // SuspendedQueue
            data->FlushBits();
            break;
        case STATUS_WAIT_JOIN:
            *data << uint32(bg->GetMapId());
            *data << uint32(Time1);                         // time to remove from queue, milliseconds
            *data << uint8(2);                              // Role: damage
            break;
        default:
            *data << uint32(bg->GetMapId());
            *data << uint32(Time1);                         // time to bg auto leave, 0 at bg start, 120000 after bg end, milliseconds
            *data << uint32(Time2);                         // time from bg start, milliseconds
            data->WriteBit(teamId == TEAM_ALLIANCE);        // ArenaFaction
            data->WriteBit(false);                          // LeftEarly
            data->WriteBit(false);                          // IsBrawl
            data->FlushBits();
            break;
    }
}

/// SMSG_BATTLEFIELD_STATUS_FAILED, 3.4.3 (handler FUN_1419b3af0), which replaces SMSG_GROUP_JOINED_BATTLEGROUND:
/// RideTicket (all zero: no queue slot is touched), u64 QueueID, u32 Reason (the client's numbering: AC's -2..-12 are
/// 2..12, -13..-15 are 14..16, a failed group join 35), PackedGuid128 ClientID. 3.3.5's "joined" form (a positive
/// result) has no message: the QUEUED status says it, so a reason the client shows nothing for goes out.
void BattlegroundMgr::BuildGroupJoinedBattlegroundPacket(WorldPacket* data, GroupJoinBattlegroundResult result)
{
    uint32 reason = 0;
    if (result <= ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS && result >= ERR_BATTLEGROUND_JOIN_FAILED)
        reason = uint32(-int32(result));
    else if (result <= ERR_LFG_CANT_USE_BATTLEGROUND && result >= ERR_IN_NON_RANDOM_BG)
        reason = uint32(-int32(result)) + 1;
    else if (result == ERR_GROUP_JOIN_BATTLEGROUND_FAIL)
        reason = 35;

    data->Initialize(SMSG_BATTLEFIELD_STATUS_FAILED, 20 + 8 + 4 + 2);
    Guid128::Write(*data, ObjectGuid::Empty);               // the ticket: none
    *data << uint32(0) << uint32(0) << int64(0);
    data->WriteBit(false);
    data->FlushBits();
    *data << uint64(0);                                     // QueueID
    *data << uint32(reason);
    Guid128::Write(*data, ObjectGuid::Empty);               // ClientID
}

void BattlegroundMgr::BuildPlayerLeftBattlegroundPacket(WorldPacket* data, ObjectGuid guid)
{
    // SMSG_BATTLEGROUND_PLAYER_LEFT, 3.4.3: PackedGuid128
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_LEFT, 18);
    Guid128::Write(*data, guid);
}

void BattlegroundMgr::BuildPlayerJoinedBattlegroundPacket(WorldPacket* data, Player* player)
{
    // SMSG_BATTLEGROUND_PLAYER_JOINED, 3.4.3: PackedGuid128
    data->Initialize(SMSG_BATTLEGROUND_PLAYER_JOINED, 18);
    Guid128::Write(*data, player->GetGUID());
}

Battleground* BattlegroundMgr::GetBattlegroundThroughClientInstance(uint32 instanceId, BattlegroundTypeId bgTypeId)
{
    //cause at HandleBattlegroundJoinOpcode the clients sends the instanceid he gets from
    //SMSG_BATTLEFIELD_LIST we need to find the battleground with this clientinstance-id
    Battleground* bg = GetBattlegroundTemplate(bgTypeId);
    if (!bg)
        return nullptr;

    if (bg->isArena())
        return GetBattleground(instanceId, bgTypeId);

    auto const& it = bgDataStore.find(bgTypeId);
    if (it == bgDataStore.end())
        return nullptr;

    for (auto const& itr : it->second._Battlegrounds)
    {
        if (itr.second->GetClientInstanceID() == instanceId)
            return itr.second;
    }

    return nullptr;
}

Battleground* BattlegroundMgr::GetBattleground(uint32 instanceId, BattlegroundTypeId bgTypeId)
{
    if (!instanceId)
        return nullptr;

    auto GetBgWithInstanceID = [instanceId](BattlegroundData const* bgData) -> Battleground*
    {
        auto const& itr = bgData->_Battlegrounds.find(instanceId);
        if (itr != bgData->_Battlegrounds.end())
            return itr->second;

        return nullptr;
    };

    if (bgTypeId == BATTLEGROUND_TYPE_NONE)
    {
        for (auto const& [bgType, bgData] : bgDataStore)
        {
            if (auto bg = GetBgWithInstanceID(&bgData))
                return bg;
        }
    }
    else
    {
        auto const& itr = bgDataStore.find(bgTypeId);
        if (itr == bgDataStore.end())
            return nullptr;

        if (auto bg = GetBgWithInstanceID(&itr->second))
            return bg;
    }

    return nullptr;
}

Battleground* BattlegroundMgr::GetBattlegroundTemplate(BattlegroundTypeId bgTypeId)
{
    BattlegroundDataContainer::const_iterator itr = bgDataStore.find(bgTypeId);
    if (itr == bgDataStore.end())
        return nullptr;

    BattlegroundContainer const& bgs = itr->second._Battlegrounds;

    // map is sorted and we can be sure that lowest instance id has only BG template
    return bgs.empty() ? nullptr : bgs.begin()->second;
}

std::vector<Battleground const*> BattlegroundMgr::GetActiveBattlegrounds()
{
    std::vector<Battleground const*> result;

    for (auto const& [bgType, bgData] : bgDataStore)
        for (auto const& [id, bg] : bgData._Battlegrounds)
            if (bg->GetStatus() == STATUS_WAIT_JOIN || bg->GetStatus() == STATUS_IN_PROGRESS)
                result.push_back(static_cast<Battleground const*>(bg));

    return result;
}

uint32 BattlegroundMgr::CreateClientVisibleInstanceId(BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id)
{
    if (IsArenaType(bgTypeId))
        return 0; // arenas don't have client-instanceids

    // we create here an instanceid, which is just for
    // displaying this to the client and without any other use..
    // the client-instanceIds are unique for each battleground-type
    // the instance-id just needs to be as low as possible, beginning with 1
    // the following works, because std::set is default ordered with "<"
    // the optimalization would be to use as bitmask std::vector<uint32> - but that would only make code unreadable

    BattlegroundClientIdsContainer& clientIds = bgDataStore[bgTypeId]._ClientBattlegroundIds[bracket_id];
    uint32 lastId = 0;

    for (BattlegroundClientIdsContainer::const_iterator itr = clientIds.begin(); itr != clientIds.end();)
    {
        if ((++lastId) != *itr) // if there is a gap between the ids, we will break..
            break;

        lastId = *itr;
    }

    clientIds.emplace(++lastId);
    return lastId;
}

// create a new battleground that will really be used to play
Battleground* BattlegroundMgr::CreateNewBattleground(BattlegroundTypeId originalBgTypeId, PvPDifficultyEntry const* bracketEntry, uint8 arenaType, bool isRated)
{
    BattlegroundTypeId bgTypeId = GetRandomBG(originalBgTypeId, bracketEntry->minLevel);

    if (originalBgTypeId == BATTLEGROUND_AA)
        originalBgTypeId = bgTypeId;

    // get the template BG
    Battleground* bg_template = GetBattlegroundTemplate(bgTypeId);
    if (!bg_template)
    {
        LOG_ERROR("bg.battleground", "Battleground: CreateNewBattleground - bg template not found for {}", bgTypeId);
        return nullptr;
    }

    Battleground* bg = nullptr;

    // create a copy of the BG template
    if (BattlegroundMgr::bgTypeToTemplate.find(bgTypeId) == BattlegroundMgr::bgTypeToTemplate.end())
    {
        return nullptr;
    }

    bg = BattlegroundMgr::bgTypeToTemplate[bgTypeId](bg_template);

    bool isRandom = bgTypeId != originalBgTypeId && !bg->isArena();

    bg->SetBracket(bracketEntry);
    bg->SetInstanceID(sMapMgr->GenerateInstanceId());
    bg->SetClientInstanceID(CreateClientVisibleInstanceId(originalBgTypeId, bracketEntry->GetBracketId()));
    bg->Init();
    bg->SetStatus(STATUS_WAIT_JOIN); // start the joining of the bg
    bg->SetArenaType(arenaType);
    bg->SetBgTypeID(originalBgTypeId);
    bg->SetRandomTypeID(bgTypeId);
    bg->SetRated(isRated);
    bg->SetRandom(isRandom);

    // Set up correct min/max player counts for scoreboards
    if (bg->isArena())
    {
        uint32 maxPlayersPerTeam = ArenaTeam::GetReqPlayersForType(arenaType) / 2;
        sScriptMgr->OnSetArenaMaxPlayersPerTeam(arenaType, maxPlayersPerTeam);
        bg->SetMaxPlayersPerTeam(maxPlayersPerTeam);
    }

    return bg;
}

// used to create the BG templates
bool BattlegroundMgr::CreateBattleground(BattlegroundTemplate const* bgTemplate)
{
    // Create the BG
    Battleground* bg = GetBattlegroundTemplate(bgTemplate->Id);

    if (!bg)
    {
        bg = BattlegroundMgr::bgtypeToBattleground[bgTemplate->Id];

        ASSERT(bg);

        if (bgTemplate->Id == BATTLEGROUND_RB)
            bg->SetRandom(true);

        bg->SetBgTypeID(bgTemplate->Id);
        bg->SetInstanceID(0);
        AddBattleground(bg);
    }

    bg->SetMapId(bgTemplate->BattlemasterEntry->mapid[0]);
    bg->SetName(bgTemplate->BattlemasterEntry->name[sWorld->GetDefaultDbcLocale()]);
    bg->SetArenaorBGType(bgTemplate->IsArena());
    bg->SetMinPlayersPerTeam(bgTemplate->MinPlayersPerTeam);
    bg->SetMaxPlayersPerTeam(bgTemplate->MaxPlayersPerTeam);
    bg->SetTeamStartPosition(TEAM_ALLIANCE, bgTemplate->StartLocation[TEAM_ALLIANCE]);
    bg->SetTeamStartPosition(TEAM_HORDE,    bgTemplate->StartLocation[TEAM_HORDE]);
    bg->SetStartMaxDist(bgTemplate->MaxStartDistSq);
    bg->SetLevelRange(bgTemplate->MinLevel, bgTemplate->MaxLevel);
    bg->SetScriptId(bgTemplate->ScriptId);

    return true;
}

void BattlegroundMgr::LoadBattlegroundTemplates()
{
    uint32 oldMSTime = getMSTime();

    _battlegroundMapTemplates.clear();
    _battlegroundTemplates.clear();

    //                                               0   1                  2                  3       4       5                 6               7              8            9             10      11
    QueryResult result = WorldDatabase.Query("SELECT ID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl, AllianceStartLoc, AllianceStartO, HordeStartLoc, HordeStartO, StartMaxDist, Weight, ScriptName FROM battleground_template");

    if (!result)
    {
        LOG_ERROR("bg.battleground", ">> Loaded 0 battlegrounds. DB table `battleground_template` is empty.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        BattlegroundTypeId bgTypeId = static_cast<BattlegroundTypeId>(fields[0].Get<uint32>());

        if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId, nullptr))
            continue;

        // can be overwrite by values from DB
        BattlemasterListEntry const* bl = sBattlemasterListStore.LookupEntry(bgTypeId);
        if (!bl)
        {
            LOG_ERROR("bg.battleground", "Battleground ID {} not found in BattlemasterList.dbc. Battleground not created.", bgTypeId);
            continue;
        }

        BattlegroundTemplate bgTemplate;
        bgTemplate.Id = bgTypeId;
        bgTemplate.MinPlayersPerTeam = fields[1].Get<uint16>();
        bgTemplate.MaxPlayersPerTeam = fields[2].Get<uint16>();
        bgTemplate.MinLevel = fields[3].Get<uint8>();
        bgTemplate.MaxLevel = fields[4].Get<uint8>();
        float dist = fields[9].Get<float>();
        bgTemplate.MaxStartDistSq = dist * dist;
        bgTemplate.Weight = fields[10].Get<uint8>();
        bgTemplate.ScriptId = sObjectMgr->GetScriptId(fields[11].Get<std::string>());
        bgTemplate.BattlemasterEntry = bl;

        if (bgTemplate.MaxPlayersPerTeam == 0 || bgTemplate.MinPlayersPerTeam > bgTemplate.MaxPlayersPerTeam)
        {
            LOG_ERROR("sql.sql", "Table `battleground_template` for id {} contains bad values for MinPlayersPerTeam ({}) and MaxPlayersPerTeam({}).",
                bgTemplate.Id, bgTemplate.MinPlayersPerTeam, bgTemplate.MaxPlayersPerTeam);

            continue;
        }

        if (bgTemplate.MinLevel == 0 || bgTemplate.MaxLevel == 0 || bgTemplate.MinLevel > bgTemplate.MaxLevel)
        {
            LOG_ERROR("sql.sql", "Table `battleground_template` for id {} has bad values for LevelMin ({}) and LevelMax({})",
                bgTemplate.Id, bgTemplate.MinLevel, bgTemplate.MaxLevel);
            continue;
        }

        if (bgTemplate.Id != BATTLEGROUND_AA && bgTemplate.Id != BATTLEGROUND_RB)
        {
            uint32 startId = fields[5].Get<uint32>();
            if (GraveyardStruct const* start = sGraveyard->GetGraveyard(startId))
            {
                bgTemplate.StartLocation[TEAM_ALLIANCE].Relocate(start->x, start->y, start->z, fields[6].Get<float>());
            }
            else
            {
                LOG_ERROR("sql.sql", "Table `battleground_template` for id {} contains a non-existing WorldSafeLocs.dbc id {} in field `AllianceStartLoc`. BG not created.", bgTemplate.Id, startId);
                continue;
            }

            startId = fields[7].Get<uint32>();
            if (GraveyardStruct const* start = sGraveyard->GetGraveyard(startId))
            {
                bgTemplate.StartLocation[TEAM_HORDE].Relocate(start->x, start->y, start->z, fields[8].Get<float>());
            }
            else
            {
                LOG_ERROR("sql.sql", "Table `battleground_template` for id {} contains a non-existing WorldSafeLocs.dbc id {} in field `HordeStartLoc`. BG not created.", bgTemplate.Id, startId);
                continue;
            }
        }

        if (!CreateBattleground(&bgTemplate))
            continue;

        _battlegroundTemplates[bgTypeId] = bgTemplate;

        if (bgTemplate.BattlemasterEntry->mapid[1] == -1) // in this case we have only one mapId
            _battlegroundMapTemplates[bgTemplate.BattlemasterEntry->mapid[0]] = &_battlegroundTemplates[bgTypeId];

    } while (result->NextRow());

    LOG_INFO("server.loading", ">> Loaded {} battlegrounds in {} ms", _battlegroundTemplates.size(), GetMSTimeDiffToNow(oldMSTime));
    LOG_INFO("server.loading", " ");
}

void BattlegroundMgr::InitAutomaticArenaPointDistribution()
{
    if (!sWorld->getBoolConfig(CONFIG_ARENA_AUTO_DISTRIBUTE_POINTS))
        return;

    Seconds wstime = Seconds(sWorldState->getWorldState(WORLD_STATE_CUSTOM_ARENA_DISTRIBUTION_TIME));
    Seconds curtime = GameTime::GetGameTime();

    LOG_INFO("server.loading", "Initializing Automatic Arena Point Distribution");

    if (wstime < curtime)
    {
        m_NextAutoDistributionTime = curtime; // reset will be called in the next update
        LOG_INFO("server.loading", "Next arena point distribution time in the past, reseting it now.");
    }
    else
    {
        m_NextAutoDistributionTime = wstime;
    }

    LOG_INFO("server.loading", "Automatic Arena Point Distribution initialized.");
}

/// SMSG_BATTLEFIELD_LIST, 3.4.3 (handler FUN_1419b3070): PackedGuid128 BattlemasterGuid, u32 Verification (echoed by
/// CMSG_BATTLEMASTER_JOIN), u32 BattlemasterListID, u8 MinLevel, u8 MaxLevel, u32 count, the client instance ids, bits
/// PvpAnywhere (0 opens the battlemaster window on the NPC), HasRandomWinToday, flush. No rewards any more.
void BattlegroundMgr::BuildBattlegroundListPacket(WorldPacket* data, ObjectGuid guid, Player* player, BattlegroundTypeId bgTypeId, uint8 /*fromWhere*/)
{
    if (!player)
        return;

    std::vector<uint32> instances;
    PvPDifficultyEntry const* bracketEntry = nullptr;
    auto const& it = bgDataStore.find(bgTypeId);
    if (it != bgDataStore.end() && !it->second._Battlegrounds.empty())
    {
        bracketEntry = GetBattlegroundBracketByLevel(it->second._Battlegrounds.begin()->second->GetMapId(), player->GetLevel());
        if (bracketEntry && bgTypeId != BATTLEGROUND_AA)
            for (auto const& itr : it->second._ClientBattlegroundIds[bracketEntry->GetBracketId()])
                instances.push_back(itr);
    }

    data->Initialize(SMSG_BATTLEFIELD_LIST, 18 + 14 + 4 * instances.size() + 1);
    Guid128::Write(*data, guid);
    *data << uint32(0);                                     // Verification
    *data << uint32(bgTypeId);
    *data << uint8(bracketEntry ? bracketEntry->minLevel : 0);
    *data << uint8(bracketEntry ? bracketEntry->maxLevel : 0);
    *data << uint32(instances.size());
    for (uint32 instanceId : instances)
        *data << uint32(instanceId);
    data->WriteBit(guid.IsEmpty());                         // PvpAnywhere
    data->WriteBit(player->GetRandomWinner());              // HasRandomWinToday
    data->FlushBits();
}

bool BattlegroundMgr::SendToBattleground(Player* player, uint32 instanceId, BattlegroundTypeId bgTypeId)
{
    if (Battleground* bg = GetBattleground(instanceId, bgTypeId))
    {
        uint32 mapid = bg->GetMapId();
        Position const* pos = bg->GetTeamStartPosition(player->GetBgTeamId());

        LOG_DEBUG("bg.battleground", "BattlegroundMgr::SendToBattleground: Sending {} to map {}, {} (bgType {})", player->GetName(), mapid, pos->ToString(), bgTypeId);
        return player->TeleportTo(mapid, pos->GetPositionX(), pos->GetPositionY(), pos->GetPositionZ(), pos->GetOrientation());
    }

    LOG_ERROR("bg.battleground", "BattlegroundMgr::SendToBattleground: Instance {} (bgType {}) not found while trying to teleport player {}", instanceId, bgTypeId, player->GetName());
    return false;
}

void BattlegroundMgr::SendAreaSpiritHealerQueryOpcode(Player* player, Battleground* bg, ObjectGuid guid)
{
    WorldPacket data(SMSG_AREA_SPIRIT_HEALER_TIME, 12);
    uint32 time_ = RESURRECTION_INTERVAL - bg->GetLastResurrectTime();      // resurrect every X seconds
    if (time_ == uint32(-1))
        time_ = 0;
    // 3.4.3 (FUN_1406b5650): PackedGuid128 HealerGuid, u32 TimeLeft (ms)
    Guid128::Write(data, guid);
    data << uint32(time_);
    player->SendDirectMessage(&data);
}

bool BattlegroundMgr::IsArenaType(BattlegroundTypeId bgTypeId)
{
    return bgTypeId == BATTLEGROUND_AA
           || bgTypeId == BATTLEGROUND_BE
           || bgTypeId == BATTLEGROUND_NA
           || bgTypeId == BATTLEGROUND_DS
           || bgTypeId == BATTLEGROUND_RV
           || bgTypeId == BATTLEGROUND_RL;
}

BattlegroundQueueTypeId BattlegroundMgr::BGQueueTypeId(BattlegroundTypeId bgTypeId, uint8 arenaType)
{
    uint32 queueTypeID = BATTLEGROUND_QUEUE_NONE;

    if (arenaType)
    {
        if (BattlegroundMgr::ArenaTypeToQueue.find(arenaType) != BattlegroundMgr::ArenaTypeToQueue.end())
        {
            queueTypeID = BattlegroundMgr::ArenaTypeToQueue.at(arenaType);
        }

        sScriptMgr->OnArenaTypeIDToQueueID(bgTypeId, arenaType, queueTypeID);

        return static_cast<BattlegroundQueueTypeId>(queueTypeID);
    }

    if (BattlegroundMgr::bgToQueue.find(bgTypeId) != BattlegroundMgr::bgToQueue.end())
    {
        queueTypeID = BattlegroundMgr::bgToQueue.at(bgTypeId);
    }

    return static_cast<BattlegroundQueueTypeId>(queueTypeID);
}

BattlegroundTypeId BattlegroundMgr::BGTemplateId(BattlegroundQueueTypeId bgQueueTypeId)
{
    if (BattlegroundMgr::queueToBg.find(bgQueueTypeId) == BattlegroundMgr::queueToBg.end())
    {
        return BattlegroundTypeId(0);
    }

    return BattlegroundMgr::queueToBg[bgQueueTypeId];
}

uint8 BattlegroundMgr::BGArenaType(BattlegroundQueueTypeId bgQueueTypeId)
{
    uint8 arenaType = ARENA_TYPE_NONE;

    if (BattlegroundMgr::QueueToArenaType.find(bgQueueTypeId) != BattlegroundMgr::QueueToArenaType.end())
    {
        arenaType = BattlegroundMgr::QueueToArenaType.at(bgQueueTypeId);
    }

    sScriptMgr->OnArenaQueueIdToArenaType(bgQueueTypeId, arenaType);

    return arenaType;
}

void BattlegroundMgr::ToggleTesting()
{
    if (sWorld->getBoolConfig(CONFIG_DEBUG_BATTLEGROUND))
    {
        m_Testing = true;
        ChatHandler(nullptr).SendWorldText(LANG_DEBUG_BG_CONF);
    }
    else
    {
        m_Testing = !m_Testing;
        ChatHandler(nullptr).SendWorldText(m_Testing ? LANG_DEBUG_BG_ON : LANG_DEBUG_BG_OFF);
    }
}

void BattlegroundMgr::ToggleArenaTesting()
{
    if (sWorld->getBoolConfig(CONFIG_DEBUG_ARENA))
    {
        m_ArenaTesting = true;
        ChatHandler(nullptr).SendWorldText(LANG_DEBUG_ARENA_CONF);
    }
    else
    {
        m_ArenaTesting = !m_ArenaTesting;
        ChatHandler(nullptr).SendWorldText(m_ArenaTesting ? LANG_DEBUG_ARENA_ON : LANG_DEBUG_ARENA_OFF);
    }
}

void BattlegroundMgr::SetHolidayWeekends(uint32 mask)
{
    for (uint32 bgtype = 1; bgtype < MAX_BATTLEGROUND_TYPE_ID; ++bgtype)
    {
        if (bgtype == BATTLEGROUND_RB)
            continue;
        if (Battleground* bgt = GetBattlegroundTemplate(BattlegroundTypeId(bgtype)))
            bgt->SetHoliday(mask & (1 << bgtype));
    }
}

void BattlegroundMgr::ScheduleQueueUpdate(uint32 arenaMatchmakerRating, uint8 arenaType, BattlegroundQueueTypeId bgQueueTypeId, BattlegroundTypeId bgTypeId, BattlegroundBracketId bracket_id)
{
    //This method must be atomic, @todo add mutex
    //we will use only 1 number created of bgTypeId and bracket_id
    uint64 const scheduleId = ((uint64)arenaMatchmakerRating << 32) | ((uint64)arenaType << 24) | ((uint64)bgQueueTypeId << 16) | ((uint64)bgTypeId << 8) | (uint64)bracket_id;
    if (std::find(m_QueueUpdateScheduler.begin(), m_QueueUpdateScheduler.end(), scheduleId) == m_QueueUpdateScheduler.end())
        m_QueueUpdateScheduler.emplace_back(scheduleId);
}

uint32 BattlegroundMgr::GetMaxRatingDifference() const
{
    uint32 diff = sWorld->getIntConfig(CONFIG_ARENA_MAX_RATING_DIFFERENCE);

    if (diff == 0)
    {
        diff = 5000;
    }

    return diff;
}

uint32 BattlegroundMgr::GetRatingDiscardTimer() const
{
    return sWorld->getIntConfig(CONFIG_ARENA_RATING_DISCARD_TIMER);
}

uint32 BattlegroundMgr::GetPrematureFinishTime() const
{
    return sWorld->getIntConfig(CONFIG_BATTLEGROUND_PREMATURE_FINISH_TIMER);
}

void BattlegroundMgr::LoadBattleMastersEntry()
{
    uint32 oldMSTime = getMSTime();

    mBattleMastersMap.clear();                                  // need for reload case

    QueryResult result = WorldDatabase.Query("SELECT entry, bg_template FROM battlemaster_entry");

    if (!result)
    {
        LOG_WARN("server.loading", ">> Loaded 0 battlemaster entries. DB table `battlemaster_entry` is empty!");
        LOG_INFO("server.loading", " ");
        return;
    }

    uint32 count = 0;

    do
    {
        ++count;

        Field* fields = result->Fetch();

        uint32 entry = fields[0].Get<uint32>();
        if (CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(entry))
        {
            if ((cInfo->npcflag & UNIT_NPC_FLAG_BATTLEMASTER) == 0)
                LOG_ERROR("sql.sql", "Creature (Entry: {}) listed in `battlemaster_entry` is not a battlemaster.", entry);
        }
        else
        {
            LOG_ERROR("sql.sql", "Creature (Entry: {}) listed in `battlemaster_entry` does not exist.", entry);
            continue;
        }

        uint32 bgTypeId  = fields[1].Get<uint32>();
        if (!sBattlemasterListStore.LookupEntry(bgTypeId))
        {
            LOG_ERROR("sql.sql", "Table `battlemaster_entry` contain entry {} for not existed battleground type {}, ignored.", entry, bgTypeId);
            continue;
        }

        mBattleMastersMap[entry] = BattlegroundTypeId(bgTypeId);
    } while (result->NextRow());

    CheckBattleMasters();

    LOG_INFO("server.loading", ">> Loaded {} battlemaster entries in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    LOG_INFO("server.loading", " ");
}

void BattlegroundMgr::CheckBattleMasters()
{
    CreatureTemplateContainer const* ctc = sObjectMgr->GetCreatureTemplates();
    for (CreatureTemplateContainer::const_iterator itr = ctc->begin(); itr != ctc->end(); ++itr)
    {
        if ((itr->second.npcflag & UNIT_NPC_FLAG_BATTLEMASTER) && mBattleMastersMap.find(itr->second.Entry) == mBattleMastersMap.end())
        {
            LOG_ERROR("sql.sql", "CreatureTemplate (Entry: {}) has UNIT_NPC_FLAG_BATTLEMASTER but no data in `battlemaster_entry` table. Removing flag!", itr->second.Entry);
            const_cast<CreatureTemplate*>(&itr->second)->npcflag &= ~UNIT_NPC_FLAG_BATTLEMASTER;
        }
    }
}

HolidayIds BattlegroundMgr::BGTypeToWeekendHolidayId(BattlegroundTypeId bgTypeId)
{
    switch (bgTypeId)
    {
        case BATTLEGROUND_AV:
            return HOLIDAY_CALL_TO_ARMS_AV;
        case BATTLEGROUND_EY:
            return HOLIDAY_CALL_TO_ARMS_EY;
        case BATTLEGROUND_WS:
            return HOLIDAY_CALL_TO_ARMS_WS;
        case BATTLEGROUND_SA:
            return HOLIDAY_CALL_TO_ARMS_SA;
        case BATTLEGROUND_AB:
            return HOLIDAY_CALL_TO_ARMS_AB;
        case BATTLEGROUND_IC:
            return HOLIDAY_CALL_TO_ARMS_IC;
        default:
            return HOLIDAY_NONE;
    }
}

BattlegroundTypeId BattlegroundMgr::WeekendHolidayIdToBGType(HolidayIds holiday)
{
    switch (holiday)
    {
        case HOLIDAY_CALL_TO_ARMS_AV:
            return BATTLEGROUND_AV;
        case HOLIDAY_CALL_TO_ARMS_EY:
            return BATTLEGROUND_EY;
        case HOLIDAY_CALL_TO_ARMS_WS:
            return BATTLEGROUND_WS;
        case HOLIDAY_CALL_TO_ARMS_SA:
            return BATTLEGROUND_SA;
        case HOLIDAY_CALL_TO_ARMS_AB:
            return BATTLEGROUND_AB;
        case HOLIDAY_CALL_TO_ARMS_IC:
            return BATTLEGROUND_IC;
        default:
            return BATTLEGROUND_TYPE_NONE;
    }
}

bool BattlegroundMgr::IsBGWeekend(BattlegroundTypeId bgTypeId)
{
    return IsHolidayActive(BGTypeToWeekendHolidayId(bgTypeId));
}

BattlegroundTypeId BattlegroundMgr::GetRandomBG(BattlegroundTypeId bgTypeId, uint32 minLevel)
{
    if (BattlegroundTemplate const* bgTemplate = GetBattlegroundTemplateByTypeId(bgTypeId))
    {
        std::vector<BattlegroundTypeId> ids;
        ids.reserve(16);
        std::vector<double> weights;
        weights.reserve(16);

        for (int32 mapId : bgTemplate->BattlemasterEntry->mapid)
        {
            if (mapId == -1)
                break;

            if (BattlegroundTemplate const* bg = GetBattlegroundTemplateByMapId(mapId))
            {
                if (bg->MinLevel <= minLevel)
                {
                    ids.push_back(bg->Id);
                    weights.push_back(bg->Weight);
                }
            }
        }

        return *Acore::Containers::SelectRandomWeightedContainerElement(ids, weights);
    }

    return BATTLEGROUND_TYPE_NONE;
}

BGFreeSlotQueueContainer& BattlegroundMgr::GetBGFreeSlotQueueStore(BattlegroundTypeId bgTypeId)
{
    return bgDataStore[bgTypeId].BGFreeSlotQueue;
}

void BattlegroundMgr::AddToBGFreeSlotQueue(BattlegroundTypeId bgTypeId, Battleground* bg)
{
    bgDataStore[bgTypeId].BGFreeSlotQueue.push_back(bg);
}

void BattlegroundMgr::RemoveFromBGFreeSlotQueue(BattlegroundTypeId bgTypeId, uint32 instanceId)
{
    BGFreeSlotQueueContainer& queues = bgDataStore[bgTypeId].BGFreeSlotQueue;
    for (BGFreeSlotQueueContainer::iterator itr = queues.begin(); itr != queues.end(); ++itr)
        if ((*itr)->GetInstanceID() == instanceId)
        {
            queues.erase(itr);
            return;
        }
}

void BattlegroundMgr::AddBattleground(Battleground* bg)
{
    if (bg)
        bgDataStore[bg->GetBgTypeID()]._Battlegrounds[bg->GetInstanceID()] = bg;

    sScriptMgr->OnBattlegroundCreate(bg);
}

void BattlegroundMgr::RemoveBattleground(BattlegroundTypeId bgTypeId, uint32 instanceId)
{
    bgDataStore[bgTypeId]._Battlegrounds.erase(instanceId);
}

// init/update unordered_map
// Battlegrounds
std::unordered_map<int, BattlegroundQueueTypeId> BattlegroundMgr::bgToQueue =
{
    { BATTLEGROUND_AV, BATTLEGROUND_QUEUE_AV},
    { BATTLEGROUND_WS, BATTLEGROUND_QUEUE_WS},
    { BATTLEGROUND_AB, BATTLEGROUND_QUEUE_AB},
    { BATTLEGROUND_EY, BATTLEGROUND_QUEUE_EY},
    { BATTLEGROUND_SA, BATTLEGROUND_QUEUE_SA},
    { BATTLEGROUND_IC, BATTLEGROUND_QUEUE_IC},
    { BATTLEGROUND_RB, BATTLEGROUND_QUEUE_RB},
    // Arena Battlegrounds
    { BATTLEGROUND_NA, BattlegroundQueueTypeId(0)},        // Nagrand Arena
    { BATTLEGROUND_BE, BattlegroundQueueTypeId(0)},        // Blade's Edge Arena
    { BATTLEGROUND_AA, BattlegroundQueueTypeId(0)},        // All Arena
    { BATTLEGROUND_RL, BattlegroundQueueTypeId(0)},        // Ruins of Lordaernon
    { BATTLEGROUND_DS, BattlegroundQueueTypeId(0)},        // Dalaran Sewer
    { BATTLEGROUND_RV, BattlegroundQueueTypeId(0)},        // Ring of Valor
};

std::unordered_map<int, BattlegroundTypeId> BattlegroundMgr::queueToBg =
{
    { BATTLEGROUND_QUEUE_NONE,  BATTLEGROUND_TYPE_NONE },
    { BATTLEGROUND_QUEUE_AV,    BATTLEGROUND_AV },
    { BATTLEGROUND_QUEUE_WS,    BATTLEGROUND_WS },
    { BATTLEGROUND_QUEUE_AB,    BATTLEGROUND_AB },
    { BATTLEGROUND_QUEUE_EY,    BATTLEGROUND_EY },
    { BATTLEGROUND_QUEUE_SA,    BATTLEGROUND_SA },
    { BATTLEGROUND_QUEUE_IC,    BATTLEGROUND_IC },
    { BATTLEGROUND_QUEUE_RB,    BATTLEGROUND_RB },
    { BATTLEGROUND_QUEUE_2v2,   BATTLEGROUND_AA },
    { BATTLEGROUND_QUEUE_3v3,   BATTLEGROUND_AA },
    { BATTLEGROUND_QUEUE_5v5,   BATTLEGROUND_AA },
};

std::unordered_map<int, Battleground*> BattlegroundMgr::bgtypeToBattleground =
{
    { BATTLEGROUND_AV, new BattlegroundAV },
    { BATTLEGROUND_WS, new BattlegroundWS },
    { BATTLEGROUND_AB, new BattlegroundAB },
    { BATTLEGROUND_NA, new BattlegroundNA },
    { BATTLEGROUND_BE, new BattlegroundBE },
    { BATTLEGROUND_EY, new BattlegroundEY },
    { BATTLEGROUND_RL, new BattlegroundRL },
    { BATTLEGROUND_SA, new BattlegroundSA },
    { BATTLEGROUND_DS, new BattlegroundDS },
    { BATTLEGROUND_RV, new BattlegroundRV },
    { BATTLEGROUND_IC, new BattlegroundIC },
    { BATTLEGROUND_AA, new Battleground },
    { BATTLEGROUND_RB, new Battleground },
};

std::unordered_map<int, bgRef> BattlegroundMgr::bgTypeToTemplate =
{
    { BATTLEGROUND_AV, [](Battleground * bg_t) -> Battleground* { return new BattlegroundAV(*(BattlegroundAV*)bg_t); } },
    { BATTLEGROUND_WS, [](Battleground * bg_t) -> Battleground* { return new BattlegroundWS(*(BattlegroundWS*)bg_t); } },
    { BATTLEGROUND_AB, [](Battleground * bg_t) -> Battleground* { return new BattlegroundAB(*(BattlegroundAB*)bg_t); } },
    { BATTLEGROUND_NA, [](Battleground * bg_t) -> Battleground* { return new BattlegroundNA(*(BattlegroundNA*)bg_t); } },
    { BATTLEGROUND_BE, [](Battleground * bg_t) -> Battleground* { return new BattlegroundBE(*(BattlegroundBE*)bg_t); } },
    { BATTLEGROUND_EY, [](Battleground * bg_t) -> Battleground* { return new BattlegroundEY(*(BattlegroundEY*)bg_t); } },
    { BATTLEGROUND_RL, [](Battleground * bg_t) -> Battleground* { return new BattlegroundRL(*(BattlegroundRL*)bg_t); } },
    { BATTLEGROUND_SA, [](Battleground * bg_t) -> Battleground* { return new BattlegroundSA(*(BattlegroundSA*)bg_t); } },
    { BATTLEGROUND_DS, [](Battleground * bg_t) -> Battleground* { return new BattlegroundDS(*(BattlegroundDS*)bg_t); } },
    { BATTLEGROUND_RV, [](Battleground * bg_t) -> Battleground* { return new BattlegroundRV(*(BattlegroundRV*)bg_t); } },
    { BATTLEGROUND_IC, [](Battleground * bg_t) -> Battleground* { return new BattlegroundIC(*(BattlegroundIC*)bg_t); } },

    { BATTLEGROUND_RB, [](Battleground * bg_t) -> Battleground* { return new Battleground(*bg_t); }, },
    { BATTLEGROUND_AA, [](Battleground * bg_t) -> Battleground* { return new Battleground(*bg_t); }, },
};

std::unordered_map<int, bgMapRef> BattlegroundMgr::getBgFromMap = {};

std::unordered_map<int, bgTypeRef> BattlegroundMgr::getBgFromTypeID =
{
    {
        BATTLEGROUND_RB,
        [](WorldPacket * data, Battleground::BattlegroundScoreMap::const_iterator itr2, Battleground * bg)
        {
            if (BattlegroundMgr::getBgFromMap.find(bg->GetMapId()) == BattlegroundMgr::getBgFromMap.end()) // this should not happen
            {
                *data << uint32(0);
            }
            else
            {
                BattlegroundMgr::getBgFromMap[bg->GetMapId()](data, itr2);
            }
        }
    }
};

std::unordered_map<uint32, BattlegroundQueueTypeId> BattlegroundMgr::ArenaTypeToQueue =
{
    { ARENA_TYPE_2v2, BATTLEGROUND_QUEUE_2v2 },
    { ARENA_TYPE_3v3, BATTLEGROUND_QUEUE_3v3 },
    { ARENA_TYPE_5v5, BATTLEGROUND_QUEUE_5v5 }
};

std::unordered_map<uint32, ArenaType> BattlegroundMgr::QueueToArenaType =
{
    { BATTLEGROUND_QUEUE_2v2, ARENA_TYPE_2v2 },
    { BATTLEGROUND_QUEUE_3v3, ARENA_TYPE_3v3 },
    { BATTLEGROUND_QUEUE_5v5, ARENA_TYPE_5v5 }
};
