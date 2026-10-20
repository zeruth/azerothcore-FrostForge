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

#include "ChannelMgr.h"
#include "ObjectMgr.h"                                      // for normalizePlayerName
#include "Player.h"
#include "Language.h"
#include "TC9Sidecar.h"
#include <cctype>

// The 3.4.3 channel commands (writers FUN_140758fb0..FUN_140759ef0) send their strings without a NUL,
// lengths first as bits: a channel name in 7 bits, a player name in 9.

/// 7-bit channel name length, flush, the name.
static std::string ReadChannelName(WorldPacket& recvPacket)
{
    uint32 length = recvPacket.ReadBits(7);
    return recvPacket.ReadString(length);
}

/// 7-bit channel name length, 9-bit player name length, flush, both names.
static void ReadChannelPlayerCommand(WorldPacket& recvPacket, std::string& channelName, std::string& targetName)
{
    uint32 channelLength = recvPacket.ReadBits(7);
    uint32 targetLength = recvPacket.ReadBits(9);
    channelName = recvPacket.ReadString(channelLength);
    targetName = recvPacket.ReadString(targetLength);
}

void WorldSession::HandleJoinChannel(WorldPacket& recvPacket)
{
    // 3.4.3 (FUN_14075a060): u32 ChatChannelId, bit CreateVoiceSession, bit Internal, 7-bit name length,
    // 7-bit password length, flush, name, password
    uint32 channelId = recvPacket.read<uint32>();
    uint8 unknown1 = recvPacket.ReadBit();
    uint8 unknown2 = recvPacket.ReadBit();
    uint32 nameLength = recvPacket.ReadBits(7);
    uint32 passwordLength = recvPacket.ReadBits(7);
    std::string channelName = recvPacket.ReadString(nameLength);
    std::string password = recvPacket.ReadString(passwordLength);

    LOG_DEBUG("chat.system", "CMSG_CHAT_JOIN_CHANNEL {} Channel: {}, unk1: {}, unk2: {}, channel: {}, password: {}", GetPlayerInfo(), channelId, unknown1, unknown2, channelName, password);
    if (channelId)
    {
        ChatChannelsEntry const* channel = sChatChannelsStore.LookupEntry(channelId);
        if (!channel)
            return;

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(GetPlayer()->GetZoneId());
        if (!zone || !GetPlayer()->CanJoinConstantChannelInZone(channel, zone))
            return;

        // Cluster mode rebuilds the localized channel name so nodes agree on it;
        // stock servers keep the client-supplied name.
        if (sToCloud9Sidecar->ClusterModeEnabled())
        {
            auto const locale = GetSessionDbcLocale();
            std::string const& zoneName = zone->area_name[locale];
            std::string const cityName = sObjectMgr->GetAcoreStringForDBCLocale(LANG_CHANNEL_CITY);
            char const* nameExt = (channel->flags & CHANNEL_DBC_FLAG_CITY_ONLY) ? cityName.c_str() : zoneName.c_str();

            std::array<char, 128> buffer{};
            if (char const* pattern = channel->pattern[locale])
            {
                std::snprintf(buffer.data(), buffer.size(), pattern, nameExt);
                channelName = buffer.data();
            }
        }
    }

    if (channelName.empty())
        return;

    if (isdigit(channelName[0]))
        return;

    if (channelName.size() >= 100 || !DisallowHyperlinksAndMaybeKick(channelName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetJoinChannel(channelName, channelId))
            channel->JoinChannel(GetPlayer(), password);
}

void WorldSession::HandleLeaveChannel(WorldPacket& recvPacket)
{
    // 3.4.3 (FUN_14075a1b0): u32 ZoneChannelID, 7-bit name length, flush, name
    uint32 unk = recvPacket.read<uint32>();
    std::string channelName = ReadChannelName(recvPacket);

    LOG_DEBUG("chat.system", "CMSG_CHAT_LEAVE_CHANNEL {} Channel: {}, unk1: {}",
                   GetPlayerInfo(), channelName, unk);
    if (channelName.empty())
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->LeaveChannel(GetPlayer(), true);
}

void WorldSession::HandleChannelList(WorldPacket& recvPacket)
{
    std::string channelName = ReadChannelName(recvPacket);

    LOG_DEBUG("chat.system", "{} {} Channel: {}",
                   recvPacket.GetOpcode() == CMSG_CHAT_CHANNEL_DISPLAY_LIST ? "CMSG_CHAT_CHANNEL_DISPLAY_LIST" : "CMSG_CHAT_CHANNEL_LIST",
                   GetPlayerInfo(), channelName);
    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->List(GetPlayer(), recvPacket.GetOpcode() == CMSG_CHAT_CHANNEL_DISPLAY_LIST);
}

void WorldSession::HandleChannelPassword(WorldPacket& recvPacket)
{
    // 3.4.3 (FUN_140759840): 7-bit name length, 7-bit password length, flush, name, password
    uint32 nameLength = recvPacket.ReadBits(7);
    uint32 passwordLength = recvPacket.ReadBits(7);
    std::string channelName = recvPacket.ReadString(nameLength);
    std::string password = recvPacket.ReadString(passwordLength);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_PASSWORD {} Channel: {}, Password: {}",
                   GetPlayerInfo(), channelName, password);
    if (password.length() > MAX_CHANNEL_PASS_STR)
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->Password(GetPlayer(), password);
}

