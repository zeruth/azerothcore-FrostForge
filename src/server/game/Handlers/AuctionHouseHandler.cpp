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

#include "AuctionHouseMgr.h"
#include "AuctionHouseSearcher.h"
#include "Chat.h"
#include "GameTime.h"
#include "Language.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ItemPackets.h"
#include "PacketGuid128.h"

namespace
{
    /// AddOnInfo ("TaintedBy"), 3.4.3 (writer FUN_14074b660): 10-bit name and version lengths (counting the NUL), bits
    /// Loaded, Disabled, flush, the strings; a length of 1 writes no bytes. Present when the CMSG's HasTaintedBy is set.
    void SkipAddOnInfo(WorldPacket& data)
    {
        uint32 nameLength = data.ReadBits(10);
        uint32 versionLength = data.ReadBits(10);
        data.ReadBit();
        data.ReadBit();
        data.ResetBitReader();
        if (nameLength > 1)
            data.read_skip(nameLength);
        if (versionLength > 1)
            data.read_skip(versionLength);
    }

    /// The bit group of most auction CMSGs: bit HasTaintedBy, flush, [AddOnInfo]
    void SkipTaintedBy(WorldPacket& data)
    {
        bool tainted = data.ReadBit();
        data.ResetBitReader();
        if (tainted)
            SkipAddOnInfo(data);
    }
}

//void called when player click on auctioneer npc
void WorldSession::HandleAuctionHelloOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075bc60): PackedGuid128 Auctioneer
    ObjectGuid guid = Guid128::Read(recvData);

    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER);
    if (!unit)
    {
        LOG_DEBUG("network", "WORLD: HandleAuctionHelloOpcode - Unit ({}) not found or you can't interact with him.", guid.ToString());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    SendAuctionHello(guid, unit);
}

//this void causes that auction window is opened
void WorldSession::SendAuctionHello(ObjectGuid guid, Creature* unit)
{
    if (GetPlayer()->GetLevel() < sWorld->getIntConfig(CONFIG_AUCTION_LEVEL_REQ))
    {
        ChatHandler(this).SendNotification(LANG_AUCTION_REQ, sWorld->getIntConfig(CONFIG_AUCTION_LEVEL_REQ));
        return;
    }

    if (!sScriptMgr->CanSendAuctionHello(this, guid, unit))
        return;

    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(unit->GetFaction());
    if (!ahEntry)
        return;

    // SMSG_AUCTION_HELLO_RESPONSE, 3.4.3 (FUN_1406b7c30, handler FUN_141c7c410): PackedGuid128 Auctioneer, u32
    // PurchasedItemDeliveryDelay, u32 CancelledItemDeliveryDelay, u32 AuctionHouseID (the outbid tracking keys on
    // it), bit OpenForBusiness, flush. The handler opens the auction interaction itself.
    WorldPacket data(SMSG_AUCTION_HELLO_RESPONSE, 18 + 12 + 1);
    Guid128::Write(data, guid);
    data << uint32(sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY));
    data << uint32(0);
    data << uint32(ahEntry->houseId);
    data.WriteBit(true);
    data.FlushBits();
    SendPacket(&data);
}

//call this method when player bids, creates, or deletes auction
/// SMSG_AUCTION_COMMAND_RESULT, 3.4.3 (FUN_1406b7890, handler FUN_141c7bf50): u32 AuctionID, u32 Command (AuctionAction),
/// u32 ErrorCode (AuctionError), u32 BagResult, PackedGuid128 Guid (the bidder on ERR_HIGHER_BID), u64 BidAmount, u64
/// MinIncrement, u32 DesiredDelay; every field always present. A multi-stack post sends its next stack only after a
/// successful sell result, so every request is answered.
void WorldSession::SendAuctionCommandResult(uint32 auctionId, uint32 Action, uint32 ErrorCode, uint32 bidError, ObjectGuid bidder, uint64 bidAmount)
{
    WorldPacket data(SMSG_AUCTION_COMMAND_RESULT, 16 + 18 + 16 + 4);
    data << uint32(auctionId);
    data << uint32(Action);
    data << uint32(ErrorCode);
    data << uint32(ErrorCode == ERR_AUCTION_INVENTORY ? ClientInventoryResult(InventoryResult(bidError)) : 0);
    Guid128::Write(data, bidder);
    data << uint64(bidAmount);
    data << uint64(ErrorCode == ERR_AUCTION_OK || ErrorCode == ERR_AUCTION_HIGHER_BID ? bidError : 0);    // MinIncrement
    data << uint32(AUCTION_SEARCH_DELAY);
    SendPacket(&data);
}

//this function sends notification, if bidder is online
/// 3.4.3 splits the bidder notification. SMSG_AUCTION_OUTBID_NOTIFICATION (FUN_1406b8240, handler FUN_141c7dcd0): u32
/// AuctionHouseID, u32 AuctionID, PackedGuid128 Bidder (the new high bidder), ItemInstance, u64 BidAmount, u64
/// MinIncrement. SMSG_AUCTION_WON_NOTIFICATION (FUN_1406b85b0, handler FUN_141c7f1b0): u32 AuctionHouseID, u32
/// AuctionID, PackedGuid128 Bidder, ItemInstance. AC passes no bid for a won auction.
void WorldSession::SendAuctionBidderNotification(uint32 location, uint32 auctionId, ObjectGuid bidder, uint32 bidSum, uint32 diff, uint32 item_template)
{
    bool won = bidSum == 0;
    WorldPacket data(won ? SMSG_AUCTION_WON_NOTIFICATION : SMSG_AUCTION_OUTBID_NOTIFICATION, 8 + 18 + 40 + 16);
    data << uint32(location);
    data << uint32(auctionId);
    Guid128::Write(data, bidder);
    WorldPackets::Item::WriteItemInstance(data, item_template);
    if (!won)
    {
        data << uint64(bidSum);
        data << uint64(diff);
    }
    SendPacket(&data);
}

