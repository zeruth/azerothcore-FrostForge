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
#include "GameTime.h"
#include "Group.h"
#include "ItemPackets.h"
#include "LFGMgr.h"
#include "LFGPackets.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <mutex>
#include <unordered_map>

namespace
{
    /// The 3.4.3 client keys a queue record on its whole RideTicket (FUN_141abdfe0: requester, id, type and
    /// time), so every message about one queue must repeat the same ticket. AC has no tickets: one is made per
    /// player when a queue message first names it, and dropped once a message has told the client the queue is
    /// gone (Joined 0), so the next queue is a new record.
    struct LfgTicket
    {
        uint32 Id;
        int64 Time;
    };

    std::mutex LfgTicketLock;
    std::unordered_map<ObjectGuid, LfgTicket> LfgTickets;
    uint32 LfgNextTicketId = 0;

    LfgTicket GetLfgTicket(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(LfgTicketLock);
        auto itr = LfgTickets.find(guid);
        if (itr == LfgTickets.end())
            itr = LfgTickets.emplace(guid, LfgTicket{ ++LfgNextTicketId, int64(GameTime::GetGameTime().count()) }).first;
        return itr->second;
    }

    void ReleaseLfgTicket(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(LfgTicketLock);
        LfgTickets.erase(guid);
    }

    /// RideTicket, 3.4.3 (FUN_14070f950): PackedGuid128 RequesterGuid, u32 Id, u32 Type (2: the dungeon finder),
    /// i64 Time, bit, flush
    void WriteRideTicket(ByteBuffer& data, ObjectGuid guid)
    {
        LfgTicket ticket = GetLfgTicket(guid);
        Guid128::Write(data, guid);
        data << uint32(ticket.Id);
        data << uint32(2);
        data << int64(ticket.Time);
        data.WriteBit(false);
        data.FlushBits();
    }

    /// AC's LfgUpdateType is 3.3.5's numbering; 3.4.3 uses the retail one (handler FUN_141ab9a40)
    uint8 ClientLfgUpdateType(lfg::LfgUpdateType type)
    {
        switch (type)
        {
            case lfg::LFG_UPDATETYPE_LEADER_UNK1:          return 1;
            case lfg::LFG_UPDATETYPE_ROLECHECK_ABORTED:    return 6;
            case lfg::LFG_UPDATETYPE_JOIN_QUEUE:           return 13;
            case lfg::LFG_UPDATETYPE_ROLECHECK_FAILED:     return 7;
            case lfg::LFG_UPDATETYPE_REMOVED_FROM_QUEUE:   return 8;
            case lfg::LFG_UPDATETYPE_PROPOSAL_FAILED:      return 9;
            case lfg::LFG_UPDATETYPE_PROPOSAL_DECLINED:    return 10;
            case lfg::LFG_UPDATETYPE_GROUP_FOUND:          return 11;
            case lfg::LFG_UPDATETYPE_ADDED_TO_QUEUE:       return 13;
            case lfg::LFG_UPDATETYPE_PROPOSAL_BEGIN:       return 15;
            case lfg::LFG_UPDATETYPE_UPDATE_STATUS:        return 16;
            case lfg::LFG_UPDATETYPE_GROUP_MEMBER_OFFLINE: return 17;
            case lfg::LFG_UPDATETYPE_GROUP_DISBAND_UNK16:  return 18;
            default:                                       return 0;
        }
    }

    /// AC's LfgJoinResult is 3.3.5's numbering; 3.4.3 shows the retail codes (table at 0x142d30340)
    uint8 ClientLfgJoinResult(lfg::LfgJoinResult result)
    {
        switch (result)
        {
            case lfg::LFG_JOIN_OK:                    return 0x00;
            case lfg::LFG_JOIN_FAILED:                return 0x2E;
            case lfg::LFG_JOIN_GROUPFULL:             return 0x1F;
            case lfg::LFG_JOIN_INTERNAL_ERROR:        return 0x21;
            case lfg::LFG_JOIN_NOT_MEET_REQS:         return 0x22;
            case lfg::LFG_JOIN_PARTY_NOT_MEET_REQS:   return 0x22;
            case lfg::LFG_JOIN_MIXED_RAID_DUNGEON:    return 0x23;
            case lfg::LFG_JOIN_MULTI_REALM:           return 0x24;
            case lfg::LFG_JOIN_DISCONNECTED:          return 0x25;
            case lfg::LFG_JOIN_PARTY_INFO_FAILED:     return 0x26;
            case lfg::LFG_JOIN_DUNGEON_INVALID:       return 0x27;
            case lfg::LFG_JOIN_DESERTER:              return 0x28;
            case lfg::LFG_JOIN_PARTY_DESERTER:        return 0x29;
            case lfg::LFG_JOIN_RANDOM_COOLDOWN:       return 0x2A;
            case lfg::LFG_JOIN_PARTY_RANDOM_COOLDOWN: return 0x2B;
            case lfg::LFG_JOIN_TOO_MUCH_MEMBERS:      return 0x2C;
            case lfg::LFG_JOIN_USING_BG_SYSTEM:       return 0x2D;
            default:                                  return 0x21;
        }
    }

