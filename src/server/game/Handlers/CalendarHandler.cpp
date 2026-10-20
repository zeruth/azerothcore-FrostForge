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

/*
----- Opcodes Not Used yet -----

SMSG_CALENDAR_INVITE_NOTES        [ packguid(Invitee), uint64(inviteId), string(Text), Boolean(Unk) ]
?CMSG_CALENDAR_EVENT_INVITE_NOTES       [ uint32(unk1), uint32(unk2), uint32(unk3), uint32(unk4), uint32(unk5) ]
SMSG_CALENDAR_INVITE_NOTES_ALERT  [ uint64(inviteId), string(Text) ]
SMSG_CALENDAR_INVITE_STATUS_ALERT [ uint64(eventId), uint32(eventTime), uint32(unkFlag), uint8(deletePending) ]

----- TODO -----

Finish complains' handling - what to do with received complains and how to respond?
Find out what to do with all "not used yet" opcodes
Correct errors sending (event/invite not found, invites exceeded, event already passed, permissions etc.)
Fix locked events to be displayed properly and response time shouldn't be shown for people that haven't respond yet
Copied events should probably have a new owner

*/

#include "ArenaTeamMgr.h"
#include "CalendarMgr.h"
#include "CalendarPackets.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "GameEventMgr.h"
#include "GameTime.h"
#include "GuildMgr.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "SocialMgr.h"
#include "WorldSession.h"
#include <utf8.h>

/// Difficulty.db2's id of an instance save: AC's 5-man 0/1 are 1/2, its raid 0-3 are 3-6
static uint32 CalendarDifficultyId(uint32 mapId, uint32 difficulty)
{
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
    return difficulty + (mapEntry && mapEntry->IsRaid() ? 3 : 1);
}

/// SMSG_CALENDAR_SEND_CALENDAR, 3.4.3 (FUN_1406f8dd0, handler FUN_141c3dc20): packed ServerTime, u32 InviteCount, u32
/// EventCount, u32 RaidLockoutCount, u32 RaidResetCount, the lockouts (u64 InstanceID, i32 MapID, u32 DifficultyID, i32
/// seconds left), the resets (i32 MapID, u32, u32 Duration, i32 Offset, u32 DifficultyID), the invites (u64 EventID,
/// u64 InviteID, u8 Status, u8 Moderator, u8 InviteType, PackedGuid128 InviterGuid, bit, flush), the events (u64
/// EventID, u8 EventType, packed Date, u32 Flags, i32 TextureID, u64 EventClubID, PackedGuid128 OwnerGuid, bits(8)
/// NameLen, flush, Name). No holidays: the client reads Holidays.db2.
void WorldSession::HandleCalendarGetCalendar(WorldPacket& /*recvData*/)
{
    ObjectGuid guid = _player->GetGUID();
    LOG_DEBUG("network", "CMSG_CALENDAR_GET [{}]", guid.ToString());

    time_t currTime = GameTime::GetGameTime().count();

    CalendarInviteStore invites = sCalendarMgr->GetPlayerInvites(guid);
    CalendarEventStore playerEvents = sCalendarMgr->GetPlayerEvents(guid);

    ByteBuffer lockouts;
    uint32 lockoutCount = 0;
    for (uint8 i = 0; i < MAX_DIFFICULTY; ++i)
    {
        BoundInstancesMap const& m_boundInstances = sInstanceSaveMgr->PlayerGetBoundInstances(_player->GetGUID(), Difficulty(i));
        for (BoundInstancesMap::const_iterator itr = m_boundInstances.begin(); itr != m_boundInstances.end(); ++itr)
        {
            if (itr->second.perm)
            {
                InstanceSave const* save = itr->second.save;
                time_t resetTime = itr->second.extended ? save->GetExtendedResetTime() : save->GetResetTime();
                lockouts << uint64(save->GetInstanceId());
                lockouts << int32(save->GetMapId());
                lockouts << uint32(CalendarDifficultyId(save->GetMapId(), save->GetDifficulty()));
                lockouts << int32(resetTime >= currTime ? resetTime - currTime : 0);
                ++lockoutCount;
            }
        }
    }

    // the raid resets: the client works the next reset out from the period and its own raid origin
    ByteBuffer resets;
    uint32 resetCount = 0;
    ResetTimeByMapDifficultyMap const& resetTimes = sInstanceSaveMgr->GetResetTimeMap();
    for (ResetTimeByMapDifficultyMap::const_iterator itr = resetTimes.begin(); itr != resetTimes.end(); ++itr)
    {
        uint32 mapId = PAIR32_LOPART(itr->first);
        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry || !mapEntry->IsRaid())
            continue;

        Difficulty difficulty = Difficulty(PAIR32_HIPART(itr->first));
        time_t period = sInstanceSaveMgr->GetExtendedResetTimeFor(mapId, difficulty) - itr->second;
        resets << int32(mapId);
        resets << uint32(0);
        resets << uint32(period);                           // Duration, seconds
        resets << int32(0);                                 // Offset [TODO: relative to the client's region raid origin]
        resets << uint32(CalendarDifficultyId(mapId, difficulty));
        ++resetCount;
    }

    WorldPacket data(SMSG_CALENDAR_SEND_CALENDAR, 20 + lockouts.size() + resets.size() + invites.size() * 40 + playerEvents.size() * 80);
    data.AppendPackedTime(currTime);
    data << uint32(invites.size());
    data << uint32(playerEvents.size());
    data << uint32(lockoutCount);
    data << uint32(resetCount);
    data.append(lockouts);
    data.append(resets);

    for (CalendarInviteStore::const_iterator itr = invites.begin(); itr != invites.end(); ++itr)
    {
        CalendarEvent* calendarEvent = sCalendarMgr->GetEvent((*itr)->GetEventId());
        data << uint64((*itr)->GetEventId());
        data << uint64((*itr)->GetInviteId());
        data << uint8((*itr)->GetStatus());
        data << uint8((*itr)->GetRank());
        data << uint8(calendarEvent && calendarEvent->IsGuildEvent() && (*itr)->GetSenderGUID() == (*itr)->GetInviteeGUID() ? 1 : 0);
        Guid128::Write(data, calendarEvent ? calendarEvent->GetCreatorGUID() : (*itr)->GetSenderGUID());
        data.WriteBit(false);                               // IgnoreFriendAndGuildRestriction
        data.FlushBits();
    }

    for (CalendarEventStore::const_iterator itr = playerEvents.begin(); itr != playerEvents.end(); ++itr)
    {
        CalendarEvent* calendarEvent = *itr;
        std::string const& title = calendarEvent->GetTitle();

        data << uint64(calendarEvent->GetEventId());
        data << uint8(calendarEvent->GetType());
        data.AppendPackedTime(calendarEvent->GetEventTime());
        data << uint32(CalendarMgr::GetWireFlags(*calendarEvent));
        data << int32(calendarEvent->GetDungeonId());
        data << uint64(CalendarMgr::GetClubId(*calendarEvent));
        Guid128::Write(data, calendarEvent->GetCreatorGUID());
        data.WriteBits(title.size(), 8);
        data.FlushBits();
        data.WriteString(title);
    }

    SendPacket(&data);
}

