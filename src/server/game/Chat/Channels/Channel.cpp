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
#include "ChannelMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "ObjectAccessor.h"
#include "PacketGuid128.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "SocialMgr.h"
#include "World.h"

Channel::Channel(std::string const& name, uint32 channelId, uint32 channelDBId, TeamId teamId, bool announce, bool ownership):
    _announce(announce),
    _ownership(ownership),
    _IsSaved(false),
    _isOwnerGM(false),
    _flags(0),
    _channelId(channelId),
    _channelDBId(channelDBId),
    _teamId(teamId),
    _name(name),
    _password("")
{
    // set special flags if built-in channel
    if (ChatChannelsEntry const* ch = sChatChannelsStore.LookupEntry(channelId)) // check whether it's a built-in channel
    {
        _announce = false;                                 // no join/leave announces
        _ownership = false;                                // no ownership handout

        _flags |= CHANNEL_FLAG_GENERAL;                    // for all built-in channels

        if (ch->flags & CHANNEL_DBC_FLAG_TRADE)             // for trade channel
            _flags |= CHANNEL_FLAG_TRADE;

        if (ch->flags & CHANNEL_DBC_FLAG_CITY_ONLY2)        // for city only channels
            _flags |= CHANNEL_FLAG_CITY;

        if (ch->flags & CHANNEL_DBC_FLAG_LFG)               // for LFG channel
            _flags |= CHANNEL_FLAG_LFG;
        else                                                // for all other channels
            _flags |= CHANNEL_FLAG_NOT_LFG;
    }
    else                                                    // it's custom channel
    {
        _flags |= CHANNEL_FLAG_CUSTOM;

        // pussywizard:
        _channelRights = ChannelMgr::GetChannelRightsFor(_name);
        if (_channelRights.flags & CHANNEL_RIGHT_FORCE_NO_ANNOUNCEMENTS)
            _announce = false;
        if (_channelRights.flags & CHANNEL_RIGHT_FORCE_ANNOUNCEMENTS)
            _announce = true;
        if (_channelRights.flags & CHANNEL_RIGHT_NO_OWNERSHIP)
            _ownership = false;
        if (_channelRights.flags & CHANNEL_RIGHT_DONT_PRESERVE)
            return;

        _IsSaved = true;

        // Xinef: loading
        if (channelDBId > 0)
            return;

        // If storing custom channels in the db is enabled either load or save the channel
        if (sWorld->getBoolConfig(CONFIG_PRESERVE_CUSTOM_CHANNELS))
        {
            _channelDBId = ++ChannelMgr::_channelIdMax;

            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHANNEL);
            stmt->SetData(0, _channelDBId);
            stmt->SetData(1, name);
            stmt->SetData(2, _teamId);
            stmt->SetData(3, _announce);
            CharacterDatabase.Execute(stmt);
        }
    }
}

bool Channel::IsBanned(ObjectGuid guid) const
{
    BannedContainer::const_iterator itr = bannedStore.find(guid);
    return itr != bannedStore.end() && itr->second > GameTime::GetGameTime().count();
}

void Channel::UpdateChannelInDB() const
{
    if (_IsSaved)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL);
        stmt->SetData(0, _announce);
        stmt->SetData(1, _password);
        stmt->SetData(2, _channelDBId);
        CharacterDatabase.Execute(stmt);

        LOG_DEBUG("chat.system", "Channel({}) updated in database", _name);
    }
}

void Channel::UpdateChannelUseageInDB() const
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHANNEL_USAGE);
    stmt->SetData(0, _channelDBId);
    CharacterDatabase.Execute(stmt);
}

void Channel::AddChannelBanToDB(ObjectGuid guid, uint32 time) const
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHANNEL_BAN);
    stmt->SetData(0, _channelDBId);
    stmt->SetData(1, guid.GetCounter());
    stmt->SetData(2, time);
    CharacterDatabase.Execute(stmt);
}

void Channel::RemoveChannelBanFromDB(ObjectGuid guid) const
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHANNEL_BAN);
    stmt->SetData(0, _channelDBId);
    stmt->SetData(1, guid.GetCounter());
    CharacterDatabase.Execute(stmt);
}

void Channel::CleanOldChannelsInDB()
{
    if (sWorld->getIntConfig(CONFIG_PRESERVE_CUSTOM_CHANNEL_DURATION) > 0)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_OLD_CHANNELS);
        stmt->SetData(0, sWorld->getIntConfig(CONFIG_PRESERVE_CUSTOM_CHANNEL_DURATION) * DAY);
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_OLD_CHANNELS_BANS);
        trans->Append(stmt);

        CharacterDatabase.CommitTransaction(trans);
    }
}

