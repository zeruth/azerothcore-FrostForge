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
#include "Item.h"
#include "Language.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "TC9Sidecar.h"
#include "World.h"
#include "ItemPackets.h"
#include "WorldPacket.h"
#include "WorldSession.h"

void WorldSession::SendTradeStatus(TradeStatusInfo const& info)
{
    // SMSG_TRADE_STATUS, 3.4.3 (inline in the client switch, handler FUN_141bac580): bit
    // PartnerIsSameBnetAccount, 5-bit Status (3.3.5's numbering: 1 the request to the target, 2 opens
    // both windows, 12 a failure), [12: bit FailureForYou], flush, then by status: 12 i32 BagResult (the
    // client's InventoryResult) and i32 ItemID; 2 u32 trade id; 1 PackedGuid128 partner and its account;
    // 22 / 23 u8 trade slot
    WorldPacket data(SMSG_TRADE_STATUS, 1 + 36);
    data.WriteBit(false);
    data.WriteBits(uint32(info.Status), 5);
    if (info.Status == TRADE_STATUS_CLOSE_WINDOW)
        data.WriteBit(!info.IsTargetResult);                // FailureForYou: the problem is the receiver's own
    data.FlushBits();

    switch (info.Status)
    {
        case TRADE_STATUS_BEGIN_TRADE:
            Guid128::Write(data, info.TraderGuid);          // Partner
            Guid128::Write(data, ObjectGuid::Empty);        // PartnerAccount
            break;
        case TRADE_STATUS_OPEN_WINDOW:
            data << uint32(_player->GetTradeData() ? _player->GetTradeData()->GetTradeId() : 0);
            break;
        case TRADE_STATUS_CLOSE_WINDOW:
            data << int32(ClientInventoryResult(info.Result));
            data << int32(info.ItemLimitedByLimitCategory);
            break;
        case TRADE_STATUS_WRONG_REALM:
        case TRADE_STATUS_NOT_ON_TAPLIST:
            data << uint8(info.Slot);                       // Trade slot; 0xFF clears the offered gold
            break;
        default:
            break;
    }

    SendPacket(&data);
}

void WorldSession::HandleIgnoreTradeOpcode(WorldPacket& /*recvPacket*/)
{
    _player->TradeCancel(true, TRADE_STATUS_IGNORE_YOU);
}

void WorldSession::HandleBusyTradeOpcode(WorldPacket& /*recvPacket*/)
{
    _player->TradeCancel(true, TRADE_STATUS_BUSY);
}

void WorldSession::SendUpdateTrade(bool trader_data /*= true*/)
{
    TradeData* view_trade = trader_data ? _player->GetTradeData()->GetTraderData() : _player->GetTradeData();

    // SMSG_TRADE_UPDATED, 3.4.3 (FUN_140706810, items FUN_14071a3b0, handler FUN_141baca90): u8 WhichPlayer
    // (0 the receiver's own offer), u32 trade id, u32 ClientStateIndex (checked on the own side: 1 + the
    // receiver's change requests), u32 CurrentStateIndex, u64 gold, i32 currency type and quantity, i32
    // ProposedEnchantment, u32 item count, per occupied slot {u8 slot, i32 stack count, PackedGuid128
    // gift creator, ItemInstance, bit HasUnwrapped, flush, [enchant, on-use enchant, creator, charges,
    // max durability, durability, 2-bit gem count, bit Lock, flush, gems]}
    TradeData* my_trade = _player->GetTradeData();
    std::vector<std::pair<uint8, Item*>> items;
    for (uint8 i = 0; i < TRADE_SLOT_COUNT; ++i)
        if (Item* item = view_trade->GetItem(TradeSlots(i)))
            items.emplace_back(i, item);

    WorldPacket data(SMSG_TRADE_UPDATED, 40 + items.size() * 80);
    data << uint8(trader_data);
    data << uint32(my_trade->GetTradeId());
    data << uint32(view_trade->GetClientStateIndex());
    data << uint32(view_trade->GetServerStateIndex());
    data << uint64(view_trade->GetMoney());
    data << int32(0);                                       // CurrencyType
    data << int32(0);                                       // CurrencyQuantity
    data << int32(view_trade->GetSpell());                  // ProposedEnchantment
    data << uint32(items.size());

    for (auto const& [slot, item] : items)
    {
        data << uint8(slot);
        data << int32(item->GetCount());
        Guid128::Write(data, item->GetGuidValue(ITEM_FIELD_GIFTCREATOR));
        WorldPackets::Item::WriteItemInstance(data, item->GetEntry(), int32(item->GetItemSuffixFactor()), item->GetItemRandomPropertyId());
        // wrapped: hide stats but show giftcreator name
        bool unwrapped = !item->IsWrapped();
        data.WriteBit(unwrapped);
        data.FlushBits();
        if (unwrapped)
        {
            data << int32(item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT));
            data << int32(item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT));    // OnUseEnchantmentID
            Guid128::Write(data, item->GetGuidValue(ITEM_FIELD_CREATOR));
            data << int32(item->GetSpellCharges());
            data << uint32(item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY));
            data << uint32(item->GetUInt32Value(ITEM_FIELD_DURABILITY));
            data.WriteBits(0, 2);                           // gems
            data.WriteBit(item->GetTemplate()->LockID != 0);
            data.FlushBits();
        }
    }
    SendPacket(&data);
}