    /// LFGBlackList, 3.4.3 (FUN_140714470): bit HasPlayerGuid, flush, u32 count, [PackedGuid128], count x {u32 Slot,
    /// u32 Reason, u32 SubReason1, u32 SubReason2, u32 SoftLock}
    void WriteLfgBlackList(ByteBuffer& data, ObjectGuid const* playerGuid, lfg::LfgLockMap const& lock)
    {
        data.WriteBit(playerGuid != nullptr);
        data.FlushBits();
        data << uint32(lock.size());
        if (playerGuid)
            Guid128::Write(data, *playerGuid);
        for (auto const& [dungeon, status] : lock)
        {
            data << uint32(dungeon);                        // the dungeon entry (id + type)
            data << uint32(status);                         // LfgLockStatusType
            data << uint32(0) << uint32(0) << uint32(0);
        }
    }

    /// LfgPlayerQuestReward, 3.4.3 (FUN_1407147b0): u8 Mask, u32 money, u32 XP, u32 item, currency and bonus
    /// currency counts, the items {u32 ItemID, u32 Quantity}, the currencies, bits HasRewardSpellID, HasUnused1,
    /// HasUnused2, HasHonor, flush
    void WriteLfgQuestReward(ByteBuffer& data, Player* player, Quest const* quest)
    {
        uint32 money = 0, xp = 0;
        std::vector<std::pair<uint32, uint32>> items;
        if (quest)
        {
            uint8 playerLevel = player->GetLevel();
            uint8 playerLevelForXP = playerLevel;
            sScriptMgr->OnPlayerBeforeGetLevelForXPGain(player, playerLevelForXP);
            money = quest->GetRewOrReqMoney(playerLevel);
            if (playerLevelForXP < player->GetUInt32Value(PLAYER_FIELD_MAX_LEVEL))
                xp = quest->XPValue(playerLevelForXP);
            for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
                if (uint32 itemId = quest->RewardItemId[i])
                    items.emplace_back(itemId, quest->RewardItemIdCount[i]);
        }

        data << uint8(0);                                   // Mask
        data << uint32(money);
        data << uint32(xp);
        data << uint32(items.size());
        data << uint32(0);                                  // currencies
        data << uint32(0);                                  // bonus currencies
        for (auto const& [itemId, count] : items)
            data << uint32(itemId) << uint32(count);
        data.WriteBit(false);                               // RewardSpellID
        data.WriteBit(false);
        data.WriteBit(false);
        data.WriteBit(false);                               // Honor
        data.FlushBits();
    }
}

void WorldSession::HandleLfgJoinOpcode(WorldPackets::LFG::LFGJoin& packet)
{
    if (!sLFGMgr->isOptionEnabled(lfg::LFG_OPTION_ENABLE_DUNGEON_FINDER | lfg::LFG_OPTION_ENABLE_RAID_BROWSER | lfg::LFG_OPTION_ENABLE_SEASONAL_BOSSES) ||
        (GetPlayer()->GetGroup() && GetPlayer()->GetGroup()->GetLeaderGUID() != GetPlayer()->GetGUID() &&
         (GetPlayer()->GetGroup()->GetMembersCount() == MAXGROUPSIZE || !GetPlayer()->GetGroup()->isLFGGroup())))
        return;

    if (packet.Slots.empty())
    {
        LOG_DEBUG("lfg", "CMSG_DF_JOIN {} no dungeons selected", GetPlayerInfo());
        return;
    }

    lfg::LfgDungeonSet newDungeons;
    for (uint32 slot : packet.Slots)
    {
        uint32 dungeon = slot & 0x00FFFFFF;                             // remove the type from the dungeon entry
        if (sLFGDungeonStore.LookupEntry(dungeon))
            newDungeons.insert(dungeon);
    }

    LOG_DEBUG("network", "CMSG_DF_JOIN [{}] roles: {}, Dungeons: {}",
                 GetPlayerInfo(), packet.Roles, newDungeons.size());

    sLFGMgr->JoinLfg(GetPlayer(), uint8(packet.Roles), newDungeons, packet.Comment);
    GetPlayer()->UpdateLFGChannel();
}

