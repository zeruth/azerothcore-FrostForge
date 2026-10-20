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

#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PetitionMgr.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "PacketGuid128.h"

/// SMSG_PETITION_SHOW_SIGNATURES, 3.4.3 (FUN_1406c73a0, handler FUN_141bd99a0): PackedGuid128 Item, PackedGuid128 Owner,
/// PackedGuid128 OwnerAccountID, u32 PetitionID, u32 count, {PackedGuid128 Signer, u32 Choice}
static void BuildPetitionSignatures(WorldPacket& data, ObjectGuid petitionGuid, ObjectGuid owner, uint32 petitionId, Signatures const* signatures)
{
    uint32 signs = signatures ? uint32(signatures->signatureMap.size()) : 0;
    data.Initialize(SMSG_PETITION_SHOW_SIGNATURES, 18 * 3 + 8 + signs * 22);
    Guid128::Write(data, petitionGuid);
    Guid128::WritePlayer(data, owner);
    Guid128::Write(data, ObjectGuid::Empty);            // OwnerAccountID
    data << uint32(petitionId);
    data << uint32(signs);
    if (signatures)
        for (auto const& [signer, account] : signatures->signatureMap)
        {
            Guid128::WritePlayer(data, signer);
            data << uint32(0);                              // Choice
        }
}

/// SMSG_PETITION_SIGN_RESULTS, 3.4.3 (FUN_1406c74d0, handler FUN_141bd9ab0): PackedGuid128 Item, PackedGuid128 Player,
/// 4-bit Error (PetitionSigns), flush
static void BuildPetitionSignResult(WorldPacket& data, ObjectGuid petitionGuid, ObjectGuid player, uint8 result)
{
    data.Initialize(SMSG_PETITION_SIGN_RESULTS, 18 * 2 + 1);
    Guid128::Write(data, petitionGuid);
    Guid128::WritePlayer(data, player);
    data.WriteBits(result, 4);
    data.FlushBits();
}

/// SMSG_TURN_IN_PETITION_RESULT, 3.4.3 (FUN_1406b6520, handler FUN_141bd9d70): 4-bit Result (PetitionTurns), flush
static void SendTurnInPetitionResult(WorldSession* session, uint8 result)
{
    WorldPacket data(SMSG_TURN_IN_PETITION_RESULT, 1);
    data.WriteBits(result, 4);
    data.FlushBits();
    session->SendPacket(&data);
}