//this void causes on client to display: "Your auction sold"
/// SMSG_AUCTION_CLOSED_NOTIFICATION, 3.4.3 (FUN_1406b7770, handler FUN_141c7db90): u32 AuctionID, u64 BidAmount,
/// ItemInstance, f32 ProceedsMailDelay (s), bit Sold, flush. Sold shows "sold" and starts the mail timer, otherwise
/// "expired".
void WorldSession::SendAuctionOwnerNotification(AuctionEntry* auction)
{
    bool sold = !auction->bidder.IsEmpty();
    WorldPacket data(SMSG_AUCTION_CLOSED_NOTIFICATION, 4 + 8 + 40 + 5);
    data << uint32(auction->Id);
    data << uint64(auction->bid);
    WorldPackets::Item::WriteItemInstance(data, auction->item_template);
    data << float(sold ? sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY) : 0);
    data.WriteBit(sold);
    data.FlushBits();
    SendPacket(&data);
}

/// SMSG_AUCTION_OWNER_BID_NOTIFICATION, 3.4.3 (FUN_1406b8370, handler FUN_141c7e510): u32 AuctionID, u64 BidAmount,
/// ItemInstance, u64 MinIncrement, PackedGuid128 Bidder. Updates the owner's rows; no chat line.
void WorldSession::SendAuctionOwnerBidNotification(AuctionEntry* auction)
{
    WorldPacket data(SMSG_AUCTION_OWNER_BID_NOTIFICATION, 4 + 8 + 40 + 8 + 18);
    data << uint32(auction->Id);
    data << uint64(auction->bid);
    WorldPackets::Item::WriteItemInstance(data, auction->item_template);
    data << uint64(AuctionEntry::CalculateAuctionOutBid(auction->bid));
    Guid128::Write(data, auction->bidder);
    SendPacket(&data);
}

