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
#include "CellImpl.h"
#include "RBAC.h"
#include "ChannelMgr.h"
#include "Chat.h"
#include "ChatPackets.h"
#include "Common.h"
#include "GameTime.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Language.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "Util.h"
#include "Warden.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

inline bool isNasty(uint8 c)
{
    if (c == '\t')
    {
        return false;
    }

    if (c <= '\037') // ASCII control block
    {
        return true;
    }

    return false;
}

/// 3.4.3 sends one opcode per kind of chat message (writers in FUN_141913710's SendChatMessage); reads
/// one into AC's (type, language, whisper target, channel, text). Strings carry no NUL, their lengths
/// travel in bit fields:
///   SAY, PARTY, RAID, RAID_WARNING, INSTANCE_CHAT: u32 Language, 11-bit text length, bit IsSecure, flush, text
///   YELL, GUILD, OFFICER: u32 Language, 11-bit length, flush, text
///   EMOTE, AFK, DND: 11-bit length, flush, text (no language)
///   WHISPER: u32 Language, 9-bit target length, 11-bit text length, flush, target, text
///   CHANNEL: u32 Language, PackedGuid128 ChannelGUID, 9-bit channel name length, 11-bit text length,
///            bit HasIsSecure, [bit IsSecure], flush, channel name, text
///   ADDON_MESSAGE: 5-bit prefix length, 8-bit text length, bit IsLogged, flush, u32 chat type, prefix, text
///   ADDON_MESSAGE_WHISPER: 9-bit target length, flush, the addon params above, PackedGuid128, target
/// Addon messages become AC's LANG_ADDON with the text "prefix\ttext".
static bool ReadChatMessage(WorldPacket& recvData, uint32& type, uint32& lang, std::string& to, std::string& channel, std::string& msg)
{
    auto readText = [&recvData](uint32 length) { return recvData.ReadString(length); };
    auto readAddon = [&](uint32& chatType)
    {
        uint32 prefixLength = recvData.ReadBits(5);
        uint32 textLength = recvData.ReadBits(8);
        recvData.ReadBit();                             // IsLogged
        recvData.ResetBitReader();
        chatType = recvData.read<uint32>();
        std::string prefix = readText(prefixLength);
        msg = prefix + '\t' + readText(textLength);
        lang = LANG_ADDON;
    };

    switch (recvData.GetOpcode())
    {
        case CMSG_CHAT_MESSAGE_SAY:
        case CMSG_CHAT_MESSAGE_PARTY:
        case CMSG_CHAT_MESSAGE_RAID:
        case CMSG_CHAT_MESSAGE_RAID_WARNING:
        case CMSG_CHAT_MESSAGE_INSTANCE_CHAT:
        {
            switch (recvData.GetOpcode())
            {
                case CMSG_CHAT_MESSAGE_SAY:           type = CHAT_MSG_SAY; break;
                case CMSG_CHAT_MESSAGE_PARTY:         type = CHAT_MSG_PARTY; break;
                case CMSG_CHAT_MESSAGE_RAID:          type = CHAT_MSG_RAID; break;
                case CMSG_CHAT_MESSAGE_RAID_WARNING:  type = CHAT_MSG_RAID_WARNING; break;
                default:                              type = CHAT_MSG_BATTLEGROUND; break;
            }
            lang = recvData.read<uint32>();
            uint32 length = recvData.ReadBits(11);
            recvData.ReadBit();                         // IsSecure
            msg = readText(length);
            return true;
        }
        case CMSG_CHAT_MESSAGE_YELL:
        case CMSG_CHAT_MESSAGE_GUILD:
        case CMSG_CHAT_MESSAGE_OFFICER:
        {
            type = recvData.GetOpcode() == CMSG_CHAT_MESSAGE_YELL ? CHAT_MSG_YELL
                : recvData.GetOpcode() == CMSG_CHAT_MESSAGE_GUILD ? CHAT_MSG_GUILD : CHAT_MSG_OFFICER;
            lang = recvData.read<uint32>();
            msg = readText(recvData.ReadBits(11));
            return true;
        }
        case CMSG_CHAT_MESSAGE_EMOTE:
        case CMSG_CHAT_MESSAGE_AFK:
        case CMSG_CHAT_MESSAGE_DND:
        {
            type = recvData.GetOpcode() == CMSG_CHAT_MESSAGE_EMOTE ? CHAT_MSG_EMOTE
                : recvData.GetOpcode() == CMSG_CHAT_MESSAGE_AFK ? CHAT_MSG_AFK : CHAT_MSG_DND;
            lang = LANG_UNIVERSAL;
            msg = readText(recvData.ReadBits(11));
            return true;
        }
        case CMSG_CHAT_MESSAGE_WHISPER:
        {
            type = CHAT_MSG_WHISPER;
            lang = recvData.read<uint32>();
            uint32 targetLength = recvData.ReadBits(9);
            uint32 textLength = recvData.ReadBits(11);
            to = readText(targetLength);
            msg = readText(textLength);
            return true;
        }
        case CMSG_CHAT_MESSAGE_CHANNEL:
        {
            type = CHAT_MSG_CHANNEL;
            lang = recvData.read<uint32>();
            Guid128::Skip(recvData);                    // ChannelGUID: AC finds the channel by name
            uint32 channelLength = recvData.ReadBits(9);
            uint32 textLength = recvData.ReadBits(11);
            if (recvData.ReadBit())                     // HasIsSecure
                recvData.ReadBit();
            channel = readText(channelLength);
            msg = readText(textLength);
            return true;
        }
        case CMSG_CHAT_ADDON_MESSAGE:
        {
            uint32 chatType = 0;
            readAddon(chatType);
            switch (chatType)
            {
                case CHAT_MSG_PARTY:
                case CHAT_MSG_RAID:
                case CHAT_MSG_GUILD:
                case CHAT_MSG_OFFICER:
                case CHAT_MSG_SAY:
                case CHAT_MSG_YELL:
                case CHAT_MSG_BATTLEGROUND:
                    type = chatType;
                    return true;
                default:
                    return false;
            }
        }
        case CMSG_CHAT_ADDON_MESSAGE_WHISPER:
        {
            uint32 targetLength = recvData.ReadBits(9);
            recvData.ResetBitReader();
            uint32 chatType = 0;
            readAddon(chatType);
            Guid128::Skip(recvData);
            to = readText(targetLength);
            type = CHAT_MSG_WHISPER;
            return true;
        }
        default:
            return false;
    }
}