void WorldSession::HandlePetitionBuyOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_PETITION_BUY");

    // 3.4.3 (writer FUN_14075f230): 7-bit title length, flush, PackedGuid128 Unit, u32 Index (SHOW_LIST's), the title
    uint32 titleLength = recvData.ReadBits(7);
    recvData.ResetBitReader();
    ObjectGuid guidNPC = Guid128::Read(recvData);
    uint32 clientIndex = recvData.read<uint32>();           // 1 for guild and arenaslot+1 for arenas in client
    std::string name = recvData.ReadString(titleLength);

    LOG_DEBUG("network", "Petitioner ({}) tried sell petition: name {}", guidNPC.ToString(), name);

    // prevent cheating
    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(guidNPC, UNIT_NPC_FLAG_PETITIONER);
    if (!creature)
    {
        LOG_DEBUG("network", "WORLD: HandlePetitionBuyOpcode - Unit ({}) not found or you can't interact with him.", guidNPC.ToString());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    uint32 charterid = 0;
    uint32 cost = 0;
    uint32 type = 0;
    if (creature->IsTabardDesigner())
    {
        // if tabard designer, then trying to buy a guild charter.
        // do not let if already in guild.
        if (_player->GetGuildId())
            return;

        if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_GUILD) && IsTrialAccount())
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_PERMISSIONS);
            return;
        }

        charterid = GUILD_CHARTER;
        cost = sWorld->getIntConfig(CONFIG_CHARTER_COST_GUILD);
        type = GUILD_CHARTER_TYPE;
    }
    else
    {
        /// @todo: find correct opcode
        if (_player->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", _player->GetName(), ERR_ARENA_TEAM_TARGET_TOO_LOW_S);
            return;
        }

        switch (clientIndex)                                 // arenaSlot+1 as received from client (1 from 3 case)
        {
            case 1:
                charterid = ARENA_TEAM_CHARTER_2v2;
                cost = sWorld->getIntConfig(CONFIG_CHARTER_COST_ARENA_2v2);
                type = ARENA_TEAM_CHARTER_2v2_TYPE;
                break;
            case 2:
                charterid = ARENA_TEAM_CHARTER_3v3;
                cost = sWorld->getIntConfig(CONFIG_CHARTER_COST_ARENA_3v3);
                type = ARENA_TEAM_CHARTER_3v3_TYPE;
                break;
            case 3:
                charterid = ARENA_TEAM_CHARTER_5v5;
                cost = sWorld->getIntConfig(CONFIG_CHARTER_COST_ARENA_5v5);
                type = ARENA_TEAM_CHARTER_5v5_TYPE;
                break;
            default:
                LOG_DEBUG("network", "unknown selection at buy arena petition: {}", clientIndex);
                return;
        }

        if (_player->GetArenaTeamId(clientIndex - 1))        // arenaSlot+1 as received from client
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, name, "", ERR_ALREADY_IN_ARENA_TEAM);
            return;
        }
    }

    sScriptMgr->OnPlayerPetitionBuy(_player, creature, charterid, cost, type);

    if (type == GUILD_CHARTER_TYPE)
    {
        if (sGuildMgr->GetGuildByName(name))
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_NAME_EXISTS_S, name);
            return;
        }

        if (!ObjectMgr::IsValidCharterName(name))
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_NAME_INVALID, name);
            return;
        }
    }
    else
    {
        if (sArenaTeamMgr->GetArenaTeamByName(name))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, name, "", ERR_ARENA_TEAM_NAME_EXISTS_S);
            return;
        }
        if (!ObjectMgr::IsValidCharterName(name))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, name, "", ERR_ARENA_TEAM_NAME_INVALID);
            return;
        }
    }

    ItemTemplate const* pProto = sObjectMgr->GetItemTemplate(charterid);
    if (!pProto)
    {
        _player->SendBuyError(BUY_ERR_CANT_FIND_ITEM, nullptr, charterid, 0);
        return;
    }

    if (!_player->HasEnoughMoney(cost))
    {
        //player hasn't got enough money
        _player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, creature, charterid, 0);
        return;
    }

    ItemPosCountVec dest;
    InventoryResult msg = _player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, charterid, pProto->BuyCount);
    if (msg != EQUIP_ERR_OK)
    {
        _player->SendEquipError(msg, nullptr, nullptr, charterid);
        return;
    }

    _player->ModifyMoney(-(int32)cost);
    Item* charter = _player->StoreNewItem(dest, charterid, true);
    if (!charter)
        return;

    // Use a 31-bit safe petition id instead of the raw item guid
    uint32 petitionId = sPetitionMgr->GeneratePetitionId();
    charter->SetUInt32Value(ITEM_FIELD_ENCHANTMENT_1_1, petitionId);
    // ITEM_FIELD_ENCHANTMENT_1_1 is guild/arenateam id
    // ITEM_FIELD_ENCHANTMENT_1_1+1 is current signatures count (showed on item)
    charter->SetState(ITEM_CHANGED, _player);
    _player->SendNewItem(charter, 1, true, false);

    // a petition is invalid, if both the owner and the type matches
    // we checked above, if this player is in an arenateam, so this must be
    // datacorruption
    Petition const* petition = sPetitionMgr->GetPetitionByOwnerWithType(_player->GetGUID(), type);

    CharacterDatabase.EscapeString(name);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    if (petition)
    {
        LOG_DEBUG("network", "Invalid petition: {}", petition->petitionGuid.ToString());

        trans->Append("DELETE FROM petition WHERE petitionguid = {}", petition->petitionGuid.GetCounter());
        trans->Append("DELETE FROM petition_sign WHERE petitionguid = {}", petition->petitionGuid.GetCounter());

        // xinef: clear petition store
        sPetitionMgr->RemovePetition(petition->petitionGuid);
    }

    // xinef: petition pointer is invalid from now on

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_PETITION);
    // petition_id, ownerguid, petitionguid(item guid), name, type
    stmt->SetData(0, petitionId);
    stmt->SetData(1, _player->GetGUID().GetCounter());
    stmt->SetData(2, charter->GetGUID().GetCounter());
    stmt->SetData(3, name);
    stmt->SetData(4, uint8(type));
    trans->Append(stmt);

    CharacterDatabase.CommitTransaction(trans);

    // xinef: fill petition store (include petitionId)
    sPetitionMgr->AddPetition(charter->GetGUID(), _player->GetGUID(), name, uint8(type), petitionId);
}