//==============================================================
// transfer the items to the players

void WorldSession::moveItems(Item* myItems[], Item* hisItems[])
{
    Player* trader = _player->GetTrader();
    if (!trader)
        return;

    for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
    {
        ItemPosCountVec traderDst;
        ItemPosCountVec playerDst;
        bool traderCanTrade = (!myItems[i] || trader->CanStoreItem(NULL_BAG, NULL_SLOT, traderDst, myItems[i], false) == EQUIP_ERR_OK);
        bool playerCanTrade = (!hisItems[i] || _player->CanStoreItem(NULL_BAG, NULL_SLOT, playerDst, hisItems[i], false) == EQUIP_ERR_OK);
        if (traderCanTrade && playerCanTrade)
        {
            // Ok, if trade item exists and can be stored
            // If we trade in both directions we had to check, if the trade will work before we actually do it
            // A roll back is not possible after we stored it
            if (myItems[i])
            {
                // logging
                LOG_DEBUG("network", "partner storing: {}", myItems[i]->GetGUID().ToString());

                // adjust time (depends on /played)
                if (myItems[i]->IsBOPTradable())
                    myItems[i]->SetUInt32Value(ITEM_FIELD_CREATE_PLAYED_TIME, trader->GetTotalPlayedTime() - (_player->GetTotalPlayedTime() - myItems[i]->GetUInt32Value(ITEM_FIELD_CREATE_PLAYED_TIME)));
                // store
                trader->MoveItemToInventory(traderDst, myItems[i], true, true);
            }
            if (hisItems[i])
            {
                // logging
                LOG_DEBUG("network", "player storing: {}", hisItems[i]->GetGUID().ToString());

                // adjust time (depends on /played)
                if (hisItems[i]->IsBOPTradable())
                    hisItems[i]->SetUInt32Value(ITEM_FIELD_CREATE_PLAYED_TIME, _player->GetTotalPlayedTime() - (trader->GetTotalPlayedTime() - hisItems[i]->GetUInt32Value(ITEM_FIELD_CREATE_PLAYED_TIME)));
                // store
                _player->MoveItemToInventory(playerDst, hisItems[i], true, true);
            }
        }
        else
        {
            // in case of fatal error log error message
            // return the already removed items to the original owner
            if (myItems[i])
            {
                if (!traderCanTrade)
                    LOG_ERROR("network.opcode", "trader can't store item: {}", myItems[i]->GetGUID().ToString());
                if (_player->CanStoreItem(NULL_BAG, NULL_SLOT, playerDst, myItems[i], false) == EQUIP_ERR_OK)
                    _player->MoveItemToInventory(playerDst, myItems[i], true, true);
                else
                    LOG_ERROR("network.opcode", "player can't take item back: {}", myItems[i]->GetGUID().ToString());
            }
            // return the already removed items to the original owner
            if (hisItems[i])
            {
                if (!playerCanTrade)
                    LOG_ERROR("network.opcode", "player can't store item: {}", hisItems[i]->GetGUID().ToString());
                if (trader->CanStoreItem(NULL_BAG, NULL_SLOT, traderDst, hisItems[i], false) == EQUIP_ERR_OK)
                    trader->MoveItemToInventory(traderDst, hisItems[i], true, true);
                else
                    LOG_ERROR("network.opcode", "trader can't take item back: {}", hisItems[i]->GetGUID().ToString());
            }
        }
    }
}

//==============================================================