void WorldSession::HandleMessagechatOpcode(WorldPacket& recvData)
{
    uint32 type = 0;
    uint32 lang = LANG_UNIVERSAL;
    std::string to, channel, msg;
    if (!ReadChatMessage(recvData, type, lang, to, channel, msg))
    {
        LOG_ERROR("network.opcode", "CHAT: unexpected chat message {} from {}", GetOpcodeNameForLogging(Opcodes(recvData.GetOpcode())), GetPlayerInfo());
        recvData.rfinish();
        return;
    }

    // 3.4.3 has one opcode for party, raid and instance chat; the leader variants are the server's
    if (lang != LANG_ADDON)
    {
        Group const* group = type == CHAT_MSG_BATTLEGROUND ? GetPlayer()->GetGroup()
            : (GetPlayer()->GetOriginalGroup() ? GetPlayer()->GetOriginalGroup() : GetPlayer()->GetGroup());
        if (group && group->IsLeader(GetPlayer()->GetGUID()))
        {
            if (type == CHAT_MSG_PARTY)
                type = CHAT_MSG_PARTY_LEADER;
            else if (type == CHAT_MSG_RAID)
                type = CHAT_MSG_RAID_LEADER;
            else if (type == CHAT_MSG_BATTLEGROUND)
                type = CHAT_MSG_BATTLEGROUND_LEADER;
        }
    }

    if (lang == LANG_UNIVERSAL && type != CHAT_MSG_AFK && type != CHAT_MSG_DND && type != CHAT_MSG_EMOTE)
    {
        LOG_ERROR("entities.player.cheat", "CMSG_MESSAGECHAT: Possible hacking-attempt: {} tried to send a message in universal language", GetPlayerInfo());
        ChatHandler(this).SendNotification(LANG_UNKNOWN_LANGUAGE);
        recvData.rfinish();
        return;
    }

    Player* sender = GetPlayer();

    // prevent talking at unknown language (cheating)
    LanguageDesc const* langDesc = GetLanguageDescByID(lang);
    if (!langDesc)
    {
        ChatHandler(this).SendNotification(LANG_UNKNOWN_LANGUAGE);
        recvData.rfinish();
        return;
    }

    if (langDesc->skill_id != 0 && !sender->HasSkill(langDesc->skill_id))
    {
        // also check SPELL_AURA_COMPREHEND_LANGUAGE (client offers option to speak in that language)
        bool foundAura = false;
        for (auto const& auraEff : sender->GetAuraEffectsByType(SPELL_AURA_COMPREHEND_LANGUAGE))
        {
            if (auraEff->GetMiscValue() == int32(lang))
            {
                foundAura = true;
                break;
            }
        }

        if (!foundAura)
        {
            ChatHandler(this).SendNotification(LANG_NOT_LEARNED_LANGUAGE);
            recvData.rfinish();
            return;
        }
    }

    // Trial accounts cannot speak in chat channels or guild/officer chat. Whisper is handled later.
    // Addon traffic (LANG_ADDON) is excluded; it carries the Warden Lua check response over guild chat.
    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_CHAT) && IsTrialAccount() && lang != LANG_ADDON
        && (type == CHAT_MSG_CHANNEL || type == CHAT_MSG_GUILD || type == CHAT_MSG_OFFICER))
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_RESTRICTED, LANG_UNIVERSAL, sender, sender, "");
        SendPacket(&data);
        recvData.rfinish();
        return;
    }

    // pussywizard: chatting on most chat types requires 2 hours played to prevent spam/abuse
    if (!HasPermission(rbac::RBAC_PERM_SKIP_CHECK_CHAT_CHANNEL_REQ))
    {
        switch (type)
        {
            case CHAT_MSG_ADDON:
            case CHAT_MSG_PARTY:
            case CHAT_MSG_RAID:
            case CHAT_MSG_GUILD:
            case CHAT_MSG_OFFICER:
            case CHAT_MSG_AFK:
            case CHAT_MSG_DND:
            case CHAT_MSG_RAID_LEADER:
            case CHAT_MSG_RAID_WARNING:
            case CHAT_MSG_BATTLEGROUND:
            case CHAT_MSG_BATTLEGROUND_LEADER:
            case CHAT_MSG_PARTY_LEADER:
                break;
            default:
            {
                if (sWorld->getBoolConfig(CONFIG_CHAT_MUTE_FIRST_LOGIN) && lang != LANG_ADDON)
                {
                    uint32 minutes = sWorld->getIntConfig(CONFIG_CHAT_TIME_MUTE_FIRST_LOGIN);

                    if (sender->GetTotalPlayedTime() < minutes * MINUTE)
                    {
                        ChatHandler(this).SendNotification(LANG_MUTED_PLAYER, minutes);
                        recvData.rfinish();
                        return;
                    }
                }
            }
        }
    }

    // pussywizard:
    switch (type)
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_TEXT_EMOTE:
        case CHAT_MSG_AFK:
        case CHAT_MSG_DND:
        if (sender->IsSpectator())
        {
            recvData.rfinish();
            return;
        }
    }

    if (sender->HasAura(1852) && type != CHAT_MSG_WHISPER)
    {
        ChatHandler(this).SendNotification(LANG_GM_SILENCE, sender->GetName());
        recvData.rfinish();
        return;
    }

    if (lang == LANG_ADDON)
    {
        // LANG_ADDON is only valid for the following message types
        switch (type)
        {
            case CHAT_MSG_PARTY:
            case CHAT_MSG_RAID:
            case CHAT_MSG_GUILD:
            case CHAT_MSG_BATTLEGROUND:
            case CHAT_MSG_WHISPER:
                // check if addon messages are disabled
                if (!sWorld->getBoolConfig(CONFIG_ADDON_CHANNEL))
                {
                    recvData.rfinish();
                    return;
                }
                break;
            default:
                LOG_ERROR("network", "Player {} ({}) sent a chatmessage with an invalid language/message type combination",
                               GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());

                recvData.rfinish();
                return;
        }
    }
    else
    {
        // send in universal language if player in .gmon mode (ignore spell effects)
        if (sender->IsGameMaster())
            lang = LANG_UNIVERSAL;
        else
        {
            // send in universal language in two side iteration allowed mode
            if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT))
                lang = LANG_UNIVERSAL;
            else
            {
                switch (type)
                {
                    case CHAT_MSG_PARTY:
                    case CHAT_MSG_PARTY_LEADER:
                    case CHAT_MSG_RAID:
                    case CHAT_MSG_RAID_LEADER:
                    case CHAT_MSG_RAID_WARNING:
                        // allow two side chat at group channel if two side group allowed
                        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP))
                            lang = LANG_UNIVERSAL;
                        break;
                    case CHAT_MSG_GUILD:
                    case CHAT_MSG_OFFICER:
                        // allow two side chat at guild channel if two side guild allowed
                        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD))
                            lang = LANG_UNIVERSAL;
                        break;
                }
            }
            // Overwritten by SPELL_AURA_MOD_LANGUAGE auras (Affects only Say and Yell)
            Unit::AuraEffectList const& ModLangAuras = sender->GetAuraEffectsByType(SPELL_AURA_MOD_LANGUAGE);
            if (!ModLangAuras.empty() && (type == CHAT_MSG_SAY || type == CHAT_MSG_YELL))
                lang = ModLangAuras.front()->GetMiscValue();
        }

        if (type != CHAT_MSG_AFK && type != CHAT_MSG_DND)
            sender->UpdateSpeakTime(lang == LANG_ADDON ? Player::ChatFloodThrottle::ADDON : Player::ChatFloodThrottle::REGULAR);
    }

    bool ignoreChecks = type == CHAT_MSG_AFK || type == CHAT_MSG_DND;
    std::wstring wmsg;
    if (lang != LANG_ADDON && !Utf8toWStr(msg, wmsg))
    {
        LOG_ERROR("network", "Player {} sent a chat message that is not valid UTF-8", GetPlayerInfo());
        return;
    }

    // Our Warden module also uses SendAddonMessage as a way to communicate Lua check results to the server, see if this is that
    if (type == CHAT_MSG_GUILD && lang == LANG_ADDON && _warden && _warden->ProcessLuaCheckResponse(msg))
    {
        return;
    }

    // pussywizard:
    if (msg.length() > 255 || (lang != LANG_ADDON && msg.find("|0") != std::string::npos))
        return;

    if (!ignoreChecks)
    {
        if (msg.empty())
            return;

        if (lang == LANG_ADDON)
        {
            if (AddonChannelCommandHandler(this).ParseCommands(msg.c_str()))
                return;
        }
        else
        {
            if (ChatHandler(this).ParseCommands(msg.c_str()))
                return;

            if (!_player->CanSpeak())
            {
                std::string timeStr = secsToTimeString(m_muteTime - GameTime::GetGameTime().count());
                ChatHandler(this).SendNotification(LANG_WAIT_BEFORE_SPEAKING, timeStr);
                return;
            }
        }
    }

    // do message validity checks
    if (lang != LANG_ADDON)
    {
        // cut at the first newline or carriage return
        std::string::size_type pos = msg.find_first_of("\n\r");

        if (pos == 0)
        {
            return;
        }
        else if (pos != std::string::npos)
        {
            msg.erase(pos);
        }

        // abort on any sort of nasty character
        for (uint8 c : msg)
        {
            if (isNasty(c))
            {
                LOG_ERROR("network", "Player {} {} sent a message containing invalid character {} - blocked", GetPlayer()->GetName(),
                    GetPlayer()->GetGUID().ToString(), uint8(c));
                return;
            }
        }

        // collapse multiple spaces into one
        if (sWorld->getBoolConfig(CONFIG_CHAT_FAKE_MESSAGE_PREVENTING))
        {
            auto end = std::unique(msg.begin(), msg.end(), [](char c1, char c2) { return (c1 == ' ') && (c2 == ' '); });
            msg.erase(end, msg.end());
        }

        // Validate hyperlinks
        if (!ValidateHyperlinksAndMaybeKick(msg))
        {
            return;
        }
    }

    else
    {
        ++_addonMessageReceiveCount;
    }

    sScriptMgr->OnPlayerBeforeSendChatMessage(_player, type, lang, msg);

    switch (type)
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_YELL:
            {
                // Prevent cheating
                if (!sender->IsAlive())
                    return;

                if (sender->GetLevel() < sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ))
                {
                    ChatHandler(this).SendNotification(LANG_SAY_REQ, sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ));
                    return;
                }

                if (type == CHAT_MSG_SAY)
                    sender->Say(msg, Language(lang));
                else if (type == CHAT_MSG_EMOTE)
                    sender->TextEmote(msg);
                else if (type == CHAT_MSG_YELL)
                    sender->Yell(msg, Language(lang));
            }
            break;
        case CHAT_MSG_WHISPER:
            {
                if (!normalizePlayerName(to))
                {
                    SendPlayerNotFoundNotice(to);
                    break;
                }

                Player* receiver = ObjectAccessor::FindPlayerByName(to, false);
                bool senderIsPlayer = !HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT);
                bool receiverIsPlayer = receiver ? !receiver->GetSession()->HasPermission(rbac::RBAC_PERM_TWO_SIDE_INTERACTION_CHAT) : true;

                if (sender->GetLevel() < sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ) && receiver != sender && receiver && !receiver->IsGameMaster())
                {
                    ChatHandler(this).SendNotification(LANG_WHISPER_REQ, sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ));
                    return;
                }

                if (!receiver || (lang != LANG_ADDON && !receiver->isAcceptWhispers() && receiver->GetSession()->HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS) && !receiver->IsInWhisperWhiteList(sender->GetGUID())))
                {
                    SendPlayerNotFoundNotice(to);
                    return;
                }

                // Trial accounts can only whisper players who have them on their friend list, or players who whispered them first.
                if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_CHAT) && IsTrialAccount() && receiver != sender
                    && !receiver->GetSocial()->HasFriend(sender->GetGUID())
                    && !sender->IsInWhisperWhiteList(receiver->GetGUID()))
                {
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_RESTRICTED, LANG_UNIVERSAL, sender, sender, "");
                    SendPacket(&data);
                    return;
                }

                if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT) && senderIsPlayer && receiverIsPlayer)
                    if (GetPlayer()->GetTeamId() != receiver->GetTeamId())
                    {
                        SendWrongFactionNotice();
                        return;
                    }

                if (GetPlayer()->HasAura(1852) && !receiver->IsGameMaster())
                {
                    ChatHandler(this).SendNotification(LANG_GM_SILENCE, GetPlayer()->GetName());
                    return;
                }

                // If player is a Gamemaster and doesn't accept whisper, we auto-whitelist every player that the Gamemaster is talking to
                // We also do that if a player is under the required level for whispers.
                if (receiver->GetLevel() < sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ) ||
                    (HasPermission(rbac::RBAC_PERM_CAN_FILTER_WHISPERS) && !sender->isAcceptWhispers() && !sender->IsInWhisperWhiteList(receiver->GetGUID())))
                    sender->AddWhisperWhiteList(receiver->GetGUID());

                // Allow a trial-account recipient to whisper back without being on the sender's friend list.
                if (receiver->GetSession()->IsTrialAccount() && !receiver->IsInWhisperWhiteList(sender->GetGUID()))
                    receiver->AddWhisperWhiteList(sender->GetGUID());

                GetPlayer()->Whisper(msg, Language(lang), receiver);
            }
            break;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            {
                // if player is in battleground, he cannot say to battleground members by /p
                Group* group = GetPlayer()->GetOriginalGroup();
                if (!group)
                {
                    group = sender->GetGroup();
                    if (!group || group->isBGGroup())
                        return;
                }

                if (type == CHAT_MSG_PARTY_LEADER && !group->IsLeader(sender->GetGUID()))
                    return;

                if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, group))
                    return;

                WorldPacket data;
                ChatHandler::BuildChatPacket(data, ChatMsg(type), Language(lang), sender, nullptr, msg);
                group->BroadcastPacket(&data, false, group->GetMemberGroup(GetPlayer()->GetGUID()));
            }
            break;
        case CHAT_MSG_GUILD:
            {
                if (GetPlayer()->GetGuildId())
                {
                    if (Guild* guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildId()))
                    {
                        if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, guild))
                            return;

                        guild->BroadcastToGuild(this, false, msg, lang == LANG_ADDON ? LANG_ADDON : LANG_UNIVERSAL);
                    }
                }
            }
            break;
        case CHAT_MSG_OFFICER:
            {
                if (GetPlayer()->GetGuildId())
                {
                    if (Guild* guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildId()))
                    {
                        if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, guild))
                            return;

                        guild->BroadcastToGuild(this, true, msg, lang == LANG_ADDON ? LANG_ADDON : LANG_UNIVERSAL);
                    }
                }
            }
            break;
        case CHAT_MSG_RAID:
            {
                // if player is in battleground, he cannot say to battleground members by /ra
                Group* group = GetPlayer()->GetOriginalGroup();
                if (!group)
                {
                    group = GetPlayer()->GetGroup();
                    if (!group || group->isBGGroup() || !group->isRaidGroup())
                        return;
                }

                if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, group))
                    return;

                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID, Language(lang), sender, nullptr, msg);
                group->BroadcastPacket(&data, false);
            }
            break;
        case CHAT_MSG_RAID_LEADER:
            {
                // if player is in battleground, he cannot say to battleground members by /ra
                Group* group = GetPlayer()->GetOriginalGroup();
                if (!group)
                {
                    group = GetPlayer()->GetGroup();
                    if (!group || group->isBGGroup() || !group->isRaidGroup() || !group->IsLeader(sender->GetGUID()))
                        return;
                }

                if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, group))
                    return;

                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_LEADER, Language(lang), sender, nullptr, msg);
                group->BroadcastPacket(&data, false);
            }
            break;
        case CHAT_MSG_RAID_WARNING:
            {
                Group* group = GetPlayer()->GetGroup();
                if (!group || !group->isRaidGroup() || !(group->IsLeader(GetPlayer()->GetGUID()) || group->IsAssistant(GetPlayer()->GetGUID())) || group->isBGGroup())
                    return;

                if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, group))
                    return;

                // In battleground, raid warning is sent only to players in battleground - code is ok
                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_RAID_WARNING, Language(lang), sender, nullptr, msg);
                group->BroadcastPacket(&data, false);
            }
            break;
        case CHAT_MSG_BATTLEGROUND:
            {
                //battleground raid is always in Player->GetGroup(), never in GetOriginalGroup()
                Group* group = GetPlayer()->GetGroup();
                if (!group || !group->isBGGroup())
                    return;

                if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, group))
                    return;

                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, Language(lang), sender, nullptr, msg);
                group->BroadcastPacket(&data, false);
            }
            break;
        case CHAT_MSG_BATTLEGROUND_LEADER:
            {
                // battleground raid is always in Player->GetGroup(), never in GetOriginalGroup()
                Group* group = GetPlayer()->GetGroup();
                if (!group || !group->isBGGroup() || !group->IsLeader(GetPlayer()->GetGUID()))
                    return;

                if (!sScriptMgr->OnPlayerCanUseChat(GetPlayer(), type, lang, msg, group))
                    return;

                WorldPacket data;
                ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND_LEADER, Language(lang), sender, nullptr, msg);
                group->BroadcastPacket(&data, false);
            }
            break;
        case CHAT_MSG_CHANNEL:
            {
                if (!HasPermission(rbac::RBAC_PERM_SKIP_CHECK_CHAT_CHANNEL_REQ))
                {
                    if (sender->GetLevel() < sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ))
                    {
                        ChatHandler(this).SendNotification(LANG_CHANNEL_REQ, sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ));
                        return;
                    }
                }

                if (ChannelMgr* cMgr = ChannelMgr::forTeam(sender->GetTeamId()))
                {
                    if (Channel* chn = cMgr->GetChannel(channel, sender))
                    {
                        if (!sScriptMgr->OnPlayerCanUseChat(sender, type, lang, msg, chn))
                            return;

                        chn->Say(sender->GetGUID(), msg.c_str(), lang);
                    }
                }
            }
            break;
        case CHAT_MSG_AFK:
            {
                if (!sender->IsInCombat())
                {
                    if (sender->isAFK())                       // Already AFK
                    {
                        if (msg.empty())
                            sender->ToggleAFK();               // Remove AFK
                        else
                            sender->autoReplyMsg = msg;        // Update message
                    }
                    else                                        // New AFK mode
                    {
                        sender->autoReplyMsg = msg.empty() ? GetAcoreString(LANG_PLAYER_AFK_DEFAULT) : msg;

                        if (sender->isDND())
                            sender->ToggleDND();

                        sender->ToggleAFK();
                    }

                    if (!sScriptMgr->OnPlayerCanUseChat(sender, type, lang, msg))
                        return;
                }
                break;
            }
        case CHAT_MSG_DND:
            {
                if (sender->isDND())                           // Already DND
                {
                    if (msg.empty())
                        sender->ToggleDND();                   // Remove DND
                    else
                        sender->autoReplyMsg = msg;            // Update message
                }
                else                                            // New DND mode
                {
                    sender->autoReplyMsg = msg.empty() ? GetAcoreString(LANG_PLAYER_DND_DEFAULT) : msg;

                    if (sender->isAFK())
                        sender->ToggleAFK();

                    sender->ToggleDND();
                }

                if (!sScriptMgr->OnPlayerCanUseChat(sender, type, lang, msg))
                    return;

                break;
            }
        default:
            LOG_ERROR("network.opcode", "CHAT: unknown message type {}, lang: {}", type, lang);
            break;
    }
}