void WorldSession::HandleCalendarGetEvent(WorldPackets::Calendar::GetEvent& packet)
{
    LOG_DEBUG("network", "CMSG_CALENDAR_GET_EVENT. Player [{}] Event [{}]", _player->GetGUID().ToString(), packet.EventId);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(packet.EventId))
        sCalendarMgr->SendCalendarEvent(_player->GetGUID(), *calendarEvent, CALENDAR_SENDTYPE_GET);
    else
        sCalendarMgr->SendCalendarCommandResult(_player->GetGUID(), CALENDAR_ERROR_EVENT_INVALID);
}

bool validUtf8String(WorldPacket& recvData, std::string& s, std::string action, ObjectGuid playerGUID)
{
    if (!utf8::is_valid(s.begin(), s.end()))
    {
        LOG_INFO("network.opcode", "CalendarHandler: Player ({}) attempt to {} an event with invalid name or description (packet modification)",
            playerGUID.ToString(), action);
        recvData.rfinish();
        return false;
    }
    return true;
}

/// CMSG_CALENDAR_ADD_EVENT, 3.4.3 (FUN_14074e080): u64 ClubID, u8 EventType, i32 TextureID, packed Time, u32 Flags,
/// u32 InviteCount, bits(8) TitleLen, bits(11) DescriptionLen, flush, the invites (PackedGuid128, u8 Status, u8
/// Moderator, bits HasUnused1..3, flush, [PackedGuid128] [u64] [u64]), Title, Description, u32 MaxSize. 3.3.5's
/// repeatable, max invites and time-zone time are gone.
void WorldSession::HandleCalendarAddEvent(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();

    uint64 clubId;
    uint8 type;
    int32 dungeonId;
    uint32 eventPackedTime;
    uint32 flags;
    uint32 inviteCount;

    recvData >> clubId >> type >> dungeonId;
    recvData.ReadPackedTime(eventPackedTime);
    recvData >> flags >> inviteCount;
    uint32 titleLength = recvData.ReadBits(8);
    uint32 descriptionLength = recvData.ReadBits(11);

    struct PreInvite { ObjectGuid Guid; uint8 Status; uint8 Rank; };
    std::vector<PreInvite> preInvites;
    for (uint32 i = 0; i < inviteCount; ++i)
    {
        PreInvite invite;
        invite.Guid = Guid128::Read(recvData);
        recvData >> invite.Status >> invite.Rank;
        recvData.ResetBitReader();
        bool hasUnused1 = recvData.ReadBit();
        bool hasUnused2 = recvData.ReadBit();
        bool hasUnused3 = recvData.ReadBit();
        if (hasUnused1)
            Guid128::Read(recvData);
        if (hasUnused2)
            recvData.read_skip<uint64>();
        if (hasUnused3)
            recvData.read_skip<uint64>();
        if (i < CALENDAR_MAX_INVITES)
            preInvites.push_back(invite);
    }

    std::string title = recvData.ReadString(titleLength);
    std::string description = recvData.ReadString(descriptionLength);
    recvData.read_skip<uint32>();                           // MaxSize
    (void)clubId;                                           // the player's own guild, for a guild event

    // prevent attacks with non-utf8 chars -> with multiple packets it will hang up the db due to errors.
    if (!validUtf8String(recvData, title, "create", guid) || title.size() > 31 || !validUtf8String(recvData, description, "create", guid) || description.size() > 255)
        return;

    // prevent events in the past
    // To Do: properly handle timezones and remove the "- time_t(86400L)" hack
    if (time_t(eventPackedTime) < (GameTime::GetGameTime().count() - time_t(86400L)))
    {
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_PASSED);
        return;
    }

    // If the event is a guild event, check if the player is in a guild
    if (CalendarEvent::IsGuildEvent(flags) || CalendarEvent::IsGuildAnnouncement(flags))
    {
        if (!_player->GetGuildId())
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_GUILD_PLAYER_NOT_IN_GUILD);
            return;
        }
    }

    // Check if the player reached the max number of events allowed to create
    if (CalendarEvent::IsGuildEvent(flags) || CalendarEvent::IsGuildAnnouncement(flags))
    {
        if (sCalendarMgr->GetGuildEvents(_player->GetGuildId()).size() >= CALENDAR_MAX_GUILD_EVENTS)
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_GUILD_EVENTS_EXCEEDED);
            return;
        }
    }
    else
    {
        if (sCalendarMgr->GetEventsCreatedBy(guid).size() >= CALENDAR_MAX_EVENTS)
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENTS_EXCEEDED);
            return;
        }
    }

    if (GetCalendarEventCreationCooldown() > GameTime::GetGameTime().count())
    {
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_INTERNAL);
        return;
    }
    SetCalendarEventCreationCooldown(GameTime::GetGameTime().count() + CALENDAR_CREATE_EVENT_COOLDOWN);

    // AzerothCore keeps 0x400 as its guild event flag; 0x800 is only how 3.4.3 says it
    flags &= ~uint32(0x800);
    CalendarEvent* calendarEvent = new CalendarEvent(sCalendarMgr->GetFreeEventId(), guid, 0, CalendarEventType(type), dungeonId,
            time_t(eventPackedTime), flags, time_t(eventPackedTime), title, description);

    if (calendarEvent->IsGuildEvent() || calendarEvent->IsGuildAnnouncement())
        if (Player* creator = ObjectAccessor::FindConnectedPlayer(guid))
            calendarEvent->SetGuildId(creator->GetGuildId());

    if (calendarEvent->IsGuildAnnouncement())
    {
        // 946684800 is 01/01/2000 00:00:00 - default response time
        CalendarInvite* invite = new CalendarInvite(0, calendarEvent->GetEventId(), ObjectGuid::Empty, guid, 946684800, CALENDAR_STATUS_NOT_SIGNED_UP, CALENDAR_RANK_PLAYER, "");
        sCalendarMgr->AddInvite(calendarEvent, invite);
    }
    else
    {
        // the creator's own invite: the calendar lists a non-guild event only to its invitees [the 3.3.5 client sent
        // the creator among the pre-invites; whether 3.4.3's does is not verified, so one is added when missing]
        if (!calendarEvent->IsGuildEvent() && std::none_of(preInvites.begin(), preInvites.end(), [&](PreInvite const& p) { return p.Guid == guid; }))
            preInvites.insert(preInvites.begin(), PreInvite{ guid, uint8(CALENDAR_STATUS_CONFIRMED), uint8(CALENDAR_RANK_OWNER) });

        CharacterDatabaseTransaction trans;
        if (preInvites.size() > 1)
            trans = CharacterDatabase.BeginTransaction();

        for (PreInvite const& preInvite : preInvites)
        {
            // 946684800 is 01/01/2000 00:00:00 - default response time
            CalendarInvite* invite = new CalendarInvite(sCalendarMgr->GetFreeInviteId(), calendarEvent->GetEventId(), preInvite.Guid, guid, 946684800, CalendarInviteStatus(preInvite.Status), CalendarModerationRank(preInvite.Rank), "");
            sCalendarMgr->AddInvite(calendarEvent, invite, trans);
        }

        if (preInvites.size() > 1)
            CharacterDatabase.CommitTransaction(trans);
    }

    sCalendarMgr->AddEvent(calendarEvent, CALENDAR_SENDTYPE_ADD);
}