void Channel::JoinChannel(Player* player, std::string const& pass)
{
    ObjectGuid guid = player->GetGUID();
    if (IsOn(guid))
    {
        // Do not send error message for built-in channels
        if (!IsConstant())
        {
            WorldPacket data;
            MakePlayerAlreadyMember(&data, guid);
            SendToOne(&data, guid);
        }
        return;
    }

    if (IsBanned(guid))
    {
        WorldPacket data;
        MakeBanned(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!_password.empty() && pass != _password)
    {
        WorldPacket data;
        MakeWrongPassword(&data);
        SendToOne(&data, guid);
        return;
    }

    if (IsLFG() && sWorld->getBoolConfig(CONFIG_RESTRICTED_LFG_CHANNEL) && !player->IsUsingLfg())
    {
        WorldPacket data;
        MakeNotInLfg(&data);
        SendToOne(&data, guid);
        return;
    }

    player->JoinedChannel(this);

    if (_announce && ShouldAnnouncePlayer(player))
    {
        WorldPacket data;
        MakeJoined(&data, guid);
        SendToAll(&data);
    }

    PlayerInfo pinfo;
    pinfo.player = guid;
    pinfo.flags = MEMBER_FLAG_NONE;
    pinfo.plrPtr = player;

    playersStore[guid] = pinfo;

    if (_channelRights.joinMessage.length())
        ChatHandler(player->GetSession()).PSendSysMessage("{}", _channelRights.joinMessage);

    WorldPacket data;
    MakeYouJoined(&data);
    SendToOne(&data, guid);

    JoinNotify(player);

    playersStore[guid].SetOwnerGM(player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR));

    // Custom channel handling
    if (!IsConstant())
    {
        // Update last_used timestamp in db
        if (!playersStore.empty())
            UpdateChannelUseageInDB();

        if (_channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end())
        {
            playersStore[guid].SetModerator(true);
            FlagsNotify(player);
        }

        // If the channel has no owner yet and ownership is allowed, set the new owner.
        // If the channel owner is a GM and the config SilentGMJoinChannel is enabled, set the new owner
        if ((!_ownerGUID || (_isOwnerGM && sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL))) && _ownership)
        {
            _isOwnerGM = playersStore[guid].IsOwnerGM();
            SetOwner(guid, false);
        }

        if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
            playersStore[guid].SetMuted(true);
    }
}