void WorldSession::HandleLfgLeaveOpcode(WorldPackets::LFG::LFGLeave& /*packet*/)
{
    Group* group = GetPlayer()->GetGroup();
    ObjectGuid guid = GetPlayer()->GetGUID();
    ObjectGuid gguid = group ? group->GetGUID() : guid;

    LOG_DEBUG("network", "CMSG_DF_LEAVE [{}] in group: {}", guid.ToString(), group ? 1 : 0);

    // Check cheating - only leader can leave the queue
    if (!group || group->GetLeaderGUID() == guid)
    {
        sLFGMgr->LeaveLfg(sLFGMgr->GetState(guid) == lfg::LFG_STATE_RAIDBROWSER ? guid : gguid);
        sLFGMgr->LeaveAllLfgQueues(guid, true, group ? group->GetGUID() : ObjectGuid::Empty);
    }
    GetPlayer()->UpdateLFGChannel();
}

void WorldSession::HandleLfgProposalResultOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_140766af0, Lua AcceptProposal / RejectProposal): RideTicket, u64 InstanceID, u32
    // ProposalID, bit Accepted, flush
    WorldPackets::LFG::RideTicket ticket;
    WorldPackets::LFG::ReadRideTicket(recvData, ticket);
    recvData.read_skip<uint64>();
    uint32 proposalID = recvData.read<uint32>();
    bool accept = recvData.ReadBit();

    LOG_DEBUG("network", "CMSG_DF_PROPOSAL_RESPONSE [{}] proposal: {} accept: {}", GetPlayer()->GetGUID().ToString(), proposalID, accept ? 1 : 0);
    sLFGMgr->UpdateProposal(proposalID, GetPlayer()->GetGUID(), accept);
}

void WorldSession::HandleLfgSetRolesOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_140766bd0, Lua CompleteLFGRoleCheck): bit HasPartyIndex, flush, u8 RolesDesired, [u8]
    recvData.ReadBit();
    recvData.ResetBitReader();
    uint8 roles = recvData.read<uint8>();
    recvData.rfinish();
    ObjectGuid guid = GetPlayer()->GetGUID();
    Group* group = GetPlayer()->GetGroup();
    if (!group)
    {
        LOG_DEBUG("network", "CMSG_DF_SET_ROLES [{}] Not in group", guid.ToString());
        return;
    }
    ObjectGuid gguid = group->GetGUID();
    LOG_DEBUG("network", "CMSG_DF_SET_ROLES: Group [{}], Player [{}], Roles: {}", gguid.ToString(), guid.ToString(), roles);
    sLFGMgr->UpdateRoleCheck(gguid, guid, roles);
}

void WorldSession::HandleLfgSetCommentOpcode(WorldPacket&  recvData)
{
    std::string comment;
    recvData >> comment;
    ObjectGuid guid = GetPlayer()->GetGUID();
    LOG_DEBUG("network", "CMSG_LFG_SET_COMMENT [{}] comment: {}", guid.ToString(), comment);

    sLFGMgr->SetComment(GetPlayer()->GetGUID(), comment);
    sLFGMgr->LfrSetComment(GetPlayer(), comment);
}

void WorldSession::HandleLfgSetBootVoteOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_140766800, Lua SetLFGBootVote): bit Vote, flush
    bool agree = recvData.ReadBit();

    ObjectGuid guid = GetPlayer()->GetGUID();
    LOG_DEBUG("network", "CMSG_DF_BOOT_PLAYER_VOTE [{}] agree: {}", guid.ToString(), agree ? 1 : 0);
    sLFGMgr->UpdateBoot(guid, agree);
}

void WorldSession::HandleLfgTeleportOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_140766c50, Lua LFGTeleport): bit TeleportOut, flush
    bool out = recvData.ReadBit();

    LOG_DEBUG("network", "CMSG_DF_TELEPORT [{}] out: {}", GetPlayer()->GetGUID().ToString(), out ? 1 : 0);
    sLFGMgr->TeleportPlayer(GetPlayer(), out);
}