static void setAcceptTradeMode(TradeData* myTrade, TradeData* hisTrade, Item * *myItems, Item * *hisItems)
{
    myTrade->SetInAcceptProcess(true);
    hisTrade->SetInAcceptProcess(true);

    // store items in local list and set 'in-trade' flag
    for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
    {
        if (Item* item = myTrade->GetItem(TradeSlots(i)))
        {
            LOG_DEBUG("network.opcode", "player trade item {} bag: {} slot: {}", item->GetGUID().ToString(), item->GetBagSlot(), item->GetSlot());
            //Can return nullptr
            myItems[i] = item;
            myItems[i]->SetInTrade();
        }

        if (Item* item = hisTrade->GetItem(TradeSlots(i)))
        {
            LOG_DEBUG("network.opcode", "partner trade item {} bag: {} slot: {}", item->GetGUID().ToString(), item->GetBagSlot(), item->GetSlot());
            hisItems[i] = item;
            hisItems[i]->SetInTrade();
        }
    }
}

static void clearAcceptTradeMode(TradeData* myTrade, TradeData* hisTrade)
{
    myTrade->SetInAcceptProcess(false);
    hisTrade->SetInAcceptProcess(false);
}

static void clearAcceptTradeMode(Item * *myItems, Item * *hisItems)
{
    // clear 'in-trade' flag
    for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
    {
        if (myItems[i])
            myItems[i]->SetInTrade(false);
        if (hisItems[i])
            hisItems[i]->SetInTrade(false);
    }
}

