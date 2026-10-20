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

#include "AccountMgr.h"
#include "Language.h"
#include "RBAC.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "Realm.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldSession.h"

void WorldSession::HandleContactListOpcode(WorldPacket& recv_data)
{
    uint32 flags;
    recv_data >> flags;

    _player->GetSocial()->SendSocialList(_player, flags);
}

void WorldSession::HandleAddFriendOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_140763f30): 9-bit name and 10-bit notes lengths, flush, the name, the notes
    uint32 nameLen = recv_data.ReadBits(9);
    uint32 noteLen = recv_data.ReadBits(10);
    recv_data.ResetBitReader();
    std::string friendName = recv_data.ReadString(nameLen);
    std::string friendNote = recv_data.ReadString(noteLen);

    if (!normalizePlayerName(friendName))
        return;

    ObjectGuid friendGuid = sCharacterCache->GetCharacterGuidByName(friendName);
    if (!friendGuid)
        return;

    CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(friendGuid);
    if (!playerData)
        return;

    uint32 friendAccountId = playerData->AccountId;
    TeamId teamId = Player::TeamIdForRace(playerData->Race);
    FriendsResult friendResult = FRIEND_NOT_FOUND;

    if (HasPermission(rbac::RBAC_PERM_ALLOW_GM_FRIEND) || sWorld->getBoolConfig(CONFIG_ALLOW_GM_FRIEND) || AccountMgr::IsPlayerAccount(AccountMgr::GetSecurity(friendAccountId, realm.Id.Realm)))
    {
        if (friendGuid)
        {
            if (friendGuid == GetPlayer()->GetGUID())
                friendResult = FRIEND_SELF;
            else if (GetPlayer()->GetTeamId() != teamId && !HasPermission(rbac::RBAC_PERM_TWO_SIDE_ADD_FRIEND))
                friendResult = FRIEND_ENEMY;
            else if (GetPlayer()->GetSocial()->HasFriend(friendGuid))
                friendResult = FRIEND_ALREADY;
            else
            {
                Player* pFriend = ObjectAccessor::FindConnectedPlayer(friendGuid);
                if (pFriend && pFriend->IsVisibleGloballyFor(GetPlayer()) && !pFriend->GetSession()->IsGMAccount())
                    friendResult = FRIEND_ADDED_ONLINE;
                else
                    friendResult = FRIEND_ADDED_OFFLINE;
                if (GetPlayer()->GetSocial()->AddToSocialList(friendGuid, SOCIAL_FLAG_FRIEND))
                    GetPlayer()->GetSocial()->SetFriendNote(friendGuid, friendNote);
                else
                friendResult = FRIEND_LIST_FULL;
            }
            GetPlayer()->GetSocial()->SetFriendNote(friendGuid, friendNote);
        }
    }

    sSocialMgr->SendFriendStatus(GetPlayer(), friendResult, friendGuid, false);

    LOG_DEBUG("network", "WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelFriendOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_140766cb0): u32 VirtualRealmAddress, PackedGuid128 Guid
    recv_data.read_skip<uint32>();
    ObjectGuid FriendGUID = Guid128::Read(recv_data);

    _player->GetSocial()->RemoveFromSocialList(FriendGUID, SOCIAL_FLAG_FRIEND);

    sSocialMgr->SendFriendStatus(GetPlayer(), FRIEND_REMOVED, FriendGUID, false);

    LOG_DEBUG("network", "WORLD: Sent motd (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleAddIgnoreOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_140764050): 9-bit name length, flush, PackedGuid128 AccountGUID, the name
    uint32 nameLen = recv_data.ReadBits(9);
    recv_data.ResetBitReader();
    Guid128::Skip(recv_data);
    std::string ignoreName = recv_data.ReadString(nameLen);

    if (!normalizePlayerName(ignoreName))
        return;

    LOG_DEBUG("network", "WORLD: {} asked to Ignore: '{}'", GetPlayer()->GetName(), ignoreName);

    ObjectGuid ignoreGuid = sCharacterCache->GetCharacterGuidByName(ignoreName);
    if (!ignoreGuid)
        return;

    FriendsResult ignoreResult;

    if (ignoreGuid == GetPlayer()->GetGUID())              //not add yourself
        ignoreResult = FRIEND_IGNORE_SELF;
    else if (GetPlayer()->GetSocial()->HasIgnore(ignoreGuid))
        ignoreResult = FRIEND_IGNORE_ALREADY;
    else
    {
        ignoreResult = FRIEND_IGNORE_ADDED;

        // ignore list full
        if (!GetPlayer()->GetSocial()->AddToSocialList(ignoreGuid, SOCIAL_FLAG_IGNORED))
            ignoreResult = FRIEND_IGNORE_FULL;
    }

    sSocialMgr->SendFriendStatus(GetPlayer(), ignoreResult, ignoreGuid, false);

    LOG_DEBUG("network", "WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelIgnoreOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_140766d00): u32 VirtualRealmAddress, PackedGuid128 Guid
    recv_data.read_skip<uint32>();
    ObjectGuid IgnoreGUID = Guid128::Read(recv_data);

    _player->GetSocial()->RemoveFromSocialList(IgnoreGUID, SOCIAL_FLAG_IGNORED);
    sSocialMgr->SendFriendStatus(GetPlayer(), FRIEND_IGNORE_REMOVED, IgnoreGUID, false);
}

void WorldSession::HandleSetContactNotesOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_140769060): u32 VirtualRealmAddress, PackedGuid128 Guid, 10-bit notes length,
    // flush, the notes
    recv_data.read_skip<uint32>();
    ObjectGuid guid = Guid128::Read(recv_data);
    uint32 noteLen = recv_data.ReadBits(10);
    recv_data.ResetBitReader();
    std::string note = recv_data.ReadString(noteLen);
    _player->GetSocial()->SetFriendNote(guid, note);
}