void WorldSession::HandleChannelSetOwner(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_SET_OWNER {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->SetOwner(GetPlayer(), targetName);
}

void WorldSession::HandleChannelOwner(WorldPacket& recvPacket)
{
    std::string channelName = ReadChannelName(recvPacket);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_OWNER {} Channel: {}",
                   GetPlayerInfo(), channelName);
    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->SendWhoOwner(GetPlayer()->GetGUID());
}

void WorldSession::HandleChannelModerator(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_MODERATOR {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->SetModerator(GetPlayer(), targetName);
}

void WorldSession::HandleChannelUnmoderator(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_UNMODERATOR {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->UnsetModerator(GetPlayer(), targetName);
}

void WorldSession::HandleChannelMute(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHANNEL_MUTE {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->SetMute(GetPlayer(), targetName);
}

void WorldSession::HandleChannelUnmute(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHANNEL_UNMUTE {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->UnsetMute(GetPlayer(), targetName);
}

void WorldSession::HandleChannelInvite(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_INVITE {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->Invite(GetPlayer(), targetName);
}

void WorldSession::HandleChannelKick(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_KICK {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->Kick(GetPlayer(), targetName);
}

void WorldSession::HandleChannelBan(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_BAN {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->Ban(GetPlayer(), targetName);
}

void WorldSession::HandleChannelUnban(WorldPacket& recvPacket)
{
    std::string channelName, targetName;
    ReadChannelPlayerCommand(recvPacket, channelName, targetName);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_UNBAN {} Channel: {}, Target: {}",
                   GetPlayerInfo(), channelName, targetName);
    if (!normalizePlayerName(targetName))
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->UnBan(GetPlayer(), targetName);
}

void WorldSession::HandleChannelAnnouncements(WorldPacket& recvPacket)
{
    std::string channelName = ReadChannelName(recvPacket);

    LOG_DEBUG("chat.system", "CMSG_CHAT_CHANNEL_ANNOUNCEMENTS {} Channel: {}",
        GetPlayerInfo(), channelName);
    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, GetPlayer()))
            channel->Announce(GetPlayer());
}

void WorldSession::HandleChannelModerateOpcode(WorldPacket& recvPacket)
{
    std::string channelName = ReadChannelName(recvPacket);

    LOG_DEBUG("chat.system", "CMSG_CHANNEL_MODERATE {} Channel: {}",
        GetPlayerInfo(), channelName);

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* chn = cMgr->GetChannel(channelName, GetPlayer()))
            chn->ToggleModeration(GetPlayer());
}

void WorldSession::HandleChannelDisplayListQuery(WorldPacket& recvPacket)
{
    // this should be OK because the 2 function _were_ the same
    HandleChannelList(recvPacket);
}

void WorldSession::HandleSetChannelWatch(WorldPacket& recvPacket)
{
    std::string channelName = ReadChannelName(recvPacket);

    GetPlayer()->ClearChannelWatch();

    if (channelName.empty())
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, nullptr, false))
            channel->AddWatching(GetPlayer());
}

void WorldSession::HandleClearChannelWatch(WorldPacket& recvPacket)
{
    std::string channelName = ReadChannelName(recvPacket);

    if (channelName.empty())
        return;

    if (ChannelMgr* cMgr = ChannelMgr::forTeam(GetPlayer()->GetTeamId()))
        if (Channel* channel = cMgr->GetChannel(channelName, nullptr, false))
            channel->RemoveWatching(GetPlayer());
}