void WorldSession::HandlePetitionShowSignOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_PETITION_SHOW_SIGNATURES");

    // 3.4.3 (writer FUN_14075f330): PackedGuid128 Item
    ObjectGuid petitionguid = Guid128::Read(recvData);

    // solve (possible) some strange compile problems with explicit use petition low guid at some GCC versions (wrong code optimization in compiler?)
    Petition const* petition = sPetitionMgr->GetPetition(petitionguid);
    if (!petition)
        return;

    uint32 type = petition->petitionType;

    // if guild petition and has guild => error, return;
    if (type == GUILD_CHARTER_TYPE && _player->GetGuildId())
        return;

    Signatures const* signatures = sPetitionMgr->GetSignature(petitionguid);
    uint8 signs = signatures ? signatures->signatureMap.size() : 0;

    LOG_DEBUG("network", "CMSG_PETITION_SHOW_SIGNATURES petition {}", petitionguid.ToString());

    (void)signs;
    WorldPacket data;
    BuildPetitionSignatures(data, petitionguid, petition->ownerGuid, petition->petitionId, signatures);
    SendPacket(&data);
}

void WorldSession::HandlePetitionQueryOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_QUERY_PETITION");   // ok

    // 3.4.3 (writer FUN_14075f610): u32 PetitionID, PackedGuid128 ItemGUID
    uint32 guildguid = recvData.read<uint32>();
    ObjectGuid petitionguid = Guid128::Read(recvData);
    LOG_DEBUG("network", "CMSG_QUERY_PETITION Petition ({}) Guild GUID {}", petitionguid.ToString(), guildguid);

    SendPetitionQueryOpcode(petitionguid);
}

/// SMSG_QUERY_PETITION_RESPONSE, 3.4.3 (cache switch FUN_140721aa0, handler FUN_140345b50): u32 PetitionID, bit Allow,
/// flush, [PetitionInfo (FUN_14070e110): u32 PetitionID, PackedGuid128 Petitioner, u32 MinSignatures, MaxSignatures,
/// DeadLine, IssueDate, AllowedGuildID, AllowedClasses, AllowedRaces, u16 AllowedGender, u32 AllowedMinLevel,
/// AllowedMaxLevel, NumChoices, StaticType (0 guild, 1 arena), Muid, 7-bit title, 12-bit body and 10 x 6-bit choice
/// lengths, flush, the choices, the title, the body]
void WorldSession::SendPetitionQueryOpcode(ObjectGuid petitionguid)
{
    Petition const* petition = sPetitionMgr->GetPetition(petitionguid);
    if (!petition)
    {
        LOG_DEBUG("network", "CMSG_QUERY_PETITION failed for petition ({})", petitionguid.ToString());
        WorldPacket data(SMSG_QUERY_PETITION_RESPONSE, 5);
        data << uint32(0);
        data.WriteBit(false);
        data.FlushBits();
        SendPacket(&data);
        return;
    }

    uint8 type = petition->petitionType;
    uint32 needed = type == GUILD_CHARTER_TYPE ? sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS) : uint32(type - 1);
    std::string_view title = std::string_view(petition->petitionName).substr(0, 127);
    WorldPacket data(SMSG_QUERY_PETITION_RESPONSE, 5 + 18 + 60 + 10 + title.size());
    data << uint32(petition->petitionId);
    data.WriteBit(true);                                    // Allow
    data.FlushBits();
    data << uint32(petition->petitionId);
    Guid128::WritePlayer(data, petition->ownerGuid);
    data << uint32(needed);                                 // MinSignatures
    data << uint32(needed);                                 // MaxSignatures
    data << uint32(0);                                      // DeadLine
    data << uint32(0);                                      // IssueDate
    data << uint32(0);                                      // AllowedGuildID
    data << uint32(0);                                      // AllowedClasses
    data << uint32(0);                                      // AllowedRaces
    data << uint16(0);                                      // AllowedGender
    data << uint32(0);                                      // AllowedMinLevel
    data << uint32(0);                                      // AllowedMaxLevel
    data << uint32(0);                                      // NumChoices
    data << uint32(type != GUILD_CHARTER_TYPE);             // StaticType: 0 guild, 1 arena team
    data << uint32(0);                                      // Muid
    data.WriteBits(title.size(), 7);
    data.WriteBits(0, 12);                                  // BodyText
    for (uint8 i = 0; i < 10; ++i)
        data.WriteBits(0, 6);                               // choices
    data.FlushBits();
    data.WriteString(title);
    SendPacket(&data);
}