/// CMSG_CALENDAR_UPDATE_EVENT, 3.4.3 (FUN_140765600): u64 ClubID, u64 EventID, u64 ModeratorID, u8 EventType, i32
/// TextureID, packed Time, u32 Flags, bits(8) TitleLen, bits(11) DescriptionLen, flush, Title, Description, u32 MaxSize
void WorldSession::HandleCalendarUpdateEvent(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    time_t oldEventTime;

    uint64 clubId;
    uint64 eventId;
    uint64 inviteId;
    uint8 type;
    int32 dungeonId;
    uint32 eventPackedTime;
    uint32 flags;

    recvData >> clubId >> eventId >> inviteId >> type >> dungeonId;
    recvData.ReadPackedTime(eventPackedTime);
    recvData >> flags;
    uint32 titleLength = recvData.ReadBits(8);
    uint32 descriptionLength = recvData.ReadBits(11);
    std::string title = recvData.ReadString(titleLength);
    std::string description = recvData.ReadString(descriptionLength);
    recvData.read_skip<uint32>();                           // MaxSize

    // prevent attacks with non-utf8 chars -> with multiple packets it will hang up the db due to errors.
    if (!validUtf8String(recvData, title, "update", guid) || title.size() > 31 || !validUtf8String(recvData, description, "update", guid) || description.size() > 255)
        return;

    // prevent events in the past
    // To Do: properly handle timezones and remove the "- time_t(86400L)" hack
    if (time_t(eventPackedTime) < (GameTime::GetGameTime().count() - time_t(86400L)))
    {
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_PASSED);
        return;
    }

    LOG_DEBUG("network", "CMSG_CALENDAR_UPDATE_EVENT [{}] EventId [{}], InviteId [{}] Title {}, Description {}, type {} "
        "Dungeon ID {}, Time {}, Flags {}",
        guid.ToString(), eventId, inviteId, title, description, type, dungeonId, eventPackedTime, flags);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        oldEventTime = calendarEvent->GetEventTime();

        calendarEvent->SetType(CalendarEventType(type));
        calendarEvent->SetFlags(flags & ~uint32(0x800));
        calendarEvent->SetEventTime(time_t(eventPackedTime));
        calendarEvent->SetTimeZoneTime(time_t(eventPackedTime));
        calendarEvent->SetDungeonId(dungeonId);
        calendarEvent->SetTitle(title);
        calendarEvent->SetDescription(description);

        sCalendarMgr->UpdateEvent(calendarEvent);
        sCalendarMgr->SendCalendarEventUpdateAlert(*calendarEvent, oldEventTime);
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

