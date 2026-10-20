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

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "Language.h"
#include "Log.h"
#include "MapMgr.h"
#include "MiscPackets.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Pet.h"
#include "Player.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "SpellAuras.h"
#include "Util.h"
#include "Vehicle.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

class Aura;

/* differeces from off:
    -you can uninvite yourself - is is useful
    -you can accept invitation even if leader went offline
*/
/* todo:
    -group_destroyed msg is sent but not shown
    -reduce xp gaining when in raid group
    -quest sharing has to be corrected
    -FIX sending PartyMemberStats
*/

void WorldSession::SendPartyResult(PartyOperation operation, std::string const& member, PartyResult res, uint32 val /* = 0 */)
{
    // SMSG_PARTY_COMMAND_RESULT, 3.4.3 (FUN_1406c61e0, handler FUN_141735e00): 9-bit name length, 4-bit
    // Command, 6-bit Result (3.3.5's numbering; 9, IGNORING_YOU_S, is silent now), flush, u32 ResultData
    // (seconds for the LFG boot results), PackedGuid128 ResultGUID, the name
    std::string_view name = std::string_view(member).substr(0, std::min<std::size_t>(member.size(), 0x1FF));
    WorldPacket data(SMSG_PARTY_COMMAND_RESULT, 3 + 4 + 18 + name.size());
    data.WriteBits(name.size(), 9);
    data.WriteBits(uint32(operation), 4);
    data.WriteBits(uint32(res), 6);
    data.FlushBits();
    data << uint32(val);
    Guid128::Write(data, ObjectGuid::Empty);
    data.WriteString(name);

    SendPacket(&data);
}

/// SMSG_PARTY_INVITE, 3.4.3 (FUN_1406c6320 -> FUN_1407014e0, handler FUN_141985d30): bits CanAccept,
/// MightCRZYou, IsXRealm, MustBeBNetFriend, AllowMultipleRoles, QuestSessionActive, 6-bit inviter name
/// length, a bit (0), flush, VirtualRealmInfo (u32 address, IsLocal, IsInternalRealm, 8/8-bit realm names,
/// flush, the names), PackedGuid128 InviterGUID, InviterBNetAccountID, u16, u8 ProposedRoles, u32 LFG slot
/// count, u32 LfgCompletedMask, the name, the LFG slots.
static void BuildPartyInvite(WorldPacket& data, Player* inviter, bool canAccept)
{
    std::string const& realmName = realm.Name;
    std::string normalizedName = realmName;
    normalizedName.erase(std::remove(normalizedName.begin(), normalizedName.end(), ' '), normalizedName.end());
    std::string_view name = std::string_view(inviter->GetName()).substr(0, std::min<std::size_t>(inviter->GetName().size(), 0x3F));
    std::string_view realmView = std::string_view(realmName).substr(0, std::min<std::size_t>(realmName.size(), 0xFF));
    std::string_view normalizedView = std::string_view(normalizedName).substr(0, std::min<std::size_t>(normalizedName.size(), 0xFF));

    data.Initialize(SMSG_PARTY_INVITE, 64 + name.size() + realmView.size() * 2);
    data.WriteBit(canAccept);
    data.WriteBit(false);                                   // MightCRZYou
    data.WriteBit(false);                                   // IsXRealm
    data.WriteBit(false);                                   // MustBeBNetFriend
    data.WriteBit(false);                                   // AllowMultipleRoles
    data.WriteBit(false);                                   // QuestSessionActive
    data.WriteBits(name.size(), 6);
    data.WriteBit(false);
    data.FlushBits();
    data << uint32(Guid128::VirtualRealmAddress());
    data.WriteBit(true);                                    // IsLocal
    data.WriteBit(false);                                   // IsInternalRealm
    data.WriteBits(realmView.size(), 8);
    data.WriteBits(normalizedView.size(), 8);
    data.FlushBits();
    data.WriteString(realmView);
    data.WriteString(normalizedView);
    Guid128::Write(data, inviter->GetGUID());
    Guid128::Write(data, ObjectGuid::Empty);                // InviterBNetAccountId
    data << uint16(0);
    data << uint8(0);                                       // ProposedRoles
    data << uint32(0);                                      // LfgSlots
    data << uint32(0);                                      // LfgCompletedMask
    data.WriteString(name);
}