void WorldSession::HandlePetitionRenameOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_PETITION_RENAME_GUILD");   // ok

    // 3.4.3 (writer FUN_140768770): PackedGuid128 PetitionGuid, 7-bit name length, flush, the name
    ObjectGuid petitionGuid = Guid128::Read(recvData);
    uint32 nameLength = recvData.ReadBits(7);
    recvData.ResetBitReader();
    std::string newName = recvData.ReadString(nameLength);

    Item* item = _player->GetItemByGuid(petitionGuid);
    if (!item)
        return;

    Petition const* petition = sPetitionMgr->GetPetition(petitionGuid);
    if (!petition)
    {
        LOG_DEBUG("network", "CMSG_QUERY_PETITION failed for petition ({})", petitionGuid.ToString());
        return;
    }

    if (petition->petitionType == GUILD_CHARTER_TYPE)
    {
        if (sGuildMgr->GetGuildByName(newName))
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_NAME_EXISTS_S, newName);
            return;
        }
        if (!ObjectMgr::IsValidCharterName(newName))
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_NAME_INVALID, newName);
            return;
        }
    }
    else
    {
        if (sArenaTeamMgr->GetArenaTeamByName(newName))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, newName, "", ERR_ARENA_TEAM_NAME_EXISTS_S);
            return;
        }
        if (!ObjectMgr::IsValidCharterName(newName))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, newName, "", ERR_ARENA_TEAM_NAME_INVALID);
            return;
        }
    }

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_PETITION_NAME);

    stmt->SetData(0, newName);
    stmt->SetData(1, petition->petitionId);

    CharacterDatabase.Execute(stmt);

    // xinef: update petition container
    const_cast<Petition*>(petition)->petitionName = newName;

    LOG_DEBUG("network", "Petition ({}) renamed to {}", petitionGuid.ToString(), newName);
    // SMSG_PETITION_RENAME_GUILD_RESPONSE, 3.4.3 (inline, handler FUN_14170e6e0): PackedGuid128 PetitionGuid, 7-bit name
    // length, flush, the name
    std::string_view nameView = std::string_view(newName).substr(0, 127);
    WorldPacket data(SMSG_PETITION_RENAME_GUILD_RESPONSE, 18 + 1 + nameView.size());
    Guid128::Write(data, petitionGuid);
    data.WriteBits(nameView.size(), 7);
    data.FlushBits();
    data.WriteString(nameView);
    SendPacket(&data);
}