void Channel::LeaveChannel(Player* player, bool send)
{
    ObjectGuid guid = player->GetGUID();
    if (!IsOn(guid))
    {
        if (send)
        {
            WorldPacket data;
            MakeNotMember(&data);
            SendToOne(&data, guid);
        }
        return;
    }

    if (send)
    {
        WorldPacket data;
        MakeYouLeft(&data);
        SendToOne(&data, guid);
        player->LeftChannel(this);
        data.clear();
    }

    bool changeowner = playersStore[guid].IsOwner();

    playersStore.erase(guid);
    if (_announce && ShouldAnnouncePlayer(player))
    {
        WorldPacket data;
        MakeLeft(&data, guid);
        SendToAll(&data);
    }

    RemoveWatching(player);
    LeaveNotify(player);

    if (!IsConstant())
    {
        // Update last_used timestamp in db
        UpdateChannelUseageInDB();

        // If the channel owner left and there are still playersStore inside, pick a new owner
        if (changeowner && _ownership)
        {
            if (!playersStore.empty())
            {
                ObjectGuid newowner;
                for (Channel::PlayerContainer::const_iterator itr = playersStore.begin(); itr != playersStore.end(); ++itr)
                {
                    newowner = itr->second.player;
                    if (itr->second.plrPtr->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
                        _isOwnerGM = true;
                    else
                        _isOwnerGM = false;
                    if (!itr->second.plrPtr->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
                        break;
                }
                SetOwner(newowner);
                // if the new owner is invisible gm, set flag to automatically choose a new owner
            }
            else
                SetOwner(ObjectGuid::Empty);
        }
    }
}

void Channel::KickOrBan(Player const* player, std::string const& badname, bool ban)
{
    ObjectGuid good = player->GetGUID();

    if (!IsOn(good))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, good);
        return;
    }

    if (!playersStore[good].IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, good);
        return;
    }

    bool banOffline = false; // pussywizard
    bool isGoodConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();

    ObjectGuid victim;
    uint32 badAccId = 0;
    Player* bad = ObjectAccessor::FindPlayerByName(badname, false);
    if (bad)
    {
        victim = bad->GetGUID();
        badAccId = bad->GetSession()->GetAccountId();
    }

    bool isOnChannel = victim && IsOn(victim);
    if (!isOnChannel)
    {
        if (ban && (player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR) || isGoodConstantModerator))
        {
            if (ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(badname))
            {
                if (CharacterCacheEntry const* gpd = sCharacterCache->GetCharacterCacheByGuid(guid))
                {
                    if (Player::TeamIdForRace(gpd->Race) == Player::TeamIdForRace(player->getRace()))
                    {
                        banOffline = true;
                        victim     = guid;
                        badAccId   = gpd->AccountId;
                    }
                    else
                    {
                        ChatHandler(player->GetSession()).PSendSysMessage("Character {} has other faction!", badname);
                        return;
                    }
                }
            }

            if (!banOffline)
            {
                WorldPacket data;
                MakePlayerNotFound(&data, badname);
                SendToOne(&data, good);
                return;
            }
        }
        else
        {
            WorldPacket data;
            MakePlayerNotFound(&data, badname);
            SendToOne(&data, good);
            return;
        }
    }

    bool changeowner = _ownerGUID == victim;
    bool isBadConstantModerator = _channelRights.moderators.find(badAccId) != _channelRights.moderators.end();

    if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR) && !isGoodConstantModerator)
    {
        if (changeowner && good != _ownerGUID)
        {
            WorldPacket data;
            MakeNotOwner(&data);
            SendToOne(&data, good);
            return;
        }

        if ((ban && (_channelRights.flags & CHANNEL_RIGHT_CANT_BAN)) || (!ban && (_channelRights.flags & CHANNEL_RIGHT_CANT_KICK)))
        {
            WorldPacket data;
            MakeNotModerator(&data);
            SendToOne(&data, good);
            return;
        }

        if (isBadConstantModerator || bad->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
        {
            WorldPacket data;
            MakeNotModerator(&data);
            SendToOne(&data, good);
            return;
        }
    }

    if (ban)
    {
        if (!IsBanned(victim))
        {
            bannedStore[victim] = GameTime::GetGameTime().count() + CHANNEL_BAN_DURATION;
            AddChannelBanToDB(victim, GameTime::GetGameTime().count() + CHANNEL_BAN_DURATION);

            if (ShouldAnnouncePlayer(player))
            {
                WorldPacket data;
                MakePlayerBanned(&data, victim, good);
                SendToAll(&data);
            }
        }
    }
    else if (ShouldAnnouncePlayer(player))
    {
        WorldPacket data;
        MakePlayerKicked(&data, victim, good);
        SendToAll(&data);
    }

    if (isOnChannel)
    {
        playersStore.erase(victim);
        bad->LeftChannel(this);
        RemoveWatching(bad);
        LeaveNotify(bad);
    }

    if (changeowner && _ownership)
    {
        if (good != victim)
            SetOwner(good);
        else if (!playersStore.empty())
        {
            ObjectGuid newowner;
            for (Channel::PlayerContainer::const_iterator itr = playersStore.begin(); itr != playersStore.end(); ++itr)
            {
                newowner = itr->second.player;
                if (!itr->second.plrPtr->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
                    break;
            }
            SetOwner(newowner);
        }
        else
            SetOwner(ObjectGuid::Empty);
    }
}

void Channel::UnBan(Player const* player, std::string const& badname)
{
    ObjectGuid good = player->GetGUID();

    if (!IsOn(good))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, good);
        return;
    }

    if (!playersStore[good].IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, good);
        return;
    }

    ObjectGuid victim;
    if (ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(badname))
    {
        victim = guid;
    }

    if (!victim || !IsBanned(victim))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, badname);
        SendToOne(&data, good);
        return;
    }

    bool isConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();
    if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR) && !isConstantModerator)
    {
        if (_channelRights.flags & CHANNEL_RIGHT_CANT_BAN)
        {
            WorldPacket data;
            MakeNotModerator(&data);
            SendToOne(&data, good);
            return;
        }
    }

    if (_channelRights.flags & CHANNEL_RIGHT_CANT_BAN)
        LOG_GM(player->GetSession()->GetAccountId(), "Command: /unban {} {} (Moderator {} [{}, account: {}] unbanned {} [{}])",
            GetName(), badname, player->GetName(), player->GetGUID().ToString(), player->GetSession()->GetAccountId(),
            badname, victim.ToString());

    bannedStore.erase(victim);
    RemoveChannelBanFromDB(victim);

    WorldPacket data;
    MakePlayerUnbanned(&data, victim, good);
    SendToAll(&data);
}