//this void creates new auction and adds auction to some auctionhouse
void WorldSession::HandleAuctionSellItem(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075c070, Lua PostAuction): PackedGuid128 Auctioneer, u64 MinBid, u64 BuyoutPrice, u32 RunTime
    // (minutes), bit HasTaintedBy, 6-bit item count, flush, [AddOnInfo], count x {PackedGuid128 item, u32 UseCount}
    ObjectGuid auctioneer = Guid128::Read(recvData);
    uint64 bid64 = recvData.read<uint64>();
    uint64 buyout64 = recvData.read<uint64>();
    uint32 etime = recvData.read<uint32>();
    bool tainted = recvData.ReadBit();
    uint32 itemsCount = recvData.ReadBits(6);
    recvData.ResetBitReader();
    if (tainted)
        SkipAddOnInfo(recvData);

    ObjectGuid itemGUIDs[MAX_AUCTION_ITEMS]; // 160 slot = 4x 36 slot bag + backpack 16 slot
    uint32 count[MAX_AUCTION_ITEMS];
    memset(count, 0, sizeof(count));

    if (itemsCount > MAX_AUCTION_ITEMS)
    {
        SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
        recvData.rfinish();
        return;
    }

    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_AUCTION) && IsTrialAccount())
    {
        SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_RESTRICTED_ACCOUNT);
        recvData.rfinish();
        return;
    }

    for (uint32 i = 0; i < itemsCount; ++i)
    {
        itemGUIDs[i] = Guid128::Read(recvData);
        recvData >> count[i];

        if (!itemGUIDs[i] || !count[i] || count[i] > 1000)
        {
            recvData.rfinish();
            SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_ITEM_NOT_FOUND);
            return;
        }
    }

    if (!bid64 || !etime)
    {
        SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
        return;
    }

    if (bid64 > MAX_MONEY_AMOUNT || buyout64 > MAX_MONEY_AMOUNT)
    {
        LOG_DEBUG("network", "WORLD: HandleAuctionSellItem - Player {} ({}) attempted to sell item with higher price than max gold amount.",
            _player->GetName(), _player->GetGUID().ToString());
        SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
        return;
    }

    uint32 bid = uint32(bid64);
    uint32 buyout = uint32(buyout64);

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(auctioneer, UNIT_NPC_FLAG_AUCTIONEER);
    if (!creature)
    {
        LOG_DEBUG("network", "WORLD: HandleAuctionSellItem - Unit ({}) not found or you can't interact with him.", auctioneer.ToString());
        SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
        return;
    }

    AuctionHouseEntry const* auctionHouseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction());
    if (!auctionHouseEntry)
    {
        LOG_DEBUG("network", "WORLD: HandleAuctionSellItem - Unit ({}) has wrong faction.", auctioneer.ToString());
        return;
    }

    etime *= MINUTE;

    switch (etime)
    {
        case 1*MIN_AUCTION_TIME:
        case 2*MIN_AUCTION_TIME:
        case 4*MIN_AUCTION_TIME:
            break;
        default:
            SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
            return;
    }

    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    Item* items[MAX_AUCTION_ITEMS];

    uint32 finalCount = 0;
    uint32 itemEntry = 0;

    for (uint32 i = 0; i < itemsCount; ++i)
    {
        Item* item = _player->GetItemByGuid(itemGUIDs[i]);

        if (!item)
        {
            SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_ITEM_NOT_FOUND);
            return;
        }

        if (itemEntry == 0)
            itemEntry = item->GetTemplate()->ItemId;

        if (sAuctionMgr->GetAItem(item->GetGUID()) || !item->CanBeTraded() || item->IsNotEmptyBag() ||
                item->GetTemplate()->HasFlag(ITEM_FLAG_CONJURED) || item->GetUInt32Value(ITEM_FIELD_DURATION) ||
                item->GetCount() < count[i] || itemEntry != item->GetTemplate()->ItemId)
        {
            SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
            return;
        }

        items[i] = item;
        finalCount += count[i];
    }

    if (!finalCount)
    {
        SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
        return;
    }

    // check if there are 2 identical guids, in this case user is most likely cheating
    for (uint32 i = 0; i < itemsCount - 1; ++i)
    {
        for (uint32 j = i + 1; j < itemsCount; ++j)
        {
            if (itemGUIDs[i] == itemGUIDs[j])
            {
                SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
                return;
            }
        }
    }

    for (uint32 i = 0; i < itemsCount; ++i)
    {
        Item* item = items[i];

        if (item->GetMaxStackCount() < finalCount)
        {
            SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
            return;
        }
    }

    for (uint32 i = 0; i < itemsCount; ++i)
    {
        Item* item = items[i];

        uint32 auctionTime = uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));
        AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(creature->GetFaction());

        uint32 deposit = sAuctionMgr->GetAuctionDeposit(auctionHouseEntry, etime, item, finalCount);
        if (!_player->HasEnoughMoney(deposit))
        {
            SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_NOT_ENOUGHT_MONEY);
            return;
        }

        _player->ModifyMoney(-int32(deposit));

        AuctionEntry* AH = new AuctionEntry;
        AH->Id = sObjectMgr->GenerateAuctionID();

        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
            AH->houseId = AuctionHouseId::Neutral;
        else
        {
            CreatureData const* auctioneerData = sObjectMgr->GetCreatureData(creature->GetSpawnId());
            if (!auctioneerData)
            {
                LOG_ERROR("network.opcode", "Data for auctioneer not found ({})", auctioneer.ToString());
                delete AH;
                return;
            }

            CreatureTemplate const* auctioneerInfo = sObjectMgr->GetCreatureTemplate(auctioneerData->id);
            if (!auctioneerInfo)
            {
                LOG_ERROR("network.opcode", "Non existing auctioneer ({})", auctioneer.ToString());
                delete AH;
                return;
            }

            AuctionHouseEntry const* AHEntry = sAuctionMgr->GetAuctionHouseEntryFromFactionTemplate(auctioneerInfo->faction);
            AH->houseId = AuctionHouseId(AHEntry->houseId);
        }

        // Required stack size of auction matches to current item stack size, just move item to auctionhouse
        if (itemsCount == 1 && item->GetCount() == count[i])
        {
            AH->item_guid = item->GetGUID();
            AH->item_template = item->GetEntry();
            AH->itemCount = item->GetCount();
            AH->owner = _player->GetGUID();
            AH->startbid = bid;
            AH->bidder = ObjectGuid::Empty;
            AH->bid = 0;
            AH->buyout = buyout;
            AH->expire_time = GameTime::GetGameTime().count() + auctionTime;
            AH->deposit = deposit;
            AH->auctionHouseEntry = auctionHouseEntry;

            LOG_DEBUG("network.opcode", "CMSG_AUCTION_SELL_ITEM: Player {} ({}) is selling item {} entry {} ({}) with count {} with initial bid {} with buyout {} and with time {} (in sec) in auctionhouse {}",
                _player->GetName(), _player->GetGUID().ToString(), item->GetTemplate()->Name1, item->GetEntry(), item->GetGUID().ToString(), item->GetCount(), bid, buyout, auctionTime, AH->GetHouseId());
            sAuctionMgr->AddAItem(item);
            auctionHouse->AddAuction(AH);

            _player->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            item->DeleteFromInventoryDB(trans);
            item->SaveToDB(trans);
            AH->SaveToDB(trans);
            _player->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            SendAuctionCommandResult(AH->Id, AUCTION_SELL_ITEM, ERR_AUCTION_OK);

            if (HasPermission(rbac::RBAC_PERM_LOG_GM_TRADE))
            {
                LOG_GM(GetAccountId(), "GM {} (Account: {}) created auction: {} (Item: {} Count: {}) Bid: {} Buyout: {}",
                    _player->GetName(), GetAccountId(), AH->Id,
                    item->GetTemplate()->Name1, item->GetCount(), bid, buyout);
            }

            GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CREATE_AUCTION, 1);

            LOG_INFO("entities.player.auctionhouse", "AuctionHouse: Account: {} (IP: {}), Player [{}] (GUID: {}) created auction #{}: Item '{}' (Entry: {}) x{}, StartBid: {} copper, Buyout: {} copper, Deposit: {} copper",
                GetAccountId(), GetRemoteAddress(), _player->GetName(), _player->GetGUID().GetCounter(), AH->Id,
                item->GetTemplate()->Name1, item->GetEntry(), item->GetCount(), bid, buyout, deposit);

            return;
        }
        else // Required stack size of auction does not match to current item stack size, clone item and set correct stack size
        {
            Item* newItem = item->CloneItem(finalCount, _player);
            if (!newItem)
            {
                LOG_ERROR("network.opcode", "CMSG_AUCTION_SELL_ITEM: Could not create clone of item {}", item->GetEntry());
                SendAuctionCommandResult(0, AUCTION_SELL_ITEM, ERR_AUCTION_DATABASE_ERROR);
                return;
            }

            AH->item_guid = newItem->GetGUID();
            AH->item_template = newItem->GetEntry();
            AH->itemCount = newItem->GetCount();
            AH->owner = _player->GetGUID();
            AH->startbid = bid;
            AH->bidder = ObjectGuid::Empty;
            AH->bid = 0;
            AH->buyout = buyout;
            AH->expire_time = GameTime::GetGameTime().count() + auctionTime;
            AH->deposit = deposit;
            AH->auctionHouseEntry = auctionHouseEntry;

            LOG_DEBUG("network.opcode", "CMSG_AUCTION_SELL_ITEM: Player {} ({}) is selling item {} entry {} ({}) with count {} with initial bid {} with buyout {} and with time {} (in sec) in auctionhouse {}",
                _player->GetName(), _player->GetGUID().ToString(), newItem->GetTemplate()->Name1, newItem->GetEntry(), newItem->GetGUID().ToString(), newItem->GetCount(), bid, buyout, auctionTime, AH->GetHouseId());
            sAuctionMgr->AddAItem(newItem);
            auctionHouse->AddAuction(AH);

            for (uint32 j = 0; j < itemsCount; ++j)
            {
                Item* item2 = items[j];

                // Item stack count equals required count, ready to delete item - cloned item will be used for auction
                if (item2->GetCount() == count[j])
                {
                    _player->MoveItemFromInventory(item2->GetBagSlot(), item2->GetSlot(), true);

                    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                    item2->DeleteFromInventoryDB(trans);
                    item2->DeleteFromDB(trans);
                    CharacterDatabase.CommitTransaction(trans);
                    delete item2;
                }
                else // Item stack count is bigger than required count, update item stack count and save to database - cloned item will be used for auction
                {
                    item2->SetCount(item2->GetCount() - count[j]);
                    item2->SetState(ITEM_CHANGED, _player);
                    _player->ItemRemovedQuestCheck(item2->GetEntry(), count[j]);
                    item2->SendUpdateToPlayer(_player);
                }
            }

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            newItem->SaveToDB(trans);
            AH->SaveToDB(trans);
            _player->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            SendAuctionCommandResult(AH->Id, AUCTION_SELL_ITEM, ERR_AUCTION_OK);

            if (HasPermission(rbac::RBAC_PERM_LOG_GM_TRADE))
            {
                LOG_GM(GetAccountId(), "GM {} (Account: {}) created auction: {} (Item: {} Count: {}) Bid: {} Buyout: {}",
                    _player->GetName(), GetAccountId(), AH->Id,
                    newItem->GetTemplate()->Name1, newItem->GetCount(), bid, buyout);
            }

            GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CREATE_AUCTION, 1);

            LOG_INFO("entities.player.auctionhouse", "AuctionHouse: Account: {} (IP: {}), Player [{}] (GUID: {}) created auction #{}: Item '{}' (Entry: {}) x{}, StartBid: {} copper, Buyout: {} copper, Deposit: {} copper",
                GetAccountId(), GetRemoteAddress(), _player->GetName(), _player->GetGUID().GetCounter(), AH->Id,
                newItem->GetTemplate()->Name1, newItem->GetEntry(), newItem->GetCount(), bid, buyout, deposit);

            return;
        }
    }
}