void WorldSession::HandleGroupInviteOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_140768330 -> FUN_14074ef70): bit HasPartyIndex, flush, 9-bit target name and 9-bit
    // realm lengths, flush, u32 ProposedRoles, PackedGuid128 TargetGUID, the name, the realm, [u8 PartyIndex]
    bool hasPartyIndex = recvData.ReadBit();
    recvData.ResetBitReader();
    uint32 nameLen = recvData.ReadBits(9);
    uint32 realmLen = recvData.ReadBits(9);
    recvData.ResetBitReader();
    recvData.read_skip<uint32>();
    Guid128::Skip(recvData);
    std::string membername = recvData.ReadString(nameLen);
    recvData.read_skip(realmLen);
    if (hasPartyIndex)
        recvData.read_skip<uint8>();

    // attempt add selected player

    // cheating
    if (!normalizePlayerName(membername))
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_BAD_PLAYER_NAME_S);
        return;
    }

    Player* invitingPlayer = GetPlayer();
    Player* invitedPlayer = ObjectAccessor::FindPlayerByName(membername, false);

    // no player or cheat self-invite
    if (!invitedPlayer || invitedPlayer == invitingPlayer)
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_BAD_PLAYER_NAME_S);
        return;
    }

    if (!sScriptMgr->OnPlayerCanGroupInvite(invitingPlayer, membername))
        return;

    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_PARTY) && IsTrialAccount())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_INVITE_RESTRICTED);
        return;
    }

    if (invitingPlayer->IsSpectator() || invitedPlayer->IsSpectator())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_INVITE_RESTRICTED);
        return;
    }

    // restrict invite to GMs
    if (!sWorld->getBoolConfig(CONFIG_ALLOW_GM_GROUP) && !invitingPlayer->IsGameMaster() && invitedPlayer->IsGameMaster())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_BAD_PLAYER_NAME_S);
        return;
    }

    // target has opted out of receiving group invites
    if (!invitedPlayer->IsAcceptGroupInvites())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_BAD_PLAYER_NAME_S);
        return;
    }

    // can't group with
    if (!invitingPlayer->IsGameMaster() && !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP) && invitingPlayer->GetTeamId() != invitedPlayer->GetTeamId())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_PLAYER_WRONG_FACTION);
        return;
    }
    if (invitingPlayer->GetInstanceId() != 0 && invitedPlayer->GetInstanceId() != 0 && invitingPlayer->GetInstanceId() != invitedPlayer->GetInstanceId() && invitingPlayer->GetMapId() == invitedPlayer->GetMapId())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_TARGET_NOT_IN_INSTANCE_S);
        return;
    }

    if (invitedPlayer->GetSocial()->HasIgnore(invitingPlayer->GetGUID()))
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_IGNORING_YOU_S);
        return;
    }

    if (!invitedPlayer->GetSocial()->HasFriend(invitingPlayer->GetGUID()) && invitingPlayer->GetLevel() < sWorld->getIntConfig(CONFIG_PARTY_LEVEL_REQ))
    {
        SendPartyResult(PARTY_OP_INVITE, invitedPlayer->GetName(), ERR_INVITE_RESTRICTED);
        return;
    }

    // Battlefield raids (e.g. Wintergrasp) have their composition managed by the BF
    // system based on queue and team balance. Letting raid members recruit outsiders
    // bypasses Battlefield::AddOrSetPlayerToCorrectBfGroup, which on WG entry then
    // refuses to add the invitee because they are already in a BF group.
    if (Group* invitingGroup = invitingPlayer->GetGroup(); invitingGroup && invitingGroup->isBFGroup())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_NOT_LEADER);
        return;
    }

    Group* group = invitingPlayer->GetGroup();
    if (group && group->isBGGroup())
        group = invitingPlayer->GetOriginalGroup();
    if (!group)
        group = invitingPlayer->GetGroupInvite();

    Group* group2 = invitedPlayer->GetGroup();
    if (group2 && group2->isBGGroup())
        group2 = invitedPlayer->GetOriginalGroup();
    // player already in another group or invited
    if (group2 || invitedPlayer->GetGroupInvite())
    {
        SendPartyResult(PARTY_OP_INVITE, membername, ERR_ALREADY_IN_GROUP_S);

        if (group2)
        {
            // tell the player that they were invited but it failed as they were already in a group
            WorldPacket data;
            BuildPartyInvite(data, invitingPlayer, false);
            invitedPlayer->SendDirectMessage(&data);
        }

        return;
    }

    if (group)
    {
        // not have permissions for invite
        if (!group->IsLeader(invitingPlayer->GetGUID()) && !group->IsAssistant(invitingPlayer->GetGUID()))
        {
            SendPartyResult(PARTY_OP_INVITE, "", ERR_NOT_LEADER);
            return;
        }
        // not have place
        if (group->IsFull())
        {
            SendPartyResult(PARTY_OP_INVITE, "", ERR_GROUP_FULL);
            return;
        }
    }

    // xinef: if player has no group, check group invite
    if (!group && invitingPlayer->GetGroupInvite() && invitingPlayer->GetGroupInvite()->GetLeaderGUID() == invitingPlayer->GetGUID())
        group = invitingPlayer->GetGroupInvite();

    // ok, but group not exist, start a new group
    // but don't create and save the group to the DB until
    // at least one person joins
    if (!group)
    {
        group = new Group();
        // new group: if can't add then delete
        if (!group->AddLeaderInvite(invitingPlayer))
        {
            delete group;
            return;
        }
        if (!group->AddInvite(invitedPlayer))
        {
            delete group;
            return;
        }
    }
    else
    {
        // already existed group: if can't add then just leave
        if (!group->AddInvite(invitedPlayer))
        {
            return;
        }
    }

    // ok, we do it
    WorldPacket data;
    BuildPartyInvite(data, invitingPlayer, true);
    invitedPlayer->SendDirectMessage(&data);

    SendPartyResult(PARTY_OP_INVITE, membername, ERR_PARTY_RESULT_OK);
}