/// SMSG_LFG_PLAYER_INFO, 3.4.3 (FUN_1406c3920, handler FUN_141aba780): u32 dungeon count, LFGBlackList (the locks,
/// no player GUID), then per random dungeon LfgPlayerDungeonInfo (FUN_1407145b0): u32 Slot, 14 x u32 (completion,
/// specific, overall and purse counters, quantity, completed and encounter masks), u32 shortage reward count,
/// bits FirstReward, ShortageEligible, flush, LfgPlayerQuestReward, the shortage rewards
void WorldSession::HandleLfgPlayerLockInfoRequestOpcode(WorldPacket& /*recvData*/)
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    LOG_DEBUG("network", "CMSG_DF_GET_SYSTEM_INFO player [{}]", guid.ToString());

    // Get Random dungeons that can be done at a certain level and expansion
    uint8 level = GetPlayer()->GetLevel();
    lfg::LfgDungeonSet const& randomDungeons =
        sLFGMgr->GetRandomAndSeasonalDungeons(level, GetPlayer()->GetSession()->Expansion());

    // Get player locked Dungeons
    sLFGMgr->InitializeLockedDungeons(GetPlayer(), GetPlayer()->GetGroup()); // pussywizard
    lfg::LfgLockMap const& lock = sLFGMgr->GetLockedDungeons(guid);

    WorldPacket data(SMSG_LFG_PLAYER_INFO, 4 + 5 + lock.size() * 20 + randomDungeons.size() * 100);
    data << uint32(randomDungeons.size());
    WriteLfgBlackList(data, nullptr, lock);

    for (uint32 dungeon : randomDungeons)
    {
        lfg::LfgReward const* reward = sLFGMgr->GetRandomDungeonReward(dungeon, level);
        Quest const* quest = nullptr;
        bool done = false;
        if (reward)
        {
            quest = sObjectMgr->GetQuestTemplate(reward->firstQuest);
            if (quest)
            {
                done = !GetPlayer()->CanRewardQuest(quest, false);
                if (done)
                    quest = sObjectMgr->GetQuestTemplate(reward->otherQuest);
            }
        }

        data << uint32(dungeon);                            // Dungeon Entry (id + type)
        for (uint8 i = 0; i < 14; ++i)
            data << uint32(0);
        data << uint32(0);                                  // shortage rewards
        data.WriteBit(!done);                               // FirstReward
        data.WriteBit(false);                               // ShortageEligible
        data.FlushBits();
        WriteLfgQuestReward(data, GetPlayer(), quest);
    }
    SendPacket(&data);
}

/// SMSG_LFG_PARTY_INFO, 3.4.3 (inline, handler FUN_141aba510): u32 count, an LFGBlackList per other member with
/// its GUID
void WorldSession::HandleLfgPartyLockInfoRequestOpcode(WorldPacket&  /*recvData*/)
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    LOG_DEBUG("network", "CMSG_DF_GET_SYSTEM_INFO party [{}]", guid.ToString());

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    // Get the locked dungeons of the other party members
    lfg::LfgLockPartyMap lockMap;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* plrg = itr->GetSource();
        if (!plrg)
            continue;

        ObjectGuid pguid = plrg->GetGUID();
        if (pguid == guid)
            continue;

        sLFGMgr->InitializeLockedDungeons(plrg, group); // pussywizard
        lockMap[pguid] = sLFGMgr->GetLockedDungeons(pguid);
    }

    WorldPacket data(SMSG_LFG_PARTY_INFO, 4 + lockMap.size() * 64);
    data << uint32(lockMap.size());
    for (auto const& [pguid, lock] : lockMap)
        WriteLfgBlackList(data, &pguid, lock);
    SendPacket(&data);
}

void WorldSession::HandleLfgGetStatus(WorldPacket& /*recvData*/)
{
    LOG_DEBUG("lfg", "CMSG_DF_GET_JOIN_STATUS {}", GetPlayerInfo());

    // the 3.4.3 client starts with no queue records: a player who is not using the dungeon finder is told nothing
    ObjectGuid guid = GetPlayer()->GetGUID();
    if (sLFGMgr->GetState(guid) == lfg::LFG_STATE_NONE)
        return;

    lfg::LfgUpdateData updateData = sLFGMgr->GetLfgStatus(guid);
    if (GetPlayer()->GetGroup())
        SendLfgUpdateParty(updateData);
    else
        SendLfgUpdatePlayer(updateData);
}