void WorldSession::HandleAcceptTradeOpcode(WorldPacket& /*recvPacket*/)
{
    TradeData* my_trade = _player->m_trade;
    if (!my_trade)
        return;

    Player* trader = my_trade->GetTrader();

    TradeData* his_trade = trader->m_trade;
    if (!his_trade)
        return;

    Item* myItems[TRADE_SLOT_TRADED_COUNT]  = { };
    Item* hisItems[TRADE_SLOT_TRADED_COUNT] = { };

    // set before checks for propertly undo at problems (it already set in to client)
    my_trade->SetAccepted(true);

    TradeStatusInfo info;
    if (!_player->IsWithinDistInMap(trader, TRADE_DISTANCE, false))
    {
        info.Status = TRADE_STATUS_TARGET_TO_FAR;
        SendTradeStatus(info);
        my_trade->SetAccepted(false);
        return;
    }

    // not accept case incorrect money amount
    if (!_player->HasEnoughMoney(my_trade->GetMoney()))
    {
        info.Status = TRADE_STATUS_CLOSE_WINDOW;
        info.Result = EQUIP_ERR_NOT_ENOUGH_MONEY;
        SendTradeStatus(info);
        my_trade->SetAccepted(false, true);
        return;
    }

    // not accept case incorrect money amount
    if (!trader->HasEnoughMoney(his_trade->GetMoney()))
    {
        info.Status = TRADE_STATUS_CLOSE_WINDOW;
        info.Result = EQUIP_ERR_NOT_ENOUGH_MONEY;
        trader->GetSession()->SendTradeStatus(info);
        his_trade->SetAccepted(false, true);
        return;
    }

    if (_player->GetMoney() >= MAX_MONEY_AMOUNT - his_trade->GetMoney())
    {
        info.Status = TRADE_STATUS_CLOSE_WINDOW;
        info.Result = EQUIP_ERR_TOO_MUCH_GOLD;
        SendTradeStatus(info);
        my_trade->SetAccepted(false, true);
        return;
    }

    if (trader->GetMoney() >= MAX_MONEY_AMOUNT - my_trade->GetMoney())
    {
        info.Status = TRADE_STATUS_CLOSE_WINDOW;
        info.Result = EQUIP_ERR_TOO_MUCH_GOLD;
        trader->GetSession()->SendTradeStatus(info);
        his_trade->SetAccepted(false, true);
        return;
    }

    // not accept if some items now can't be trade (cheating)
    for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
    {
        if (Item* item = my_trade->GetItem(TradeSlots(i)))
        {
            if (!item->CanBeTraded(false, true))
            {
                info.Status = TRADE_STATUS_TRADE_CANCELED;
                SendTradeStatus(info);
                return;
            }

            if (item->IsBindedNotWith(trader))
            {
                info.Status = TRADE_STATUS_CLOSE_WINDOW;
                info.Result = EQUIP_ERR_CANNOT_TRADE_THAT;
                SendTradeStatus(info);
                return;
            }
        }

        if (Item* item = his_trade->GetItem(TradeSlots(i)))
        {
            if (!item->CanBeTraded(false, true))
            {
                info.Status = TRADE_STATUS_TRADE_CANCELED;
                SendTradeStatus(info);
                return;
            }
            //if (item->IsBindedNotWith(_player))   // dont mark as invalid when his item isnt good (not exploitable because if item is invalid trade will fail anyway later on the same check)
            //{
            //    SendTradeStatus(TRADE_STATUS_NOT_ELIGIBLE);
            //    his_trade->SetAccepted(false, true);
            //    return;
            //}
        }
    }

    if (his_trade->IsAccepted())
    {
        setAcceptTradeMode(my_trade, his_trade, myItems, hisItems);

        Spell* my_spell = nullptr;
        SpellCastTargets my_targets;

        Spell* his_spell = nullptr;
        SpellCastTargets his_targets;

        // not accept if spell can't be casted now (cheating)
        if (uint32 my_spell_id = my_trade->GetSpell())
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(my_spell_id);
            Item* castItem = my_trade->GetSpellCastItem();

            if (!spellInfo || !his_trade->GetItem(TRADE_SLOT_NONTRADED) ||
                    (my_trade->HasSpellCastItem() && !castItem))
            {
                clearAcceptTradeMode(my_trade, his_trade);
                clearAcceptTradeMode(myItems, hisItems);

                my_trade->SetSpell(0);
                return;
            }

            my_spell = new Spell(_player, spellInfo, TRIGGERED_FULL_MASK);
            my_spell->m_CastItem = castItem;
            my_targets.SetTradeItemTarget(_player);
            my_spell->m_targets = my_targets;

            SpellCastResult res = my_spell->CheckCast(true);
            if (res != SPELL_CAST_OK)
            {
                my_spell->SendCastResult(res);

                clearAcceptTradeMode(my_trade, his_trade);
                clearAcceptTradeMode(myItems, hisItems);

                delete my_spell;
                my_trade->SetSpell(0);
                return;
            }
        }

        // not accept if spell can't be casted now (cheating)
        if (uint32 his_spell_id = his_trade->GetSpell())
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(his_spell_id);
            Item* castItem = his_trade->GetSpellCastItem();

            if (!spellInfo || !my_trade->GetItem(TRADE_SLOT_NONTRADED) || (his_trade->HasSpellCastItem() && !castItem))
            {
                delete my_spell;
                his_trade->SetSpell(0);

                clearAcceptTradeMode(my_trade, his_trade);
                clearAcceptTradeMode(myItems, hisItems);
                return;
            }

            his_spell = new Spell(trader, spellInfo, TRIGGERED_FULL_MASK);
            his_spell->m_CastItem = castItem;
            his_targets.SetTradeItemTarget(trader);
            his_spell->m_targets = his_targets;

            SpellCastResult res = his_spell->CheckCast(true);
            if (res != SPELL_CAST_OK)
            {
                his_spell->SendCastResult(res);

                clearAcceptTradeMode(my_trade, his_trade);
                clearAcceptTradeMode(myItems, hisItems);

                delete my_spell;
                delete his_spell;

                his_trade->SetSpell(0);
                return;
            }
        }

        // inform partner client
        info.Status = TRADE_STATUS_TRADE_ACCEPT;
        trader->GetSession()->SendTradeStatus(info);

        // test if item will fit in each inventory
        TradeStatusInfo myCanCompleteInfo, hisCanCompleteInfo;
        hisCanCompleteInfo.Result = trader->CanStoreItems(myItems, TRADE_SLOT_TRADED_COUNT, &hisCanCompleteInfo.ItemLimitedByLimitCategory);
        myCanCompleteInfo.Result = _player->CanStoreItems(hisItems, TRADE_SLOT_TRADED_COUNT, &myCanCompleteInfo.ItemLimitedByLimitCategory);

        clearAcceptTradeMode(myItems, hisItems);

        // in case of missing space report error
        if (myCanCompleteInfo.Result != EQUIP_ERR_OK)
        {
            clearAcceptTradeMode(my_trade, his_trade);

            myCanCompleteInfo.Status = TRADE_STATUS_CLOSE_WINDOW;
            trader->GetSession()->SendTradeStatus(myCanCompleteInfo);
            myCanCompleteInfo.IsTargetResult = true;
            SendTradeStatus(myCanCompleteInfo);
            my_trade->SetAccepted(false);
            his_trade->SetAccepted(false);
            delete my_spell;
            delete his_spell;
            return;
        }
        else if (hisCanCompleteInfo.Result != EQUIP_ERR_OK)
        {
            clearAcceptTradeMode(my_trade, his_trade);

            hisCanCompleteInfo.Status = TRADE_STATUS_CLOSE_WINDOW;
            SendTradeStatus(hisCanCompleteInfo);
            hisCanCompleteInfo.IsTargetResult = true;
            trader->GetSession()->SendTradeStatus(hisCanCompleteInfo);
            my_trade->SetAccepted(false);
            his_trade->SetAccepted(false);
            delete my_spell;
            delete his_spell;
            return;
        }

        // cache traded item info before moving (pointers become invalid after moveItems if items merge into existing stacks)
        struct TradedItemInfo
        {
            bool present = false;
            std::string name;
            uint32 entry = 0;
            uint32 count = 0;
        };
        TradedItemInfo myItemsInfo[TRADE_SLOT_TRADED_COUNT];
        TradedItemInfo hisItemsInfo[TRADE_SLOT_TRADED_COUNT];
        std::string myItemsStr, hisItemsStr;
        for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
        {
            if (myItems[i])
            {
                myItemsInfo[i].present = true;
                myItemsInfo[i].name = myItems[i]->GetTemplate()->Name1;
                myItemsInfo[i].entry = myItems[i]->GetEntry();
                myItemsInfo[i].count = myItems[i]->GetCount();
                myItemsStr += Acore::StringFormat("{} (Entry:{}) x{}, ", myItemsInfo[i].name, myItemsInfo[i].entry, myItemsInfo[i].count);
            }
            if (hisItems[i])
            {
                hisItemsInfo[i].present = true;
                hisItemsInfo[i].name = hisItems[i]->GetTemplate()->Name1;
                hisItemsInfo[i].entry = hisItems[i]->GetEntry();
                hisItemsInfo[i].count = hisItems[i]->GetCount();
                hisItemsStr += Acore::StringFormat("{} (Entry:{}) x{}, ", hisItemsInfo[i].name, hisItemsInfo[i].entry, hisItemsInfo[i].count);
            }
        }

        // execute trade: 1. remove
        for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
        {
            if (myItems[i])
            {
                myItems[i]->SetGuidValue(ITEM_FIELD_GIFTCREATOR, _player->GetGUID());
                _player->MoveItemFromInventory(myItems[i]->GetBagSlot(), myItems[i]->GetSlot(), true);
            }
            if (hisItems[i])
            {
                hisItems[i]->SetGuidValue(ITEM_FIELD_GIFTCREATOR, trader->GetGUID());
                trader->MoveItemFromInventory(hisItems[i]->GetBagSlot(), hisItems[i]->GetSlot(), true);
            }
        }

        bool needsCrossrealmHandling = sToCloud9Sidecar->IsCrossrealm() && _player->GetGUID().GetRealmID() != trader->GetGUID().GetRealmID();

        // Create new items for crossrealm usage.
        if (needsCrossrealmHandling)
        {
            CharacterDatabasePreparedStatement* stmt = nullptr;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_NO_OP_PROVIDE_REALM_CONTEXT);
            stmt->SetData(0, _player->GetGUID().GetRealmID());
            trans->Append(stmt);

            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; i++)
            {
                if (myItems[i])
                {
                    Item* newItem = myItems[i]->CloneItem(myItems[i]->GetCount(), trader);
                    if (!newItem)
                    {
                        ItemPosCountVec playerDst;
                        if (_player->CanStoreItem(NULL_BAG, NULL_SLOT, playerDst, myItems[i], false) == EQUIP_ERR_OK)
                            _player->MoveItemToInventory(playerDst, myItems[i], true, true);

                        // Should we handle else statement here?

                        continue;
                    }

                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_INSTANCE);
                    stmt->SetData(0, myItems[i]->GetGUID().GetCounter());
                    trans->Append(stmt);

                    delete myItems[i];

                    myItems[i] = newItem;
                }
            }
            CharacterDatabase.CommitTransaction(trans);

            trans = CharacterDatabase.BeginTransaction();

            stmt = CharacterDatabase.GetPreparedStatement(CHAR_NO_OP_PROVIDE_REALM_CONTEXT);
            stmt->SetData(0, trader->GetGUID().GetRealmID());
            trans->Append(stmt);

            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; i++)
            {
                if (hisItems[i])
                {
                    Item* newItem = hisItems[i]->CloneItem(hisItems[i]->GetCount(), _player);
                    if (!newItem)
                    {
                        ItemPosCountVec playerDst;
                        if (trader->CanStoreItem(NULL_BAG, NULL_SLOT, playerDst, hisItems[i], false) == EQUIP_ERR_OK)
                            trader->MoveItemToInventory(playerDst, hisItems[i], true, true);

                        // Should we handle else statement here?

                        continue;
                    }

                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_INSTANCE);
                    stmt->SetData(0, hisItems[i]->GetGUID().GetCounter());
                    trans->Append(stmt);

                    delete hisItems[i];

                    hisItems[i] = newItem;
                }
            }
            CharacterDatabase.CommitTransaction(trans);

        }

        // execute trade: 2. store
        moveItems(myItems, hisItems);

        // logging money
        if (my_trade->GetMoney() >= 10 * GOLD )
        {
            CharacterDatabase.Execute("INSERT INTO log_money VALUES({}, {}, \"{}\", \"{}\", {}, \"{}\", {}, \"goods\", NOW(), {})",
                GetAccountId(), _player->GetGUID().GetCounter(), _player->GetName(), GetRemoteAddress(), trader->GetSession()->GetAccountId(), trader->GetName(), my_trade->GetMoney(), 6);
        }
        if (his_trade->GetMoney() >= 10 * GOLD )
        {
            CharacterDatabase.Execute("INSERT INTO log_money VALUES({}, {}, \"{}\", \"{}\", {}, \"{}\", {}, \"goods\", NOW(), {})",
                trader->GetSession()->GetAccountId(), trader->GetGUID().GetCounter(), trader->GetName(), trader->GetSession()->GetRemoteAddress(), GetAccountId(), _player->GetName(), his_trade->GetMoney(), 6);
        }

        // update money
        _player->ModifyMoney(-int32(my_trade->GetMoney()));
        _player->ModifyMoney(his_trade->GetMoney());
        trader->ModifyMoney(-int32(his_trade->GetMoney()));
        trader->ModifyMoney(my_trade->GetMoney());

        if (HasPermission(rbac::RBAC_PERM_LOG_GM_TRADE))
        {
            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
            {
                if (myItemsInfo[i].present)
                    LOG_GM(GetAccountId(), "GM {} (Account: {}) traded item: {} (Entry: {} Count: {}) to {}",
                        _player->GetName(), GetAccountId(),
                        myItemsInfo[i].name, myItemsInfo[i].entry, myItemsInfo[i].count,
                        trader->GetName());
            }
            if (my_trade->GetMoney() > 0)
                LOG_GM(GetAccountId(), "GM {} (Account: {}) traded money: {} to {}",
                    _player->GetName(), GetAccountId(), my_trade->GetMoney(), trader->GetName());
        }
        if (trader->GetSession()->HasPermission(rbac::RBAC_PERM_LOG_GM_TRADE))
        {
            for (uint8 i = 0; i < TRADE_SLOT_TRADED_COUNT; ++i)
            {
                if (hisItemsInfo[i].present)
                    LOG_GM(trader->GetSession()->GetAccountId(), "GM {} (Account: {}) traded item: {} (Entry: {} Count: {}) to {}",
                        trader->GetName(), trader->GetSession()->GetAccountId(),
                        hisItemsInfo[i].name, hisItemsInfo[i].entry, hisItemsInfo[i].count,
                        _player->GetName());
            }
            if (his_trade->GetMoney() > 0)
                LOG_GM(trader->GetSession()->GetAccountId(), "GM {} (Account: {}) traded money: {} to {}",
                    trader->GetName(), trader->GetSession()->GetAccountId(), his_trade->GetMoney(), _player->GetName());
        }

        // log completed trade
        LOG_INFO("entities.player.trade", "Trade: Account: {} (IP: {}), Player [{}] ({}) traded with Player [{}] ({}): gave {} copper, received {} copper, gave item(s) [{}], received item(s) [{}]",
            GetAccountId(), GetRemoteAddress(), _player->GetName(), _player->GetGUID().GetCounter(),
            trader->GetName(), trader->GetGUID().GetCounter(),
            my_trade->GetMoney(), his_trade->GetMoney(),
            myItemsStr, hisItemsStr);

        if (my_spell)
            my_spell->prepare(&my_targets);

        if (his_spell)
            his_spell->prepare(&his_targets);

        // cleanup
        clearAcceptTradeMode(my_trade, his_trade);
        delete _player->m_trade;
        _player->m_trade = nullptr;
        delete trader->m_trade;
        trader->m_trade = nullptr;

        // We can't use single transaction with different databases.
        if (needsCrossrealmHandling)
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            _player->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            trans = CharacterDatabase.BeginTransaction();
            trader->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);
        }
        else
        {
            // desynchronized with the other saves here (SaveInventoryAndGoldToDB() not have own transaction guards)
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            _player->SaveInventoryAndGoldToDB(trans);
            trader->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);
        }

        info.Status = TRADE_STATUS_TRADE_COMPLETE;
        trader->GetSession()->SendTradeStatus(info);
        SendTradeStatus(info);
    }
    else
    {
        info.Status = TRADE_STATUS_TRADE_ACCEPT;
        trader->GetSession()->SendTradeStatus(info);
    }
}