void WorldSession::HandlePetitionSignOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_SIGN_PETITION");    // ok

    // 3.4.3 (writer FUN_140760b20): PackedGuid128 PetitionGUID, u8 Choice
    ObjectGuid petitionGuid = Guid128::Read(recvData);
    recvData.read_skip<uint8>();

    Petition const* petition = sPetitionMgr->GetPetition(petitionGuid);
    if (!petition)
    {
        LOG_ERROR("network.opcode", "Petition {} is not found for player {} (Name: {})", petitionGuid.ToString(), GetPlayer()->GetGUID().ToString(), GetPlayer()->GetName());
        return;
    }

    uint8 type = petition->petitionType;

    ObjectGuid playerGuid = _player->GetGUID();
    if (petition->ownerGuid == playerGuid)
        return;

    Signatures const* signatures = sPetitionMgr->GetSignature(petitionGuid);
    if (!signatures)
        return;

    if (type != GUILD_CHARTER_TYPE)
    {
        if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_ARENA) && GetPlayer()->GetTeamId() != sCharacterCache->GetCharacterTeamByGuid(petition->ownerGuid))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, "", "", ERR_ARENA_TEAM_NOT_ALLIED);
            return;
        }

        if (_player->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, "", _player->GetName().c_str(), ERR_ARENA_TEAM_TARGET_TOO_LOW_S);
            return;
        }

        uint8 slot = ArenaTeam::GetSlotByType(type);
        if (slot >= MAX_ARENA_SLOT)
            return;

        if (_player->GetArenaTeamId(slot))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, "", _player->GetName().c_str(), ERR_ALREADY_IN_ARENA_TEAM_S);
            return;
        }

        if (_player->GetArenaTeamIdInvited())
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, "", _player->GetName().c_str(), ERR_ALREADY_INVITED_TO_ARENA_TEAM_S);
            return;
        }
    }
    else
    {
        if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD) && GetPlayer()->GetTeamId() != sCharacterCache->GetCharacterTeamByGuid(petition->ownerGuid))
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_NOT_ALLIED);
            return;
        }

        if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_GUILD) && IsTrialAccount())
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_INVITE, ERR_GUILD_PERMISSIONS);
            return;
        }

        if (_player->GetGuildId())
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_INVITE, ERR_ALREADY_IN_GUILD_S, _player->GetName());
            return;
        }
        if (_player->GetGuildIdInvited())
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_INVITE, ERR_ALREADY_INVITED_TO_GUILD_S, _player->GetName());
            return;
        }
    }

    uint32 signs = signatures->signatureMap.size();
    if (++signs > type)                                        // client signs maximum
        return;

    // Client doesn't allow to sign petition two times by one character, but not check sign by another character from same account
    // not allow sign another player from already sign player account

    bool found = false;
    for (SignatureMap::const_iterator itr = signatures->signatureMap.begin(); itr != signatures->signatureMap.end(); ++itr)
        if (itr->second == GetAccountId())
        {
            found = true;
            break;
        }

    sScriptMgr->OnPlayerBeforePetitionSign(_player, petitionGuid, found);

    if (found)
    {
        WorldPacket data;
        BuildPetitionSignResult(data, petitionGuid, playerGuid, PETITION_SIGN_ALREADY_SIGNED);

        // close at signer side
        SendPacket(&data);

        // update for owner if online
        if (Player* owner = ObjectAccessor::FindConnectedPlayer(petition->ownerGuid))
            owner->SendDirectMessage(&data);
        return;
    }

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_PETITION_SIGNATURE);

    stmt->SetData(0, petition->ownerGuid.GetCounter());
    stmt->SetData(1, petition->petitionId);
    stmt->SetData(2, playerGuid.GetCounter());
    stmt->SetData(3, GetAccountId());

    CharacterDatabase.Execute(stmt);

    // xinef: fill petition store
    sPetitionMgr->AddSignature(petitionGuid, GetAccountId(), playerGuid);

    LOG_DEBUG("network", "PETITION SIGN: {} by player: {} ({}, Account: {})", petitionGuid.ToString(), _player->GetName(), playerGuid.ToString(), GetAccountId());

    WorldPacket data;
    BuildPetitionSignResult(data, petitionGuid, playerGuid, PETITION_SIGN_OK);

    // close at signer side
    SendPacket(&data);

    // update signs count on charter, required testing...
    //Item* item = _player->GetItemByGuid(petitionguid));
    //if (item)
    //    item->SetUInt32Value(ITEM_FIELD_ENCHANTMENT_1_1+1, signs);

    // update for owner if online
    if (Player* owner = ObjectAccessor::FindConnectedPlayer(petition->ownerGuid))
        owner->SendDirectMessage(&data);
}

