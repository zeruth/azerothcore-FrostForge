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
#include "BattlefieldMgr.h"
#include "RBAC.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterPackets.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Common.h"
#include "DBCEnums.h"
#include "DatabaseEnv.h"
#include "GameObjectAI.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "Group.h"
#include "GuildMgr.h"
#include "InstancePackets.h"
#include "InstanceScript.h"
#include "Language.h"
#include "Log.h"
#include "LootMgr.h"
#include "MapMgr.h"
#include "MiscPackets.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "OutdoorPvP.h"
#include "Pet.h"
#include "Player.h"
#include "ItemPackets.h"
#include "PacketGuid128.h"
#include "QueryPackets.h"
#include "ReputationMgr.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "Spell.h"
#include "Vehicle.h"
#include "WhoListCacheMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <zlib.h>

#include "Corpse.h"

void WorldSession::HandleRepopRequestOpcode(WorldPacket& recv_data)
{
    LOG_DEBUG("network", "WORLD: Recvd CMSG_REPOP_REQUEST Message");

    // 3.4.3 (writer FUN_14075fd50): bit CheckInstance, flush
    recv_data.read_skip<uint8>();

    if (GetPlayer()->IsAlive() || GetPlayer()->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return;

    if (GetPlayer()->HasPreventResurectionAura())
        return; // silently return, client should display the error by itself

    // the world update order is sessions, players, creatures
    // the netcode runs in parallel with all of these
    // creatures can kill players
    // so if the server is lagging enough the player can
    // release spirit after he's killed but before he is updated
    if (GetPlayer()->getDeathState() == DeathState::JustDied)
    {
        LOG_DEBUG("network", "HandleRepopRequestOpcode: got request after player {} ({}) was killed and before he was updated",
            GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());
        GetPlayer()->KillPlayer();
    }

    //this is spirit release confirm?
    GetPlayer()->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);
    GetPlayer()->BuildPlayerRepop();
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleGossipSelectOptionOpcode(WorldPacket& recv_data)
{
    LOG_DEBUG("network", "WORLD: CMSG_GOSSIP_SELECT_OPTION");

    // 3.4.3 (writer FUN_14075d670): PackedGuid128 GossipUnit, u32 GossipID, u32 GossipOptionID, u8 code
    // length, the code (sent for every option, empty for those that ask for none)
    ObjectGuid guid = Guid128::Read(recv_data);
    uint32 menuId = recv_data.read<uint32>();
    uint32 gossipListId = recv_data.read<uint32>();
    std::string code = recv_data.ReadString(recv_data.read<uint8>());

    if (!_player->PlayerTalkClass->GetGossipMenu().GetItem(gossipListId))
    {
        recv_data.rfinish();
        return;
    }

    if (!_player->PlayerTalkClass->IsGossipOptionCoded(gossipListId))
        code.clear();

    // Prevent cheating on C++ scripted menus
    if (_player->PlayerTalkClass->GetGossipMenu().GetSenderGUID() != guid)
    {
        return;
    }

    Creature* unit = nullptr;
    GameObject* go = nullptr;
    Item* item = nullptr;
    if (guid.IsCreatureOrVehicle())
    {
        unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
        if (!unit)
        {
            LOG_DEBUG("network", "WORLD: HandleGossipSelectOptionOpcode - Unit ({}) not found or you can't interact with him.", guid.ToString());
            return;
        }
    }
    else if (guid.IsGameObject())
    {
        go = _player->GetMap()->GetGameObject(guid);
        if (!go)
        {
            LOG_DEBUG("network", "WORLD: HandleGossipSelectOptionOpcode - GameObject ({}) not found.", guid.ToString());
            return;
        }
    }
    else if (guid.IsItem())
    {
        item = _player->GetItemByGuid(guid);
        if (!item || _player->IsBankPos(item->GetPos()))
        {
            //LOG_DEBUG("network", "WORLD: HandleGossipSelectOptionOpcode - {} not found.", guid.ToString());
            return;
        }
    }
    else if (guid.IsPlayer())
    {
        if (guid != _player->GetGUID() || menuId != _player->PlayerTalkClass->GetGossipMenu().GetMenuId())
        {
            //LOG_DEBUG("network", "WORLD: HandleGossipSelectOptionOpcode - {} not found.", guid.ToString());
            return;
        }
    }
    else
    {
        LOG_DEBUG("network", "WORLD: HandleGossipSelectOptionOpcode - unsupported GUID type for {}.", guid.ToString());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    if ((unit && unit->GetScriptId() != unit->LastUsedScriptID) || (go && go->GetScriptId() != go->LastUsedScriptID))
    {
        LOG_DEBUG("network", "WORLD: HandleGossipSelectOptionOpcode - Script reloaded while in use, ignoring and set new scipt id");
        if (unit)
            unit->LastUsedScriptID = unit->GetScriptId();
        if (go)
            go->LastUsedScriptID = go->GetScriptId();
        _player->PlayerTalkClass->SendCloseGossip();
        return;
    }
    if (!code.empty())
    {
        if (unit)
        {
            unit->AI()->sGossipSelectCode(_player, menuId, gossipListId, code.c_str());
            if (!sScriptMgr->OnGossipSelectCode(_player, unit, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId), code.c_str()))
                _player->OnGossipSelect(unit, gossipListId, menuId);
        }
        else if (go)
        {
            go->AI()->GossipSelectCode(_player, menuId, gossipListId, code.c_str());
            sScriptMgr->OnGossipSelectCode(_player, go, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId), code.c_str());
        }
        else if (item)
        {
            sScriptMgr->OnGossipSelectCode(_player, item, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId), code.c_str());
        }
        else
        {
            sScriptMgr->OnPlayerGossipSelectCode(_player, menuId, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId), code.c_str());
        }
    }
    else
    {
        if (unit)
        {
            unit->AI()->sGossipSelect(_player, menuId, gossipListId);
            if (!sScriptMgr->OnGossipSelect(_player, unit, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId)))
                _player->OnGossipSelect(unit, gossipListId, menuId);
        }
        else if (go)
        {
            go->AI()->GossipSelect(_player, menuId, gossipListId);
            if (!sScriptMgr->OnGossipSelect(_player, go, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId)))
                _player->OnGossipSelect(go, gossipListId, menuId);
        }
        else if (item)
        {
            sScriptMgr->OnGossipSelect(_player, item, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId));
        }
        else
        {
            sScriptMgr->OnPlayerGossipSelect(_player, menuId, _player->PlayerTalkClass->GetGossipOptionSender(gossipListId), _player->PlayerTalkClass->GetGossipOptionAction(gossipListId));
        }
    }
}