void WorldSession::HandleEmoteOpcode(WorldPackets::Chat::EmoteClient& packet)
{
    if (GetPlayer()->IsSpectator())
        return;

    // 3.4.3's CMSG_EMOTE is empty (FUN_14075cc20): the client sends it to end the emote it shows
    // (a moving or acting player stops dancing), so it clears the emote state
    (void)packet;
    if (!_player->IsAlive() || _player->HasUnitState(UNIT_STATE_DIED))
        return;

    sScriptMgr->OnPlayerEmote(_player, EMOTE_ONESHOT_NONE);
    if (_player->GetUInt32Value(UNIT_NPC_EMOTESTATE))
        _player->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
}

namespace Acore
{
    class EmoteChatBuilder
    {
    public:
        EmoteChatBuilder(Player const& player, uint32 text_emote, uint32 emote_num, Unit const* target)
            : i_player(player), i_text_emote(text_emote), i_emote_num(emote_num), i_target(target) {}

        void operator()(WorldPacket& data, LocaleConstant loc_idx)
        {
            // SMSG_TEXT_EMOTE, 3.4.3 (FUN_1406cc110, handler FUN_141907360): PackedGuid128 SourceGUID,
            // SourceAccountGUID, i32 EmoteID, i32 SoundIndex, PackedGuid128 TargetGUID. The client
            // finds the target's name itself (it queries a player's), so none is sent.
            (void)loc_idx;
            data.Initialize(SMSG_TEXT_EMOTE, 2 * 18 + 8 + 18);
            Guid128::Write(data, i_player.GetGUID());
            Guid128::Write(data, ObjectGuid::Empty);
            data << int32(i_text_emote);
            data << int32(i_emote_num);
            Guid128::Write(data, i_target ? i_target->GetGUID() : ObjectGuid::Empty);
        }

