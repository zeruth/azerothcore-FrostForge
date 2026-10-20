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

#include "Guild.h"
#include "GuildMgr.h"
#include "GuildPackets.h"
#include "Log.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldSession.h"

void WorldSession::HandleGuildQueryOpcode(WorldPackets::Guild::QueryGuildInfo& query)
{
    LOG_DEBUG("guild", "CMSG_QUERY_GUILD_INFO [{}]: Guild: {}", GetPlayerInfo(), query.GuildId);
    if (Guild* guild = query.GuildId ? sGuildMgr->GetGuildById(query.GuildId) : nullptr)
    {
        guild->HandleQuery(this);
        return;
    }

    // unknown: the client caches "no such guild" for that GUID
    WorldPackets::Guild::QueryGuildInfoResponse response;
    response.HasGuildInfo = false;
    response.GuildGuidLow = query.GuildGuidLow;
    response.GuildGuidHigh = query.GuildGuidHigh;
    SendPacket(response.Write());
}

void WorldSession::HandleGuildInviteOpcode(WorldPackets::Guild::GuildInviteByName& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_INVITE_BY_NAME [{}]: Invited: {}", GetPlayerInfo(), packet.Name);
    if (normalizePlayerName(packet.Name))
        if (Guild* guild = GetPlayer()->GetGuild())
            guild->HandleInviteMember(this, packet.Name);
}

void WorldSession::HandleGuildRemoveOpcode(WorldPackets::Guild::GuildOfficerRemoveMember& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_OFFICER_REMOVE_MEMBER [{}]: Target: {}", GetPlayerInfo(), packet.Removee.ToString());

    if (Guild* guild = GetPlayer()->GetGuild())
        if (Guild::Member const* member = guild->GetMember(packet.Removee))
            guild->HandleRemoveMember(this, member->GetName());
}

void WorldSession::HandleGuildAcceptOpcode(WorldPackets::Guild::AcceptGuildInvite& /*invite*/)
{
    LOG_DEBUG("guild", "CMSG_ACCEPT_GUILD_INVITE [{}]", GetPlayer()->GetName());

    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_GUILD) && IsTrialAccount())
    {
        Guild::SendCommandResult(this, GUILD_COMMAND_INVITE, ERR_GUILD_PERMISSIONS);
        GetPlayer()->SetGuildIdInvited(0);
        GetPlayer()->SetInGuild(0);
        return;
    }

    if (!GetPlayer()->GetGuildId())
        if (Guild* guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildIdInvited()))
            guild->HandleAcceptMember(this);
}

void WorldSession::HandleGuildDeclineOpcode(WorldPackets::Guild::GuildDeclineInvitation& /*decline*/)
{
    LOG_DEBUG("guild", "CMSG_GUILD_DECLINE_INVITATION [{}]", GetPlayerInfo());

    if (GetPlayer()->GetGuild())
    {
        return;
    }

    // SMSG_GUILD_INVITE_DECLINED to the inviter is not sent: AC does not remember who invited
    GetPlayer()->SetGuildIdInvited(0);
    GetPlayer()->SetInGuild(0);
}

void WorldSession::HandleGuildGetRanksOpcode(WorldPackets::Guild::GuildGetRanks& packet)
{
    // CMSG_GUILD_GET_RANKS (sent with every roster request): the player's own guild only
    if (Guild* guild = GetPlayer()->GetGuild())
        if (!packet.GuildId || packet.GuildId == guild->GetId())
            guild->SendRanks(this);
}

void WorldSession::HandleGuildRosterOpcode(WorldPackets::Guild::GuildGetRoster& /*packet*/)
{
    LOG_DEBUG("guild", "CMSG_GUILD_GET_ROSTER [{}]", GetPlayerInfo());

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->HandleRoster(this);
    else
        Guild::SendCommandResult(this, GUILD_COMMAND_ROSTER, ERR_GUILD_PLAYER_NOT_IN_GUILD);
}