void WorldSession::HandleWhoOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "WORLD: Recvd CMSG_WHO Message");

    // 3.4.3 (writer FUN_14076a270, WhoRequest FUN_14074fa00): 4-bit area count, bit IsFromAddOn, flush,
    // i32 MinLevel, i32 MaxLevel, i64 RaceFilter, i32 ClassFilter, 6-bit name, 9-bit realm, 7-bit guild,
    // 9-bit guild realm lengths, 3-bit word count, bits ShowEnemies, ShowArenaPlayers, ExactName,
    // HasServerInfo, flush, the words (7-bit length, flush, the word), the four strings, [i32, i32, u32
    // server info], u32 RequestID, u8 Origin, i32 areas
    uint32 zonesCount = recvData.ReadBits(4);
    recvData.ReadBit();
    recvData.ResetBitReader();
    int32 levelMin = recvData.read<int32>();
    int32 levelMax = recvData.read<int32>();
    uint64 racemask = recvData.read<uint64>();
    uint32 classmask = recvData.read<uint32>();
    uint32 nameLen = recvData.ReadBits(6);
    uint32 realmLen = recvData.ReadBits(9);
    uint32 guildLen = recvData.ReadBits(7);
    uint32 guildRealmLen = recvData.ReadBits(9);
    uint32 strCount = recvData.ReadBits(3);
    recvData.ReadBit();                                     // ShowEnemies
    recvData.ReadBit();                                     // ShowArenaPlayers
    recvData.ReadBit();                                     // ExactName
    bool hasServerInfo = recvData.ReadBit();
    recvData.ResetBitReader();

    std::wstring str[8];
    for (uint32 i = 0; i < strCount; ++i)
    {
        uint32 len = recvData.ReadBits(7);
        recvData.ResetBitReader();
        std::string temp = recvData.ReadString(len);
        if (!Utf8toWStr(temp, str[i]))
            continue;
        wstrToLower(str[i]);
        LOG_DEBUG("network.who", "String {}: {}", i, temp);
    }
    std::string packetPlayerName = recvData.ReadString(nameLen);
    recvData.read_skip(realmLen);
    std::string packetGuildName = recvData.ReadString(guildLen);
    recvData.read_skip(guildRealmLen);
    if (hasServerInfo)
        recvData.read_skip(12);
    uint32 requestId = recvData.read<uint32>();
    recvData.read_skip<uint8>();                            // Origin

    std::array<uint32, 16> zoneids = {};
    for (uint32 i = 0; i < zonesCount; ++i)
        zoneids[i] = recvData.read<int32>();

    LOG_DEBUG("network.who", "Minlvl {}, maxlvl {}, name {}, guild {}, racemask {}, classmask {}, zones {}, strings {}",
        levelMin, levelMax, packetPlayerName, packetGuildName, racemask, classmask, zonesCount, strCount);

    std::wstring wpacketPlayerName;
    std::wstring wpacketGuildName;
    if (!(Utf8toWStr(packetPlayerName, wpacketPlayerName) && Utf8toWStr(packetGuildName, wpacketGuildName)))
        return;

    wstrToLower(wpacketPlayerName);
    wstrToLower(wpacketGuildName);

    // client send in case not set max level value 100 but Acore supports 255 max level,
    // update it to show GMs with characters after 100 level
    if (levelMax >= MAX_LEVEL)
        levelMax = STRONG_MAX_LEVEL;

    uint32 team = _player->GetTeamId();
    uint32 gmLevelInWhoList = sWorld->getIntConfig(CONFIG_GM_LEVEL_IN_WHO_LIST);
    // the 6-bit entry count holds at most 63 players
    uint32 maxShown = std::min<uint32>(sWorld->getIntConfig(CONFIG_MAX_WHO_LIST_RETURN), 63);

    ByteBuffer entries(200);
    uint32 displaycount = 0;

    for (auto const& target : sWhoListCacheMgr->GetWhoList())
    {
        if (target.GetTeamId() != team && !HasPermission(rbac::RBAC_PERM_TWO_SIDE_WHO_LIST))
            continue;

        // player can see MODERATOR, GAME MASTER, ADMINISTRATOR only if CONFIG_GM_IN_WHO_LIST
        if (!HasPermission(rbac::RBAC_PERM_WHO_SEE_ALL_SEC_LEVELS) && target.GetSecurity() > AccountTypes(gmLevelInWhoList))
            continue;

        // check if target is globally visible for player
        if ((_player->GetGUID() != target.GetGuid() && !target.IsVisible()) &&
            (AccountMgr::IsPlayerAccount(_player->GetSession()->GetSecurity()) || target.GetSecurity() > _player->GetSession()->GetSecurity()))
            continue;

        // check if target's level is in level range
        int32 lvl = target.GetLevel();
        if (lvl < levelMin || lvl > levelMax)
            continue;

        // check if class matches classmask
        uint8 class_ = target.GetClass();
        if (!(classmask & (1 << class_)))
            continue;

        // check if race matches racemask (3.4.3: race N is bit N-1)
        uint32 race = target.GetRace();
        if (race && !(racemask & (uint64(1) << (race - 1))))
            continue;

        uint32 playerZoneId = target.GetZoneId();
        bool showZones = zonesCount == 0;
        for (uint32 i = 0; i < zonesCount; ++i)
            if (zoneids[i] == playerZoneId)
                showZones = true;
        if (!showZones)
            continue;

        std::wstring const& wideplayername = target.GetWidePlayerName();
        if (!(wpacketPlayerName.empty() || wideplayername.find(wpacketPlayerName) != std::wstring::npos))
            continue;

        std::wstring const& wideguildname = target.GetWideGuildName();
        if (!(wpacketGuildName.empty() || wideguildname.find(wpacketGuildName) != std::wstring::npos))
            continue;

        std::string aname;
        if (AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(playerZoneId))
            aname = areaEntry->area_name[GetSessionDbcLocale()];

        bool s_show = true;
        for (uint32 i = 0; i < strCount; ++i)
        {
            if (!str[i].empty())
            {
                if (wideguildname.find(str[i]) != std::wstring::npos ||
                    wideplayername.find(str[i]) != std::wstring::npos ||
                    Utf8FitTo(aname, str[i]))
                {
                    s_show = true;
                    break;
                }

                s_show = false;
            }
        }

        if (!s_show)
            continue;

        if (displaycount >= maxShown)
            break;

        // WhoEntry: PlayerGuidLookupData, PackedGuid128 GuildGUID, u32 GuildVirtualRealmAddress, i32 AreaID,
        // 7-bit guild name length, bit IsGM, flush, the guild name
        WorldPackets::Query::NameCacheLookupResult lookup;
        lookup.Player = target.GetGuid();
        lookup.Result = 0;
        lookup.HasData = true;
        lookup.Name = target.GetPlayerName();
        lookup.Race = race;
        lookup.Sex = target.GetGender();
        lookup.Class = class_;
        lookup.Level = uint8(lvl);
        WorldPackets::Query::WritePlayerGuidLookupData(entries, lookup);

        ObjectGuid::LowType guildId = sCharacterCache->GetCharacterGuildIdByGuid(target.GetGuid());
        std::string_view guildName = std::string_view(target.GetGuildName()).substr(0, std::min<std::size_t>(target.GetGuildName().size(), 0x7F));
        if (guildId)
            Guid128::WritePacked(entries, guildId, (uint64(28) << 58) | Guid128::RealmBits());
        else
            Guid128::WritePacked(entries, 0, 0);
        entries << uint32(guildId ? Guid128::VirtualRealmAddress() : 0);
        entries << int32(playerZoneId);
        entries.WriteBits(guildName.size(), 7);
        entries.WriteBit(target.GetSecurity() > SEC_PLAYER);
        entries.FlushBits();
        entries.WriteString(guildName);

        ++displaycount;
    }

    // SMSG_WHO, 3.4.3 (inline in the chat switch FUN_140723060): u32 RequestID, 6-bit entry count, flush,
    // the entries
    WorldPacket data(SMSG_WHO, 5 + entries.size());
    data << uint32(requestId);
    data.WriteBits(displaycount, 6);
    data.FlushBits();
    data.append(entries);

    SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Send SMSG_WHO Message");
}

void WorldSession::HandleLogoutRequestOpcode(WorldPackets::Character::LogoutRequest& /*logoutRequest*/)
{
    LOG_DEBUG("network", "WORLD: Recvd CMSG_LOGOUT_REQUEST Message, security - {}", GetSecurity());

    if (ObjectGuid lguid = GetPlayer()->GetLootGUID())
        DoLootRelease(lguid);

    bool instantLogout = (HasPermission(rbac::RBAC_PERM_INSTANT_LOGOUT)
                          || (GetPlayer()->HasPlayerFlag(PLAYER_FLAGS_RESTING) && !GetPlayer()->IsInCombat())) || GetPlayer()->IsInFlight();

    bool preventAfkSanctuaryLogout = sWorld->getIntConfig(CONFIG_AFK_PREVENT_LOGOUT) == 1
                                     && GetPlayer()->isAFK() && sAreaTableStore.LookupEntry(GetPlayer()->GetAreaId())->IsSanctuary();

    bool preventAfkLogout = ((sWorld->getIntConfig(CONFIG_AFK_PREVENT_LOGOUT) == 2) || HasAccountFlag(ACCOUNT_FLAG_NOKICK))
                            && GetPlayer()->isAFK();

    /// @todo: Possibly add RBAC permission to log out in combat
    bool canLogoutInCombat = GetPlayer()->HasPlayerFlag(PLAYER_FLAGS_RESTING);

    uint32 reason = 0;
    if (GetPlayer()->IsInCombat() && !canLogoutInCombat)
        reason = 1;
    else if (GetPlayer()->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR))
        reason = 3;                                         // is jumping or falling
    else if (preventAfkSanctuaryLogout || preventAfkLogout || GetPlayer()->duel || GetPlayer()->HasAura(9454)) // is dueling or frozen by GM via freeze command
        reason = 2;                                         // FIXME - Need the correct value

    WorldPackets::Character::LogoutResponse logoutResponse;
    logoutResponse.LogoutResult = reason;
    logoutResponse.Instant = instantLogout;
    SendPacket(logoutResponse.Write());

    if (reason)
    {
        SetLogoutStartTime(0);
        return;
    }

    //instant logout in taverns/cities or on taxi or for admins, gm's, mod's if its enabled in worldserver.conf
    if (instantLogout)
    {
        LogoutPlayer(true);
        return;
    }

    // not set flags if player can't free move to prevent lost state at logout cancel
    if (GetPlayer()->CanFreeMove())
    {
        if (GetPlayer()->getStandState() == UNIT_STAND_STATE_STAND)
        {
            GetPlayer()->SetStandState(UNIT_STAND_STATE_SIT);
        }

        GetPlayer()->SetRooted(true, true, true);
        GetPlayer()->SetUnitFlag(UNIT_FLAG_STUNNED);
    }

    SetLogoutStartTime(GameTime::GetGameTime().count());
}

void WorldSession::HandlePlayerLogoutOpcode(WorldPackets::Character::PlayerLogout& /*playerLogout*/)
{
}

void WorldSession::HandleLogoutCancelOpcode(WorldPackets::Character::LogoutCancel& /*logoutCancel*/)
{
    // Player have already logged out serverside, too late to cancel
    if (!GetPlayer())
        return;

    SetLogoutStartTime(0);

    SendPacket(WorldPackets::Character::LogoutCancelAck().Write());

    // not remove flags if can't free move - its not set in Logout request code.
    if (GetPlayer()->CanFreeMove())
    {
        GetPlayer()->SetRooted(false, true, true);

        GetPlayer()->SetStandState(UNIT_STAND_STATE_STAND);
        GetPlayer()->RemoveUnitFlag(UNIT_FLAG_STUNNED);
    }
}

void WorldSession::HandleTogglePvP(WorldPacket& recv_data)
{
    // 3.4.3: CMSG_TOGGLE_PVP is empty (writer FUN_140761240), CMSG_SET_PVP (FUN_1407607f0) a bit
    // EnablePVP, flush
    if (recv_data.GetOpcode() == CMSG_SET_PVP)
    {
        bool newPvPStatus = recv_data.ReadBit();
        GetPlayer()->ApplyModFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP, newPvPStatus);
    }
    else
        GetPlayer()->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP);

    if (GetPlayer()->HasPlayerFlag(PLAYER_FLAGS_IN_PVP))
        GetPlayer()->UpdatePvP(true, true);
    else if (!GetPlayer()->pvpInfo.IsHostile && GetPlayer()->IsPvP()) // pussywizard: in pvp mode, but doesn't want to be and not in hostile territory, so start timer
        GetPlayer()->UpdatePvP(true, false);

    //if (OutdoorPvP* pvp = _player->GetOutdoorPvP())
    //    pvp->HandlePlayerActivityChanged(_player);
}

void WorldSession::HandleZoneUpdateOpcode(WorldPacket& recv_data)
{
    uint32 newZone;
    recv_data >> newZone;

    LOG_DEBUG("network", "WORLD: Recvd ZONE_UPDATE: {}", newZone);

    // use server side data, but only after update the player position. See Player::UpdatePosition().
    GetPlayer()->SetNeedZoneUpdate(true);

    //GetPlayer()->SendInitWorldStates(true, newZone);
}