void WorldSession::HandleGroupAcceptOpcode(WorldPacket& recvData)
{
    // CMSG_PARTY_INVITE_RESPONSE, 3.4.3 (writer FUN_1407683c0): bits HasPartyIndex, Accept,
    // HasRolesDesired, flush, [u8 PartyIndex], [u8 RolesDesired]. 3.3.5 had an opcode for each answer.
    bool hasPartyIndex = recvData.ReadBit();
    bool accept = recvData.ReadBit();
    bool hasRoles = recvData.ReadBit();
    recvData.ResetBitReader();
    if (hasPartyIndex)
        recvData.read_skip<uint8>();
    if (hasRoles)
        recvData.read_skip<uint8>();

    if (!accept)
    {
        HandleGroupDeclineOpcode(recvData);
        return;
    }

    Group* group = GetPlayer()->GetGroupInvite();

    if (!group)
        return;

    // Remove player from invitees in any case
    group->RemoveInvite(GetPlayer());

    if (GetPlayer()->IsSpectator())
    {
        SendPartyResult(PARTY_OP_INVITE, "", ERR_INVITE_RESTRICTED);
        return;
    }

    if (!sScriptMgr->OnPlayerCanGroupAccept(GetPlayer(), group))
        return;

    Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());

    // Trial accounts cannot join a group whose existing members are above the trial level cap.
    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_PARTY) && IsTrialAccount())
    {
        if (uint32 trialLevelCap = sWorld->getIntConfig(CONFIG_TRIAL_LEVEL_CAP))
        {
            uint8 leaderLevel = leader ? leader->GetLevel() : sCharacterCache->GetCharacterLevelByGuid(group->GetLeaderGUID());
            if (leaderLevel > trialLevelCap)
            {
                SendPartyResult(PARTY_OP_INVITE, "", ERR_INVITE_RESTRICTED);
                return;
            }

            for (auto const& slot : group->GetMemberSlots())
            {
                if (slot.guid == GetPlayer()->GetGUID())
                    continue;

                uint8 memberLevel = 0;
                if (Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid))
                    memberLevel = member->GetLevel();
                else
                    memberLevel = sCharacterCache->GetCharacterLevelByGuid(slot.guid);

                if (memberLevel > trialLevelCap)
                {
                    SendPartyResult(PARTY_OP_INVITE, "", ERR_INVITE_RESTRICTED);
                    return;
                }
            }
        }
    }

    if (group->GetLeaderGUID() == GetPlayer()->GetGUID())
    {
        LOG_ERROR("network.opcode", "HandleGroupAcceptOpcode: player {} ({}) tried to accept an invite to his own group",
            GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());
        return;
    }

    // Group is full
    if (group->IsFull())
    {
        SendPartyResult(PARTY_OP_INVITE, "", ERR_GROUP_FULL);
        return;
    }

    // Forming a new group, create it
    if (!group->IsCreated())
    {
        // This can happen if the leader is zoning. To be removed once delayed actions for zoning are implemented
        if (!leader || leader->IsSpectator())
        {
            group->RemoveAllInvites();
            return;
        }

        // If we're about to create a group there really should be a leader present
        ASSERT(leader);
        group->RemoveInvite(leader);
        group->Create(leader);
        sGroupMgr->AddGroup(group);
    }

    // Everything is fine, do it, PLAYER'S GROUP IS SET IN ADDMEMBER!!!
    if (!group->AddMember(GetPlayer()))
        return;

    group->BroadcastGroupUpdate();
}

void WorldSession::HandleGroupDeclineOpcode(WorldPacket& /*recvData*/)
{
    Group* group = GetPlayer()->GetGroupInvite();
    if (!group)
        return;

    // Remember leader if online (group pointer will be invalid if group gets disbanded)
    Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());

    // uninvite, group can be deleted
    GetPlayer()->UninviteFromGroup();

    if (!leader)
        return;

    // report
    // SMSG_GROUP_DECLINE, 3.4.3 (FUN_1406c19c0): 9-bit name length, flush, the name
    WorldPacket data(SMSG_GROUP_DECLINE, 2 + GetPlayer()->GetName().length());
    data.WriteBits(GetPlayer()->GetName().size(), 9);
    data.FlushBits();
    data.WriteString(GetPlayer()->GetName());
    leader->SendDirectMessage(&data);
}

void WorldSession::HandleGroupUninviteGuidOpcode(WorldPacket& recvData)
{
    // CMSG_PARTY_UNINVITE, 3.4.3 (writer FUN_140768630): bit HasPartyIndex, 8-bit reason length, flush,
    // PackedGuid128 TargetGUID, [u8 PartyIndex], the reason
    bool hasPartyIndex = recvData.ReadBit();
    uint32 reasonLen = recvData.ReadBits(8);
    recvData.ResetBitReader();
    ObjectGuid guid = Guid128::Read(recvData);
    if (hasPartyIndex)
        recvData.read_skip<uint8>();
    std::string reason = recvData.ReadString(reasonLen);
    std::string name;

    //can't uninvite yourself
    if (guid == GetPlayer()->GetGUID())
    {
        LOG_ERROR("network.opcode", "WorldSession::HandleGroupUninviteGuidOpcode: leader {} ({}) tried to uninvite himself from the group.",
            GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());
        return;
    }

    sCharacterCache->GetCharacterNameByGuid(guid, name);

    PartyResult res = GetPlayer()->CanUninviteFromGroup(guid);
    if (res != ERR_PARTY_RESULT_OK)
    {
        if (res == ERR_PARTY_LFG_BOOT_NOT_ELIGIBLE_S)
        {
            if (Player* kickTarget = ObjectAccessor::FindConnectedPlayer(guid))
            {
                if (Aura* dungeonCooldownAura = kickTarget->GetAura(lfg::LFG_SPELL_DUNGEON_COOLDOWN))
                {
                    int32 elapsedTime = dungeonCooldownAura->GetMaxDuration() - dungeonCooldownAura->GetDuration();
                    if (static_cast<int32>(sWorld->getIntConfig(CONFIG_LFG_KICK_PREVENTION_TIMER)) > elapsedTime)
                    {
                        SendPartyResult(PARTY_OP_UNINVITE, name, res, (sWorld->getIntConfig(CONFIG_LFG_KICK_PREVENTION_TIMER) - elapsedTime) / 1000);
                    }
                }
            }
        } else
        {
            SendPartyResult(PARTY_OP_UNINVITE, name, res);
        }

        return;
    }

    Group* grp = GetPlayer()->GetGroup();
    if (!grp)
        return;

    // Xinef: do not allow to kick with empty reason, this will resend packet with given reason
    if (grp->isLFGGroup(true) && reason.empty())
    {
        SendPartyResult(PARTY_OP_UNINVITE, name, ERR_VOTE_KICK_REASON_NEEDED);
        return;
    }

    if (grp->IsLeader(guid) && !grp->isLFGGroup(true))
    {
        SendPartyResult(PARTY_OP_UNINVITE, name, ERR_NOT_LEADER);
        return;
    }

    if (grp->IsMember(guid))
    {
        Player::RemoveFromGroup(grp, guid, GROUP_REMOVEMETHOD_KICK, GetPlayer()->GetGUID(), reason.c_str());
        return;
    }

    if (Player* player = grp->GetInvited(guid))
    {
        player->UninviteFromGroup();
        return;
    }

    SendPartyResult(PARTY_OP_UNINVITE, name, ERR_TARGET_NOT_IN_GROUP_S);
}