    private:
        Player const& i_player;
        uint32        i_text_emote;
        uint32        i_emote_num;
        Unit const*   i_target;
    };
}                                                           // namespace Acore

void WorldSession::HandleTextEmoteOpcode(WorldPacket& recvData)
{
    // CMSG_SEND_TEXT_EMOTE, 3.4.3 (FUN_14074ef00): PackedGuid128 Target, i32 EmoteID, i32 SoundIndex,
    // u32 SpellVisualKitIDs count, i32 SequenceVariation, i32 SpellVisualKitIDs[count]
    ObjectGuid guid = Guid128::Read(recvData);
    uint32 text_emote = recvData.read<uint32>();
    uint32 emoteNum = recvData.read<uint32>();            // SoundIndex
    uint32 kitCount = recvData.read<uint32>();
    recvData.read_skip<int32>();                          // SequenceVariation
    recvData.read_skip(std::size_t(std::min<uint32>(kitCount, 16)) * 4);

    constexpr uint32 readyEmote = 126;

    // Handle confirmation redirect when the player types "/ready" after the new node becomes available for redirection.
    if (text_emote == readyEmote && GetPlayer()->GetMap()->IsPlayerRedirectKickTimerActive())
    {
        HandleTC9PrepareForRedirect(recvData);
        return;
    }

    if (!GetPlayer()->IsAlive())
        return;

    GetPlayer()->UpdateSpeakTime(Player::ChatFloodThrottle::REGULAR);

    if (!GetPlayer()->CanSpeak())
    {
        std::string timeStr = secsToTimeString(m_muteTime - GameTime::GetGameTime().count());
        ChatHandler(this).SendNotification(LANG_WAIT_BEFORE_SPEAKING, timeStr);
        return;
    }

    if (GetPlayer()->IsSpectator())
        return;

    sScriptMgr->OnPlayerTextEmote(GetPlayer(), text_emote, emoteNum, guid);

    EmotesTextEntry const* em = sEmotesTextStore.LookupEntry(text_emote);
    if (!em)
        return;

    uint32 emote_anim = em->textid;

    switch (emote_anim)
    {
        case EMOTE_STATE_SLEEP:
        case EMOTE_STATE_SIT:
        case EMOTE_STATE_KNEEL:
        case EMOTE_ONESHOT_NONE:
            break;
        case EMOTE_STATE_DANCE:
            GetPlayer()->SetUInt32Value(UNIT_NPC_EMOTESTATE, emote_anim);
            break;
        default:
            // Only allow text-emotes for "dead" entities (feign death included)
            if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
                break;
            GetPlayer()->HandleEmoteCommand(emote_anim);
            break;
    }

    Unit* unit = ObjectAccessor::GetUnit(*_player, guid);

    Acore::EmoteChatBuilder emote_builder(*GetPlayer(), text_emote, emoteNum, unit);
    Acore::LocalizedPacketDo<Acore::EmoteChatBuilder > emote_do(emote_builder);
    Acore::PlayerDistWorker<Acore::LocalizedPacketDo<Acore::EmoteChatBuilder > > emote_worker(GetPlayer(), sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), emote_do);
    Cell::VisitObjects(GetPlayer(), emote_worker, sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));

    GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DO_EMOTE, text_emote, 0, unit);

    //Send scripted event call
    if (unit && unit->IsCreature() && ((Creature*)unit)->AI())
        ((Creature*)unit)->AI()->ReceiveEmote(GetPlayer(), text_emote);
}