void Channel::UnBan(ObjectGuid guid)
{
    if (!IsBanned(guid))
        return;
    bannedStore.erase(guid);
    RemoveChannelBanFromDB(guid);
}

void Channel::Password(Player const* player, std::string const& pass)
{
    ObjectGuid guid = player->GetGUID();

    ChatHandler chat(player->GetSession());
    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    if (_channelRights.flags & CHANNEL_RIGHT_CANT_CHANGE_PASSWORD)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    _password = pass;

    WorldPacket data;
    MakePasswordChanged(&data, guid);
    SendToAll(&data);

    UpdateChannelInDB();
}

void Channel::SetMode(Player const* player, std::string const& p2n, bool mod, bool set)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    if (guid == _ownerGUID && std::string(p2n) == player->GetName() && mod)
        return;

    Player* newp = ObjectAccessor::FindPlayerByName(p2n, false);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!victim || !IsOn(victim) ||
            // allow make moderator from another team only if both is GMs
            // at this moment this only way to show channel post for GM from another team
            ((!player->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL) || !newp->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHANNEL)) && player->GetTeamId() != newp->GetTeamId() &&
             !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL)))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, p2n);
        SendToOne(&data, guid);
        return;
    }

    if (_ownerGUID == victim && _ownerGUID != guid)
    {
        WorldPacket data;
        MakeNotOwner(&data);
        SendToOne(&data, guid);
        return;
    }

    if (mod)
    {
        bool isBadConstantModerator = _channelRights.moderators.find(newp->GetSession()->GetAccountId()) != _channelRights.moderators.end();
        if (!isBadConstantModerator)
            SetModerator(newp->GetGUID(), set);
    }
    else
    {
        bool isGoodConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();
        if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR) && !isGoodConstantModerator)
        {
            if (_channelRights.flags & CHANNEL_RIGHT_CANT_MUTE)
            {
                WorldPacket data;
                MakeNotModerator(&data);
                SendToOne(&data, guid);
                return;
            }
        }

        SetMute(newp->GetGUID(), set);
    }
}