void WorldSession::HandlePetitionDeclineOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_DECLINE_PETITION");  // ok

    // 3.4.3 (writer FUN_14075d260): PackedGuid128 PetitionGUID. There is no "petition declined" message for the owner.
    ObjectGuid petitionguid = Guid128::Read(recvData);
    LOG_DEBUG("network", "Petition {} declined by {}", petitionguid.ToString(), _player->GetGUID().ToString());
}

void WorldSession::HandleOfferPetitionOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_OFFER_PETITION");   // ok

    // 3.4.3 (writer FUN_14075ee50): u32, PackedGuid128 ItemGUID, PackedGuid128 TargetPlayer
    Player* player;
    recvData.read_skip<uint32>();
    ObjectGuid petitionguid = Guid128::Read(recvData);
    ObjectGuid plguid = Guid128::Read(recvData);

    player = ObjectAccessor::FindConnectedPlayer(plguid);
    if (!player)
        return;

    Petition const* petition = sPetitionMgr->GetPetition(petitionguid);
    if (!petition)
        return;

    if (petition->petitionType != GUILD_CHARTER_TYPE)
    {
        if (GetPlayer()->GetTeamId() != player->GetTeamId() && !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_ARENA))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, "", "", ERR_ARENA_TEAM_NOT_ALLIED);
            return;
        }

        if (player->GetLevel() < sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        {
            // player is too low level to join an arena team
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, player->GetName().c_str(), "", ERR_ARENA_TEAM_TARGET_TOO_LOW_S);
            return;
        }

        uint8 slot = ArenaTeam::GetSlotByType(petition->petitionType);
        if (slot >= MAX_ARENA_SLOT)
            return;

        if (player->GetArenaTeamId(slot))
        {
            // player is already in an arena team
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, player->GetName().c_str(), "", ERR_ALREADY_IN_ARENA_TEAM_S);
            return;
        }

        if (player->GetArenaTeamIdInvited())
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_INVITE_SS, "", _player->GetName().c_str(), ERR_ALREADY_INVITED_TO_ARENA_TEAM_S);
            return;
        }
    }
    else
    {
        if (GetPlayer()->GetTeamId() != player->GetTeamId() && !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD))
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_NOT_ALLIED);
            return;
        }

        if (player->GetGuildId())
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_INVITE, ERR_ALREADY_IN_GUILD_S, _player->GetName());
            return;
        }

        if (player->GetGuildIdInvited())
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_INVITE, ERR_ALREADY_INVITED_TO_GUILD_S, _player->GetName());
            return;
        }
    }

    Signatures const* signatures = sPetitionMgr->GetSignature(petitionguid);

    WorldPacket data;
    BuildPetitionSignatures(data, petitionguid, _player->GetGUID(), petition->petitionId, signatures);
    player->SendDirectMessage(&data);
}