/// SMSG_LFG_UPDATE_STATUS, 3.4.3 (FUN_1407003f0, handler FUN_141ab9a40): RideTicket, u8 SubType (1 dungeon),
/// u8 Reason (retail LfgUpdateType), u32 slot count, u8 RequestedRoles, u32 suspended player count, u32
/// QueueMapID, the slots, the suspended players, then bits NotifyUI (gates every message), IsParty (compared with
/// "am I in a group"), Joined (0 deletes the client's queue record), LfgJoined, Queued, Unused, flush.
/// 3.3.5's player and party variants are one message.
static void SendLfgUpdateStatus(WorldSession* session, lfg::LfgUpdateData const& updateData, bool party)
{
    Player* player = session->GetPlayer();
    if (!player)
        return;

    bool join = false;
    bool queued = false;
    switch (updateData.updateType)
    {
        case lfg::LFG_UPDATETYPE_JOIN_QUEUE:
        case lfg::LFG_UPDATETYPE_ADDED_TO_QUEUE:                // Rolecheck Success
            join = true;
            queued = true;
            break;
        case lfg::LFG_UPDATETYPE_PROPOSAL_BEGIN:
            join = true;
            break;
        case lfg::LFG_UPDATETYPE_UPDATE_STATUS:
            join = updateData.state != lfg::LFG_STATE_ROLECHECK && updateData.state != lfg::LFG_STATE_NONE;
            queued = updateData.state == lfg::LFG_STATE_QUEUED;
            break;
        default:
            break;
    }

    ObjectGuid guid = player->GetGUID();
    LOG_DEBUG("lfg", "SMSG_LFG_UPDATE_STATUS {} updatetype: {} party {}", session->GetPlayerInfo(), updateData.updateType, party);

    WorldPacket data(SMSG_LFG_UPDATE_STATUS, 64 + updateData.dungeons.size() * 4);
    WriteRideTicket(data, guid);
    data << uint8(1);                                       // SubType: the dungeon finder
    data << uint8(ClientLfgUpdateType(updateData.updateType));
    data << uint32(updateData.dungeons.size());
    data << uint8(sLFGMgr->GetRoles(guid));
    data << uint32(0);                                      // suspended players
    data << uint32(updateData.dungeons.size() == 1 ? sLFGMgr->GetDungeonMapId(guid) : 0);
    for (uint32 dungeon : updateData.dungeons)
        data << uint32(sLFGMgr->GetLFGDungeonEntry(dungeon & 0x00FFFFFF));
    data.WriteBit(true);                                    // NotifyUI
    data.WriteBit(party);                                   // IsParty
    data.WriteBit(join);                                    // Joined
    data.WriteBit(updateData.updateType != lfg::LFG_UPDATETYPE_REMOVED_FROM_QUEUE);   // LfgJoined
    data.WriteBit(queued);                                  // Queued
    data.WriteBit(false);
    data.FlushBits();
    session->SendPacket(&data);

    if (!join)
        ReleaseLfgTicket(guid);
}

void WorldSession::SendLfgUpdatePlayer(lfg::LfgUpdateData const& updateData)
{
    SendLfgUpdateStatus(this, updateData, false);
}

void WorldSession::SendLfgUpdateParty(lfg::LfgUpdateData const& updateData)
{
    SendLfgUpdateStatus(this, updateData, true);
}

/// SMSG_ROLE_CHOSEN, 3.4.3 (inline, handler FUN_141abacb0): PackedGuid128 Player, u8 RoleMask, bit Accepted, flush
void WorldSession::SendLfgRoleChosen(ObjectGuid guid, uint8 roles)
{
    LOG_DEBUG("network", "SMSG_ROLE_CHOSEN [{}] guid: [{}] roles: {}", GetPlayer()->GetGUID().ToString(), guid.ToString(), roles);

    WorldPacket data(SMSG_ROLE_CHOSEN, 18 + 1 + 1);
    Guid128::Write(data, guid);
    data << uint8(roles);
    data.WriteBit(roles > 0);
    data.FlushBits();
    SendPacket(&data);
}

/// SMSG_LFG_ROLE_CHECK_UPDATE, 3.4.3 (FUN_140700210, handler FUN_141ab95d0): u8 PartyIndex, u8 RoleCheckStatus
/// (AC's LfgRoleCheckState), u32 slot count, u32 BgQueueID count, u32 GroupFinderActivityID, u32 member count, the
/// slots, the queue IDs, bits IsBeginning, ShowRoleCheck, flush, per member {PackedGuid128, u8 RolesDesired, u8
/// Level, bit RoleCheckComplete, flush}
void WorldSession::SendLfgRoleCheckUpdate(lfg::LfgRoleCheck const& roleCheck)
{
    lfg::LfgDungeonSet dungeons;
    if (roleCheck.rDungeonId)
        dungeons.insert(roleCheck.rDungeonId);
    else
        dungeons = roleCheck.dungeons;

    LOG_DEBUG("network", "SMSG_LFG_ROLE_CHECK_UPDATE [{}]", GetPlayer()->GetGUID().ToString());
    WorldPacket data(SMSG_LFG_ROLE_CHECK_UPDATE, 18 + dungeons.size() * 4 + roleCheck.roles.size() * 21);

    data << uint8(0);                                       // PartyIndex
    data << uint8(roleCheck.state);
    data << uint32(dungeons.size());
    data << uint32(0);                                      // BgQueueIDs
    data << uint32(0);                                      // GroupFinderActivityID
    data << uint32(roleCheck.roles.size());
    for (uint32 dungeon : dungeons)
        data << uint32(sLFGMgr->GetLFGDungeonEntry(dungeon));
    data.WriteBit(roleCheck.state == lfg::LFG_ROLECHECK_INITIALITING);   // IsBeginning
    data.WriteBit(true);                                    // ShowRoleCheck
    data.FlushBits();

    auto writeMember = [&](ObjectGuid guid, uint8 roles)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(guid);
        Guid128::Write(data, guid);
        data << uint8(roles);
        data << uint8(player ? player->GetLevel() : 0);
        data.WriteBit(roles > 0);                           // RoleCheckComplete
        data.FlushBits();
    };

    // the leader first, as AC always sent it
    if (auto itr = roleCheck.roles.find(roleCheck.leader); itr != roleCheck.roles.end())
        writeMember(itr->first, itr->second);
    for (auto const& [guid, roles] : roleCheck.roles)
        if (guid != roleCheck.leader)
            writeMember(guid, roles);
    SendPacket(&data);
}