void WorldSession::HandleSetSelectionOpcode(WorldPacket& recv_data)
{
    // 3.4.3: PackedGuid128 Selection
    ObjectGuid guid = Guid128::Read(recv_data);

    _player->SetSelection(guid);

    // Change target of current autoshoot spell
    if (guid)
    {
        if (Spell* autoReapeatSpell = _player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
        {
            if (autoReapeatSpell->m_targets.GetUnitTargetGUID() != guid)
            {
                if (Unit* unit = ObjectAccessor::GetUnit(*_player, guid))
                {
                    if (unit->IsAlive() && !_player->IsFriendlyTo(unit) && unit->isTargetableForAttack(true, _player))
                    {
                        autoReapeatSpell->m_targets.SetUnitTarget(unit);
                    }
                }
            }
        }
    }
}

void WorldSession::HandleStandStateChangeOpcode(WorldPacket& recv_data)
{
    uint32 animstate;
    recv_data >> animstate;

    switch (animstate)
    {
        case UNIT_STAND_STATE_STAND:
        case UNIT_STAND_STATE_SIT:
        case UNIT_STAND_STATE_SLEEP:
        case UNIT_STAND_STATE_KNEEL:
            break;
        default:
            return;
    }

    _player->SetStandState(animstate);
}

void WorldSession::HandleLoadActionsSwitchSpec(PreparedQueryResult result)
{
    if (!GetPlayer())
        return;

    if (result)
        GetPlayer()->_LoadActions(result);

    GetPlayer()->SendActionButtons(1);
}

void WorldSession::HandleCharacterAuraFrozen(PreparedQueryResult result)
{
    if (!GetPlayer())
        return;

    ChatHandler handler = ChatHandler(this);

    // Select
    if (!result)
    {
        handler.SendSysMessage(LANG_COMMAND_NO_FROZEN_PLAYERS);
        return;
    }

    // Header of the names
    handler.PSendSysMessage(LANG_COMMAND_LIST_FREEZE);

    // Output of the results
    do
    {
        Field* fields = result->Fetch();
        std::string player = fields[0].Get<std::string>();
        handler.PSendSysMessage(LANG_COMMAND_FROZEN_PLAYERS, player);
    } while (result->NextRow());
}

void WorldSession::HandleBugOpcode(WorldPacket& recv_data)
{
    uint32 suggestion, contentlen, typelen;
    std::string content, type;

    recv_data >> suggestion >> contentlen >> content;

    recv_data >> typelen >> type;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_BUG_REPORT);

    stmt->SetData(0, type);
    stmt->SetData(1, content);

    CharacterDatabase.Execute(stmt);
}

void WorldSession::HandleReclaimCorpseOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_14075fc60): PackedGuid128 CorpseGUID
    ObjectGuid guid = Guid128::Read(recv_data);

    if (_player->IsAlive())
        return;

    // do not allow corpse reclaim in arena
    if (_player->InArena())
        return;

    // body not released yet
    if (!_player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        return;

    Corpse* corpse = _player->GetCorpse();
    if (!corpse)
        return;

    // prevent resurrect before 30-sec delay after body release not finished
    if (time_t(corpse->GetGhostTime() + _player->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP)) > time_t(GameTime::GetGameTime().count()))
        return;

    // skip phase check
    if (!corpse->IsInMap(_player) || !corpse->IsWithinDist(_player, CORPSE_RECLAIM_RADIUS, true))
        return;

    // resurrect
    _player->ResurrectPlayer(_player->InBattleground() ? 1.0f : 0.5f);

    // spawn bones
    _player->SpawnCorpseBones();
}

void WorldSession::HandleResurrectResponseOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_140768e50): PackedGuid128 Resurrecter, u32 Response (0 accept, 1 decline, 2 timeout;
    // 3.3.5 sent a u8 with 1 to accept)
    ObjectGuid guid = Guid128::Read(recv_data);
    uint32 response = recv_data.read<uint32>();

    // Xinef: Prevent resurrect with prevent resurrection aura
    if (GetPlayer()->IsAlive() || GetPlayer()->HasPreventResurectionAura())
        return;

    if (response != 0)
    {
        GetPlayer()->clearResurrectRequestData();           // reject
        return;
    }

    if (!GetPlayer()->isResurrectRequestedBy(guid))
        return;

    GetPlayer()->ResurectUsingRequestData();
}

void WorldSession::HandleAreaTriggerOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_14075bb50): u32 AreaTriggerID, bits Entered, FromClient, flush; AC acts on entering
    uint32 triggerId = recv_data.read<uint32>();
    bool entered = recv_data.ReadBit();
    recv_data.ReadBit();
    if (!entered)
        return;

    LOG_DEBUG("network", "CMSG_AREA_TRIGGER. Trigger ID: {}", triggerId);

    Player* player = GetPlayer();
    if (player->IsInFlight())
    {
        LOG_DEBUG("network", "HandleAreaTriggerOpcode: Player '{}' ({}) in flight, ignore Area Trigger ID:{}",
                       player->GetName(), player->GetGUID().ToString(), triggerId);
        return;
    }

    AreaTrigger const* atEntry = sObjectMgr->GetAreaTrigger(triggerId);
    if (!atEntry)
    {
        LOG_DEBUG("network", "HandleAreaTriggerOpcode: Player '{}' ({}) send unknown (by DBC) Area Trigger ID:{}",
                       player->GetName(), player->GetGUID().ToString(), triggerId);
        return;
    }

    uint32 teamFaction = player->GetTeamId(true) == TEAM_ALLIANCE ? FACTION_MASK_ALLIANCE : FACTION_MASK_HORDE;
    bool isTavernAreatrigger = sObjectMgr->IsTavernAreaTrigger(triggerId, teamFaction);
    if (!player->IsInAreaTriggerRadius(atEntry, isTavernAreatrigger ? 5.f : 0.f))
    {
        LOG_DEBUG("network", "HandleAreaTriggerOpcode: Player {} ({}) too far (trigger map: {} player map: {}), ignore Area Trigger ID: {}",
                       player->GetName(), player->GetGUID().ToString(), atEntry->map, player->GetMapId(), triggerId);
        return;
    }

    if (player->isDebugAreaTriggers)
        ChatHandler(this).PSendSysMessage(LANG_DEBUG_AREATRIGGER_REACHED, triggerId);

    // Skip areatrigger scripts for GMs unless debug areatriggers is enabled
    if ((!player->IsGameMaster() || player->isDebugAreaTriggers) && sScriptMgr->OnAreaTrigger(player, atEntry))
        return;

    if (player->IsAlive())
        if (uint32 questId = sObjectMgr->GetQuestForAreaTrigger(triggerId))
            if (player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                player->AreaExploredOrEventHappens(questId);

    if (isTavernAreatrigger)
    {
        // set resting flag we are in the inn
        player->SetRestFlag(REST_FLAG_IN_TAVERN, atEntry->entry);

        if (sWorld->IsFFAPvPRealm())
        {
            if (player->HasByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP))
            {
                player->RemoveByteFlag(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_FFA_PVP);
                sScriptMgr->OnPlayerFfaPvpStateUpdate(player, false);

            }
        }
        return;
    }

    if (Battleground* bg = player->GetBattleground())
        if (bg->GetStatus() == STATUS_IN_PROGRESS)
        {
            bg->HandleAreaTrigger(player, triggerId);
            return;
        }

    if (OutdoorPvP* pvp = player->GetOutdoorPvP())
        if (pvp->HandleAreaTrigger(_player, triggerId))
            return;

    AreaTriggerTeleport const* at = sObjectMgr->GetAreaTriggerTeleport(triggerId);
    if (!at)
        return;

    bool teleported = false;
    if (player->GetMapId() != at->target_mapId)
    {
       if (Map::EnterState denyReason = sMapMgr->PlayerCannotEnter(at->target_mapId, player, false))
        {
            bool reviveAtTrigger = false; // should we revive the player if he is trying to enter the correct instance?
            switch (denyReason)
            {
                case Map::CANNOT_ENTER_NOT_IN_RAID:
                case Map::CANNOT_ENTER_INSTANCE_BIND_MISMATCH:
                case Map::CANNOT_ENTER_TOO_MANY_INSTANCES:
                case Map::CANNOT_ENTER_MAX_PLAYERS:
                case Map::CANNOT_ENTER_ZONE_IN_COMBAT:
                    reviveAtTrigger = true;
                    break;
                default:
                    break;
            }

            if (reviveAtTrigger) // check if the player is touching the areatrigger leading to the map his corpse is on
            {
                if (!player->IsAlive() && player->HasCorpse())
                {
                    if (player->GetCorpseLocation().GetMapId() == at->target_mapId)
                    {
                        player->ResurrectPlayer(0.5f);
                        player->SpawnCorpseBones();
                    }
                }
            }

            return;
        }

        if (Group* group = player->GetGroup())
            if (group->isLFGGroup() && player->GetMap()->IsDungeon())
                teleported = player->TeleportToEntryPoint();
    }

    if (!teleported)
        player->TeleportTo(at->target_mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation, TELE_TO_NOT_LEAVE_TRANSPORT);
}

// CMSG_UPDATE_ACCOUNT_DATA, 3.4.3 (writer FUN_140769e80): PackedGuid128 PlayerGuid, i64 Time,
// u32 Size (uncompressed), 4 bits DataType, flush, u32 CompressedSize, zlib data. The GUID is the
// character being played even for the account-wide types. No answer.
void WorldSession::HandleUpdateAccountData(WorldPacket& recv_data)
{
    Guid128::Skip(recv_data);
    int64 time = recv_data.read<int64>();
    uint32 decompressedSize = recv_data.read<uint32>();
    uint32 type = recv_data.ReadBits(4);
    uint32 compressedSize = recv_data.read<uint32>();
    uint32 timestamp = uint32(time);

    if (compressedSize > recv_data.size() - recv_data.rpos())
    {
        recv_data.rfinish();
        LOG_ERROR("network.opcode", "UAD: compressed size {} past the packet end", compressedSize);
        return;
    }

    LOG_DEBUG("network", "UAD: type {}, time {}, decompressedSize {}", type, timestamp, decompressedSize);

    if (type >= NUM_ACCOUNT_DATA_TYPES)
        return;

    if (decompressedSize == 0)                               // erase
    {
        SetAccountData(AccountDataType(type), 0, "");
        return;
    }

    if (decompressedSize > 0xFFFF)
    {
        recv_data.rfinish();                   // unnneded warning spam in this case
        LOG_ERROR("network.opcode", "UAD: Account data packet too big, size {}", decompressedSize);
        return;
    }

    ByteBuffer dest;
    dest.resize(decompressedSize);

    uLongf realSize = decompressedSize;
    if (uncompress(dest.contents(), &realSize, recv_data.contents() + recv_data.rpos(), recv_data.size() - recv_data.rpos()) != Z_OK)
    {
        recv_data.rfinish();                   // unnneded warning spam in this case
        LOG_ERROR("network.opcode", "UAD: Failed to decompress account data");
        return;
    }

    recv_data.rfinish();                       // uncompress read (recv_data.size() - recv_data.rpos())

    std::string adata(reinterpret_cast<char const*>(dest.contents()), realSize);
    if (std::size_t nul = adata.find('\0'); nul != std::string::npos)
        adata.resize(nul);

    // the 3.4.3 client has no SMSG_UPDATE_ACCOUNT_DATA_COMPLETE
    SetAccountData(AccountDataType(type), timestamp, adata);
}