void WorldSession::HandleGuildPromoteOpcode(WorldPackets::Guild::GuildPromoteMember& promote)
{
    LOG_DEBUG("guild", "CMSG_GUILD_PROMOTE_MEMBER [{}]: Target: {}", GetPlayerInfo(), promote.Promotee.ToString());

    if (Guild* guild = GetPlayer()->GetGuild())
        if (Guild::Member const* member = guild->GetMember(promote.Promotee))
            guild->HandleUpdateMemberRank(this, member->GetName(), false);
}

void WorldSession::HandleGuildDemoteOpcode(WorldPackets::Guild::GuildDemoteMember& demote)
{
    LOG_DEBUG("guild", "CMSG_GUILD_DEMOTE_MEMBER [{}]: Target: {}", GetPlayerInfo(), demote.Demotee.ToString());

    if (Guild* guild = GetPlayer()->GetGuild())
        if (Guild::Member const* member = guild->GetMember(demote.Demotee))
            guild->HandleUpdateMemberRank(this, member->GetName(), true);
}

void WorldSession::HandleGuildLeaveOpcode(WorldPackets::Guild::GuildLeave& /*leave*/)
{
    LOG_DEBUG("guild", "CMSG_GUILD_LEAVE [{}]", GetPlayerInfo());

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->HandleLeaveMember(this);
}

void WorldSession::HandleGuildDisbandOpcode(WorldPackets::Guild::GuildDelete& /*packet*/)
{
    LOG_DEBUG("guild", "CMSG_GUILD_DELETE [{}]", GetPlayerInfo());

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->HandleDisband(this);
}

void WorldSession::HandleGuildLeaderOpcode(WorldPackets::Guild::GuildSetGuildMaster& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_SET_GUILD_MASTER [{}]: Target: {}", GetPlayerInfo(), packet.NewMasterName);

    if (normalizePlayerName(packet.NewMasterName))
        if (Guild* guild = GetPlayer()->GetGuild())
            guild->HandleSetLeader(this, packet.NewMasterName);
}

void WorldSession::HandleGuildMOTDOpcode(WorldPackets::Guild::GuildUpdateMotdText& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_UPDATE_MOTD_TEXT [{}]: MOTD: {}", GetPlayerInfo(), packet.MotdText);

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->HandleSetMOTD(this, packet.MotdText);
}

void WorldSession::HandleGuildSetPublicNoteOpcode(WorldPackets::Guild::GuildSetMemberNote& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_SET_MEMBER_NOTE [{}]: Target: {}, Note: {}, public {}", GetPlayerInfo(), packet.NoteeGUID.ToString(), packet.Note, packet.IsPublic);

    // 3.4.3: one message for both notes
    if (Guild* guild = GetPlayer()->GetGuild())
        if (Guild::Member const* member = guild->GetMember(packet.NoteeGUID))
            guild->HandleSetMemberNote(this, member->GetName(), packet.Note, packet.IsPublic);
}

void WorldSession::HandleGuildRankOpcode(WorldPackets::Guild::GuildSetRankPermissions& packet)
{
    Guild* guild = GetPlayer()->GetGuild();
    if (!guild)
    {
        return;
    }

    std::array<GuildBankRightsAndSlots, GUILD_BANK_MAX_TABS> rightsAndSlots;

    for (uint8 tabId = 0; tabId < GUILD_BANK_MAX_TABS; ++tabId)
    {
        // For some reason the client is sending - 1 for guildmaster tab withdraw item limit, that's ilegal for us because we expect unsigned int.
        // Probably core handling for this should be changed.
        if (packet.TabWithdrawItemLimit[tabId] <= 0)
        {
            packet.TabWithdrawItemLimit[tabId] = uint32(GUILD_WITHDRAW_SLOT_UNLIMITED);
        }
        rightsAndSlots[tabId] = GuildBankRightsAndSlots(tabId, uint8(packet.TabFlags[tabId]), packet.TabWithdrawItemLimit[tabId]);
    }

    LOG_DEBUG("guild", "CMSG_GUILD_SET_RANK_PERMISSIONS [{}]: Rank: {} ({})", GetPlayerInfo(), packet.RankName, packet.RankID);

    // Same as the issue above but this time with rights.
    if (packet.Flags <= 0)
    {
        packet.Flags = GUILD_BANK_RIGHT_FULL;
    }

    guild->HandleSetRankInfo(this, packet.RankID, packet.RankName, packet.Flags, packet.WithdrawGoldLimit, rightsAndSlots);
}