void Channel::SetOwner(Player const* player, std::string const& newname)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    bool isGoodConstantModerator = _channelRights.moderators.find(player->GetSession()->GetAccountId()) != _channelRights.moderators.end();
    if (!player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR) && guid != _ownerGUID && !isGoodConstantModerator)
    {
        WorldPacket data;
        MakeNotOwner(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindPlayerByName(newname, false);
    ObjectGuid victim = newp ? newp->GetGUID() : ObjectGuid::Empty;

    if (!victim || !IsOn(victim) || (newp->GetTeamId() != player->GetTeamId() &&
                                     !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL)))
    {
        WorldPacket data;
        MakePlayerNotFound(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    SetOwner(victim);
}

void Channel::SendWhoOwner(ObjectGuid guid)
{
    WorldPacket data;
    if (IsOn(guid))
        MakeChannelOwner(&data);
    else
        MakeNotMember(&data);
    SendToOne(&data, guid);
}

void Channel::List(Player const* player, bool display /*= true*/)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    // SMSG_CHANNEL_LIST, 3.4.3 (inline in the chat switch, handler FUN_141905aa0): bit Display, 7-bit
    // name length (one byte together), u32 ChannelFlags, u32 MemberCount, the name, then per member
    // {PackedGuid128, u32 VirtualRealmAddress, u8 Flags}. Display is set for /chatlist's own listing.
    LOG_DEBUG("chat.system", "SMSG_CHANNEL_LIST {} Channel: {}", player->GetSession()->GetPlayerInfo(), GetName());
    std::string_view channel = std::string_view(_name).substr(0, std::min<std::size_t>(_name.size(), 127));
    WorldPacket data(SMSG_CHANNEL_LIST, 1 + 4 + 4 + channel.size() + playersStore.size() * (18 + 4 + 1));
    data.WriteBit(display);
    data.WriteBits(channel.size(), 7);
    data.FlushBits();
    data << uint32(GetFlags());

    std::size_t pos = data.wpos();
    data << uint32(0);                                  // MemberCount, placeholder
    data.WriteString(channel);

    uint32 gmLevelInWhoList = sWorld->getIntConfig(CONFIG_GM_LEVEL_IN_WHO_LIST);

    uint32 count  = 0;
    if (!(_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK))
        for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
            if (Player* member = i->second.plrPtr)
                if ((player->GetSession()->HasPermission(rbac::RBAC_PERM_WHO_SEE_ALL_SEC_LEVELS) ||
                     member->GetSession()->GetSecurity() <= AccountTypes(gmLevelInWhoList)) &&
                    member->IsVisibleGloballyFor(player))
                {
                    Guid128::Write(data, i->first);
                    data << uint32(Guid128::VirtualRealmAddress());
                    data << uint8(i->second.flags);
                    ++count;
                }

    data.put<uint32>(pos, count);

    SendToOne(&data, guid);
}

void Channel::Announce(Player const* player)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    if (!playersStore[guid].IsModerator() && !player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    if (_channelRights.flags & (CHANNEL_RIGHT_FORCE_NO_ANNOUNCEMENTS | CHANNEL_RIGHT_FORCE_ANNOUNCEMENTS))
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    _announce = !_announce;

    WorldPacket data;
    if (_announce)
        MakeAnnouncementsOn(&data, guid);
    else
        MakeAnnouncementsOff(&data, guid);
    SendToAll(&data);

    UpdateChannelInDB();
}

void Channel::Say(ObjectGuid guid, std::string const& what, uint32 lang)
{
    if (what.empty())
        return;

    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHANNEL))
        lang = LANG_UNIVERSAL;

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    PlayerInfo& pinfo = playersStore[guid];

    if (pinfo.IsMuted())
    {
        WorldPacket data;
        MakeMuted(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* player = pinfo.plrPtr;
    WorldPacket data;

    if (player)
    {
        ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, Language(lang), player, player, what, 0, _name);
    }
    else
    {
        ChatHandler::BuildChatPacket(data, CHAT_MSG_CHANNEL, Language(lang), guid, guid, what, 0, "", "", 0, false, _name);
    }

    SendToAll(&data, pinfo.IsModerator() ? ObjectGuid::Empty : guid);
}

void Channel::Invite(Player const* player, std::string const& newname)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    Player* newp = ObjectAccessor::FindPlayerByName(newname, false);
    if (!newp || !newp->isGMVisible())
    {
        WorldPacket data;
        MakePlayerNotFound(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    if (IsBanned(newp->GetGUID()))
    {
        WorldPacket data;
        MakePlayerInviteBanned(&data, newname);
        SendToOne(&data, guid);
        return;
    }

    if (newp->GetTeamId() != player->GetTeamId())
    {
        WorldPacket data;
        MakeInviteWrongFaction(&data);
        SendToOne(&data, guid);
        return;
    }

    if (IsOn(newp->GetGUID()))
    {
        WorldPacket data;
        MakePlayerAlreadyMember(&data, newp->GetGUID());
        SendToOne(&data, guid);
        return;
    }

    if (!newp->GetSocial()->HasIgnore(guid))
    {
        WorldPacket data;
        MakeInvite(&data, guid);
        SendToOne(&data, newp->GetGUID());
        data.clear();
    }

    WorldPacket data;
    MakePlayerInvited(&data, newp->GetName());
    SendToOne(&data, guid);
}

void Channel::SetOwner(ObjectGuid guid, bool exclaim)
{
    if (_ownerGUID)
    {
        // [] will re-add player after it possible removed
        PlayerContainer::iterator p_itr = playersStore.find(_ownerGUID);
        if (p_itr != playersStore.end())
        {
            p_itr->second.SetOwner(false);
            FlagsNotify(p_itr->second.plrPtr);
        }
    }

    _ownerGUID = guid;
    if (_ownerGUID)
    {
        PlayerInfo& pinfo = playersStore[_ownerGUID];

        pinfo.SetModerator(true);
        uint8 oldFlag = pinfo.flags;
        pinfo.SetOwner(true);
        FlagsNotify(pinfo.plrPtr);

        Player* player = ObjectAccessor::FindPlayer(_ownerGUID);
        if (player)
        {
            if (ShouldAnnouncePlayer(player))
            {
                WorldPacket data;

                MakeModeChange(&data, _ownerGUID, oldFlag);
                SendToAll(&data);

                if (exclaim)
                {
                    // MakeOwnerChanged will reset the packet for us
                    MakeOwnerChanged(&data, _ownerGUID);
                    SendToAll(&data);
                }
            }
        }
    }
}

void Channel::SendToAll(WorldPacket* data, ObjectGuid guid)
{
    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
        if (!guid || !i->second.plrPtr->GetSocial()->HasIgnore(guid))
            i->second.plrPtr->SendDirectMessage(data);
}

void Channel::SendToAllButOne(WorldPacket* data, ObjectGuid who)
{
    for (PlayerContainer::const_iterator i = playersStore.begin(); i != playersStore.end(); ++i)
        if (i->first != who)
            i->second.plrPtr->SendDirectMessage(data);
}

void Channel::SendToOne(WorldPacket* data, ObjectGuid who)
{
    if (Player* player = ObjectAccessor::FindConnectedPlayer(who))
        player->SendDirectMessage(data);
}

void Channel::SendToAllWatching(WorldPacket* data)
{
    for (PlayersWatchingContainer::const_iterator i = playersWatchingStore.begin(); i != playersWatchingStore.end(); ++i)
        (*i)->SendDirectMessage(data);
}

bool Channel::ShouldAnnouncePlayer(Player const* player) const
{
    return !(player->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL) && sWorld->getBoolConfig(CONFIG_SILENTLY_GM_JOIN_TO_CHANNEL));
}