/// CMSG_CALENDAR_REMOVE_EVENT, 3.4.3 (FUN_1407654c0): u64 EventID, u64 ModeratorID, u64 ClubID, u32 Flags
void WorldSession::HandleCalendarRemoveEvent(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    uint64 eventId;

    recvData >> eventId;
    recvData.rfinish(); // Skip moderator, club & flags, we don't use them

    sCalendarMgr->RemoveEvent(eventId, guid);
}

/// CMSG_CALENDAR_COPY_EVENT, 3.4.3 (FUN_140765160): u64 EventID, u64 ModeratorID, u64 EventClubID, packed Date
void WorldSession::HandleCalendarCopyEvent(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    uint64 eventId;
    uint64 inviteId;
    uint64 clubId;
    uint32 eventTime;

    recvData >> eventId >> inviteId >> clubId;
    recvData.ReadPackedTime(eventTime);
    LOG_DEBUG("network", "CMSG_CALENDAR_COPY_EVENT [{}], EventId [{}] inviteId [{}] Time: {}", guid.ToString(), eventId, inviteId, eventTime);

    // prevent events in the past
    // To Do: properly handle timezones and remove the "- time_t(86400L)" hack
    if (time_t(eventTime) < (GameTime::GetGameTime().count() - time_t(86400L)))
    {
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_PASSED);
        return;
    }

    if (CalendarEvent* oldEvent = sCalendarMgr->GetEvent(eventId))
    {
        // Ensure that the player has access to the event
        if (oldEvent->IsGuildEvent() || oldEvent->IsGuildAnnouncement())
        {
            if (oldEvent->GetGuildId() != _player->GetGuildId())
            {
                sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
                return;
            }
        }
        else
        {
            if (oldEvent->GetCreatorGUID() != guid)
            {
                sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
                return;
            }
        }

        // Check if the player reached the max number of events allowed to create
        if (oldEvent->IsGuildEvent() || oldEvent->IsGuildAnnouncement())
        {
            if (sCalendarMgr->GetGuildEvents(_player->GetGuildId()).size() >= CALENDAR_MAX_GUILD_EVENTS)
            {
                sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_GUILD_EVENTS_EXCEEDED);
                return;
            }
        }
        else
        {
            if (sCalendarMgr->GetEventsCreatedBy(guid).size() >= CALENDAR_MAX_EVENTS)
            {
                sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENTS_EXCEEDED);
                return;
            }
        }

        if (GetCalendarEventCreationCooldown() > GameTime::GetGameTime().count())
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_INTERNAL);
            return;
        }
        SetCalendarEventCreationCooldown(GameTime::GetGameTime().count() + CALENDAR_CREATE_EVENT_COOLDOWN);

        CalendarEvent* newEvent = new CalendarEvent(*oldEvent, sCalendarMgr->GetFreeEventId());
        newEvent->SetEventTime(time_t(eventTime));
        sCalendarMgr->AddEvent(newEvent, CALENDAR_SENDTYPE_COPY);

        CalendarInviteStore invites = sCalendarMgr->GetEventInvites(eventId);
        CharacterDatabaseTransaction trans;
        if (invites.size() > 1)
            trans = CharacterDatabase.BeginTransaction();

        for (CalendarInviteStore::const_iterator itr = invites.begin(); itr != invites.end(); ++itr)
            sCalendarMgr->AddInvite(newEvent, new CalendarInvite(**itr, sCalendarMgr->GetFreeInviteId(), newEvent->GetEventId()), trans);

        if (invites.size() > 1)
            CharacterDatabase.CommitTransaction(trans);
        // should we change owner when somebody makes a copy of event owned by another person?
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