void WorldSession::HandleGroupUninviteOpcode(WorldPacket& recvData)
{
    std::string membername;
    recvData >> membername;

    // player not found
    if (!normalizePlayerName(membername))
        return;

    // can't uninvite yourself
    if (GetPlayer()->GetName() == membername)
    {
        LOG_ERROR("network.opcode", "WorldSession::HandleGroupUninviteOpcode: leader {} ({}) tried to uninvite himself from the group.",
            GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());
        return;
    }

    Group* grp = GetPlayer()->GetGroup();
    if (!grp)
    {
        return;
    }

    PartyResult res = GetPlayer()->CanUninviteFromGroup(grp->GetMemberGUID(membername));
    if (res != ERR_PARTY_RESULT_OK)
    {
        SendPartyResult(PARTY_OP_UNINVITE, "", res);
        return;
    }

    if (ObjectGuid guid = grp->GetMemberGUID(membername))
    {
        Player::RemoveFromGroup(grp, guid, GROUP_REMOVEMETHOD_KICK, GetPlayer()->GetGUID());
        return;
    }

    if (Player* player = grp->GetInvited(membername))
    {
        player->UninviteFromGroup();
        return;
    }

    SendPartyResult(PARTY_OP_UNINVITE, membername, ERR_TARGET_NOT_IN_GROUP_S);
}

void WorldSession::HandleGroupSetLeaderOpcode(WorldPacket& recvData)
{
    // CMSG_SET_PARTY_LEADER, 3.4.3 (writer FUN_140769370): bit HasPartyIndex, flush, PackedGuid128
    // TargetGUID, [u8 PartyIndex]
    recvData.ReadBit();
    recvData.ResetBitReader();
    ObjectGuid guid = Guid128::Read(recvData);
    recvData.rfinish();

    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    Group* group = GetPlayer()->GetGroup();

    if (!group || !player)
        return;

    if (!group->IsLeader(GetPlayer()->GetGUID()) || player->GetGroup() != group || guid == GetPlayer()->GetGUID())
        return;

    // Everything's fine, accepted.
    group->ChangeLeader(guid);
    group->SendUpdate();
}

void WorldSession::HandleGroupDisbandOpcode(WorldPacket& /*recvData*/)
{
    Group* grp = GetPlayer()->GetGroup();
    Group* grpInvite = GetPlayer()->GetGroupInvite();
    if (!grp && !grpInvite)
        return;

    if (_player->InBattleground()) // Do not leave group, give no error. Verified on TBC Classic
        return;

    /** error handling **/
    /********************/

    // everything's fine, do it
    if (grp)
    {
        SendPartyResult(PARTY_OP_LEAVE, GetPlayer()->GetName(), ERR_PARTY_RESULT_OK);
        GetPlayer()->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
    }
    else if (grpInvite && grpInvite->GetLeaderGUID() == GetPlayer()->GetGUID())
    { // pending group creation being cancelled
        SendPartyResult(PARTY_OP_LEAVE, GetPlayer()->GetName(), ERR_PARTY_RESULT_OK);
        grpInvite->Disband();
    }
}

void WorldSession::HandleLootMethodOpcode(WorldPacket& recvData)
{
    // CMSG_SET_LOOT_METHOD, 3.4.3 (writer FUN_140769220): bit HasPartyIndex, flush, u8 LootMethod,
    // PackedGuid128 LootMasterGUID, u32 LootThreshold, [u8 PartyIndex]
    recvData.ReadBit();
    recvData.ResetBitReader();
    uint32 lootMethod = recvData.read<uint8>();
    ObjectGuid lootMaster = Guid128::Read(recvData);
    uint32 lootThreshold = recvData.read<uint32>();
    recvData.rfinish();

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    /** error handling **/
    // Xinef: Check if group is LFG
    if (!group->IsLeader(GetPlayer()->GetGUID()) || group->isLFGGroup(true))
        return;

    if (lootMethod > NEED_BEFORE_GREED)
        return;

    if (lootThreshold < ITEM_QUALITY_UNCOMMON || lootThreshold > ITEM_QUALITY_ARTIFACT)
        return;

    if (lootMethod == MASTER_LOOT && !group->IsMember(lootMaster))
        return;
    /********************/

    // everything's fine, do it
    group->SetLootMethod((LootMethod)lootMethod);
    group->SetMasterLooterGuid(lootMaster);
    group->SetLootThreshold((ItemQualities)lootThreshold);
    group->SendUpdate();
}

void WorldSession::HandleLootRoll(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075e950): PackedGuid128 LootObj (SMSG_START_LOOT_ROLL's), u8 LootListID, u8 RollType
    // (0 pass, 1 need, 2 greed, 3 disenchant)
    ObjectGuid lootObj = Guid128::Read(recvData);
    uint8 lootListId = recvData.read<uint8>();
    uint8 rollType = recvData.read<uint8>();
    if (rollType >= MAX_ROLL_TYPE)
        return;

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (GetPlayer()->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME))
    {
        SendPlayTimeWarning(PTF_UNHEALTHY_TIME, 0);
        rollType = ROLL_PASS;
    }

    group->CountRollVote(GetPlayer()->GetGUID(), lootObj, lootListId, rollType);

    switch (rollType)
    {
        case ROLL_NEED:
            GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_NEED, 1);
            break;
        case ROLL_GREED:
            GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_GREED, 1);
            break;
        case ROLL_DISENCHANT:
            GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_DISENCHANT, 1);
            break;
    }
}