void Channel::Voice(ObjectGuid /*guid1*/, ObjectGuid /*guid2*/)
{
}

void Channel::DeVoice(ObjectGuid /*guid1*/, ObjectGuid /*guid2*/)
{
}

void Channel::MakeNotifyPacket(WorldPacket* data, uint8 notify_type)
{
    MakeNotifyPacket(data, notify_type, ObjectGuid::Empty, ObjectGuid::Empty, {});
}

void Channel::MakeNotifyPacket(WorldPacket* data, uint8 notify_type, ObjectGuid sender, ObjectGuid target, std::string_view senderName, uint8 oldFlags, uint8 newFlags)
{
    // SMSG_CHANNEL_NOTIFY, 3.4.3 (FUN_1406bb060, handler FUN_141905bf0): 6-bit Type, 7-bit channel name
    // length, 6-bit sender name length, flush, PackedGuid128 SenderGuid, SenderAccountID, u32
    // SenderVirtualRealm, PackedGuid128 TargetGuid, u32 TargetVirtualRealm, i32 ChatChannelID,
    // [u8 OldFlags, u8 NewFlags for MODE_CHANGE only], channel name, sender name
    std::string_view channel = std::string_view(_name).substr(0, std::min<std::size_t>(_name.size(), 127));
    senderName = senderName.substr(0, std::min<std::size_t>(senderName.size(), 63));

    data->Initialize(SMSG_CHANNEL_NOTIFY, 3 + 3 * 18 + 12 + channel.size() + senderName.size());
    data->WriteBits(notify_type, 6);
    data->WriteBits(channel.size(), 7);
    data->WriteBits(senderName.size(), 6);
    data->FlushBits();
    Guid128::Write(*data, sender);
    Guid128::Write(*data, ObjectGuid::Empty);           // SenderAccountID
    *data << uint32(Guid128::VirtualRealmAddress());
    Guid128::Write(*data, target);
    *data << uint32(Guid128::VirtualRealmAddress());
    *data << int32(GetChannelId());
    if (notify_type == CHAT_MODE_CHANGE_NOTICE)
        *data << uint8(oldFlags) << uint8(newFlags);
    data->WriteString(channel);
    data->WriteString(senderName);
}

void Channel::MakeJoined(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_JOINED_NOTICE, guid);
}

void Channel::MakeLeft(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_LEFT_NOTICE, guid);
}

void Channel::MakeYouJoined(WorldPacket* data)
{
    // 3.4.3 tells the joining player through its own message, SMSG_CHANNEL_NOTIFY_JOINED (handler
    // FUN_141905f90), which creates the channel on the client: 7-bit name length, 11-bit welcome
    // message length, flush, u32 ChannelFlags, i32 ChatChannelID, u64 InstanceID, PackedGuid128
    // ChannelGUID, the name, the welcome message
    std::string_view channel = std::string_view(_name).substr(0, std::min<std::size_t>(_name.size(), 127));
    data->Initialize(SMSG_CHANNEL_NOTIFY_JOINED, 3 + 16 + 18 + channel.size());
    data->WriteBits(channel.size(), 7);
    data->WriteBits(0, 11);
    data->FlushBits();
    *data << uint32(GetFlags());
    *data << int32(GetChannelId());
    *data << uint64(0);                                 // InstanceID
    Guid128::Write(*data, ObjectGuid::Empty);           // ChannelGUID [TODO: if a use turns up; the client then sends 0]
    data->WriteString(channel);
}