/// CMSG_CALENDAR_INVITE, 3.4.3 (FUN_1407652c0): u64 EventID, u64 ModeratorID, u64 ClubID, bits(9) NameLen, bit
/// IsSignUp, bit Creating (AC's pre-invite: the event is not saved yet), flush, Name
void WorldSession::HandleCalendarEventInvite(WorldPacket& recvData)
{
    LOG_DEBUG("network", "CMSG_CALENDAR_INVITE");

    ObjectGuid playerGuid = _player->GetGUID();

    uint64 eventId;
    uint64 inviteId;
    uint64 clubId;

    ObjectGuid inviteeGuid;
    uint32 inviteeTeamId = TEAM_NEUTRAL;
    uint32 inviteeGuildId = 0;

    recvData >> eventId >> inviteId >> clubId;
    uint32 nameLength = recvData.ReadBits(9);
    bool isGuildEvent = recvData.ReadBit();                 // IsSignUp
    bool isPreInvite = recvData.ReadBit();                  // Creating
    std::string name = recvData.ReadString(nameLength);

    if (Player* player = ObjectAccessor::FindPlayerByName(name.c_str(), false))
    {
        // Invitee is online
        inviteeGuid = player->GetGUID();
        inviteeTeamId = player->GetTeamId();
        inviteeGuildId = player->GetGuildId();
    }
    else
    {
        // xinef: Get Data From global storage
        if (ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name))
        {
            if (CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(guid))
            {
                inviteeGuid = guid;
                inviteeTeamId = Player::TeamIdForRace(playerData->Race);
                inviteeGuildId = playerData->GuildId;
            }
        }
    }

    if (!inviteeGuid)
    {
        sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_PLAYER_NOT_FOUND);
        return;
    }

    if (_player->GetTeamId() != inviteeTeamId && !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CALENDAR))
    {
        sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_NOT_ALLIED);
        return;
    }

    // xinef: sync query
    if (QueryResult result = CharacterDatabase.Query("SELECT flags FROM character_social WHERE guid = {} AND friend = {}", inviteeGuid.GetCounter(), playerGuid.GetCounter()))
    {
        Field* fields = result->Fetch();
        if (fields[0].Get<uint8>() & SOCIAL_FLAG_IGNORED)
        {
            sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_IGNORING_YOU_S, name.c_str());
            return;
        }
    }

    if (!isPreInvite)
    {
        if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
        {
            if (calendarEvent->IsGuildEvent() && calendarEvent->GetGuildId() == inviteeGuildId)
            {
                // we can't invite guild members to guild events
                sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_NO_GUILD_INVITES);
                return;
            }

            // 946684800 is 01/01/2000 00:00:00 - default response time
            CalendarInvite* invite = new CalendarInvite(sCalendarMgr->GetFreeInviteId(), eventId, inviteeGuid, playerGuid, 946684800, CALENDAR_STATUS_INVITED, CALENDAR_RANK_PLAYER, "");
            sCalendarMgr->AddInvite(calendarEvent, invite);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_EVENT_INVALID);
    }
    else
    {
        if (isGuildEvent && inviteeGuildId == _player->GetGuildId())
        {
            sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_NO_GUILD_INVITES);
            return;
        }

        // 946684800 is 01/01/2000 00:00:00 - default response time
        CalendarInvite* invite = new CalendarInvite(inviteId, 0, inviteeGuid, playerGuid, 946684800, CALENDAR_STATUS_INVITED, CALENDAR_RANK_PLAYER, "");
        sCalendarMgr->SendCalendarEventInvite(*invite);
        delete invite;
    }
}