void WorldSession::HandleGuildAddRankOpcode(WorldPackets::Guild::GuildAddRank& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_ADD_RANK [{}]: Rank: {}", GetPlayerInfo(), packet.Name);

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->HandleAddNewRank(this, packet.Name);
}

void WorldSession::HandleGuildDelRankOpcode(WorldPackets::Guild::GuildDeleteRank& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_DELETE_RANK [{}]: {}", GetPlayerInfo(), packet.RankOrder);

    // 3.4.3 names the rank (AC's rank ids are their order)
    if (Guild* guild = GetPlayer()->GetGuild())
        guild->HandleRemoveRank(this, uint8(packet.RankOrder));
}

void WorldSession::HandleGuildChangeInfoTextOpcode(WorldPackets::Guild::GuildUpdateInfoText& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_UPDATE_INFO_TEXT [{}]: {}", GetPlayerInfo(), packet.InfoText);

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->HandleSetInfo(this, packet.InfoText);
}

void WorldSession::HandleSaveGuildEmblemOpcode(WorldPackets::Guild::SaveGuildEmblem& packet)
{
    EmblemInfo emblemInfo;
    emblemInfo.ReadPacket(packet);

    LOG_DEBUG("guild", "CMSG_SAVE_GUILD_EMBLEM [{}]: Guid: [{}] Style: {}, Color: {}, BorderStyle: {}, BorderColor: {}, BackgroundColor: {}"
                   , GetPlayerInfo(), packet.Vendor.ToString(), emblemInfo.GetStyle()
                   , emblemInfo.GetColor(), emblemInfo.GetBorderStyle()
                   , emblemInfo.GetBorderColor(), emblemInfo.GetBackgroundColor());
    if (GetPlayer()->GetNPCIfCanInteractWith(packet.Vendor, UNIT_NPC_FLAG_TABARDDESIGNER))
    {
        // Remove fake death
        if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
            GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

        if (Guild* guild = GetPlayer()->GetGuild())
            guild->HandleSetEmblem(this, emblemInfo);
        else
            Guild::SendSaveEmblemResult(this, ERR_GUILDEMBLEM_NOGUILD); // "You are not part of a guild!";
    }
    else
        Guild::SendSaveEmblemResult(this, ERR_GUILDEMBLEM_INVALIDVENDOR); // "That's not an emblem vendor!"
}

void WorldSession::HandleGuildEventLogQueryOpcode(WorldPackets::Guild::GuildEventLogQuery& /*packet*/)
{
    LOG_DEBUG("guild", "CMSG_GUILD_EVENT_LOG_QUERY [{}]", GetPlayerInfo());

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->SendEventLog(this);
}

void WorldSession::HandleGuildBankMoneyWithdrawn(WorldPackets::Guild::GuildBankRemainingWithdrawMoneyQuery& /*packet*/)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_REMAINING_WITHDRAW_MONEY_QUERY [{}]", GetPlayerInfo());

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->SendMoneyInfo(this);
}

void WorldSession::HandleGuildPermissions(WorldPackets::Guild::GuildPermissionsQuery& /* packet */)
{
    if (Guild* guild = GetPlayer()->GetGuild())
        guild->SendPermissions(this);
}

// Called when clicking on Guild bank gameobject
void WorldSession::HandleGuildBankerActivate(WorldPackets::Guild::GuildBankActivate& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_ACTIVATE [{}]: Go: [{}] AllSlots: {}"
    , GetPlayerInfo(), packet.Banker.ToString(), packet.FullUpdate);

    GameObject const* const go = GetPlayer()->GetGameObjectIfCanInteractWith(packet.Banker, GAMEOBJECT_TYPE_GUILD_BANK);
    if (!go)
        return;

    Guild* const guild = GetPlayer()->GetGuild();
    if (!guild)
    {
        Guild::SendCommandResult(this, GUILD_COMMAND_VIEW_TAB, ERR_GUILD_PLAYER_NOT_IN_GUILD);
        return;
    }

    guild->SendBankTabsInfo(this, packet.FullUpdate);
}