void WorldSession::HandleTurnInPetitionOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received opcode CMSG_TURN_IN_PETITION");

    // 3.4.3 (writer FUN_140761340): PackedGuid128 Item, u32 BackgroundColor, EmblemStyle, EmblemColor, BorderStyle,
    // BorderColor (the arena banner; 0xFFFFFFFF for a guild charter)
    ObjectGuid petitionGuid = Guid128::Read(recvData);
    uint32 background = recvData.read<uint32>();
    uint32 icon = recvData.read<uint32>();
    uint32 iconcolor = recvData.read<uint32>();
    uint32 border = recvData.read<uint32>();
    uint32 bordercolor = recvData.read<uint32>();

    // Check if player really has the required petition charter
    Item* item = _player->GetItemByGuid(petitionGuid);
    if (!item)
        return;

    LOG_DEBUG("network", "Petition {} turned in by {}", petitionGuid.ToString(), _player->GetGUID().ToString());

    Petition const* petition = sPetitionMgr->GetPetition(petitionGuid);
    if (!petition)
    {
        LOG_ERROR("network.opcode", "Player {} ({}) tried to turn in petition ({}) that is not present in the database",
            _player->GetName(), _player->GetGUID().ToString(), petitionGuid.ToString());
        return;
    }

    ObjectGuid ownerGuid = petition->ownerGuid;
    uint8 type = petition->petitionType;
    std::string name = petition->petitionName;

    // Only the petition owner can turn in the petition
    if (_player->GetGUID() != ownerGuid)
        return;

    // Petition type (guild/arena) specific checks
    if (type == GUILD_CHARTER_TYPE)
    {
        if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_GUILD) && IsTrialAccount())
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_PERMISSIONS);
            return;
        }

        // Check if player is already in a guild
        if (_player->GetGuildId())
        {
            SendTurnInPetitionResult(this, PETITION_TURN_ALREADY_IN_GUILD);
            return;
        }

        // Check if guild name is already taken
        if (sGuildMgr->GetGuildByName(name))
        {
            Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_NAME_EXISTS_S, name);
            return;
        }
    }
    else
    {
        // Check for valid arena bracket (2v2, 3v3, 5v5)
        uint8 slot = ArenaTeam::GetSlotByType(type);
        if (slot >= MAX_ARENA_SLOT)
            return;

        // Check if player is already in an arena team
        if (_player->GetArenaTeamId(slot))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, name, "", ERR_ALREADY_IN_ARENA_TEAM);
            return;
        }

        // Check if arena team name is already taken
        if (sArenaTeamMgr->GetArenaTeamByName(name))
        {
            SendArenaTeamCommandResult(ERR_ARENA_TEAM_CREATE_S, name, "", ERR_ARENA_TEAM_NAME_EXISTS_S);
            return;
        }
    }

    // Get petition signatures from db
    Signatures const* signatures = sPetitionMgr->GetSignature(petitionGuid);
    uint8 signs = signatures ? signatures->signatureMap.size() : 0;
    SignatureMap signatureCopy;
    if (signs)
        signatureCopy = signatures->signatureMap;

    uint32 requiredSignatures;
    if (type == GUILD_CHARTER_TYPE)
        requiredSignatures = sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS);
    else
        requiredSignatures = type - 1;

    // Notify player if signatures are missing
    if (signs < requiredSignatures)
    {
        SendTurnInPetitionResult(this, PETITION_TURN_NEED_MORE_SIGNATURES);
        return;
    }

    // Proceed with guild/arena team creation

    // Delete charter item
    _player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

    if (type == GUILD_CHARTER_TYPE)
    {
        // Create guild
        Guild* guild = new Guild;

        if (!guild->Create(_player, name))
        {
            delete guild;
            return;
        }

        // Register guild and add guild master
        sGuildMgr->AddGuild(guild);

        Guild::SendCommandResult(this, GUILD_COMMAND_CREATE, ERR_GUILD_COMMAND_SUCCESS, name);

        // Add members from signatures
        if (signs)
            for (SignatureMap::const_iterator itr = signatureCopy.begin(); itr != signatureCopy.end(); ++itr)
                guild->AddMember(itr->first);
    }
    else
    {
        // Create arena team
        ArenaTeam* arenaTeam = new ArenaTeam();

        if (!arenaTeam->Create(_player->GetGUID(), type, name, background, icon, iconcolor, border, bordercolor))
        {
            delete arenaTeam;
            return;
        }

        // Register arena team
        sArenaTeamMgr->AddArenaTeam(arenaTeam);
        LOG_DEBUG("network", "PetitonsHandler: Arena team (guid: {}) added to ObjectMgr", arenaTeam->GetId());

        // Add members
        if (signs)
            for (SignatureMap::const_iterator itr = signatureCopy.begin(); itr != signatureCopy.end(); ++itr)
            {
                LOG_DEBUG("network", "PetitionsHandler: Adding arena team (guid: {}) member {}", arenaTeam->GetId(), itr->first.ToString());
                arenaTeam->AddMember(itr->first);
            }
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PETITION_BY_ID);
    stmt->SetData(0, petition->petitionId);
    trans->Append(stmt);

    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_PETITION_SIGNATURE_BY_ID);
    stmt->SetData(0, petition->petitionId);
    trans->Append(stmt);

    CharacterDatabase.CommitTransaction(trans);

    // xinef: clear petition store (petition and signatures)
    sPetitionMgr->RemovePetition(petitionGuid);

    // created
    LOG_DEBUG("network", "TURN IN PETITION {}", petitionGuid.ToString());

    SendTurnInPetitionResult(this, PETITION_TURN_OK);
}