//this function is called when client bids or buys out auction
void WorldSession::HandleAuctionPlaceBid(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075be90, Lua PlaceAuctionBid): PackedGuid128 Auctioneer, u32 AuctionID, u64 BidAmount (a buyout
    // is a bid of the buyout price), bit HasTaintedBy, flush, [AddOnInfo]
    ObjectGuid auctioneer = Guid128::Read(recvData);
    uint32 auctionId = recvData.read<uint32>();
    uint64 price64 = recvData.read<uint64>();
    SkipTaintedBy(recvData);

    if (!auctionId || !price64 || price64 > MAX_MONEY_AMOUNT)
    {
        SendAuctionCommandResult(auctionId, AUCTION_PLACE_BID, ERR_AUCTION_DATABASE_ERROR);
        return;                                             //check for cheaters
    }
    uint32 price = uint32(price64);

    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_AUCTION) && IsTrialAccount())
    {
        SendAuctionCommandResult(0, AUCTION_PLACE_BID, ERR_AUCTION_RESTRICTED_ACCOUNT);
        return;
    }

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(auctioneer, UNIT_NPC_FLAG_AUCTIONEER);
    if (!creature)
    {
        LOG_DEBUG("network", "WORLD: HandleAuctionPlaceBid - Unit ({}) not found or you can't interact with him.", auctioneer.ToString());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(creature->GetFaction());

    AuctionEntry* auction = auctionHouse->GetAuction(auctionId);
    Player* player = GetPlayer();

    if (!sScriptMgr->OnPlayerCanPlaceAuctionBid(player, auction))
    {
        SendAuctionCommandResult(0, AUCTION_PLACE_BID, ERR_AUCTION_RESTRICTED_ACCOUNT);
        return;
    }

    if (!auction)
    {
        // auction was cancelled or already bought by someone else
        SendAuctionCommandResult(0, AUCTION_PLACE_BID, ERR_AUCTION_ITEM_NOT_FOUND);
        return;
    }

    if (auction->owner == player->GetGUID())
    {
        //you cannot bid your own auction:
        SendAuctionCommandResult(0, AUCTION_PLACE_BID, ERR_AUCTION_BID_OWN);
        return;
    }

    // impossible have online own another character (use this for speedup check in case online owner)
    Player* auction_owner = ObjectAccessor::FindConnectedPlayer(auction->owner);
    if (!auction_owner && sCharacterCache->GetCharacterAccountIdByGuid(auction->owner) == GetAccountId())
    {
        //you cannot bid your another character auction:
        SendAuctionCommandResult(0, AUCTION_PLACE_BID, ERR_AUCTION_BID_OWN);
        return;
    }

    // someone bid higher meanwhile: the client takes the new bid from the answer
    if (price <= auction->bid || price < auction->startbid)
    {
        SendAuctionCommandResult(auction->Id, AUCTION_PLACE_BID, ERR_AUCTION_HIGHER_BID, AuctionEntry::CalculateAuctionOutBid(auction->bid), auction->bidder, auction->bid);
        return;
    }

    // price too low for next bid if not buyout
    if ((price < auction->buyout || auction->buyout == 0) &&
            price < auction->bid + AuctionEntry::CalculateAuctionOutBid(auction->bid))
    {
        SendAuctionCommandResult(auction->Id, AUCTION_PLACE_BID, ERR_AUCTION_BID_INCREMENT);
        return;
    }

    if (!player->HasEnoughMoney(price))
    {
        SendAuctionCommandResult(auction->Id, AUCTION_PLACE_BID, ERR_AUCTION_NOT_ENOUGHT_MONEY);
        return;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    if (price < auction->buyout || auction->buyout == 0)
    {
        if (auction->bidder)
        {
            if (auction->bidder == player->GetGUID())
                player->ModifyMoney(-int32(price - auction->bid));
            else
            {
                // mail to last bidder and return money
                sAuctionMgr->SendAuctionOutbiddedMail(auction, price, GetPlayer(), trans);
                player->ModifyMoney(-int32(price));
            }
        }
        else
            player->ModifyMoney(-int32(price));

        auction->bidder = player->GetGUID();
        auction->bid = price;

        sAuctionMgr->GetAuctionHouseSearcher()->UpdateBid(auction);

        GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_AUCTION_BID, price);

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AUCTION_BID);
        stmt->SetData(0, auction->bidder.GetCounter());
        stmt->SetData(1, auction->bid);
        stmt->SetData(2, auction->Id);
        trans->Append(stmt);

        SendAuctionCommandResult(auction->Id, AUCTION_PLACE_BID, ERR_AUCTION_OK, AuctionEntry::CalculateAuctionOutBid(auction->bid), auction->bidder, auction->bid);
        if (auction_owner)
            auction_owner->GetSession()->SendAuctionOwnerBidNotification(auction);

        if (HasPermission(rbac::RBAC_PERM_LOG_GM_TRADE))
        {
            LOG_GM(GetAccountId(), "GM {} (Account: {}) bid on auction: {} (Item: {} Count: {}) Bid: {}",
                player->GetName(), GetAccountId(), auction->Id,
                auction->item_template, auction->itemCount, price);
        }

        LOG_INFO("entities.player.auctionhouse", "AuctionHouse: Account: {} (IP: {}), Player [{}] (GUID: {}) placed bid on auction #{}: Item (Entry: {}) x{}, Bid: {} copper, Owner: {} (GUID: {})",
            GetAccountId(), GetRemoteAddress(), player->GetName(), player->GetGUID().GetCounter(),
            auction->Id, auction->item_template, auction->itemCount, price, auction->owner.GetCounter(), auction->owner.GetCounter());
    }
    else
    {
        //buyout:
        if (player->GetGUID() == auction->bidder)
            player->ModifyMoney(-int32(auction->buyout - auction->bid));
        else
        {
            player->ModifyMoney(-int32(auction->buyout));
            if (auction->bidder)                          //buyout for bidded auction ..
                sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, GetPlayer(), trans);
        }
        auction->bidder = player->GetGUID();
        auction->bid = auction->buyout;
        GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_AUCTION_BID, auction->buyout);

        //- Mails must be under transaction control too to prevent data loss
        sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
        sAuctionMgr->SendAuctionWonMail(auction, trans);
        sScriptMgr->OnAuctionSuccessful(auctionHouse, auction);

        SendAuctionCommandResult(auction->Id, AUCTION_PLACE_BID, ERR_AUCTION_OK);

        if (HasPermission(rbac::RBAC_PERM_LOG_GM_TRADE))
        {
            LOG_GM(GetAccountId(), "GM {} (Account: {}) bought out auction: {} (Item: {} Count: {}) Buyout: {}",
                player->GetName(), GetAccountId(), auction->Id,
                auction->item_template, auction->itemCount, auction->buyout);
        }

        LOG_INFO("entities.player.auctionhouse", "AuctionHouse: Account: {} (IP: {}), Player [{}] (GUID: {}) bought out auction #{}: Item (Entry: {}) x{}, Buyout: {} copper, Owner: {} (GUID: {})",
            GetAccountId(), GetRemoteAddress(), player->GetName(), player->GetGUID().GetCounter(),
            auction->Id, auction->item_template, auction->itemCount, auction->buyout, auction->owner.GetCounter(), auction->owner.GetCounter());

        auction->DeleteFromDB(trans);

        sAuctionMgr->RemoveAItem(auction->item_guid);
        auctionHouse->RemoveAuction(auction);
    }
    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
}