/// SMSG_LFG_JOIN_RESULT, 3.4.3 (FUN_1406ffa10, handler FUN_141ab8890): RideTicket, u8 Result (retail codes), u8
/// ResultDetail (the role check state), u32 BlackList count, u32 BlackListNames count, the LFGBlackLists (one per
/// member, with its GUID), the names' 24-bit lengths, the names
void WorldSession::SendLfgJoinResult(lfg::LfgJoinResultData const& joinData)
{
    LOG_DEBUG("network", "SMSG_LFG_JOIN_RESULT [{}] checkResult: {} checkValue: {}", GetPlayer()->GetGUID().ToString(), joinData.result, joinData.state);
    WorldPacket data(SMSG_LFG_JOIN_RESULT, 48 + joinData.lockmap.size() * 64);
    WriteRideTicket(data, GetPlayer()->GetGUID());
    data << uint8(ClientLfgJoinResult(joinData.result));
    data << uint8(joinData.state);
    data << uint32(joinData.lockmap.size());
    data << uint32(0);                                      // names
    for (auto const& [pguid, lock] : joinData.lockmap)
        WriteLfgBlackList(data, &pguid, lock);
    SendPacket(&data);

    if (joinData.result != lfg::LFG_JOIN_OK)
        ReleaseLfgTicket(GetPlayer()->GetGUID());
}

/// SMSG_LFG_QUEUE_STATUS, 3.4.3 (inline, handler FUN_141ab9180): RideTicket, u32 Slot, u32 AvgWaitTimeMe, u32
/// AvgWaitTime, 3 x {u32 AvgWaitTimeByRole, u8 LastNeeded} (tank, healer, damage), u32 QueuedTime
void WorldSession::SendLfgQueueStatus(lfg::LfgQueueStatusData const& queueData)
{
    LOG_DEBUG("network", "SMSG_LFG_QUEUE_STATUS [{}] dungeon: {} - waitTime: {} - avgWaitTime: {} - waitTimeTanks: {} - waitTimeHealer: {} - waitTimeDps: {} - queuedTime: {} - tanks: {} - healers: {} - dps: {}",
                   GetPlayer()->GetGUID().ToString(), queueData.dungeonId, queueData.waitTime, queueData.waitTimeAvg, queueData.waitTimeTank,
                   queueData.waitTimeHealer, queueData.waitTimeDps, queueData.queuedTime, queueData.tanks, queueData.healers, queueData.dps);
    WorldPacket data(SMSG_LFG_QUEUE_STATUS, 36 + 4 + 4 + 4 + 15 + 4);
    WriteRideTicket(data, GetPlayer()->GetGUID());
    data << uint32(sLFGMgr->GetLFGDungeonEntry(queueData.dungeonId & 0x00FFFFFF));
    data << int32(queueData.waitTime);                      // AvgWaitTimeMe
    data << int32(queueData.waitTimeAvg);                   // AvgWaitTime
    data << int32(queueData.waitTimeTank) << uint8(queueData.tanks);
    data << int32(queueData.waitTimeHealer) << uint8(queueData.healers);
    data << int32(queueData.waitTimeDps) << uint8(queueData.dps);
    data << uint32(queueData.queuedTime);
    SendPacket(&data);
}