// CMSG_REQUEST_ACCOUNT_DATA, 3.4.3 (writer FUN_140768c20, sent from the SMSG_ACCOUNT_DATA_TIMES handler):
// PackedGuid128 PlayerGuid, 4 bits DataType, flush.
// SMSG_UPDATE_ACCOUNT_DATA (parser FUN_1406ccbc0, handler FUN_14156f190): PackedGuid128 Player, i64 Time,
// u32 Size (uncompressed), 4 bits DataType, flush, u32 CompressedSize, zlib data. The client takes it
// only for a type it asked for, and for a per-character type only with its own GUID.
void WorldSession::HandleRequestAccountData(WorldPacket& recv_data)
{
    Guid128::Skip(recv_data);
    uint32 type = recv_data.ReadBits(4);

    LOG_DEBUG("network", "RAD: type {}", type);

    if (type >= NUM_ACCOUNT_DATA_TYPES)
        return;

    AccountData* adata = GetAccountData(AccountDataType(type));

    uint32 size = adata->Data.size();

    uLongf destSize = compressBound(size);

    ByteBuffer dest;
    dest.resize(destSize);

    if (size && compress(dest.contents(), &destSize, (uint8 const*)adata->Data.c_str(), size) != Z_OK)
    {
        LOG_DEBUG("network", "RAD: Failed to compress account data");
        return;
    }

    dest.resize(destSize);

    WorldPacket data(SMSG_UPDATE_ACCOUNT_DATA, 18 + 8 + 4 + 1 + 4 + destSize);
    Guid128::WritePlayer(data, _player ? _player->GetGUID() : ObjectGuid::Empty);
    data << int64(adata->Time);                                 // Time
    data << uint32(size);                                       // Size (uncompressed)
    data.WriteBits(type, 4);                                    // DataType
    data.FlushBits();
    data << uint32(size ? destSize : 0);                        // CompressedSize
    if (size)
        data.append(dest);                                      // compressed data
    SendPacket(&data);
}

// CMSG_SET_ACTION_BUTTON, 3.4.3 (writer FUN_1407604d0): u32 Action (packed as in
// SMSG_UPDATE_ACTION_BUTTONS, 0 = clear), u8 Index (< 180)
void WorldSession::HandleSetActionButtonOpcode(WorldPacket& recv_data)
{
    uint32 packetData;
    uint8 button;
    recv_data >> packetData >> button;

    uint32 action = ACTION_BUTTON_ACTION(packetData);
    uint8  type   = ACTION_BUTTON_TYPE(packetData);

    LOG_DEBUG("network.opcode", "BUTTON: {} ACTION: {} TYPE: {}", button, action, type);
    if (!packetData)
    {
        LOG_DEBUG("network.opcode", "MISC: Remove action from button {}", button);
        GetPlayer()->removeActionButton(button);
    }
    else
    {
        switch (type)
        {
            case ACTION_BUTTON_MACRO:
            case ACTION_BUTTON_CMACRO:
                LOG_DEBUG("network.opcode", "MISC: Added Macro {} into button {}", action, button);
                break;
            case ACTION_BUTTON_EQSET:
                LOG_DEBUG("network.opcode", "MISC: Added EquipmentSet {} into button {}", action, button);
                break;
            case ACTION_BUTTON_SPELL:
                LOG_DEBUG("network.opcode", "MISC: Added Spell {} into button {}", action, button);
                break;
            case ACTION_BUTTON_ITEM:
                LOG_DEBUG("network.opcode", "MISC: Added Item {} into button {}", action, button);
                break;
            default:
                LOG_ERROR("network.opcode", "MISC: Unknown action button type {} for action {} into button {} for player {} ({})",
                    type, action, button, _player->GetName(), _player->GetGUID().ToString());
                return;
        }
        GetPlayer()->addActionButton(button, action, type);
    }
}

void WorldSession::HandleCompleteCinematic(WorldPacket& /*recv_data*/)
{
    // End the current cinematic and restore the normal player view
    GetPlayer()->GetCinematicMgr().EndCinematic();
}

void WorldSession::HandleNextCinematicCamera(WorldPacket& /*recv_data*/)
{
    // Sent by client when cinematic actually begun. So we begin the server side process
    GetPlayer()->GetCinematicMgr().StartCinematicCamera();
}

// CMSG_SET_ACTION_BAR_TOGGLES, 3.4.3 (writer FUN_140760490): u8 Mask
void WorldSession::HandleSetActionBarToggles(WorldPacket& recv_data)
{
    uint8 ActionBar;

    recv_data >> ActionBar;

    if (!GetPlayer())                                        // ignore until not logged (check needed because STATUS_AUTHED)
    {
        if (ActionBar != 0)
            LOG_ERROR("network.opcode", "WorldSession::HandleSetActionBarToggles in not logged state with value: {}, ignored", uint32(ActionBar));
        return;
    }

    GetPlayer()->SetByteValue(PLAYER_FIELD_BYTES, 2, ActionBar);
}

// CMSG_REQUEST_FORCED_REACTIONS, 3.4.3 (writer FUN_14075fe70, empty): the client asks for them
// each time it rebuilds its reputation list, starting with SMSG_INITIALIZE_FACTIONS.
void WorldSession::HandleRequestForcedReactions(WorldPacket& /*recvData*/)
{
    GetPlayer()->GetReputationMgr().SendForceReactions();
}

// CMSG_REQUEST_CEMETERY_LIST, 3.4.3 (writer FUN_14075fe30, empty) ->
// SMSG_REQUEST_CEMETERY_LIST_RESPONSE (handler FUN_141736c00): bit IsGossipTriggered, flush,
// u32 Count, u32 CemeteryID[Count]. It feeds the preferred-graveyard choice, which Wrath does not
// have, so the list is empty.
void WorldSession::HandleRequestCemeteryList(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_REQUEST_CEMETERY_LIST_RESPONSE, 1 + 4);
    data.WriteBit(false);                                   // IsGossipTriggered
    data.FlushBits();
    data << uint32(0);
    SendPacket(&data);
}

// CMSG_VIOLENCE_LEVEL, 3.4.3 (writer FUN_140763d50): u8 ViolenceLevel, the client's gore setting.
// Nothing on the server depends on it.
void WorldSession::HandleViolenceLevel(WorldPacket& recvData)
{
    recvData.read_skip<uint8>();
}

// CMSG_LOADING_SCREEN_NOTIFY, 3.4.3 (writer FUN_140768110): u32 MapID, bit Showing, flush. Showing = 1
// when a loading screen comes up (EnterWorld, SMSG_TRANSFER_PENDING), 0 when it goes away, which at
// login happens once SMSG_UPDATE_OBJECT has created the active player. Informational.
// CMSG_SERVER_TIME_OFFSET_REQUEST, 3.4.3 (writer 0x140768f20, empty; sent every 90 s or so once
// SMSG_AUTH_RESPONSE succeeded) -> SMSG_SERVER_TIME_OFFSET (handler FUN_1414f81d0): i64 Time, unix seconds.
void WorldSession::HandleServerTimeOffsetRequest(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_SERVER_TIME_OFFSET, 8);
    data << int64(GameTime::GetGameTime().count());
    SendPacket(&data);
}

// CMSG_GET_UNDELETE_CHARACTER_COOLDOWN_STATUS, 3.4.3 (writer FUN_140767130, empty) ->
// SMSG_UNDELETE_COOLDOWN_STATUS_RESPONSE (parser FUN_1406cc9b0, handler FUN_14167b170): bit OnCooldown,
// flush, u32 MaxCooldown, u32 CurrentCooldown. Undelete is not offered (the glue feature status has
// CharUndeleteEnabled off), so: no cooldown.
void WorldSession::HandleGetUndeleteCharacterCooldownStatus(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_UNDELETE_COOLDOWN_STATUS_RESPONSE, 9);
    data.WriteBit(false);                                   // OnCooldown
    data.FlushBits();
    data << uint32(0);                                      // MaxCooldown
    data << uint32(0);                                      // CurrentCooldown
    SendPacket(&data);
}

// CMSG_SOCIAL_CONTRACT_REQUEST, 3.4.3 (empty, Lua C_SocialContractGlue.GetShouldShowSocialContract) ->
// SMSG_SOCIAL_CONTRACT_REQUEST_RESPONSE (parser FUN_1406bcba0, handler FUN_1419ee980): bit ShouldShow, flush.
void WorldSession::HandleSocialContractRequest(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_SOCIAL_CONTRACT_REQUEST_RESPONSE, 1);
    data.WriteBit(false);                                   // ShouldShow
    data.FlushBits();
    SendPacket(&data);
}

// CMSG_BATTLE_PAY_GET_PRODUCT_LIST, 3.4.3 (empty) -> SMSG_BATTLE_PAY_GET_PRODUCT_LIST_RESPONSE (parser
// FUN_1406f84c0): u32 Result, u32 CurrencyID, u32 ProductInfos, u32 Products, u32 ProductGroups, u32 Shops,
// then the arrays. There is no shop: an empty list.
void WorldSession::HandleBattlePayGetProductList(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_BATTLE_PAY_GET_PRODUCT_LIST_RESPONSE, 24);
    data << uint32(0) << uint32(0) << uint32(0) << uint32(0) << uint32(0) << uint32(0);
    SendPacket(&data);
}

// CMSG_BATTLE_PAY_GET_PURCHASE_LIST, 3.4.3 (empty) -> SMSG_BATTLE_PAY_GET_PURCHASE_LIST_RESPONSE (parser
// FUN_1406b8f90): u32 Result, u32 Count, purchases. None.
void WorldSession::HandleBattlePayGetPurchaseList(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_BATTLE_PAY_GET_PURCHASE_LIST_RESPONSE, 8);
    data << uint32(0) << uint32(0);
    SendPacket(&data);
}