//this void is called when auction_owner cancels his auction
void WorldSession::HandleAuctionRemoveItem(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075bf30, Lua CancelAuction): PackedGuid128 Auctioneer, u32 AuctionID, bit HasTaintedBy, flush,
    // [AddOnInfo]
    ObjectGuid auctioneer = Guid128::Read(recvData);
    uint32 auctionId = recvData.read<uint32>();
    SkipTaintedBy(recvData);

    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_AUCTION) && IsTrialAccount())
    {
        SendAuctionCommandResult(0, AUCTION_CANCEL, ERR_AUCTION_RESTRICTED_ACCOUNT);
        return;
    }

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(auctioneer, UNIT_NPC_FLAG_AUCTIONEER);
    if (!creature)
    {
        LOG_DEBUG("network", "WORLD: HandleAuctionRemoveItem - Unit ({}) not found or you can't interact with him.", auctioneer.ToString());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(creature->GetFaction());

    AuctionEntry* auction = auctionHouse->GetAuction(auctionId);
    Player* player = GetPlayer();

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (auction && auction->owner == player->GetGUID())
    {
        Item* pItem = sAuctionMgr->GetAItem(auction->item_guid);
        if (!pItem)
        {
            SendAuctionCommandResult(auctionId, AUCTION_CANCEL, ERR_AUCTION_ITEM_NOT_FOUND);
            return;
        }

        if (auction->bidder)                        // If we have a bidder, we have to send him the money he paid
        {
            uint32 auctionCut = auction->GetAuctionCut();
            if (!player->HasEnoughMoney(auctionCut))
            {
                SendAuctionCommandResult(auctionId, AUCTION_CANCEL, ERR_AUCTION_NOT_ENOUGHT_MONEY);
                return;
            }
            //some auctionBidderNotification would be needed, but don't know that parts..
            sAuctionMgr->SendAuctionCancelledToBidderMail(auction, trans);
            player->ModifyMoney(-int32(auctionCut));
        }

        // item will deleted or added to received mail list
        MailDraft(auction->BuildAuctionMailSubject(AUCTION_CANCELED), AuctionEntry::BuildAuctionMailBody(ObjectGuid::Empty, 0, auction->buyout, auction->deposit))
        .AddItem(pItem)
        .SendMailTo(trans, player, auction, MAIL_CHECK_MASK_COPIED);
    }
    else
    {
        SendAuctionCommandResult(0, AUCTION_CANCEL, ERR_AUCTION_DATABASE_ERROR);
        //this code isn't possible ... maybe there should be assert
        LOG_ERROR("network.opcode", "CHEATER : {}, he tried to cancel auction (id: {}) of another player, or auction is nullptr", player->GetGUID().ToString(), auctionId);
        return;
    }

    //inform player, that auction is removed
    SendAuctionCommandResult(auction->Id, AUCTION_CANCEL, ERR_AUCTION_OK);

    LOG_INFO("entities.player.auctionhouse", "AuctionHouse: Account: {} (IP: {}), Player [{}] (GUID: {}) cancelled auction #{}: Item (Entry: {}) x{}, Buyout: {} copper",
        GetAccountId(), GetRemoteAddress(), player->GetName(), player->GetGUID().GetCounter(),
        auction->Id, auction->item_template, auction->itemCount, auction->buyout);

    // Now remove the auction

    player->SaveInventoryAndGoldToDB(trans);
    auction->DeleteFromDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    sAuctionMgr->RemoveAItem(auction->item_guid);
    auctionHouse->RemoveAuction(auction);
}