void WorldSession::HandleUnacceptTradeOpcode(WorldPacket& /*recvPacket*/)
{
    TradeData* my_trade = _player->GetTradeData();
    if (!my_trade)
        return;

    my_trade->SetAccepted(false, true);
}

void WorldSession::HandleBeginTradeOpcode(WorldPacket& /*recvPacket*/)
{
    TradeData* my_trade = _player->GetTradeData();
    if (!my_trade)
        return;

    TradeStatusInfo info;
    info.Status = TRADE_STATUS_OPEN_WINDOW;
    my_trade->GetTrader()->GetSession()->SendTradeStatus(info);
    SendTradeStatus(info);
}

void WorldSession::SendCancelTrade(TradeStatus status)
{
    if (PlayerRecentlyLoggedOut() || PlayerLogout())
        return;

    TradeStatusInfo info;
    info.Status = status;
    SendTradeStatus(info);
}

void WorldSession::HandleCancelTradeOpcode(WorldPacket& /*recvPacket*/)
{
    // sended also after LOGOUT COMPLETE
    if (_player)                                             // needed because STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT
        _player->TradeCancel(true);
}

void WorldSession::HandleInitiateTradeOpcode(WorldPacket& recvPacket)
{
    // 3.4.3 (writer FUN_14075e1a0): PackedGuid128 Guid
    ObjectGuid ID = Guid128::Read(recvPacket);

    if (GetPlayer()->GetTradeData())
        return;

    TradeStatusInfo info;
    if (!GetPlayer()->IsAlive())
    {
        info.Status = TRADE_STATUS_YOU_DEAD;
        SendTradeStatus(info);
        return;
    }

    if (GetPlayer()->HasUnitState(UNIT_STATE_STUNNED))
    {
        info.Status = TRADE_STATUS_YOU_STUNNED;
        SendTradeStatus(info);
        return;
    }

    if (IsLoggingOut())
    {
        info.Status = TRADE_STATUS_YOU_LOGOUT;
        SendTradeStatus(info);
        return;
    }

    if (GetPlayer()->IsInFlight())
    {
        info.Status = TRADE_STATUS_TARGET_TO_FAR;
        SendTradeStatus(info);
        return;
    }

    if (GetPlayer()->GetLevel() < sWorld->getIntConfig(CONFIG_TRADE_LEVEL_REQ))
    {
        ChatHandler(this).SendNotification(LANG_TRADE_REQ, sWorld->getIntConfig(CONFIG_TRADE_LEVEL_REQ));
        info.Status = TRADE_STATUS_CLOSE_WINDOW;
        SendTradeStatus(info);
        return;
    }

    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_TRADE) && IsTrialAccount())
    {
        info.Status = TRADE_STATUS_TRIAL_ACCOUNT;
        SendTradeStatus(info);
        return;
    }

    if (GetPlayer()->IsSpectator())
        return;

    Player* pOther = ObjectAccessor::FindPlayer(ID);

    if (!pOther)
    {
        info.Status = TRADE_STATUS_NO_TARGET;
        SendTradeStatus(info);
        return;
    }

    if (pOther == GetPlayer() || pOther->m_trade)
    {
        info.Status = TRADE_STATUS_BUSY;
        SendTradeStatus(info);
        return;
    }

    if (!pOther->IsAlive())
    {
        info.Status = TRADE_STATUS_TARGET_DEAD;
        SendTradeStatus(info);
        return;
    }

    if (pOther->IsInFlight())
    {
        info.Status = TRADE_STATUS_TARGET_TO_FAR;
        SendTradeStatus(info);
        return;
    }

    if (pOther->HasUnitState(UNIT_STATE_STUNNED))
    {
        info.Status = TRADE_STATUS_TARGET_STUNNED;
        SendTradeStatus(info);
        return;
    }

    if (pOther->GetSession()->IsLoggingOut())
    {
        info.Status = TRADE_STATUS_TARGET_LOGOUT;
        SendTradeStatus(info);
        return;
    }

    if (sWorld->getBoolConfig(CONFIG_TRIAL_RESTRICTION_TRADE) && pOther->GetSession()->IsTrialAccount())
    {
        info.Status = TRADE_STATUS_TRIAL_ACCOUNT;
        SendTradeStatus(info);
        return;
    }

    if (pOther->GetTeamId() != _player->GetTeamId() &&
        !GetPlayer()->GetSession()->HasPermission(rbac::RBAC_PERM_ALLOW_TWO_SIDE_TRADE))
    {
        info.Status = TRADE_STATUS_WRONG_FACTION;
        SendTradeStatus(info);
        return;
    }

    if (!pOther->IsWithinDistInMap(_player, TRADE_DISTANCE, false))
    {
        info.Status = TRADE_STATUS_TARGET_TO_FAR;
        SendTradeStatus(info);
        return;
    }

    if (!sScriptMgr->OnPlayerCanInitTrade(_player, pOther))
        return;

    // OK start trade
    _player->m_trade = new TradeData(_player, pOther);
    pOther->m_trade = new TradeData(pOther, _player);
    static std::atomic<uint32> nextTradeId{0};
    uint32 tradeId = ++nextTradeId;
    _player->m_trade->SetTradeId(tradeId);
    pOther->m_trade->SetTradeId(tradeId);

    // WorldPacket data(SMSG_TRADE_STATUS, 12);
    // data << uint32(TRADE_STATUS_BEGIN_TRADE);
    // data << _player->GetGUID();
    // pOther->SendDirectMessage(&data);
    info.Status = TRADE_STATUS_BEGIN_TRADE;
    info.TraderGuid = _player->GetGUID();
    pOther->GetSession()->SendTradeStatus(info);
}