// CMSG_UPDATE_VAS_PURCHASE_STATES, 3.4.3 (empty) -> SMSG_ENUM_VAS_PURCHASE_STATES_RESPONSE (parser
// FUN_1406bf210): 6 bits count, flush, entries. None.
void WorldSession::HandleUpdateVasPurchaseStates(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_ENUM_VAS_PURCHASE_STATES_RESPONSE, 1);
    data.WriteBits(0, 6);
    data.FlushBits();
    SendPacket(&data);
}

// CMSG_DB_QUERY_BULK, 3.4.3 (writer FUN_140766750): u32 TableHash, 13-bit count, flush, u32 RecordID per
// record. The client asks for the records its own DB2 files lack (an item id that is not in its
// Item/ItemSparse, ...) -> one SMSG_DB_REPLY each (cache switch FUN_140721aa0 case 0x290E): u32 TableHash,
// u32 RecordID, u32 Timestamp, 3-bit Status, flush, u32 size, the record. Status is the client's
// VALIDATION_RESULT_* (0 NotSet, 1 Valid, 2 Delete, 3 Invalid, 4 NotPublic). This server has no hotfix
// records yet [TODO: serve AC's *_dbc overrides as records], so every record is Invalid: the client
// stops asking and keeps what it has.
void WorldSession::HandleDBQueryBulk(WorldPacket& recvData)
{
    uint32 tableHash = recvData.read<uint32>();
    uint32 count = recvData.ReadBits(13);
    for (uint32 i = 0; i < count; ++i)
    {
        uint32 recordId = recvData.read<uint32>();

        WorldPacket data(SMSG_DB_REPLY, 4 + 4 + 4 + 1 + 4);
        data << uint32(tableHash);
        data << uint32(recordId);
        data << uint32(GameTime::GetGameTime().count());
        data.WriteBits(3, 3);                               // VALIDATION_RESULT_INVALID
        data.FlushBits();
        data << uint32(0);
        SendPacket(&data);
    }
}

// CMSG_HOTFIX_REQUEST, 3.4.3 (writer FUN_140767420): u32 ClientBuild, u32 DataBuild, u32 count, u32
// hotfix ids. Sent for the ids SMSG_AVAILABLE_HOTFIXES announced; this server announces none, but an
// answer closes the client's hotfix phase either way -> SMSG_HOTFIX_CONNECT (case 0x2911): u32 record
// count, {u32 x5, 3-bit status, flush} (FUN_1406ff150), u32 size, the record data. Empty.
void WorldSession::HandleHotfixRequest(WorldPacket& recvData)
{
    recvData.read_skip<uint32>();                           // ClientBuild
    recvData.read_skip<uint32>();                           // DataBuild
    recvData.rfinish();                                     // the ids

    WorldPacket data(SMSG_HOTFIX_CONNECT, 8);
    data << uint32(0);
    data << uint32(0);
    SendPacket(&data);
}

// CMSG_UPDATE_CLIENT_SETTINGS, 3.4.3 (writer FUN_140769f20): f32 FarClip. Sent on entering the world and
// when the view distance changes; nothing on the server depends on it.
void WorldSession::HandleUpdateClientSettings(WorldPacket& recvData)
{
    recvData.read_skip<float>();
}

// Requests the client makes on entering the world (GameUI enter-world, login-sequence.ANALYSIS.md 5)
// for systems Wrath has no use for, or that answer only when there is something to say: the countdown
// timer (u32 type), the conquest formula (empty), the premade-group finder status (empty), the battle
// pet journal and its lock (empty); and the addon prefix registrations (u32 count, {5-bit length, flush,
// prefix}; and the empty unregister), which AC does not filter on. Read and dropped.
void WorldSession::HandleIgnoredWorldRequest(WorldPacket& recvData)
{
    recvData.rfinish();
}

// CMSG_REQUEST_LFG_LIST_BLACKLIST, 3.4.3 (empty) -> SMSG_LFG_LIST_UPDATE_BLACKLIST (FUN_140713ca0): u32 count,
// {u32 ActivityID, u32 Reason}. The premade-group finder is not offered: an empty list.
void WorldSession::HandleRequestLfgListBlacklist(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_LFG_LIST_UPDATE_BLACKLIST, 4);
    data << uint32(0);
    SendPacket(&data);
}

// CMSG_DF_GET_SYSTEM_INFO, 3.4.3 (writer FUN_140766880): bit Player, bit HasPartyIndex, flush, [u8]. Player 1
// (every world enter, Lua RequestLFDPlayerLockInfo) asks for SMSG_LFG_PLAYER_INFO, Player 0 (Lua
// RequestLFDPartyLockInfo) for SMSG_LFG_PARTY_INFO.
void WorldSession::HandleDFGetSystemInfo(WorldPacket& recvData)
{
    bool player = recvData.ReadBit();
    recvData.rfinish();

    if (player)
        HandleLfgPlayerLockInfoRequestOpcode(recvData);
    else
        HandleLfgPartyLockInfoRequestOpcode(recvData);
}

void WorldSession::HandleLoadingScreenNotify(WorldPacket& recvData)
{
    uint32 mapId = recvData.read<uint32>();
    bool showing = recvData.ReadBit();
    LOG_DEBUG("network", "CMSG_LOADING_SCREEN_NOTIFY: map {} {}", mapId, showing ? "shown" : "hidden");
}

void WorldSession::HandlePlayedTime(WorldPackets::Character::PlayedTimeClient& packet)
{
    WorldPackets::Character::PlayedTime playedTime;
    playedTime.TotalTime = _player->GetTotalPlayedTime();
    playedTime.LevelTime = _player->GetLevelPlayedTime();
    playedTime.TriggerScriptEvent = packet.TriggerScriptEvent;  // 0-1 - will not show in chat frame
    SendPacket(playedTime.Write());
}

void WorldSession::HandleInspectOpcode(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_14075e1e0): PackedGuid128 Target
    ObjectGuid guid = Guid128::Read(recv_data);

    Player* player = ObjectAccessor::GetPlayer(*_player, guid);
    if (!player)
    {
        LOG_DEBUG("network", "CMSG_INSPECT: No player found from {}", guid.ToString());
        return;
    }

    if (!GetPlayer()->IsWithinDistInMap(player, INSPECT_DISTANCE, false))
    {
        return;
    }

    if (GetPlayer()->IsValidAttackTarget(player))
    {
        return;
    }

    // SMSG_INSPECT_RESULT, 3.4.3 (FUN_1406c2780 -> FUN_1406ff210, handler FUN_141735940; the client applies it
    // only for the GUID of its last CMSG_INSPECT):
    // PlayerModelDisplayInfo (FUN_140718160): PackedGuid128, i32 SpecializationID, u32 item count, 6-bit name
    // length, flush, u8 gender, race, class, u32 customization count, the name, the customizations, the
    // items (InspectItemData FUN_14070c200: creator, u8 slot, u32 azerite power and essence counts,
    // ItemInstance, bit Usable, 4-bit enchant and 2-bit gem counts, flush, {i32 id, u8 slot} enchants);
    // u32 glyph count, i32 ItemLevel, u8 LifetimeMaxRank, u16 today / yesterday kills, u32 lifetime kills,
    // u32 HonorLevel, the glyphs, ClassicTalentInfoUpdate (as SMSG_UPDATE_TALENT_DATA), bits HasGuildData,
    // HasAzeriteLevel, flush, seven PVPBracketData (u8, 17 i32, Disqualified bit: 70 bytes), [guild GUID,
    // members, achievement points], TraitInspectInfo (i32 level, i32 spec, an empty TraitConfig)
    struct InspectItem
    {
        Item* Item;
        uint8 Slot;
        std::vector<std::pair<uint32, uint8>> Enchants;
    };
    std::vector<InspectItem> items;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        InspectItem& entry = items.emplace_back(InspectItem{ item, slot, {} });
        for (uint8 e = PERM_ENCHANTMENT_SLOT; e <= TEMP_ENCHANTMENT_SLOT && entry.Enchants.size() < 15; ++e)
            if (uint32 enchant = item->GetEnchantmentId(EnchantmentSlot(e)))
                entry.Enchants.emplace_back(enchant, e);
    }

    std::string_view name = std::string_view(player->GetName()).substr(0, std::min<std::size_t>(player->GetName().size(), 0x3F));
    std::vector<ChrCustomizationChoice> const& customizations = player->GetCustomizations();

    WorldPacket data(SMSG_INSPECT_RESULT, 600 + items.size() * 64);
    Guid128::Write(data, player->GetGUID());
    data << int32(0);                                       // SpecializationID
    data << uint32(items.size());
    data.WriteBits(name.size(), 6);
    data.FlushBits();
    data << uint8(player->getGender());
    data << uint8(player->getRace());
    data << uint8(player->getClass());
    data << uint32(customizations.size());
    data.WriteString(name);
    for (ChrCustomizationChoice const& c : customizations)
        data << uint32(c.ChrCustomizationOptionID) << uint32(c.ChrCustomizationChoiceID);
    for (InspectItem const& entry : items)
    {
        Guid128::Write(data, entry.Item->GetGuidValue(ITEM_FIELD_CREATOR));
        data << uint8(entry.Slot);
        data << uint32(0);                                  // AzeritePowers
        data << uint32(0);                                  // AzeriteEssences
        WorldPackets::Item::WriteItemInstance(data, entry.Item->GetEntry(), int32(entry.Item->GetItemSuffixFactor()), entry.Item->GetItemRandomPropertyId());
        data.WriteBit(true);                                // Usable
        data.WriteBits(entry.Enchants.size(), 4);
        data.WriteBits(0, 2);                               // gems
        data.FlushBits();
        for (auto const& [id, slot] : entry.Enchants)
            data << int32(id) << uint8(slot);
    }

    data << uint32(0);                                      // glyphs: they travel in the talent groups
    data << int32(0);                                       // ItemLevel
    data << uint8(0);                                       // LifetimeMaxRank
    data << uint16(0);                                      // TodayHK
    data << uint16(0);                                      // YesterdayHK
    data << uint32(player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS));
    data << uint32(0);                                      // HonorLevel

    if (sWorld->getBoolConfig(CONFIG_TALENTS_INSPECTING) || _player->CanBeGameMaster())
        player->BuildClassicTalentInfoUpdate(data, false);
    else
    {
        data << uint32(0);                                  // Unspent
        data << uint8(0);                                   // ActiveGroup
        data << uint32(0);                                  // GroupCount
        data.WriteBit(false);                               // IsPetTalents
        data.FlushBits();
    }

    data.WriteBit(false);                                   // HasGuildData
    data.WriteBit(false);                                   // HasAzeriteLevel
    data.FlushBits();
    for (uint8 bracket = 0; bracket < 7; ++bracket)
    {
        data << uint8(bracket);
        for (uint8 i = 0; i < 17; ++i)
            data << int32(0);
        data.WriteBit(false);                               // Disqualified
        data.FlushBits();
    }

    data << int32(player->GetLevel());                      // TraitInspectInfo.Level
    data << int32(0);                                       // ChrSpecializationID
    data << int32(0);                                       // TraitConfig.ID
    data << int32(0);                                       // Type
    data << uint32(0);                                      // entries
    data.WriteBits(0, 9);                                   // name
    data.FlushBits();
    SendPacket(&data);
}