//called when player lists his bids
void WorldSession::HandleAuctionListBidderItems(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075bca0): PackedGuid128 Auctioneer, u32 Offset, 7-bit AuctionID count, bit HasTaintedBy (after
    // the count here), flush, [AddOnInfo], the AuctionIDs (the outbid auctions the client remembers)
    ObjectGuid guid = Guid128::Read(recvData);
    recvData.read_skip<uint32>();                          // Offset
    uint32 outbiddedCount = recvData.ReadBits(7);
    bool tainted = recvData.ReadBit();
    recvData.ResetBitReader();
    if (tainted)
        SkipAddOnInfo(recvData);

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER);
    if (!creature)
    {
        LOG_DEBUG("network", "WORLD: HandleAuctionListBidderItems - Unit ({}) not found or you can't interact with him.", guid.ToString());
        recvData.rfinish();
        return;
    }

    // Arbitrary cap, can be adjusted if needed
    if (outbiddedCount > 1000)
        return;

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction());
    if (!ahEntry)
        return;

    AuctionHouseFaction auctionHouseFaction = AuctionHouseMgr::GetAuctionHouseFactionFromHouseId(AuctionHouseId(ahEntry->houseId));

    // Client sends this list, which I'm honestly not entirely sure why?
    std::vector<uint32> auctionIds;
    auctionIds.reserve(outbiddedCount);
    while (outbiddedCount > 0) // add all data, which client requires
    {
        --outbiddedCount;
        uint32 outbiddedAuctionId;
        recvData >> outbiddedAuctionId;
        auctionIds.push_back(outbiddedAuctionId);
    }

    sAuctionMgr->GetAuctionHouseSearcher()->QueueSearchRequest(new AuctionSearchBidderListRequest(auctionHouseFaction, std::move(auctionIds), GetPlayer()->GetGUID()));
}