void WorldSession::HandleMinimapPingOpcode(WorldPackets::Misc::MinimapPingClient& packet)
{
    if (!sMapMgr->IsValidMapCoord(GetPlayer()->GetMap()->GetId(), packet.MapX, packet.MapY))
        return;

    Group* group = GetPlayer()->GetGroup();

    if (!group)
        return;

    group->DoMinimapPing(GetPlayer()->GetGUID(), packet.MapX, packet.MapY);
}

void WorldSession::HandleRandomRollOpcode(WorldPackets::Misc::RandomRollClient& packet)
{
    uint32 minimum, maximum;
    minimum = packet.Min;
    maximum = packet.Max;

    /** error handling **/
    if (minimum > maximum || maximum > sWorld->getIntConfig(CONFIG_RANDOM_ROLL_MAXIMUM))
        return;

    GetPlayer()->DoRandomRoll(minimum, maximum);
}

void WorldSession::HandleRaidTargetUpdateOpcode(WorldPacket& recvData)
{
    // CMSG_UPDATE_RAID_TARGET, 3.4.3 (writer FUN_140769f60): bit HasPartyIndex, flush, PackedGuid128 Target, u8
    // Symbol (0-based), [u8 PartyIndex]. An empty Target clears that symbol. There is no list request any more:
    // the server pushes SMSG_SEND_RAID_TARGET_UPDATE_ALL on joining and on login.
    bool hasPartyIndex = recvData.ReadBit();
    recvData.ResetBitReader();
    ObjectGuid guid = Guid128::Read(recvData);
    uint8 symbol = recvData.read<uint8>();
    if (hasPartyIndex)
        recvData.read_skip<uint8>();

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (group->isRaidGroup() && !group->IsLeader(GetPlayer()->GetGUID()) && !group->IsAssistant(GetPlayer()->GetGUID()))
        return;

    if (guid.IsPlayer())
    {
        Player* target = ObjectAccessor::FindConnectedPlayer(guid);

        if (!target || target->IsHostileTo(GetPlayer()))
            return;
    }

    group->SetTargetIcon(symbol, _player->GetGUID(), guid);
}

void WorldSession::HandleGroupRaidConvertOpcode(WorldPacket& recvData)
{
    // CMSG_CONVERT_RAID, 3.4.3 (writer FUN_1407666b0): bit Raid, flush. AC converts a party into a raid only.
    if (!recvData.ReadBit())
        return;

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (_player->InBattleground())
        return;

    /** error handling **/
    if (group->CheckLevelForRaid())
    {
        SendPartyResult(PARTY_OP_INVITE, "", ERR_RAID_DISALLOWED_BY_LEVEL);
        return;
    }

    if (!group->IsLeader(GetPlayer()->GetGUID()) || group->GetMembersCount() < 2 || group->isLFGGroup()) // pussywizard: not allowed for lfg groups, it is either raid from the beginning or not!
        return;
    /********************/

    // everything's fine, do it (is it 0 (PARTY_OP_INVITE) correct code)
    SendPartyResult(PARTY_OP_INVITE, "", ERR_PARTY_RESULT_OK);
    group->ConvertToRaid();
}

void WorldSession::HandleGroupChangeSubGroupOpcode(WorldPacket& recvData)
{
    // CMSG_CHANGE_SUB_GROUP, 3.4.3 (writer FUN_1407658f0, Lua SetRaidSubgroup): PackedGuid128 TargetGUID, u8
    // NewSubGroup (0-based), then bit HasPartyIndex, flush, [u8 PartyIndex]
    ObjectGuid guid = Guid128::Read(recvData);
    uint8 groupNr = recvData.read<uint8>();
    recvData.rfinish();

    // we will get correct pointer for group here, so we don't have to check if group is BG raid
    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (groupNr >= MAX_RAID_SUBGROUPS)
        return;

    ObjectGuid senderGuid = GetPlayer()->GetGUID();
    if (!group->IsLeader(senderGuid) && !group->IsAssistant(senderGuid))
        return;

    if (!group->HasFreeSlotSubGroup(groupNr) || !group->IsMember(guid))
        return;

    group->ChangeMembersGroup(guid, groupNr);
}

void WorldSession::HandleGroupAssistantLeaderOpcode(WorldPacket& recvData)
{
    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (!group->IsLeader(GetPlayer()->GetGUID()))
        return;

    // CMSG_SET_ASSISTANT_LEADER, 3.4.3 (writer FUN_140768f40): bits HasPartyIndex, Apply, flush,
    // PackedGuid128 Target, [u8 PartyIndex]
    recvData.ReadBit();
    uint8 apply = recvData.ReadBit();
    recvData.ResetBitReader();
    ObjectGuid guid = Guid128::Read(recvData);
    recvData.rfinish();

    group->SetGroupMemberFlag(guid, apply, MEMBER_FLAG_ASSISTANT);

    group->SendUpdate();
}

void WorldSession::HandlePartyAssignmentOpcode(WorldPacket& recvData)
{
    // CMSG_SET_PARTY_ASSIGNMENT, 3.4.3 (writer FUN_1407692b0): bits HasPartyIndex, Set, flush, u8 Assignment (0 main
    // tank, 1 main assist: AC's GROUP_ASSIGN_*), PackedGuid128 Target, [u8 PartyIndex]
    recvData.ReadBit();
    bool apply = recvData.ReadBit();
    recvData.ResetBitReader();
    uint8 assignment = recvData.read<uint8>();
    ObjectGuid guid = Guid128::Read(recvData);
    recvData.rfinish();

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    ObjectGuid senderGuid = GetPlayer()->GetGUID();
    if (!group->IsLeader(senderGuid) && !group->IsAssistant(senderGuid))
        return;

    switch (assignment)
    {
        case GROUP_ASSIGN_MAINASSIST:
            group->RemoveUniqueGroupMemberFlag(MEMBER_FLAG_MAINASSIST);
            group->SetGroupMemberFlag(guid, apply, MEMBER_FLAG_MAINASSIST);
            break;
        case GROUP_ASSIGN_MAINTANK:
            group->RemoveUniqueGroupMemberFlag(MEMBER_FLAG_MAINTANK);           // Remove main assist flag from current if any.
            group->SetGroupMemberFlag(guid, apply, MEMBER_FLAG_MAINTANK);
        default:
            break;
    }

    group->SendUpdate();
}