void WorldSession::HandleInspectHonorStatsOpcode(WorldPacket& recv_data)
{
    // CMSG_REQUEST_HONOR_STATS, 3.4.3 (writer FUN_14075fe90): PackedGuid128 TargetGUID
    ObjectGuid guid = Guid128::Read(recv_data);

    Player* player = ObjectAccessor::GetPlayer(*_player, guid);
    if (!player)
    {
        LOG_DEBUG("network", "CMSG_REQUEST_HONOR_STATS: No player found from {}", guid.ToString());
        return;
    }

    if (!GetPlayer()->IsWithinDistInMap(player, INSPECT_DISTANCE, false))
    {
        return;
    }

    if (GetPlayer()->IsValidAttackTarget(player))
    {
        return;
    }

    // SMSG_INSPECT_HONOR_STATS, 3.4.3 (handler FUN_141aa76b0): PackedGuid128, u8 LifetimeHighestRank, 8 x u16, 6 x u32,
    // u8 (GetInspectHonorData's order: today HK, today DK, yesterday HK, ...) [field names inferred]
    uint32 kills = player->GetUInt32Value(PLAYER_FIELD_KILLS);
    WorldPacket data(SMSG_INSPECT_HONOR_STATS, 18 + 1 + 16 + 24 + 1);
    Guid128::Write(data, player->GetGUID());
    data << uint8(0);                                       // LifetimeHighestRank
    data << uint16(kills & 0xFFFF);                         // today HK
    data << uint16(0);                                      // today DK
    data << uint16(kills >> 16);                            // yesterday HK
    data << uint16(0) << uint16(0) << uint16(0) << uint16(0) << uint16(0);
    data << uint32(player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS));
    data << uint32(0);                                      // lifetime DK
    data << uint32(player->GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION));
    data << uint32(0) << uint32(0) << uint32(0);
    data << uint8(0);
    SendPacket(&data);
}

void WorldSession::HandleWorldTeleportOpcode(WorldPacket& recv_data)
{
    uint32 time;
    uint32 mapid;
    float PositionX;
    float PositionY;
    float PositionZ;
    float Orientation;

    recv_data >> time;                                      // time in m.sec.
    recv_data >> mapid;
    recv_data >> PositionX;
    recv_data >> PositionY;
    recv_data >> PositionZ;
    recv_data >> Orientation;                               // o (3.141593 = 180 degrees)

    if (GetPlayer()->IsInFlight())
    {
        LOG_DEBUG("network", "Player '{}' ({}) in flight, ignore worldport command.", GetPlayer()->GetName(), GetPlayer()->GetGUID().ToString());
        return;
    }

    LOG_DEBUG("network", "CMSG_WORLD_TELEPORT: Player = {}, Time = {}, map = {}, x = {}, y = {}, z = {}, o = {}", GetPlayer()->GetName(), time, mapid, PositionX, PositionY, PositionZ, Orientation);

    if (HasPermission(rbac::RBAC_PERM_OPCODE_WORLD_TELEPORT))
        GetPlayer()->TeleportTo(mapid, PositionX, PositionY, PositionZ, Orientation);
    else
        ChatHandler(this).SendNotification(LANG_PERMISSION_DENIED);
}

void WorldSession::HandleWhoisOpcode(WorldPacket& recv_data)
{
    LOG_DEBUG("network", "Received opcode CMSG_WHO_IS");
    // CMSG_WHO_IS, 3.4.3 (writer FUN_14076a370): 6-bit name length, flush, the name
    uint32 nameLength = recv_data.ReadBits(6);
    recv_data.ResetBitReader();
    std::string charname = recv_data.ReadString(nameLength);

    if (!HasPermission(rbac::RBAC_PERM_OPCODE_WHOIS))
    {
        ChatHandler(this).SendNotification(LANG_PERMISSION_DENIED);
        return;
    }

    if (charname.empty() || !normalizePlayerName (charname))
    {
        ChatHandler(this).SendNotification(LANG_NEED_CHARACTER_NAME);
        return;
    }

    Player* player = ObjectAccessor::FindPlayerByName(charname, false);

    if (!player)
    {
        ChatHandler(this).SendNotification(LANG_PLAYER_NOT_EXIST_OR_OFFLINE, charname.c_str());
        return;
    }

    uint32 accid = player->GetSession()->GetAccountId();

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_WHOIS);

    stmt->SetData(0, accid);

    PreparedQueryResult result = LoginDatabase.Query(stmt);

    if (!result)
    {
        ChatHandler(this).SendNotification(LANG_ACCOUNT_FOR_PLAYER_NOT_FOUND, charname.c_str());
        return;
    }

    Field* fields = result->Fetch();
    std::string acc = fields[0].Get<std::string>();
    if (acc.empty())
        acc = "Unknown";
    std::string email = fields[1].Get<std::string>();
    if (email.empty())
        email = "Unknown";
    std::string lastip = fields[2].Get<std::string>();
    if (lastip.empty())
        lastip = "Unknown";

    std::string msg = charname + "'s " + "account is " + acc + ", e-mail: " + email + ", last ip: " + lastip;

    // SMSG_WHO_IS, 3.4.3 (FUN_1406cdfc0, handler FUN_141572ff0): 11-bit length, flush, the text
    std::string_view text = std::string_view(msg).substr(0, 2047);
    WorldPacket data(SMSG_WHO_IS, 2 + text.size());
    data.WriteBits(text.size(), 11);
    data.FlushBits();
    data.WriteString(text);
    SendPacket(&data);

    LOG_DEBUG("network", "Received whois command from player {} for character {}", GetPlayer()->GetName(), charname);
}

void WorldSession::HandleComplainOpcode(WorldPackets::Misc::Complain& packet)
{
    LOG_DEBUG("network", "WORLD: CMSG_COMPLAINT");

    // NOTE: all chat messages from this spammer automatically ignored by spam reporter until logout in case chat spam.
    // if it's mail spam - ALL mails from this spammer automatically removed by client

    // Complaint Received message
    SendPacket(WorldPackets::Misc::ComplainResult().Write());

    if (sWorld->getBoolConfig(CONFIG_LOGSPAMREPORTS))
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_SPAM_REPORT);

        stmt->SetData(0, packet.SpamType);
        stmt->SetData(1, packet.SpammerGuid.GetCounter());
        stmt->SetData(2, packet.Unk1);
        stmt->SetData(3, packet.MailIdOrMessageType);
        stmt->SetData(4, packet.ChannelId);
        stmt->SetData(5, packet.SecondsSinceMessage);
        stmt->SetData(6, packet.Description);
        stmt->SetData(7, GameTime::GetGameTime().count());

        CharacterDatabase.Execute(stmt);
    }
}

void WorldSession::HandleFarSightOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "WORLD: CMSG_FAR_SIGHT");

    // 3.4.3 (writer FUN_14075d500): bit Enable, flush
    bool apply = recvData.ReadBit();

    if (apply)
    {
        LOG_DEBUG("network", "Added FarSight {} to player {}", _player->GetGuidValue(PLAYER_FARSIGHT).ToString(), _player->GetGUID().ToString());
        if (WorldObject* target = _player->GetViewpoint())
            _player->SetSeer(target);
        else
            LOG_DEBUG("network.opcode", "Player {} requests non-existing seer {}", _player->GetName(), _player->GetGuidValue(PLAYER_FARSIGHT).ToString());
    }
    else
    {
        WorldObject* newFarsightobject = nullptr;
        if (WorldObject* viewpoint = _player->GetViewpoint())
        {
            if (DynamicObject* viewpointDynamicObject = viewpoint->ToDynObject())
            {
                newFarsightobject = ObjectAccessor::GetUnit(*viewpointDynamicObject, viewpointDynamicObject->GetOldFarsightGUID());
            }
            else if (DynamicObject* viewpointDynamicObject = _player->GetDynObject(_player->GetUInt32Value(UNIT_CHANNEL_SPELL)))
            {
                if (viewpointDynamicObject->IsViewpoint() && viewpointDynamicObject->GetCasterGUID() == _player->GetGUID())
                {
                    newFarsightobject = viewpointDynamicObject;
                }
            }
        }

        if (newFarsightobject)
        {
            LOG_DEBUG("network", "Player {} set vision to old farsight {}", _player->GetGUID().ToString(), newFarsightobject->GetGUID().ToString());
            _player->SetViewpoint(_player->GetViewpoint(), false);
            _player->SetViewpoint(newFarsightobject, true);
        }
        else
        {
            LOG_DEBUG("network", "Player {} set vision to self", _player->GetGUID().ToString());
            _player->SetSeer(_player);
        }
    }

    GetPlayer()->UpdateVisibilityForPlayer();
}

void WorldSession::HandleSetTitleOpcode(WorldPacket& recv_data)
{
    LOG_DEBUG("network", "CMSG_SET_TITLE");

    int32 title;
    recv_data >> title;

    // -1 at none
    if (title > 0 && title < MAX_TITLE_INDEX)
    {
        if (!GetPlayer()->HasTitle(title))
            return;
    }
    else
        title = 0;

    GetPlayer()->SetUInt32Value(PLAYER_CHOSEN_TITLE, title);
}