void Channel::MakeYouLeft(WorldPacket* data)
{
    // SMSG_CHANNEL_NOTIFY_LEFT, 3.4.3 (handler FUN_141906270): 7-bit name length, bit Suspended, flush,
    // i32 ChatChannelID, the name
    std::string_view channel = std::string_view(_name).substr(0, std::min<std::size_t>(_name.size(), 127));
    data->Initialize(SMSG_CHANNEL_NOTIFY_LEFT, 2 + 4 + channel.size());
    data->WriteBits(channel.size(), 7);
    data->WriteBit(false);                              // Suspended
    data->FlushBits();
    *data << int32(GetChannelId());
    data->WriteString(channel);
}

void Channel::MakeWrongPassword(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_WRONG_PASSWORD_NOTICE);
}

void Channel::MakeNotMember(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MEMBER_NOTICE);
}

void Channel::MakeNotModerator(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MODERATOR_NOTICE);
}

void Channel::MakePasswordChanged(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_PASSWORD_CHANGED_NOTICE, guid);
}

void Channel::MakeOwnerChanged(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_OWNER_CHANGED_NOTICE, guid);
}

void Channel::MakePlayerNotFound(WorldPacket* data, std::string const& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_NOT_FOUND_NOTICE, ObjectGuid::Empty, ObjectGuid::Empty, name);
}

void Channel::MakeNotOwner(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_OWNER_NOTICE);
}

void Channel::MakeChannelOwner(WorldPacket* data)
{
    std::string name = "";

    if (!sCharacterCache->GetCharacterNameByGuid(_ownerGUID, name) || name.empty())
        name = "PLAYER_NOT_FOUND";

    MakeNotifyPacket(data, CHAT_CHANNEL_OWNER_NOTICE, ObjectGuid::Empty, ObjectGuid::Empty, (IsConstant() || !_ownerGUID) ? "Nobody" : name);
}

void Channel::MakeModeChange(WorldPacket* data, ObjectGuid guid, uint8 oldflags)
{
    MakeNotifyPacket(data, CHAT_MODE_CHANGE_NOTICE, guid, ObjectGuid::Empty, {}, oldflags, GetPlayerFlags(guid));
}

void Channel::MakeAnnouncementsOn(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_ANNOUNCEMENTS_ON_NOTICE, guid);
}

void Channel::MakeAnnouncementsOff(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_ANNOUNCEMENTS_OFF_NOTICE, guid);
}

void Channel::MakeMuted(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_MUTED_NOTICE);
}

// For kicks and bans the sender is the one who acts and the target the player it falls on.
void Channel::MakePlayerKicked(WorldPacket* data, ObjectGuid bad, ObjectGuid good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_KICKED_NOTICE, good, bad);
}

void Channel::MakeBanned(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_BANNED_NOTICE);
}

void Channel::MakePlayerBanned(WorldPacket* data, ObjectGuid bad, ObjectGuid good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_BANNED_NOTICE, good, bad);
}

void Channel::MakePlayerUnbanned(WorldPacket* data, ObjectGuid bad, ObjectGuid good)
{
    MakeNotifyPacket(data, CHAT_PLAYER_UNBANNED_NOTICE, good, bad);
}

void Channel::MakePlayerNotBanned(WorldPacket* data, std::string const& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_NOT_BANNED_NOTICE, ObjectGuid::Empty, ObjectGuid::Empty, name);
}

void Channel::MakePlayerAlreadyMember(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_PLAYER_ALREADY_MEMBER_NOTICE, guid);
}

void Channel::MakeInvite(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_INVITE_NOTICE, guid);
}

void Channel::MakeInviteWrongFaction(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_INVITE_WRONG_FACTION_NOTICE);
}

void Channel::MakeWrongFaction(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_WRONG_FACTION_NOTICE);
}

void Channel::MakeInvalidName(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_INVALID_NAME_NOTICE);
}

void Channel::MakeNotModerated(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_MODERATED_NOTICE);
}