void WorldSession::HandleChatIgnoredOpcode(WorldPacket& recvData)
{
    // 3.4.3 (FUN_14075ae20): PackedGuid128 IgnoredGUID, u8 Reason (0 ignored, 1 a stranger's whisper)
    ObjectGuid iguid = Guid128::Read(recvData);
    recvData.read_skip<uint8>();

    Player* player = ObjectAccessor::FindConnectedPlayer(iguid);
    if (!player)
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_IGNORED, LANG_UNIVERSAL, _player, _player, GetPlayer()->GetName());
    player->SendDirectMessage(&data);
}

void WorldSession::HandleChannelDeclineInvite(WorldPacket& recvPacket)
{
    // used only with EXTRA_LOGS
    (void)recvPacket;

    LOG_DEBUG("network", "Opcode {}", recvPacket.GetOpcode());
}

void WorldSession::SendPlayerNotFoundNotice(std::string const& name)
{
    // 3.4.3: 9-bit name length, flush, the name
    WorldPacket data(SMSG_CHAT_PLAYER_NOTFOUND, 2 + name.size());
    data.WriteBits(std::min<std::size_t>(name.size(), 511), 9);
    data.FlushBits();
    data.WriteString(std::string_view(name).substr(0, 511));
    SendPacket(&data);
}

void WorldSession::SendPlayerAmbiguousNotice(std::string const& name)
{
    // 3.4.3: 9-bit name length, flush, the name
    WorldPacket data(SMSG_CHAT_PLAYER_AMBIGUOUS, 2 + name.size());
    data.WriteBits(std::min<std::size_t>(name.size(), 511), 9);
    data.FlushBits();
    data.WriteString(std::string_view(name).substr(0, 511));
    SendPacket(&data);
}

void WorldSession::SendWrongFactionNotice()
{
    // the 3.4.3 client has no SMSG_CHAT_WRONG_FACTION
}

void WorldSession::SendChatRestrictedNotice(ChatRestrictionType restriction)
{
    // 3.4.3 (handler FUN_14173f530): u8 Reason, 0 restricted, 1 throttled, 2 squelched, 3-4 trial
    WorldPacket data(SMSG_CHAT_RESTRICTED, 1);
    data << uint8(restriction);
    SendPacket(&data);
}