void WorldSession::HandleResetInstancesOpcode(WorldPackets::Instance::ResetInstances& /*packet*/)
{
    LOG_DEBUG("network", "WORLD: CMSG_RESET_INSTANCES");

    if (Group* group = _player->GetGroup())
    {
        if (group->IsLeader(_player->GetGUID()))
            group->ResetInstances(INSTANCE_RESET_ALL, false, _player);
    }
    else
        Player::ResetInstances(_player->GetGUID(), INSTANCE_RESET_ALL, false);
}

void WorldSession::HandleSetDungeonDifficultyOpcode(WorldPackets::Instance::SetDungeonDifficultyClient& packet)
{
    LOG_DEBUG("network", "CMSG_SET_DUNGEON_DIFFICULTY");

    if (packet.Mode >= MAX_DUNGEON_DIFFICULTY)
        return;

    if (Difficulty(packet.Mode) == _player->GetDungeonDifficulty())
        return;

    Group* group = _player->GetGroup();
    if (group)
    {
        if (group->IsLeader(_player->GetGUID()))
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* groupGuy = itr->GetSource();
                if (!groupGuy)
                    continue;

                if (!groupGuy->IsInWorld())
                {
                    _player->SendDungeonDifficulty(group != nullptr);
                    return;
                }

                if (groupGuy->GetGUID() == _player->GetGUID() ? groupGuy->GetMap()->IsDungeon() : groupGuy->GetMap()->IsNonRaidDungeon())
                {
                    _player->SendDungeonDifficulty(group != nullptr);
                    return;
                }
            }

            group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false, _player);
            group->SetDungeonDifficulty(Difficulty(packet.Mode));
        }
    }
    else
    {
        if (_player->FindMap() && _player->FindMap()->IsDungeon())
        {
            _player->SendDungeonDifficulty(group != nullptr);
            return;
        }
        Player::ResetInstances(_player->GetGUID(), INSTANCE_RESET_CHANGE_DIFFICULTY, false);
        _player->SetDungeonDifficulty(Difficulty(packet.Mode));
    }
}

void WorldSession::HandleSetRaidDifficultyOpcode(WorldPackets::Instance::SetRaidDifficultyClient& packet)
{
    LOG_DEBUG("network", "CMSG_SET_RAID_DIFFICULTY");

    if (packet.Mode >= MAX_RAID_DIFFICULTY)
        return;

    if (Difficulty(packet.Mode) == _player->GetRaidDifficulty())
        return;

    Group* group = _player->GetGroup();
    if (group)
    {
        if (group->IsLeader(_player->GetGUID()))
        {
            std::set<uint32> foundMaps;
            std::set<Map*> foundMapsPtr;
            Map* currMap = nullptr;

            if (uint32 preventionTime = group->GetDifficultyChangePreventionTime())
            {
                switch (group->GetDifficultyChangePreventionReason())
                {
                    case DIFFICULTY_PREVENTION_CHANGE_BOSS_KILLED:
                        ChatHandler(this).PSendSysMessage("Raid was in combat recently and may not change difficulty again for {} sec.", preventionTime);
                        break;
                    case DIFFICULTY_PREVENTION_CHANGE_RECENTLY_CHANGED:
                    default:
                        ChatHandler(this).PSendSysMessage("Raid difficulty has changed recently, and may not change again for {} sec.", preventionTime);
                        break;
                }

                _player->SendRaidDifficulty(group != nullptr);
                return;
            }

            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* groupGuy = itr->GetSource();
                if (!groupGuy)
                    continue;

                if (!groupGuy->IsInWorld())
                {
                    _player->SendRaidDifficulty(group != nullptr);
                    return;
                }

                if (IsSharedDifficultyMap(groupGuy->GetMap()->GetId()) && (uint32(packet.Mode % 2) == uint32(_player->GetRaidDifficulty() % 2)) && group->isRaidGroup())
                {
                    if (!currMap)
                        currMap = groupGuy->GetMap();
                    foundMaps.insert(groupGuy->GetMap()->GetId());
                    foundMapsPtr.insert(groupGuy->GetMap());
                    if (foundMaps.size() > 1 || foundMapsPtr.size() > 1)
                    {
                        _player->SendRaidDifficulty(group != nullptr);
                        return;
                    }

                    if (!groupGuy->IsAlive() || groupGuy->IsInCombat() || groupGuy->GetVictim() || groupGuy->m_mover != groupGuy || groupGuy->IsNonMeleeSpellCast(true) || (!groupGuy->GetMotionMaster()->empty() && groupGuy->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
                            || !groupGuy->movespline->Finalized() || !groupGuy->GetMap()->ToInstanceMap() || !groupGuy->GetMap()->ToInstanceMap()->GetInstanceScript() || groupGuy->GetMap()->ToInstanceMap()->GetInstanceScript()->IsEncounterInProgress()
                            || !groupGuy->Satisfy(sObjectMgr->GetAccessRequirement(groupGuy->GetMap()->GetId(), Difficulty(packet.Mode)), groupGuy->GetMap()->GetId(), false))
                    {
                        _player->SendRaidDifficulty(group != nullptr);
                        return;
                    }
                }
                else if (groupGuy->GetGUID() == _player->GetGUID() ? groupGuy->GetMap()->IsDungeon() : groupGuy->GetMap()->IsRaid())
                {
                    _player->SendRaidDifficulty(group != nullptr);
                    return;
                }
            }

            Map* homeMap571 = sMapMgr->CreateMap(MAP_NORTHREND, nullptr);
            Map* homeMap0 = sMapMgr->CreateMap(MAP_EASTERN_KINGDOMS, nullptr);
            ASSERT(homeMap0 && homeMap571);

            std::map<Player*, Position> playerTeleport;
            // handle here all players in the instance that are not in the group
            if (currMap)
                for (Map::PlayerList::const_iterator itr = currMap->GetPlayers().begin(); itr != currMap->GetPlayers().end(); ++itr)
                    if (Player* p = itr->GetSource())
                        if (p->GetGroup() != group)
                        {
                            if (!p->IsInWorld() || !p->IsAlive() || p->IsInCombat() || p->GetVictim() || p->m_mover != p || p->IsNonMeleeSpellCast(true) || (!p->GetMotionMaster()->empty() && p->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
                                    || !p->movespline->Finalized() || !p->GetMap()->ToInstanceMap() || !p->GetMap()->ToInstanceMap()->GetInstanceScript() || p->GetMap()->ToInstanceMap()->GetInstanceScript()->IsEncounterInProgress())
                            {
                                _player->SendRaidDifficulty(group != nullptr);
                                return;
                            }
                            playerTeleport[p];
                        }
            for (std::map<Player*, Position>::iterator itr = playerTeleport.begin(); itr != playerTeleport.end(); ++itr)
            {
                Player* p = itr->first;
                Map* oldMap = p->GetMap();
                oldMap->RemovePlayerFromMap(p, false);
                p->ResetMap();
                oldMap->AfterPlayerUnlinkFromMap();
                p->SetMap(homeMap0);
                p->Relocate(0.0f, 0.0f, 0.0f, 0.0f);
                if (!p->TeleportTo(MAP_NORTHREND, 5790.20f, 2071.36f, 636.07f, 3.60f))
                    p->GetSession()->KickPlayer("HandleSetRaidDifficultyOpcode 1");
            }

            bool anyoneInside = false;
            playerTeleport.clear();
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* groupGuy = itr->GetSource();
                if (!groupGuy)
                    continue;

                if (IsSharedDifficultyMap(groupGuy->GetMap()->GetId()))
                {
                    anyoneInside = true;

                    float x, y, z, o;
                    groupGuy->GetPosition(x, y, z, o);
                    Map* oldMap = groupGuy->GetMap();
                    oldMap->RemovePlayerFromMap(groupGuy, false);
                    groupGuy->ResetMap();
                    oldMap->AfterPlayerUnlinkFromMap();
                    groupGuy->SetMap(homeMap571);
                    groupGuy->Relocate(5790.20f, 2071.36f, 636.07f, 3.60f);
                    Position dest = {x, y, z + 0.1f, o};
                    playerTeleport[groupGuy] = dest;
                }
            }

            if (!anyoneInside) // pussywizard: don't reset if changing ICC/RS difficulty while inside
                group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, true, _player);
            group->SetRaidDifficulty(Difficulty(packet.Mode));
            group->SetDifficultyChangePrevention(DIFFICULTY_PREVENTION_CHANGE_RECENTLY_CHANGED);

            for (std::map<Player*, Position>::iterator itr = playerTeleport.begin(); itr != playerTeleport.end(); ++itr)
            {
                itr->first->SetRaidDifficulty(Difficulty(packet.Mode)); // needed for teleport not to fail
                if (!itr->first->TeleportTo(*(foundMaps.begin()), itr->second.GetPositionX(), itr->second.GetPositionY(), itr->second.GetPositionZ(), itr->second.GetOrientation()))
                    itr->first->GetSession()->KickPlayer("HandleSetRaidDifficultyOpcode 2");
            }
        }
    }
    else
    {
        if (_player->FindMap() && _player->FindMap()->IsDungeon())
        {
            _player->SendRaidDifficulty(group != nullptr);
            return;
        }
        Player::ResetInstances(_player->GetGUID(), INSTANCE_RESET_CHANGE_DIFFICULTY, true);
        _player->SetRaidDifficulty(Difficulty(packet.Mode));
    }
}

void WorldSession::HandleCancelMountAuraOpcode(WorldPacket& /*recv_data*/)
{
    LOG_DEBUG("network", "WORLD: CMSG_CANCEL_MOUNT_AURA");

    //If player is not mounted, so go out :)
    if (!_player->IsMounted())                              // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_CHAR_NON_MOUNTED);
        return;
    }

    if (_player->IsInFlight())                               // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_YOU_IN_FLIGHT);
        return;
    }

    _player->Dismount();
    _player->RemoveAurasByType(SPELL_AURA_MOUNTED);
}