/// CMSG_CALENDAR_EVENT_SIGN_UP, 3.4.3 (FUN_1407651d0): u64 EventID, u64 ClubID, bit Tentative, flush
void WorldSession::HandleCalendarEventSignup(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    uint64 eventId;
    uint64 clubId;

    recvData >> eventId >> clubId;
    bool tentative = recvData.ReadBit();
    LOG_DEBUG("network", "CMSG_CALENDAR_EVENT_SIGN_UP [{}] EventId [{}] Tentative {}", guid.ToString(), eventId, tentative);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (calendarEvent->IsGuildEvent() && calendarEvent->GetGuildId() != _player->GetGuildId())
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_GUILD_PLAYER_NOT_IN_GUILD);
            return;
        }

        CalendarInviteStatus status = tentative ? CALENDAR_STATUS_TENTATIVE : CALENDAR_STATUS_SIGNED_UP;
        CalendarInvite* invite = new CalendarInvite(sCalendarMgr->GetFreeInviteId(), eventId, guid, guid, GameTime::GetGameTime().count(), status, CALENDAR_RANK_PLAYER, "");
        sCalendarMgr->AddInvite(calendarEvent, invite);
        sCalendarMgr->SendCalendarClearPendingAction(guid);
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

/// CMSG_CALENDAR_RSVP, 3.4.3 (FUN_140765460): u64 EventID, u64 InviteID, u8 Status
void WorldSession::HandleCalendarEventRsvp(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    uint64 eventId;
    uint64 inviteId;
    uint8 status;

    recvData >> eventId >> inviteId >> status;
    LOG_DEBUG("network", "CMSG_CALENDAR_RSVP [{}] EventId [{}], InviteId [{}], status {}",
        guid.ToString(), eventId, inviteId, status);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        // i think we still should be able to remove self from locked events
        if (status != CALENDAR_STATUS_REMOVED && calendarEvent->GetFlags() & CALENDAR_FLAG_INVITES_LOCKED)
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_LOCKED);
            return;
        }

        if (CalendarInvite* invite = sCalendarMgr->GetInvite(inviteId))
        {
            invite->SetStatus(CalendarInviteStatus(status));
            invite->SetStatusTime(GameTime::GetGameTime().count());

            sCalendarMgr->UpdateInvite(invite);
            sCalendarMgr->SendCalendarEventStatus(*calendarEvent, *invite);
            sCalendarMgr->SendCalendarClearPendingAction(guid);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE); // correct?
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

/// CMSG_CALENDAR_REMOVE_INVITE, 3.4.3 (FUN_140765530): PackedGuid128 Guid (the invitee), u64 InviteID, u64 ModeratorID,
/// u64 EventID
void WorldSession::HandleCalendarEventRemoveInvite(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    ObjectGuid invitee = Guid128::Read(recvData);
    uint64 eventId;
    uint64 ownerInviteId; // isn't it sender's inviteId?
    uint64 inviteId;

    recvData >> inviteId >> ownerInviteId >> eventId;

    LOG_DEBUG("network", "CMSG_CALENDAR_REMOVE_INVITE [{}] EventId [{}], ownerInviteId [{}], Invitee ([{}] id: [{}])",
                   guid.ToString(), eventId, ownerInviteId, invitee.ToString(), inviteId);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (calendarEvent->GetCreatorGUID() == invitee)
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_DELETE_CREATOR_FAILED);
            return;
        }

        sCalendarMgr->RemoveInvite(inviteId, eventId, guid);
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE);
}