void Channel::MakePlayerInvited(WorldPacket* data, std::string const& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_INVITED_NOTICE, ObjectGuid::Empty, ObjectGuid::Empty, name);
}

void Channel::MakePlayerInviteBanned(WorldPacket* data, std::string const& name)
{
    MakeNotifyPacket(data, CHAT_PLAYER_INVITE_BANNED_NOTICE, ObjectGuid::Empty, ObjectGuid::Empty, name);
}

void Channel::MakeThrottled(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_THROTTLED_NOTICE);
}

void Channel::MakeNotInArea(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_IN_AREA_NOTICE);
}

void Channel::MakeNotInLfg(WorldPacket* data)
{
    MakeNotifyPacket(data, CHAT_NOT_IN_LFG_NOTICE);
}

void Channel::MakeVoiceOn(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_VOICE_ON_NOTICE, guid);
}

void Channel::MakeVoiceOff(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_VOICE_OFF_NOTICE, guid);
}

/// SMSG_USERLIST_ADD / _UPDATE, 3.4.3 (handlers FUN_1419074f0 / FUN_1419077e0): PackedGuid128, u8
/// UserFlags, u32 ChannelFlags, i32 ChannelID, 7-bit name length, flush, the name. _REMOVE (FUN_141907680)
/// has no UserFlags.
void Channel::MakeUserListPacket(WorldPacket& data, Opcodes opcode, Player const* p)
{
    std::string_view channel = std::string_view(_name).substr(0, std::min<std::size_t>(_name.size(), 127));
    data.Initialize(opcode, 18 + 1 + 4 + 4 + 1 + channel.size());
    Guid128::Write(data, p->GetGUID());
    if (opcode != SMSG_USERLIST_REMOVE)
        data << uint8(GetPlayerFlags(p->GetGUID()));
    data << uint32(GetFlags());
    data << int32(GetChannelId());
    data.WriteBits(channel.size(), 7);
    data.FlushBits();
    data.WriteString(channel);
}

void Channel::JoinNotify(Player* p)
{
    if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
        return;
    if (p->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL))
        return;

    WorldPacket data;
    MakeUserListPacket(data, SMSG_USERLIST_ADD, p);
    SendToAllWatching(&data);
}

void Channel::LeaveNotify(Player* p)
{
    if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
        return;
    if (p->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL))
        return;

    WorldPacket data;
    MakeUserListPacket(data, SMSG_USERLIST_REMOVE, p);
    SendToAllWatching(&data);
}

void Channel::FlagsNotify(Player* p)
{
    if (_channelRights.flags & CHANNEL_RIGHT_CANT_SPEAK)
        return;
    if (p->GetSession()->HasPermission(rbac::RBAC_PERM_SILENTLY_JOIN_CHANNEL))
        return;

    WorldPacket data;
    MakeUserListPacket(data, SMSG_USERLIST_UPDATE, p);
    SendToAllWatching(&data);
}

void Channel::AddWatching(Player* p)
{
    if (!IsOn(p->GetGUID()))
        return;
    playersWatchingStore.insert(p);
}

void Channel::RemoveWatching(Player* p)
{
    PlayersWatchingContainer::iterator itr = playersWatchingStore.find(p);
    if (itr != playersWatchingStore.end())
        playersWatchingStore.erase(itr);
}

void Channel::ToggleModeration(Player* player)
{
    ObjectGuid guid = player->GetGUID();

    if (!IsOn(guid))
    {
        WorldPacket data;
        MakeNotMember(&data);
        SendToOne(&data, guid);
        return;
    }

    const bool gm = player->GetSession()->HasPermission(rbac::RBAC_PERM_CHANGE_CHANNEL_NOT_MODERATOR);

    if (!playersStore[guid].IsModerator() && !gm)
    {
        WorldPacket data;
        MakeNotModerator(&data);
        SendToOne(&data, guid);
        return;
    }

    // toggle channel moderation
    _moderation = !_moderation;

    WorldPacket data;
    if (_moderation)
    {
        MakeModerationOn(&data, guid);
    }
    else
    {
        MakeModerationOff(&data, guid);
    }

    SendToAll(&data);
}

void Channel::MakeModerationOn(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_MODERATION_ON_NOTICE, guid);
}

void Channel::MakeModerationOff(WorldPacket* data, ObjectGuid guid)
{
    MakeNotifyPacket(data, CHAT_MODERATION_OFF_NOTICE, guid);
}