void WorldSession::HandleMoveFlagChangeOpcode(WorldPacket& recv_data)
{
    LOG_DEBUG("network", "WORLD: {}", GetOpcodeNameForLogging((Opcodes)recv_data.GetOpcode()));

    Opcodes opcode = (Opcodes)recv_data.GetOpcode();

    // 3.4.3 (e.g. CMSG_MOVE_HOVER_ACK, writer FUN_140762c00): the status, u32 AckIndex
    uint32 counter;

    if (!_player)
    {
        recv_data.rfinish(); // prevent warnings spam
        return;
    }

    MovementInfo movementInfo;
    ReadMovementInfo(recv_data, &movementInfo);
    recv_data >> counter;

    sScriptMgr->AnticheatSetCanFlybyServer(_player, movementInfo.HasMovementFlag(MOVEMENTFLAG_CAN_FLY));

    Unit* mover = _player->m_mover;
    Player* plrMover = mover->ToPlayer();

    mover->m_movementInfo.flags = movementInfo.GetMovementFlags();

    // old map - async processing, ignore
    if (counter <= _player->GetMapChangeOrderCounter())
        return;

    if (!ProcessMovementInfo(movementInfo, mover, plrMover, recv_data))
    {
        recv_data.rfinish();                     // prevent warnings spam
        return;
    }

    if (_player->GetPendingFlightChange() == counter && opcode == CMSG_MOVE_SET_CAN_FLY_ACK)
        _player->SetPendingFlightChange(false);

    Opcodes response;

    switch (recv_data.GetOpcode())
    {
        case CMSG_MOVE_HOVER_ACK: response = SMSG_MOVE_UPDATE; break;
        case CMSG_MOVE_FEATHER_FALL_ACK: response = SMSG_MOVE_UPDATE; break;
        case CMSG_MOVE_WATER_WALK_ACK: response = SMSG_MOVE_UPDATE; break;
        case CMSG_MOVE_SET_CAN_FLY_ACK: response = SMSG_MOVE_UPDATE; break;
        case CMSG_MOVE_GRAVITY_DISABLE_ACK: response = SMSG_MOVE_UPDATE; break;
        case CMSG_MOVE_GRAVITY_ENABLE_ACK: response = SMSG_MOVE_UPDATE; break;
        default: return;
    }

    WorldPacket data(response, 8);
    WriteMovementInfo(&data, &movementInfo);
    _player->m_mover->SendMessageToSet(&data, _player);
}

void WorldSession::HandleRequestPetInfo(WorldPackets::Pet::RequestPetInfo& /*packet*/)
{
    /*
        LOG_DEBUG("network.opcode", "WORLD: CMSG_REQUEST_PET_INFO");
        recv_data.hexlike();
    */

    if (_player->GetPet())
        _player->PetSpellInitialize();
    else if (Unit* charm = _player->GetCharm())
    {
        if (charm->HasUnitState(UNIT_STATE_POSSESSED))
            _player->PossessSpellInitialize();
        else if (charm->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED) && charm->HasUnitFlag(UNIT_FLAG_POSSESSED))
            _player->VehicleSpellInitialize();
        else
            _player->CharmSpellInitialize();
    }
}

void WorldSession::HandleSetTaxiBenchmarkOpcode(WorldPacket& recv_data)
{
    uint8 mode;
    recv_data >> mode;

    mode ? _player->SetPlayerFlag(PLAYER_FLAGS_TAXI_BENCHMARK) : _player->RemovePlayerFlag(PLAYER_FLAGS_TAXI_BENCHMARK);

    LOG_DEBUG("network", "Client used \"/timetest {}\" command", mode);
}

void WorldSession::HandleQueryInspectAchievements(WorldPacket& recv_data)
{
    // 3.4.3 (writer FUN_14075f4d0, Lua SetAchievementComparisonUnit): PackedGuid128 Guid
    ObjectGuid guid = Guid128::Read(recv_data);

    Player* player = ObjectAccessor::GetPlayer(*_player, guid);
    if (!player)
    {
        return;
    }

    if (!GetPlayer()->IsWithinDistInMap(player, INSPECT_DISTANCE, false))
    {
        return;
    }

    if (GetPlayer()->IsValidAttackTarget(player))
    {
        return;
    }

    player->SendRespondInspectAchievements(_player);
}

void WorldSession::HandleReadyForAccountDataTimes(WorldPacket& /*recv_data*/)
{
    // empty opcode
    LOG_DEBUG("network", "WORLD: CMSG_READY_FOR_ACCOUNT_DATA_TIMES");

    SendAccountDataTimes(GLOBAL_CACHE_MASK);
}

// SMSG_PHASE_SHIFT_CHANGE, 3.4.3 (parser FUN_1407022e0, phase data FUN_14071d4e0, handler FUN_141677460):
// PackedGuid128 Client, u32 PhaseShiftFlags, u32 PhaseCount, PackedGuid128 PersonalGUID,
// {u16 PhaseFlags, u16 Id}[PhaseCount], then three byte-counted u16 arrays: VisibleMapIDs,
// PreloadMapIDs, UiMapPhaseIDs.
// AC's phases are a 3.3.5 bit mask the 3.4.3 client cannot use (its phases are Phase.db2 ids), so
// the client is told it is unphased; the server keeps filtering visibility by the mask itself.
void WorldSession::SendSetPhaseShift(uint32 /*phaseShift*/)
{
    if (!_player)
        return;

    WorldPacket data(SMSG_PHASE_SHIFT_CHANGE, 2 + 16 + 4 + 4 + 2 + 12);
    Guid128::WritePlayer(data, _player->GetGUID());         // Client
    data << uint32(0x08);                                   // PhaseShiftFlags: Unphased
    data << uint32(0);                                      // PhaseCount
    Guid128::WritePacked(data, 0, 0);                       // PersonalGUID
    data << uint32(0);                                      // VisibleMapIDs (bytes)
    data << uint32(0);                                      // PreloadMapIDs (bytes)
    data << uint32(0);                                      // UiMapPhaseIDs (bytes)
    SendPacket(&data);
}

//Battlefield and Battleground
void WorldSession::HandleAreaSpiritHealerQueryOpcode(WorldPacket& recv_data)
{
    LOG_DEBUG("network", "WORLD: CMSG_AREA_SPIRIT_HEALER_QUERY");

    Battleground* bg = _player->GetBattleground();

    // 3.4.3 (writer FUN_14075bad0): PackedGuid128 HealerGuid
    ObjectGuid guid = Guid128::Read(recv_data);

    Creature* unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
        return;

    if (!unit->IsSpiritService())                            // it's not spirit service
        return;

    if (bg)
        sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(_player, bg, guid);

    if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(_player->GetZoneId()))
        bf->SendAreaSpiritHealerQueryOpcode(_player, guid);
}

void WorldSession::HandleAreaSpiritHealerQueueOpcode(WorldPacket& recv_data)
{
    LOG_DEBUG("network", "WORLD: CMSG_AREA_SPIRIT_HEALER_QUEUE");

    Battleground* bg = _player->GetBattleground();

    // 3.4.3 (writer FUN_14075bb10): PackedGuid128 HealerGuid
    ObjectGuid guid = Guid128::Read(recv_data);

    Creature* unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
        return;

    if (!unit->IsSpiritService())                            // it's not spirit service
        return;

    if (bg)
        bg->AddPlayerToResurrectQueue(guid, _player->GetGUID());

    if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(_player->GetZoneId()))
        bf->AddPlayerToResurrectQueue(guid, _player->GetGUID());
}

void WorldSession::HandleHearthAndResurrect(WorldPacket& /*recv_data*/)
{
    if (_player->IsInFlight())
        return;

    if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(_player->GetZoneId()))
    {
        bf->PlayerAskToLeave(_player);
        return;
    }

    AreaTableEntry const* atEntry = sAreaTableStore.LookupEntry(_player->GetAreaId());
    if (!atEntry || !(atEntry->flags & AREA_FLAG_WINTERGRASP_2))
        return;

    _player->BuildPlayerRepop();
    _player->ResurrectPlayer(1.0f);
    _player->SpawnCorpseBones();
    _player->TeleportTo(_player->m_homebindMapId, _player->m_homebindX, _player->m_homebindY, _player->m_homebindZ, _player->GetOrientation());
}

void WorldSession::HandleInstanceLockResponse(WorldPackets::Instance::InstanceLockResponse& packet)
{
    if (!_player->HasPendingBind() || _player->GetPendingBind() != _player->GetInstanceId() || (_player->GetGroup() && _player->GetGroup()->isLFGGroup() && _player->GetGroup()->IsLfgRandomInstance()))
    {
        LOG_DEBUG("network.opcode", "InstanceLockResponse: Player {} ({}) tried to bind himself/teleport to graveyard without a pending bind!",
            _player->GetName(), _player->GetGUID().ToString());
        return;
    }

    if (packet.Accept)
        _player->BindToInstance();
    else
        _player->RepopAtGraveyard();

    _player->SetPendingBind(0, 0);
}

void WorldSession::HandleUpdateMissileTrajectory(WorldPacket& recvPacket)
{
    LOG_DEBUG("network", "WORLD: CMSG_UPDATE_MISSILE_TRAJECTORY");

    ObjectGuid guid;
    uint32 spellId;
    float elevation, speed;
    float curX, curY, curZ;
    float targetX, targetY, targetZ;
    uint8 moveStop;

    recvPacket >> guid >> spellId >> elevation >> speed;
    recvPacket >> curX >> curY >> curZ;
    recvPacket >> targetX >> targetY >> targetZ;
    recvPacket >> moveStop;

    Unit* caster = ObjectAccessor::GetUnit(*_player, guid);
    Spell* spell = caster ? caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
    if (!spell || spell->m_spellInfo->Id != spellId || !spell->m_targets.HasDst() || !spell->m_targets.HasSrc())
    {
        recvPacket.rfinish();
        return;
    }

    Position pos = *spell->m_targets.GetSrcPos();
    pos.Relocate(curX, curY, curZ);
    spell->m_targets.ModSrc(pos);

    pos = *spell->m_targets.GetDstPos();
    pos.Relocate(targetX, targetY, targetZ);
    spell->m_targets.ModDst(pos);

    spell->m_targets.SetElevation(elevation);
    spell->m_targets.SetSpeed(speed);

    if (moveStop)
    {
        uint32 opcode;
        recvPacket >> opcode;
        recvPacket.SetOpcode(opcode);
        HandleMovementOpcodes(recvPacket);
    }
}