/// SMSG_LFG_PLAYER_REWARD, 3.4.3 (FUN_1406c3320, handler FUN_141ab8d70): u32 QueuedSlot, u32 ActualSlot, u32
/// RewardMoney, u32 AddedXP, u32 count, per reward {bits HasRewardItem, IsCurrency, flush, [ItemInstance], u32
/// Quantity, u32 BonusQuantity, [u32 currency]}
void WorldSession::SendLfgPlayerReward(lfg::LfgPlayerRewardData const& rewardData)
{
    if (!rewardData.rdungeonEntry || !rewardData.sdungeonEntry || !rewardData.quest)
        return;

    LOG_DEBUG("lfg", "SMSG_LFG_PLAYER_REWARD {} rdungeonEntry: {}, sdungeonEntry: {}, done: {}",
                   GetPlayerInfo(), rewardData.rdungeonEntry, rewardData.sdungeonEntry, rewardData.done);

    uint8 playerLevel = GetPlayer() ? GetPlayer()->GetLevel() : 0;
    uint8 playerLevelForXP = playerLevel;
    sScriptMgr->OnPlayerBeforeGetLevelForXPGain(GetPlayer(), playerLevelForXP);

    std::vector<std::pair<uint32, uint32>> items;
    for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
        if (uint32 itemId = rewardData.quest->RewardItemId[i])
            items.emplace_back(itemId, rewardData.quest->RewardItemIdCount[i]);

    WorldPacket data(SMSG_LFG_PLAYER_REWARD, 20 + items.size() * 32);
    data << uint32(rewardData.rdungeonEntry);              // Random Dungeon Finished
    data << uint32(rewardData.sdungeonEntry);              // Dungeon Finished
    data << uint32(rewardData.quest->GetRewOrReqMoney(playerLevel));
    data << uint32(rewardData.quest->XPValue(playerLevelForXP));
    data << uint32(items.size());
    for (auto const& [itemId, count] : items)
    {
        data.WriteBit(true);                                // HasRewardItem
        data.WriteBit(false);                               // IsCurrency
        data.FlushBits();
        WorldPackets::Item::WriteItemInstance(data, itemId);
        data << uint32(count);
        data << uint32(0);                                  // BonusQuantity
    }
    SendPacket(&data);
}

/// SMSG_LFG_BOOT_PLAYER, 3.4.3 (inline, handler FUN_141aba390): bits VoteInProgress, VotePassed, MyVoteCompleted,
/// MyVote, 8-bit reason length, flush, PackedGuid128 Target, u32 TotalVotes, BootVotes, TimeLeft (s), VotesNeeded,
/// the reason
void WorldSession::SendLfgBootProposalUpdate(lfg::LfgPlayerBoot const& boot)
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    lfg::LfgAnswer playerVote = boot.votes.find(guid)->second;
    uint8 votesNum = 0;
    uint8 agreeNum = 0;
    uint32 secsleft = boot.cancelTime - GameTime::GetGameTime().count();
    for (lfg::LfgAnswerContainer::const_iterator it = boot.votes.begin(); it != boot.votes.end(); ++it)
    {
        if (it->second != lfg::LFG_ANSWER_PENDING)
        {
            ++votesNum;
            if (it->second == lfg::LFG_ANSWER_AGREE)
                ++agreeNum;
        }
    }
    LOG_DEBUG("network", "SMSG_LFG_BOOT_PLAYER [{}] inProgress: {} - didVote: {} - agree: {} - victim: [{}] votes: {} - agrees: {} - left: {} - needed: {} - reason {}",
                   guid.ToString(), uint8(boot.inProgress), uint8(playerVote != lfg::LFG_ANSWER_PENDING), uint8(playerVote == lfg::LFG_ANSWER_AGREE),
                   boot.victim.ToString(), votesNum, agreeNum, secsleft, lfg::LFG_GROUP_KICK_VOTES_NEEDED, boot.reason);
    std::string_view reason = std::string_view(boot.reason).substr(0, 0xFF);
    WorldPacket data(SMSG_LFG_BOOT_PLAYER, 2 + 18 + 16 + reason.size());
    data.WriteBit(boot.inProgress);
    data.WriteBit(agreeNum >= lfg::LFG_GROUP_KICK_VOTES_NEEDED);   // VotePassed
    data.WriteBit(playerVote != lfg::LFG_ANSWER_PENDING);
    data.WriteBit(playerVote == lfg::LFG_ANSWER_AGREE);
    data.WriteBits(reason.size(), 8);
    data.FlushBits();
    Guid128::Write(data, boot.victim);
    data << uint32(votesNum);
    data << uint32(agreeNum);
    data << uint32(secsleft);
    data << uint32(lfg::LFG_GROUP_KICK_VOTES_NEEDED);
    data.WriteString(reason);
    SendPacket(&data);
}