//this void sends player info about his auctions
void WorldSession::HandleAuctionListOwnerItems(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075bde0): PackedGuid128 Auctioneer, u32 Offset, bit HasTaintedBy, flush, [AddOnInfo]
    ObjectGuid guid = Guid128::Read(recvData);
    recvData.read_skip<uint32>();
    SkipTaintedBy(recvData);

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER);
    if (!creature)
        return;

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction());
    if (!ahEntry)
        return;

    AuctionHouseFaction auctionHouseFaction = AuctionHouseMgr::GetAuctionHouseFactionFromHouseId(AuctionHouseId(ahEntry->houseId));

    sAuctionMgr->GetAuctionHouseSearcher()->QueueSearchRequest(new AuctionSearchOwnerListRequest(auctionHouseFaction, GetPlayer()->GetGUID()));
}

//this void is called when player clicks on search button
void WorldSession::HandleAuctionListItems(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075bda0 -> FUN_140750000, Lua QueryAuctionItems): PackedGuid128 Auctioneer, u32 Offset (page *
    // 50), u8 MinLevel, u8 MaxLevel, u32 Quality (-1 any), u8 sort count, u32 known pets size, u8 MaxPetLevel, the known
    // pets, bit HasTaintedBy, 8-bit name length, flush, the name, 3-bit class filter count, bits OnlyUsable, ExactMatch,
    // flush, [AddOnInfo], the class filters {u32 ItemClass, 5-bit subclass count, flush, {u64 InvTypeMask, u32
    // ItemSubclass}}, u32 sorts size, the sorts {u8 SortType, u8 Reversed}. getAll is CMSG_AUCTION_REPLICATE_ITEMS now.
    ObjectGuid guid = Guid128::Read(recvData);
    uint32 listfrom = recvData.read<uint32>();
    uint8 levelmin = recvData.read<uint8>();
    uint8 levelmax = recvData.read<uint8>();
    uint32 quality = recvData.read<uint32>();
    uint8 sortOrderCount = recvData.read<uint8>();
    uint32 knownPetsSize = recvData.read<uint32>();
    recvData.read_skip<uint8>();                            // MaxPetLevel
    if (knownPetsSize > recvData.size())
    {
        recvData.rfinish();
        return;
    }
    recvData.read_skip(knownPetsSize);
    bool tainted = recvData.ReadBit();
    uint32 nameLength = recvData.ReadBits(8);
    recvData.ResetBitReader();
    std::string searchedname = recvData.ReadString(nameLength);
    uint32 classFilterCount = recvData.ReadBits(3);
    uint8 usable = recvData.ReadBit();
    bool exactMatch = recvData.ReadBit();
    recvData.ResetBitReader();
    if (tainted)
        SkipAddOnInfo(recvData);

    std::vector<AuctionHouseClassFilter> classFilters(classFilterCount);
    for (AuctionHouseClassFilter& filter : classFilters)
    {
        filter.itemClass = recvData.read<uint32>();
        uint32 subClassCount = recvData.ReadBits(5);
        recvData.ResetBitReader();
        filter.subClasses.resize(subClassCount);
        for (AuctionHouseSubClassFilter& sub : filter.subClasses)
        {
            sub.invTypeMask = recvData.read<uint64>();
            sub.itemSubClass = recvData.read<int32>();
        }
    }

    recvData.read_skip<uint32>();                           // sorts size, 2 * count
    AuctionSortOrderVector sortOrder;
    for (uint8 i = 0; i < sortOrderCount; i++)
    {
        uint8 sortMode = recvData.read<uint8>();
        uint8 isDesc = recvData.read<uint8>();
        if (sortMode >= AUCTION_SORT_MAX)                   // 3.4.3's unit price columns: the client sorts its page itself
            continue;
        AuctionSortInfo sortInfo;
        sortInfo.isDesc = (isDesc == 1);
        sortInfo.sortOrder = static_cast<AuctionSortOrder>(sortMode);
        sortOrder.push_back(std::move(sortInfo));
    }
    uint8 getAll = 0;
    uint32 auctionSlotID = 0xFFFFFFFF, auctionMainCategory = 0xFFFFFFFF, auctionSubCategory = 0xFFFFFFFF;

    // converting string that we try to find to lower case
    std::wstring wsearchedname;
    if (!Utf8toWStr(searchedname, wsearchedname))
        return;

    wstrToLower(wsearchedname);

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER);
    if (!creature)
        return;

    // remove fake death
    if (_player->HasUnitState(UNIT_STATE_DIED))
        _player->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction());
    if (!ahEntry)
        return;

    AuctionHouseFaction auctionHouseFaction = AuctionHouseMgr::GetAuctionHouseFactionFromHouseId(AuctionHouseId(ahEntry->houseId));

    AuctionHouseSearchInfo ahSearchInfo;
    ahSearchInfo.wsearchedname = wsearchedname;
    ahSearchInfo.listfrom = listfrom;
    ahSearchInfo.levelmin = levelmin;
    ahSearchInfo.levelmax = levelmax;
    ahSearchInfo.usable = usable;
    ahSearchInfo.inventoryType = auctionSlotID;
    ahSearchInfo.itemClass = auctionMainCategory;
    ahSearchInfo.itemSubClass = auctionSubCategory;
    ahSearchInfo.quality = quality;
    ahSearchInfo.getAll = getAll;
    ahSearchInfo.sorting = std::move(sortOrder);
    ahSearchInfo.classFilters = std::move(classFilters);
    ahSearchInfo.exactMatch = exactMatch;

    AuctionHousePlayerInfo ahPlayerInfo;
    ahPlayerInfo.playerGuid = GetPlayer()->GetGUID();
    ahPlayerInfo.faction = GetPlayer()->GetFaction();
    ahPlayerInfo.loc_idx = GetPlayer()->GetSession()->GetSessionDbLocaleIndex();
    ahPlayerInfo.locdbc_idx = GetPlayer()->GetSession()->GetSessionDbcLocale();
    if (usable)
    {
        AuctionHouseUsablePlayerInfo usablePlayerInfo;
        usablePlayerInfo.classMask = GetPlayer()->getClassMask();
        usablePlayerInfo.raceMask = GetPlayer()->getRaceMask();
        usablePlayerInfo.level = GetPlayer()->GetLevel();

        SkillStatusMap const& skillMap = GetPlayer()->GetSkillStatusMap();
        for (auto const& pair : skillMap)
            usablePlayerInfo.skills.insert(std::make_pair(pair.first, GetPlayer()->GetSkillValue(pair.first)));

        PlayerSpellMap const& spellMap = GetPlayer()->GetSpellMap();
        for (auto const& pair : spellMap)
        {
            if (pair.second->State != PLAYERSPELL_REMOVED && pair.second->IsInSpec(GetPlayer()->GetActiveSpec()))
                usablePlayerInfo.spells.insert(pair.first);
        }
        ahPlayerInfo.usablePlayerInfo = std::move(usablePlayerInfo);
    }

    sAuctionMgr->GetAuctionHouseSearcher()->QueueSearchRequest(new AuctionSearchListRequest(auctionHouseFaction, std::move(ahSearchInfo), std::move(ahPlayerInfo)));
}