/// CMSG_CALENDAR_STATUS, 3.4.3 (FUN_140765590): PackedGuid128 Guid, u64 EventID, u64 InviteID, u64 ModeratorID, u8
/// Status
void WorldSession::HandleCalendarEventStatus(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    ObjectGuid invitee = Guid128::Read(recvData);
    uint64 eventId;
    uint64 inviteId;
    uint64 ownerInviteId; // isn't it sender's inviteId?
    uint8 status;

    recvData >> eventId >> inviteId >> ownerInviteId >> status;
    LOG_DEBUG("network", "CMSG_CALENDAR_STATUS [{}] EventId [{}] ownerInviteId [{}], Invitee ({}) id: [{}], status {}",
        guid.ToString(), eventId, ownerInviteId, invitee.ToString(), inviteId, status);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (CalendarInvite* invite = sCalendarMgr->GetInvite(inviteId))
        {
            invite->SetStatus((CalendarInviteStatus)status);
            invite->SetStatusTime(GameTime::GetGameTime().count());

            sCalendarMgr->UpdateInvite(invite);
            sCalendarMgr->SendCalendarEventStatus(*calendarEvent, *invite);
            sCalendarMgr->SendCalendarClearPendingAction(invitee);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE); // correct?
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

/// CMSG_CALENDAR_MODERATOR_STATUS, 3.4.3 (FUN_1407653f0): PackedGuid128 Guid, u64 EventID, u64 InviteID, u64
/// ModeratorID, u8 Status (the rank)
void WorldSession::HandleCalendarEventModeratorStatus(WorldPacket& recvData)
{
    ObjectGuid guid = _player->GetGUID();
    ObjectGuid invitee = Guid128::Read(recvData);
    uint64 eventId;
    uint64 inviteId;
    uint64 ownerInviteId; // isn't it sender's inviteId?
    uint8 rank;

    recvData >> eventId >>  inviteId >> ownerInviteId >> rank;
    LOG_DEBUG("network", "CMSG_CALENDAR_MODERATOR_STATUS [{}] EventId [{}] ownerInviteId [{}], Invitee ([{}] id: [{}], rank {}",
        guid.ToString(), eventId, ownerInviteId, invitee.ToString(), inviteId, rank);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (CalendarInvite* invite = sCalendarMgr->GetInvite(inviteId))
        {
            invite->SetRank(CalendarModerationRank(rank));
            sCalendarMgr->UpdateInvite(invite);
            sCalendarMgr->SendCalendarEventModeratorStatusAlert(*calendarEvent, *invite);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE); // correct?
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

/// CMSG_CALENDAR_COMPLAIN (CalendarComplain::Read). The client's calendar shows COMPLAINT_ADDED on
/// SMSG_COMPLAINT_RESULT type 12 (handler FUN_14173f5e0): u32 ComplaintType, u8 Result.
void WorldSession::HandleCalendarComplain(WorldPackets::Calendar::CalendarComplain& packet)
{
    if (sWorld->getBoolConfig(CONFIG_LOGSPAMREPORTS))
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_SPAM_REPORT);

        stmt->SetData(0, 2); // SpamType 2 = Calendar
        stmt->SetData(1, packet.ComplainGuid.GetCounter());
        stmt->SetData(2, 0);
        stmt->SetData(3, 0);
        stmt->SetData(4, 0);
        stmt->SetData(5, 0);
        stmt->SetData(6, "EventId: " + std::to_string(packet.EventId));
        stmt->SetData(7, GameTime::GetGameTime().count());

        CharacterDatabase.Execute(stmt);
    }

    WorldPacket data(SMSG_COMPLAINT_RESULT, 5);
    data << uint32(12);                                     // ComplaintType: calendar
    data << uint8(0);                                       // Result
    SendPacket(&data);
}

/// CMSG_CALENDAR_COMMUNITY_INVITE, 3.4.3 (FUN_1407650a0, Lua MassInviteGuild): u64 ClubID, u8 MinLevel, u8 MaxLevel,
/// u8 MaxRankOrder. The answer is SMSG_CALENDAR_COMMUNITY_INVITE (FUN_1406b9d80): u32 Count, Count x {PackedGuid128
/// InviteGuid, u8 Level}: the guild members in the level range whose rank is at most MaxRankOrder.
void WorldSession::HandleCalendarCommunityInvite(WorldPacket& recvData)
{
    uint64 clubId;
    uint8 minLevel;
    uint8 maxLevel;
    uint8 maxRankOrder;
    recvData >> clubId >> minLevel >> maxLevel >> maxRankOrder;

    Guild* guild = sGuildMgr->GetGuildById(_player->GetGuildId());
    if (!guild || clubId != guild->GetId())
        return;

    std::vector<std::pair<ObjectGuid, uint8>> members;
    for (auto const& [counter, member] : guild->GetUnorderedMembers())
        if (member.GetGUID() != _player->GetGUID() && member.GetLevel() >= minLevel && member.GetLevel() <= maxLevel && member.GetRankId() <= maxRankOrder)
            members.emplace_back(member.GetGUID(), member.GetLevel());

    WorldPacket data(SMSG_CALENDAR_COMMUNITY_INVITE, 4 + members.size() * 19);
    data << uint32(members.size());
    for (auto const& member : members)
    {
        Guid128::Write(data, member.first);
        data << uint8(member.second);
    }
    SendPacket(&data);
}