void WorldSession::HandleRaidReadyCheckOpcode(WorldPacket& recvData)
{
    // CMSG_DO_READY_CHECK, 3.4.3 (writer FUN_140766d50, Lua DoReadyCheck): bit HasPartyIndex, flush, [u8 PartyIndex].
    // Never empty; the answers come as CMSG_READY_CHECK_RESPONSE.
    recvData.rfinish();

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    if (!group->IsLeader(GetPlayer()->GetGUID()) && !group->IsAssistant(GetPlayer()->GetGUID()))
        return;

    // Check if Ready Check in BG is enabled
    if (sWorld->getBoolConfig(CONFIG_BATTLEGROUND_DISABLE_READY_CHECK_IN_BG))
    {
        // Check if player is in BG
        if (_player->InBattleground())
        {
            ChatHandler(_player->GetSession()).SendNotification(LANG_BG_READY_CHECK_ERROR);
            return;
        }
    }

    group->StartReadyCheck(GetPlayer());
}

void WorldSession::HandleReadyCheckResponseOpcode(WorldPacket& recvData)
{
    // CMSG_READY_CHECK_RESPONSE, 3.4.3 (writer FUN_140768ac0, Lua ConfirmReadyCheck): bits HasPartyIndex, IsReady,
    // flush, [u8 PartyIndex]
    recvData.ReadBit();
    bool isReady = recvData.ReadBit();
    recvData.rfinish();

    if (Group* group = GetPlayer()->GetGroup())
        group->SetMemberReadyCheck(GetPlayer()->GetGUID(), isReady);
}

/// PartyMemberAuraStates (FUN_140715cb0): i32 SpellID, u16 Flags (0x4 duration, 0x10 negative), u32 ActiveFlags (the
/// effect mask), u32 point count, the points. Returns the count; the list itself goes to `out`.
static uint32 BuildPartyMemberAuras(Unit* unit, ByteBuffer& out)
{
    uint32 count = 0;
    for (uint8 i = 0; unit && i < MAX_AURAS_GROUP_UPDATE; ++i)
    {
        AuraApplication const* aurApp = unit->GetVisibleAura(i);
        if (!aurApp)
            continue;
        uint16 flags = (aurApp->GetFlags() & AFLAG_NEGATIVE) ? 0x10 : 0;
        if (aurApp->GetBase()->GetMaxDuration() > 0)
            flags |= 0x4;
        out << int32(aurApp->GetBase()->GetId());
        out << uint16(flags);
        out << uint32(aurApp->GetEffectMask());
        out << uint32(0);
        ++count;
    }
    return count;
}

static uint16 PartyMemberStatus(Player* player)
{
    uint16 playerStatus = MEMBER_STATUS_ONLINE;
    if (player->IsPvP())
        playerStatus |= MEMBER_STATUS_PVP;
    if (!player->IsAlive())
        playerStatus |= player->HasPlayerFlag(PLAYER_FLAGS_GHOST) ? MEMBER_STATUS_GHOST : MEMBER_STATUS_DEAD;
    if (player->IsFFAPvP())
        playerStatus |= MEMBER_STATUS_PVP_FFA;
    if (player->isAFK())
        playerStatus |= MEMBER_STATUS_AFK;
    if (player->isDND())
        playerStatus |= MEMBER_STATUS_DND;
    return playerStatus;
}