// Called when opening guild bank tab only
void WorldSession::HandleGuildBankQueryTab(WorldPackets::Guild::GuildBankQueryTab& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_QUERY_TAB [{}]: Go: [{}], TabId: {}, ShowTabs: {}"
    , GetPlayerInfo(), packet.Banker.ToString(), packet.Tab, packet.FullUpdate);

    if (GetPlayer()->GetGameObjectIfCanInteractWith(packet.Banker, GAMEOBJECT_TYPE_GUILD_BANK))
        if (Guild* guild = GetPlayer()->GetGuild())
            guild->SendBankTabData(this, packet.Tab, packet.FullUpdate);
}

void WorldSession::HandleGuildBankDepositMoney(WorldPackets::Guild::GuildBankDepositMoney& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_DEPOSIT_MONEY [{}]: Go: [{}], money: {}",
              GetPlayerInfo(), packet.Banker.ToString(), packet.Money);

    if (GetPlayer()->GetGameObjectIfCanInteractWith(packet.Banker, GAMEOBJECT_TYPE_GUILD_BANK))
        if (packet.Money && GetPlayer()->HasEnoughMoney(packet.Money))
            if (Guild* guild = GetPlayer()->GetGuild())
                guild->HandleMemberDepositMoney(this, packet.Money);
}

void WorldSession::HandleGuildBankWithdrawMoney(WorldPackets::Guild::GuildBankWithdrawMoney& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_WITHDRAW_MONEY [{}]: Go: [{}], money: {}",
                   GetPlayerInfo(), packet.Banker.ToString(), packet.Money);
    if (packet.Money && GetPlayer()->GetGameObjectIfCanInteractWith(packet.Banker, GAMEOBJECT_TYPE_GUILD_BANK))
        if (Guild* guild = GetPlayer()->GetGuild())
            guild->HandleMemberWithdrawMoney(this, packet.Money);
}