void WorldSession::HandleCalendarGetNumPending(WorldPacket& /*recvData*/)
{
    ObjectGuid guid = _player->GetGUID();
    uint32 pending = sCalendarMgr->GetPlayerNumPending(guid);

    LOG_DEBUG("network", "CMSG_CALENDAR_GET_NUM_PENDING: [{}] Pending: {}", guid.ToString(), pending);

    WorldPacket data(SMSG_CALENDAR_SEND_NUM_PENDING, 4);
    data << uint32(pending);
    SendPacket(&data);
}

void WorldSession::HandleSetSavedInstanceExtend(WorldPacket& recvData)
{
    uint32 mapId, difficulty;
    uint8 toggleExtendOn;
    recvData >> mapId >> difficulty >> toggleExtendOn;

    MapEntry const* entry = sMapStore.LookupEntry(mapId);
    if (!entry || !entry->IsRaid())
        return;

    InstancePlayerBind* instanceBind = sInstanceSaveMgr->PlayerGetBoundInstance(GetPlayer()->GetGUID(), mapId, Difficulty(difficulty));
    if (!instanceBind || !instanceBind->perm || (bool)toggleExtendOn == instanceBind->extended)
        return;

    instanceBind->extended = (bool)toggleExtendOn;

    // update in db
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_INSTANCE_EXTENDED);
    stmt->SetData(0, toggleExtendOn ? 1 : 0);
    stmt->SetData(1, GetPlayer()->GetGUID().GetCounter());
    stmt->SetData(2, instanceBind->save->GetInstanceId());
    CharacterDatabase.Execute(stmt);

    SendCalendarRaidLockoutUpdated(instanceBind->save, (bool)toggleExtendOn);
}

// ----------------------------------- SEND ------------------------------------

/// SMSG_CALENDAR_RAID_LOCKOUT_ADDED, 3.4.3 (raw, handler FUN_141c335c0): u64 InstanceID, packed ServerTime, i32 MapID,
/// u32 DifficultyID, i32 seconds left. SMSG_CALENDAR_RAID_LOCKOUT_REMOVED (raw, FUN_141c401e0): u64 InstanceID, i32
/// MapID, u32 DifficultyID.
void WorldSession::SendCalendarRaidLockout(InstanceSave const* save, bool add)
{
    LOG_DEBUG("network", "{}", add ? "SMSG_CALENDAR_RAID_LOCKOUT_ADDED" : "SMSG_CALENDAR_RAID_LOCKOUT_REMOVED");
    time_t currTime = GameTime::GetGameTime().count();

    WorldPacket data(add ? SMSG_CALENDAR_RAID_LOCKOUT_ADDED : SMSG_CALENDAR_RAID_LOCKOUT_REMOVED, 24);
    data << uint64(save->GetInstanceId());
    if (add)
        data.AppendPackedTime(currTime);
    data << int32(save->GetMapId());
    data << uint32(CalendarDifficultyId(save->GetMapId(), save->GetDifficulty()));
    if (add)
        data << int32(save->GetResetTime() >= currTime ? save->GetResetTime() - currTime : 0);
    SendPacket(&data);
}

/// SMSG_CALENDAR_RAID_LOCKOUT_UPDATED, 3.4.3 (raw, handler FUN_141c40930): packed ServerTime, i32 MapID, u32
/// DifficultyID, i32 old seconds left, i32 new seconds left
void WorldSession::SendCalendarRaidLockoutUpdated(InstanceSave const* save, bool isExtended)
{
    time_t currTime = GameTime::GetGameTime().count();
    time_t resetTime = isExtended ? save->GetExtendedResetTime() : save->GetResetTime();
    time_t resetTimeOp = isExtended ? save->GetResetTime() : save->GetExtendedResetTime();
    WorldPacket data(SMSG_CALENDAR_RAID_LOCKOUT_UPDATED, 20);
    data.AppendPackedTime(currTime);
    data << int32(save->GetMapId());
    data << uint32(CalendarDifficultyId(save->GetMapId(), save->GetDifficulty()));
    data << int32(resetTimeOp >= currTime ? resetTimeOp - currTime : resetTimeOp); // pussywizard: old time in secs to reset
    data << int32(resetTime >= currTime ? resetTime - currTime : 0); // pussywizard: new time in secs to reset
    SendPacket(&data);
}