/// SMSG_PARTY_MEMBER_FULL_STATE, 3.4.3 (FUN_1406c63e0 -> FUN_140715fe0, handler FUN_141736300): bit
/// ForEnemy, flush, PartyMemberStats (u8 PartyType[2], u16 Status, u8 PowerType, u16 PowerDisplayID, i32
/// health and max, u16 power and max, u16 level, spec, zone, WMO group, u32 WMO doodad placement, i16 x,
/// y, z, i32 vehicle seat, u32 aura count, the phases, the chromie time block, the auras {i32 spell, u16
/// flags, u32 active flags, u32 point count, points}, bit HasPetStats, flush, [pet: GUID, model, health
/// and max, auras, u8 name length, name]), then the member's PackedGuid128. 3.4.3's partial state has its
/// own optional layout; the full state carries everything, so it answers both.
void WorldSession::BuildPartyMemberFullState(Player* player, ObjectGuid memberGuid, WorldPacket& data)
{
    data.Initialize(SMSG_PARTY_MEMBER_FULL_STATE, 128);
    data.WriteBit(false);                                   // ForEnemy
    data.FlushBits();

    auto buildAuras = &BuildPartyMemberAuras;

    if (!player)
    {
        data << uint8(0) << uint8(0);                       // PartyType
        data << uint16(MEMBER_STATUS_OFFLINE);
        data << uint8(0) << uint16(0);                      // power type, display
        data << int32(0) << int32(0);
        data << uint16(0) << uint16(0) << uint16(0) << uint16(0) << uint16(0) << uint16(0);
        data << uint32(0);
        data << int16(0) << int16(0) << int16(0);
        data << int32(0);                                   // VehicleSeat
        data << uint32(0);                                  // auras
        data << uint32(0) << uint32(0);                     // phases
        Guid128::Write(data, ObjectGuid::Empty);
        data << uint32(0) << int32(0) << uint32(0);         // chromie time
        data.WriteBit(false);                               // HasPetStats
        data.FlushBits();
        Guid128::Write(data, memberGuid);
        return;
    }

    uint16 playerStatus = PartyMemberStatus(player);

    Powers powerType = player->getPowerType();
    data << uint8(player->GetGroup() ? 1 : 0) << uint8(0);   // PartyType: in a normal party
    data << uint16(playerStatus);
    data << uint8(powerType);
    data << uint16(0);                                      // PowerDisplayID
    data << int32(player->GetHealth());
    data << int32(player->GetMaxHealth());
    data << uint16(player->GetPower(powerType));
    data << uint16(player->GetMaxPower(powerType));
    data << uint16(player->GetLevel());
    data << uint16(0);                                      // SpecID
    data << uint16(player->GetZoneId());
    data << uint16(0);                                      // WmoGroupID
    data << uint32(0);                                      // WmoDoodadPlacementID
    data << int16(player->GetPositionX());
    data << int16(player->GetPositionY());
    data << int16(player->GetPositionZ());
    Vehicle* vehicle = player->GetVehicle();
    data << int32(vehicle ? vehicle->GetVehicleInfo()->m_seatID[player->m_movementInfo.transport.seat] : 0);
    ByteBuffer auras;
    data << uint32(buildAuras(player, auras));
    data << uint32(0);                                      // PhaseShiftFlags
    data << uint32(0);                                      // phases
    Guid128::Write(data, ObjectGuid::Empty);                // PersonalGUID
    data << uint32(0) << int32(0) << uint32(0);             // chromie time
    data.append(auras);

    Pet* pet = player->GetPet();
    data.WriteBit(pet != nullptr);                          // HasPetStats
    data.FlushBits();
    if (pet)
    {
        Guid128::Write(data, pet->GetGUID());
        data << int32(pet->GetDisplayId());
        data << int32(pet->GetHealth());
        data << int32(pet->GetMaxHealth());
        ByteBuffer petAuras;
        data << uint32(buildAuras(pet, petAuras));
        data.append(petAuras);
        std::string_view petName = std::string_view(pet->GetName()).substr(0, std::min<std::size_t>(pet->GetName().size(), 0xFF));
        data << uint8(petName.size());
        data.WriteString(petName);
    }
    Guid128::Write(data, memberGuid);
}

/// SMSG_PARTY_MEMBER_PARTIAL_STATE, 3.4.3 (FUN_1406c6560 -> FUN_1407016c0, handler FUN_1417364c0): bits ForEnemy,
/// Unk1 (sets or clears Status 0x20 on every partial: 0), Unk2, then a Has bit per field (PartyType, Status, PowerType,
/// PowerDisplayID, CurrentHealth, MaxHealth, CurrentPower, MaxPower, Level, SpecID, ZoneID, WmoGroupID,
/// WmoDoodadPlacementID, Position, VehicleSeat, Auras, Pet, Phase, ChromieTime), flush, [pet: bits HasGUID, HasName,
/// HasModelId, HasCurrentHealth, HasMaxHealth, HasAuras, flush, the name, GUID, model, health, max, auras], the member's
/// PackedGuid128, then the fields that are present in the full state's order. The client merges it into what it has
/// (members it does not know are dropped), except the pet: a partial without HasPet removes it.
void WorldSession::BuildPartyMemberStatsChangedPacket(Player* player, WorldPacket* data)
{
    uint32 mask = player->GetGroupUpdateFlag();
    if (mask == GROUP_UPDATE_FLAG_NONE)
        return;

    if (mask & GROUP_UPDATE_FLAG_POWER_TYPE)
        mask |= GROUP_UPDATE_FLAG_CUR_POWER | GROUP_UPDATE_FLAG_MAX_POWER;

    Pet* pet = player->GetPet();
    Powers powerType = player->getPowerType();
    data->Initialize(SMSG_PARTY_MEMBER_PARTIAL_STATE, 64);
    data->WriteBit(false);                                  // ForEnemy
    data->WriteBit(false);                                  // Unk1
    data->WriteBit(false);                                  // Unk2
    data->WriteBit(false);                                  // PartyType
    data->WriteBit((mask & GROUP_UPDATE_FLAG_STATUS) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_POWER_TYPE) != 0);
    data->WriteBit(false);                                  // PowerDisplayID
    data->WriteBit((mask & GROUP_UPDATE_FLAG_CUR_HP) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_MAX_HP) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_CUR_POWER) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_MAX_POWER) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_LEVEL) != 0);
    data->WriteBit(false);                                  // SpecID
    data->WriteBit((mask & GROUP_UPDATE_FLAG_ZONE) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_ZONE) != 0);   // WmoGroupID
    data->WriteBit((mask & GROUP_UPDATE_FLAG_ZONE) != 0);   // WmoDoodadPlacementID
    data->WriteBit((mask & GROUP_UPDATE_FLAG_POSITION) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_VEHICLE_SEAT) != 0);
    data->WriteBit((mask & GROUP_UPDATE_FLAG_AURAS) != 0);
    data->WriteBit(pet != nullptr);                         // HasPet: always while there is one
    data->WriteBit(false);                                  // Phase
    data->WriteBit(false);                                  // ChromieTime
    data->FlushBits();

    if (pet)
    {
        bool hasName = (mask & GROUP_UPDATE_FLAG_PET_NAME) != 0;
        data->WriteBit((mask & GROUP_UPDATE_FLAG_PET_GUID) != 0);
        data->WriteBit(hasName);
        data->WriteBit((mask & GROUP_UPDATE_FLAG_PET_MODEL_ID) != 0);
        data->WriteBit((mask & GROUP_UPDATE_FLAG_PET_CUR_HP) != 0);
        data->WriteBit((mask & GROUP_UPDATE_FLAG_PET_MAX_HP) != 0);
        data->WriteBit((mask & GROUP_UPDATE_FLAG_PET_AURAS) != 0);
        data->FlushBits();
        if (hasName)
        {
            std::string_view petName = std::string_view(pet->GetName()).substr(0, 0xFF);
            *data << uint8(petName.size());
            data->WriteString(petName);
        }
        if (mask & GROUP_UPDATE_FLAG_PET_GUID)
            Guid128::Write(*data, pet->GetGUID());
        if (mask & GROUP_UPDATE_FLAG_PET_MODEL_ID)
            *data << int32(pet->GetDisplayId());
        if (mask & GROUP_UPDATE_FLAG_PET_CUR_HP)
            *data << int32(pet->GetHealth());
        if (mask & GROUP_UPDATE_FLAG_PET_MAX_HP)
            *data << int32(pet->GetMaxHealth());
        if (mask & GROUP_UPDATE_FLAG_PET_AURAS)
        {
            ByteBuffer petAuras;
            *data << uint32(BuildPartyMemberAuras(pet, petAuras));
            data->append(petAuras);
        }
    }

    Guid128::Write(*data, player->GetGUID());
    if (mask & GROUP_UPDATE_FLAG_STATUS)
        *data << uint16(PartyMemberStatus(player));
    if (mask & GROUP_UPDATE_FLAG_POWER_TYPE)
        *data << uint8(powerType);
    if (mask & GROUP_UPDATE_FLAG_CUR_HP)
        *data << int32(player->GetHealth());
    if (mask & GROUP_UPDATE_FLAG_MAX_HP)
        *data << int32(player->GetMaxHealth());
    if (mask & GROUP_UPDATE_FLAG_CUR_POWER)
        *data << uint16(player->GetPower(powerType));
    if (mask & GROUP_UPDATE_FLAG_MAX_POWER)
        *data << uint16(player->GetMaxPower(powerType));
    if (mask & GROUP_UPDATE_FLAG_LEVEL)
        *data << uint16(player->GetLevel());
    if (mask & GROUP_UPDATE_FLAG_ZONE)
    {
        *data << uint16(player->GetZoneId());
        *data << uint16(0);                                 // WmoGroupID
        *data << uint32(0);                                 // WmoDoodadPlacementID
    }
    if (mask & GROUP_UPDATE_FLAG_POSITION)
    {
        *data << int16(player->GetPositionX());
        *data << int16(player->GetPositionY());
        *data << int16(player->GetPositionZ());
    }
    if (mask & GROUP_UPDATE_FLAG_VEHICLE_SEAT)
    {
        Vehicle* vehicle = player->GetVehicle();
        *data << int32(vehicle ? vehicle->GetVehicleInfo()->m_seatID[player->m_movementInfo.transport.seat] : 0);
    }
    if (mask & GROUP_UPDATE_FLAG_AURAS)
    {
        ByteBuffer auras;
        *data << uint32(BuildPartyMemberAuras(player, auras));
        data->append(auras);
    }
}