/// SMSG_LFG_PROPOSAL_UPDATE, 3.4.3 (FUN_140700060, handler FUN_141ab8ef0): RideTicket, u64 InstanceID, u32
/// ProposalID (both echoed by CMSG_DF_PROPOSAL_RESPONSE), u32 Slot, u8 State, u32 CompletedMask, u32 EncounterMask,
/// u32 player count, u8 PromisedShortageRolePriority, bits ValidCompletedMask, ProposalSilent, IsRequeue, flush,
/// per player {u8 Roles, bits Me, SameParty, MyParty, Responded, Accepted, flush}
void WorldSession::SendLfgUpdateProposal(lfg::LfgProposal const& proposal)
{
    ObjectGuid guid = GetPlayer()->GetGUID();
    ObjectGuid gguid = proposal.players.find(guid)->second.group;
    bool silent = !proposal.isNew && gguid == proposal.group;
    uint32 dungeonEntry = proposal.dungeonId;

    LOG_DEBUG("network", "SMSG_LFG_PROPOSAL_UPDATE [{} state: {}", guid.ToString(), proposal.state);

    // show random dungeon if player selected random dungeon and it's not lfg group
    if (!silent)
    {
        lfg::LfgDungeonSet const& playerDungeons = sLFGMgr->GetSelectedDungeons(guid);
        if (playerDungeons.find(proposal.dungeonId) == playerDungeons.end())
            dungeonEntry = (*playerDungeons.begin());
    }

    dungeonEntry = sLFGMgr->GetLFGDungeonEntry(dungeonEntry);

    WorldPacket data(SMSG_LFG_PROPOSAL_UPDATE, 36 + 8 + 4 + 4 + 1 + 12 + 2 + proposal.players.size() * 2);
    WriteRideTicket(data, guid);
    data << uint64(0);                                      // InstanceID
    data << uint32(proposal.id);
    data << uint32(dungeonEntry);
    data << uint8(proposal.state);
    data << uint32(proposal.encounters);                    // CompletedMask
    data << uint32(0);                                      // EncounterMask
    data << uint32(proposal.players.size());
    data << uint8(0);                                       // PromisedShortageRolePriority
    data.WriteBit(true);                                    // ValidCompletedMask
    data.WriteBit(silent);                                  // ProposalSilent
    data.WriteBit(false);                                   // IsRequeue
    data.FlushBits();

    // Sort by roles: tank, healer, dps
    std::vector<ObjectGuid> ordered;
    ordered.reserve(proposal.players.size());

    std::vector<ObjectGuid> tanks, healers, dps;

    for (auto const& [pguid, player] : proposal.players)
    {
        if (player.role & lfg::PLAYER_ROLE_TANK)
            tanks.push_back(pguid);
        else if (player.role & lfg::PLAYER_ROLE_HEALER)
            healers.push_back(pguid);
        else
            dps.push_back(pguid);
    }

    ordered.insert(ordered.end(), tanks.begin(), tanks.end());
    ordered.insert(ordered.end(), healers.begin(), healers.end());
    ordered.insert(ordered.end(), dps.begin(), dps.end());

    for (auto const& pguid : ordered)
    {
        lfg::LfgProposalPlayer const& player = proposal.players.find(pguid)->second;
        data << uint8(player.role);
        data.WriteBit(pguid == guid);                       // Me
        data.WriteBit(player.group && player.group == proposal.group);   // SameParty: already in the dungeon
        data.WriteBit(player.group && player.group == gguid);            // MyParty
        data.WriteBit(player.accept != lfg::LFG_ANSWER_PENDING);         // Responded
        data.WriteBit(player.accept == lfg::LFG_ANSWER_AGREE);           // Accepted
        data.FlushBits();
    }
    SendPacket(&data);
}

void WorldSession::SendLfgLfrList(bool /*update*/)
{
    // the 3.4.3 client has no raid browser list (SMSG_LFG_UPDATE_SEARCH)
}

/// SMSG_LFG_DISABLED, 3.4.3: empty. The client wipes all its dungeon finder state (handler FUN_141ab8550).
void WorldSession::SendLfgDisabled()
{
    LOG_DEBUG("network", "SMSG_LFG_DISABLED [{}]", GetPlayer()->GetGUID().ToString());
    WorldPacket data(SMSG_LFG_DISABLED, 0);
    SendPacket(&data);
}

/// SMSG_LFG_OFFER_CONTINUE, 3.4.3 (raw, handler FUN_141ab8cf0): u32 Slot
void WorldSession::SendLfgOfferContinue(uint32 dungeonEntry)
{
    LOG_DEBUG("network", "SMSG_LFG_OFFER_CONTINUE [{}] dungeon entry: {}", GetPlayer()->GetGUID().ToString(), dungeonEntry);
    WorldPacket data(SMSG_LFG_OFFER_CONTINUE, 4);
    data << uint32(dungeonEntry);
    SendPacket(&data);
}

/// SMSG_LFG_TELEPORT_DENIED, 3.4.3 (FUN_140720780, handler FUN_141ab99b0): 4-bit Reason (AC's LfgTeleportError), flush
void WorldSession::SendLfgTeleportError(uint8 err)
{
    LOG_DEBUG("network", "SMSG_LFG_TELEPORT_DENIED [{}] reason: {}", GetPlayer()->GetGUID().ToString(), err);
    WorldPacket data(SMSG_LFG_TELEPORT_DENIED, 1);
    data.WriteBits(err, 4);
    data.FlushBits();
    SendPacket(&data);
}