void WorldSession::HandleGuildBankBuyTab(WorldPackets::Guild::GuildBankBuyTab& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_BUY_TAB [{}]: [{}[, TabId: {}", GetPlayerInfo(), packet.Banker .ToString(), packet.BankTab);

    if (GetPlayer()->GetGameObjectIfCanInteractWith(packet.Banker, GAMEOBJECT_TYPE_GUILD_BANK))
        if (Guild* guild = GetPlayer()->GetGuild())
            guild->HandleBuyBankTab(this, packet.BankTab);
}

void WorldSession::HandleGuildBankUpdateTab(WorldPackets::Guild::GuildBankUpdateTab& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_UPDATE_TAB [{}]: Go: [{}], TabId: {}, Name: {}, Icon: {}"
    , GetPlayerInfo(), packet.Banker.ToString(), packet.BankTab, packet.Name, packet.Icon);

    if (!packet.Name.empty() && !packet.Icon.empty())
        if (GetPlayer()->GetGameObjectIfCanInteractWith(packet.Banker, GAMEOBJECT_TYPE_GUILD_BANK))
            if (Guild* guild = GetPlayer()->GetGuild())
                guild->HandleSetBankTabInfo(this, packet.BankTab, packet.Name, packet.Icon);
}

void WorldSession::HandleGuildBankLogQuery(WorldPackets::Guild::GuildBankLogQuery& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_LOG_QUERY [{}]: TabId: {}", GetPlayerInfo(), packet.Tab);

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->SendBankLog(this, packet.Tab);
}

void WorldSession::HandleQueryGuildBankTabText(WorldPackets::Guild::GuildBankTextQuery& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_TEXT_QUERY [{}]: TabId: {}", GetPlayerInfo(), packet.Tab);

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->SendBankTabText(this, packet.Tab);
}

void WorldSession::HandleSetGuildBankTabText(WorldPackets::Guild::GuildBankSetTabText& packet)
{
    LOG_DEBUG("guild", "CMSG_GUILD_BANK_SET_TAB_TEXT [{}]: TabId: {}, Text: {}", GetPlayerInfo(), packet.Tab, packet.TabText);

    if (Guild* guild = GetPlayer()->GetGuild())
        guild->SetBankTabText(packet.Tab, packet.TabText);
}

/// The 3.4.3 client splits 3.3.5's CMSG_GUILD_BANK_SWAP_ITEMS into eleven messages (spec_guild.md 6.3), each led by
/// PackedGuid128 Banker. Bank to bank: u8 tab, u8 slot, u8 tab1, u8 slot1, [u32 StackCount] (merge and split); the
/// first pair is the source. Bank and inventory: u8 BankTab, u8 BankSlot, u8 ContainerItemSlot, [u32 StackCount],
/// bit HasContainerSlot, flush, [u8 ContainerSlot] (InvSlots numbering; none: the backpack).
void WorldSession::HandleGuildBankMoveItem(WorldPacket& recvData)
{
    ObjectGuid banker = Guid128::Read(recvData);
    uint16 opcode = recvData.GetOpcode();

    if (!GetPlayer()->GetGameObjectIfCanInteractWith(banker, GAMEOBJECT_TYPE_GUILD_BANK))
    {
        recvData.rfinish();
        return;
    }

    Guild* guild = GetPlayer()->GetGuild();
    if (!guild)
    {
        recvData.rfinish();
        return;
    }

    switch (opcode)
    {
        case CMSG_SWAP_GUILD_BANK_ITEM_WITH_GUILD_BANK_ITEM:
        case CMSG_MOVE_GUILD_BANK_ITEM:
        case CMSG_MERGE_GUILD_BANK_ITEM_WITH_GUILD_BANK_ITEM:
        case CMSG_SPLIT_GUILD_BANK_ITEM:
        {
            uint8 tab = recvData.read<uint8>();
            uint8 slot = recvData.read<uint8>();
            uint8 tab1 = recvData.read<uint8>();
            uint8 slot1 = recvData.read<uint8>();
            uint32 count = 0;
            if (opcode == CMSG_MERGE_GUILD_BANK_ITEM_WITH_GUILD_BANK_ITEM || opcode == CMSG_SPLIT_GUILD_BANK_ITEM)
                count = recvData.read<uint32>();
            guild->SwapItems(GetPlayer(), tab, slot, tab1, slot1, count);
            return;
        }
        case CMSG_AUTO_STORE_GUILD_BANK_ITEM:
        {
            uint8 tab = recvData.read<uint8>();
            uint8 slot = recvData.read<uint8>();
            guild->SwapItemsWithInventory(GetPlayer(), true, tab, slot, NULL_BAG, NULL_SLOT, 0);
            return;
        }
        default:
            break;
    }

    uint8 tab = recvData.read<uint8>();
    uint8 slot = recvData.read<uint8>();
    uint8 containerItemSlot = recvData.read<uint8>();
    uint32 count = 0;
    bool toChar = false;
    switch (opcode)
    {
        case CMSG_MERGE_ITEM_WITH_GUILD_BANK_ITEM:
        case CMSG_SPLIT_ITEM_TO_GUILD_BANK:
            count = recvData.read<uint32>();
            break;
        case CMSG_MERGE_GUILD_BANK_ITEM_WITH_ITEM:
        case CMSG_SPLIT_GUILD_BANK_ITEM_TO_INVENTORY:
            count = recvData.read<uint32>();
            toChar = true;
            break;
        case CMSG_STORE_GUILD_BANK_ITEM:
            toChar = true;
            break;
        default:                                            // AUTO_GUILD_BANK_ITEM, SWAP_ITEM_WITH_GUILD_BANK_ITEM
            break;
    }
    bool hasContainer = recvData.ReadBit();
    recvData.ResetBitReader();
    uint8 containerSlot = hasContainer ? recvData.read<uint8>() : uint8(0xFF);

    uint8 playerBag, playerSlot;
    ServerItemPos(containerSlot, containerItemSlot, playerBag, playerSlot);

    // Player <-> Bank. Allow to work with inventory only
    if (!Player::IsInventoryPos(playerBag, playerSlot))
        GetPlayer()->SendEquipError(EQUIP_ERR_NONE, nullptr);
    else
        guild->SwapItemsWithInventory(GetPlayer(), toChar, tab, slot, playerBag, playerSlot, count);
}