void WorldSession::HandleAuctionListPendingSales(WorldPacket& /*recvData*/)
{
    // CMSG_AUCTION_LIST_PENDING_SALES, 3.4.3: empty. SMSG_AUCTION_LIST_PENDING_SALES_RESULT is a mail list (parser
    // FUN_1406b8170): u32 count, i32 total, the mails. Its rows are rebuilt from the auction mail texts [TODO: list the
    // sold auctions' pending mails once the auction mail bodies are in 3.4.3's text form]; the answer releases the
    // client's owner query slot.
    WorldPacket data(SMSG_AUCTION_LIST_PENDING_SALES_RESULT, 8);
    data << uint32(0);
    data << int32(0);
    SendPacket(&data);
}

/// CMSG_AUCTION_REPLICATE_ITEMS, 3.4.3 (writer FUN_14075bfc0, QueryAuctionItems with getAll): PackedGuid128 Auctioneer,
/// u32 ChangeNumberGlobal, ChangeNumberCursor, ChangeNumberTombstone (echoed from the last answer), u32 Count (1024),
/// bit HasTaintedBy, flush, [AddOnInfo]. The whole house in pages; the client asks again while a page is full.
void WorldSession::HandleAuctionReplicateItems(WorldPacket& recvData)
{
    ObjectGuid guid = Guid128::Read(recvData);
    recvData.read_skip<uint32>();                           // ChangeNumberGlobal
    uint32 cursor = recvData.read<uint32>();
    recvData.read_skip<uint32>();                           // ChangeNumberTombstone
    uint32 count = recvData.read<uint32>();
    SkipTaintedBy(recvData);

    Creature* creature = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER);
    AuctionHouseEntry const* ahEntry = creature ? AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction()) : nullptr;
    if (!ahEntry)
    {
        // the browse slot is released by any answer
        WorldPacket data(SMSG_AUCTION_REPLICATE_RESPONSE, 24);
        data << uint32(1) << uint32(AUCTION_SEARCH_DELAY) << uint32(0) << uint32(0) << uint32(0) << uint32(0);
        SendPacket(&data);
        return;
    }

    AuctionHouseSearchInfo ahSearchInfo;
    ahSearchInfo.listfrom = cursor;
    ahSearchInfo.levelmin = 0;
    ahSearchInfo.levelmax = 0;
    ahSearchInfo.usable = false;
    ahSearchInfo.inventoryType = 0xFFFFFFFF;
    ahSearchInfo.itemClass = 0xFFFFFFFF;
    ahSearchInfo.itemSubClass = 0xFFFFFFFF;
    ahSearchInfo.quality = 0xFFFFFFFF;
    ahSearchInfo.getAll = true;
    ahSearchInfo.replicateCount = std::min<uint32>(count ? count : 1024, 1024);

    AuctionHousePlayerInfo ahPlayerInfo;
    ahPlayerInfo.playerGuid = GetPlayer()->GetGUID();
    ahPlayerInfo.faction = GetPlayer()->GetFaction();
    ahPlayerInfo.loc_idx = GetPlayer()->GetSession()->GetSessionDbLocaleIndex();
    ahPlayerInfo.locdbc_idx = GetPlayer()->GetSession()->GetSessionDbcLocale();

    AuctionHouseFaction auctionHouseFaction = AuctionHouseMgr::GetAuctionHouseFactionFromHouseId(AuctionHouseId(ahEntry->houseId));
    sAuctionMgr->GetAuctionHouseSearcher()->QueueSearchRequest(new AuctionSearchListRequest(auctionHouseFaction, std::move(ahSearchInfo), std::move(ahPlayerInfo)));
}