void WorldSession::HandlePetitionShowListOpcode(WorldPacket& recvData)
{
    LOG_DEBUG("network", "Received CMSG_PETITION_SHOW_LIST");

    // 3.4.3 (writer FUN_14075f2f0): PackedGuid128 PetitionUnit
    ObjectGuid guid = Guid128::Read(recvData);

    SendPetitionShowList(guid);
}

void WorldSession::SendPetitionShowList(ObjectGuid guid)
{
    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_PETITIONER);
    if (!creature)
    {
        LOG_DEBUG("network", "WORLD: HandlePetitionShowListOpcode - Unit ({}) not found or you can't interact with him.", guid.ToString());
        return;
    }

    // SMSG_PETITION_SHOW_LIST, 3.4.3 (FUN_1406c7250, handler FUN_141bb6150): PackedGuid128 Unit, u32 count, {u32 Index
    // (echoed by CMSG_PETITION_BUY), u32 Cost, u32 CharterEntry, u32 IsArena (0 the guild registrar frame), u32
    // ArenaType / required signatures}
    WorldPacket data(SMSG_PETITION_SHOW_LIST, 18 + 4 + 3 * 20);
    Guid128::Write(data, guid);

    auto writeEntry = [&](uint32 index, uint32 cost, uint32 entry, uint32 isArena, uint32 arenaType)
    {
        data << uint32(index) << uint32(cost) << uint32(entry) << uint32(isArena) << uint32(arenaType);
    };

    // For guild default
    uint32 CharterEntry = GUILD_CHARTER;
    uint32 CharterDispayID = CHARTER_DISPLAY_ID;
    uint32 CharterCost = sWorld->getIntConfig(CONFIG_CHARTER_COST_GUILD);

    if (creature->IsTabardDesigner())
    {
        sScriptMgr->OnPlayerPetitionShowList(_player, creature, CharterEntry, CharterDispayID, CharterCost);
        data << uint32(1);
        writeEntry(1, CharterCost, CharterEntry, 0, sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS));
    }
    else
    {
        data << uint32(3);
        uint32 const charters[3] = { ARENA_TEAM_CHARTER_2v2, ARENA_TEAM_CHARTER_3v3, ARENA_TEAM_CHARTER_5v5 };
        uint32 const costs[3] = { sWorld->getIntConfig(CONFIG_CHARTER_COST_ARENA_2v2), sWorld->getIntConfig(CONFIG_CHARTER_COST_ARENA_3v3), sWorld->getIntConfig(CONFIG_CHARTER_COST_ARENA_5v5) };
        uint32 const types[3] = { 2, 3, 5 };
        for (uint8 i = 0; i < 3; ++i)
        {
            CharterEntry = charters[i];
            CharterDispayID = CHARTER_DISPLAY_ID;
            CharterCost = costs[i];
            sScriptMgr->OnPlayerPetitionShowList(_player, creature, CharterEntry, CharterDispayID, CharterCost);
            writeEntry(i + 1, CharterCost, CharterEntry, 1, types[i]);
        }
    }

    SendPacket(&data);
    LOG_DEBUG("network", "Sent SMSG_PETITION_SHOW_LIST");
}