/*this procedure handles clients CMSG_REQUEST_PARTY_MEMBER_STATS request*/
void WorldSession::HandleRequestPartyMemberStatsOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_140768d70): bit HasPartyIndex, flush, PackedGuid128 TargetGUID, [u8 PartyIndex]
    recvData.ReadBit();
    recvData.ResetBitReader();
    ObjectGuid Guid = Guid128::Read(recvData);
    recvData.rfinish();

    Player* player = HashMapHolder<Player>::Find(Guid);
    if (player && !player->IsInSameRaidWith(_player))
        player = nullptr;

    WorldPacket data;
    BuildPartyMemberFullState(player, Guid, data);
    SendPacket(&data);
}

void WorldSession::HandleRequestRaidInfoOpcode(WorldPacket& /*recvData*/)
{
    // every time the player checks the character screen
    _player->SendRaidInfo();
}

void WorldSession::HandleOptOutOfLootOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075ef10, Lua SetOptOutOfLoot): bit PassOnLoot, flush
    bool passOnLoot = recvData.ReadBit();

    if (!GetPlayer())
        return;

    GetPlayer()->SetPassOnGroupLoot(passOnLoot);
}

void WorldSession::HandleGroupSwapSubGroupOpcode(WorldPacket& recv_data)
{
    // CMSG_SWAP_SUB_GROUPS, 3.4.3 (writer FUN_140769c70, Lua SwapRaidSubgroup): bit HasPartyIndex, flush,
    // PackedGuid128 FirstTarget, PackedGuid128 SecondTarget, [u8 PartyIndex]
    recv_data.ReadBit();
    recv_data.ResetBitReader();
    ObjectGuid guid1 = Guid128::Read(recv_data);
    ObjectGuid guid2 = Guid128::Read(recv_data);
    recv_data.rfinish();

    Group* group = GetPlayer()->GetGroup();
    if (!group || !group->isRaidGroup())
    {
        return;
    }

    if (!group->IsLeader(GetPlayer()->GetGUID()) && !group->IsAssistant(GetPlayer()->GetGUID()))
    {
        return;
    }

    if (!group->IsMember(guid1) || !group->IsMember(guid2))
    {
        SendPartyResult(PARTY_OP_SWAP, "", ERR_GROUP_SWAP_FAILED);
        return;
    }

    uint8 groupId1 = group->GetMemberGroup(guid1);
    uint8 groupId2 = group->GetMemberGroup(guid2);

    if (groupId1 == MAX_RAID_SUBGROUPS + 1 || groupId2 == MAX_RAID_SUBGROUPS + 1)
    {
        return;
    }

    if (groupId1 == groupId2)
    {
        return;
    }

    group->ChangeMembersGroup(guid1, groupId2);
    group->ChangeMembersGroup(guid2, groupId1);
}