void WorldSession::HandleSetTradeGoldOpcode(WorldPacket& recvPacket)
{
    // 3.4.3 (writer FUN_140760a40): u64 Coinage, the whole amount offered
    uint64 gold64 = recvPacket.read<uint64>();
    uint32 gold = uint32(std::min<uint64>(gold64, std::numeric_limits<uint32>::max()));

    TradeData* my_trade = _player->GetTradeData();
    if (!my_trade)
        return;

    my_trade->UpdateClientStateIndex();

    my_trade->SetMoney(gold);
}

void WorldSession::HandleSetTradeItemOpcode(WorldPacket& recvPacket)
{
    // send update
    // 3.4.3 (writer FUN_140760a80): u8 TradeSlot, u8 PackSlot (the bag's InvSlots index, 0xFF the player),
    // u8 ItemSlotInPack
    uint8 tradeSlot = recvPacket.read<uint8>();
    uint8 clientBag = recvPacket.read<uint8>();
    uint8 clientSlot = recvPacket.read<uint8>();
    uint8 bag, slot;
    ServerItemPos(clientBag, clientSlot, bag, slot);

    TradeData* my_trade = _player->GetTradeData();
    if (!my_trade)
        return;

    my_trade->UpdateClientStateIndex();

    TradeStatusInfo info;
    // invalid slot number
    if (tradeSlot >= TRADE_SLOT_COUNT)
    {
        info.Status = TRADE_STATUS_TRADE_CANCELED;
        SendTradeStatus(info);
        return;
    }

    // check cheating, can't fail with correct client operations
    Item* item = _player->GetItemByPos(bag, slot);
    if (!item || (tradeSlot != TRADE_SLOT_NONTRADED && !item->CanBeTraded(false, true)))
    {
        info.Status = TRADE_STATUS_TRADE_CANCELED;
        SendTradeStatus(info);
        return;
    }

    ObjectGuid iGUID = item->GetGUID();

    // prevent place single item into many trade slots using cheating and client bugs
    if (my_trade->HasItem(iGUID))
    {
        // cheating attempt
        info.Status = TRADE_STATUS_TRADE_CANCELED;
        SendTradeStatus(info);
        return;
    }

    // PlayerScript Hook for checking traded items if we want to filter them in a custom module
    if (!sScriptMgr->OnPlayerCanSetTradeItem(_player, item, tradeSlot))
    {
        // Do not send TRADE_STATUS_TRADE_CANCELED because it will cause double display of "Transaction canceled" notification
        // On the trade initiator screen
        info.Status = TRADE_STATUS_CLOSE_WINDOW;
        SendTradeStatus(info);
        return;
    }

    if (tradeSlot != TRADE_SLOT_NONTRADED && item->IsBindedNotWith(my_trade->GetTrader()))
    {
        info.Status = TRADE_STATUS_NOT_ON_TAPLIST;
        info.Slot = tradeSlot;
        SendTradeStatus(info);
        return;
    }

    my_trade->SetItem(TradeSlots(tradeSlot), item);
}

void WorldSession::HandleClearTradeItemOpcode(WorldPacket& recvPacket)
{
    // 3.4.3 (writer FUN_14075b8d0): u8 TradeSlot
    uint8 tradeSlot = recvPacket.read<uint8>();

    TradeData* my_trade = _player->m_trade;
    if (!my_trade)
        return;

    my_trade->UpdateClientStateIndex();

    // invalid slot number
    if (tradeSlot >= TRADE_SLOT_COUNT)
        return;

    my_trade->SetItem(TradeSlots(tradeSlot), nullptr);
}
