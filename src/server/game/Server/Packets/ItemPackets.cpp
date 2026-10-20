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

#include "ItemPackets.h"
#include "PacketGuid128.h"
#include "Player.h"

// The 3.4.3 client names an item's place by its ActivePlayer InvSlots index (equipment 0-18, bags
// 30-33, backpack 35-50, ...) and a bag by the bag's own index, 0xFF for the player: ServerItemPos and
// ServerInventorySlot (Player.h) turn them into AC's slots. Every inventory request starts with the
// InvUpdate the client keeps for its own bookkeeping (2-bit count, flush, {u8 container, u8 slot}).
namespace
{
    void SkipInvUpdate(ByteBuffer& data)
    {
        uint32 count = data.ReadBits(2);
        data.ResetBitReader();
        data.read_skip(std::size_t(count) * 2);
    }
}

void WorldPackets::Item::SplitItem::Read()
{
    // 3.4.3 (writer FUN_140760d30): InvUpdate, u8 FromPackSlot, u8 FromSlot, u8 ToPackSlot, u8 ToSlot,
    // u32 Quantity
    SkipInvUpdate(_worldPacket);
    uint8 fromBag = _worldPacket.read<uint8>();
    uint8 fromSlot = _worldPacket.read<uint8>();
    uint8 toBag = _worldPacket.read<uint8>();
    uint8 toSlot = _worldPacket.read<uint8>();
    ServerItemPos(fromBag, fromSlot, SourceBag, SourceSlot);
    ServerItemPos(toBag, toSlot, DestinationBag, DestinationSlot);
    _worldPacket >> Count;
}

void WorldPackets::Item::SwapInventoryItem::Read()
{
    // 3.4.3 (writer FUN_140760e90): InvUpdate, u8 Slot2 (destination), u8 Slot1 (source), both on the player
    SkipInvUpdate(_worldPacket);
    DestinationSlot = ServerInventorySlot(_worldPacket.read<uint8>());
    SourceSlot = ServerInventorySlot(_worldPacket.read<uint8>());
}

void WorldPackets::Item::AutoEquipItemSlot::Read()
{
    // 3.4.3 (writer FUN_14075c310): InvUpdate, PackedGuid128 Item, u8 ItemDstSlot (an equipment slot)
    SkipInvUpdate(_worldPacket);
    ItemGuid = Guid128::Read(_worldPacket);
    DestinationSlot = ServerInventorySlot(_worldPacket.read<uint8>());
}

void WorldPackets::Item::SwapItem::Read()
{
    // 3.4.3 (writer FUN_140760f50): InvUpdate, u8 ContainerSlotB, u8 ContainerSlotA, u8 SlotB, u8 SlotA;
    // A is the item moved, B where it goes
    SkipInvUpdate(_worldPacket);
    uint8 bagB = _worldPacket.read<uint8>();
    uint8 bagA = _worldPacket.read<uint8>();
    uint8 slotB = _worldPacket.read<uint8>();
    uint8 slotA = _worldPacket.read<uint8>();
    ServerItemPos(bagB, slotB, DestinationBag, DestinationSlot);
    ServerItemPos(bagA, slotA, SourceBag, SourceSlot);
}

void WorldPackets::Item::AutoEquipItem::Read()
{
    // 3.4.3 (writer FUN_14075c250): InvUpdate, u8 PackSlot, u8 Slot
    SkipInvUpdate(_worldPacket);
    uint8 bag = _worldPacket.read<uint8>();
    uint8 slot = _worldPacket.read<uint8>();
    ServerItemPos(bag, slot, SourceBag, SourceSlot);
}

void WorldPackets::Item::DestroyItem::Read()
{
    // 3.4.3 (writer FUN_14075d2e0): u32 Count, u8 ContainerId, u8 SlotNum
    uint32 count = _worldPacket.read<uint32>();
    Count = uint8(std::min<uint32>(count, 0xFF));
    uint8 bag = _worldPacket.read<uint8>();
    uint8 slot = _worldPacket.read<uint8>();
    ServerItemPos(bag, slot, Bag, Slot);
}

void WorldPackets::Item::ReadItem::Read()
{
    // 3.4.3 (writer FUN_14075fc10): u8 PackSlot, u8 Slot
    uint8 bag = _worldPacket.read<uint8>();
    uint8 slot = _worldPacket.read<uint8>();
    ServerItemPos(bag, slot, Bag, Slot);
}

void WorldPackets::Item::SellItem::Read()
{
    // 3.4.3 (writer FUN_1407603c0): PackedGuid128 VendorGUID, PackedGuid128 ItemGUID, u32 Amount
    VendorGuid = Guid128::Read(_worldPacket);
    ItemGuid = Guid128::Read(_worldPacket);
    _worldPacket >> Count;
}

void WorldPackets::Item::BuybackItem::Read()
{
    // 3.4.3 (writer FUN_14075ca00): PackedGuid128 VendorGUID, u32 Slot (the buyback slot's InvSlots index)
    VendorGuid = Guid128::Read(_worldPacket);
    Slot = ServerInventorySlot(uint8(std::min<uint32>(_worldPacket.read<uint32>(), 0xFF)));
}

void WorldPackets::Item::BuyItem::Read()
{
    // 3.4.3 (writer FUN_14075ca90): PackedGuid128 VendorGUID, PackedGuid128 ContainerGUID, u32 Quantity,
    // u32 Muid (the vendor slot SMSG_VENDOR_INVENTORY numbered), u32 Slot, u32 ItemType, ItemInstance
    VendorGuid = Guid128::Read(_worldPacket);
    Guid128::Skip(_worldPacket);                            // ContainerGUID: AC stores where it finds room
    _worldPacket >> Count;
    _worldPacket >> Slot;
    _worldPacket.read_skip<uint32>();                       // destination slot
    _worldPacket.read_skip<uint32>();                       // ItemType (1 item)
    Item = ReadItemInstance(_worldPacket);
}

void WorldPackets::Item::ListInventory::Read()
{
    // 3.4.3 (writer FUN_14075e700): PackedGuid128 Unit
    VendorGuid = Guid128::Read(_worldPacket);
}

void WorldPackets::Item::AutoStoreBagItem::Read()
{
    // 3.4.3 (writer FUN_14075c3d0, sender FUN_14170aab0): InvUpdate, u8 the item's container, u8 the
    // destination bag, u8 the item's slot
    SkipInvUpdate(_worldPacket);
    uint8 srcBag = _worldPacket.read<uint8>();
    uint8 dstBag = _worldPacket.read<uint8>();
    uint8 srcSlot = _worldPacket.read<uint8>();
    ServerItemPos(srcBag, srcSlot, SourceBag, SourceSlot);
    DestinationBag = dstBag == INVENTORY_SLOT_BAG_0 ? uint8(INVENTORY_SLOT_BAG_0) : ServerInventorySlot(dstBag);
}

WorldPacket const* WorldPackets::Item::EnchantmentLog::Write()
{
    // 3.4.3 (FUN_1406bef40): PackedGuid128 Owner, Caster, ItemGUID, u32 ItemID, u32 Enchantment, u32 EnchantSlot
    Guid128::Write(_worldPacket, Target);
    Guid128::Write(_worldPacket, Caster);
    Guid128::Write(_worldPacket, ItemGuid);
    _worldPacket << uint32(ItemId);
    _worldPacket << uint32(EnchantId);
    _worldPacket << uint32(EnchantSlot);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Item::ItemEnchantTimeUpdate::Write()
{
    // 3.4.3 (FUN_1406c3000): PackedGuid128 ItemGuid, u32 DurationLeft, u32 Slot, PackedGuid128 OwnerGuid
    Guid128::Write(_worldPacket, ItemGuid);
    _worldPacket << uint32(Duration);
    _worldPacket << uint32(Slot);
    Guid128::Write(_worldPacket, PlayerGuid);

    return &_worldPacket;
}

// CMSG_WRAP_ITEM, 3.4.3 (writer FUN_140761970): InvUpdate of 2 positions in InvSlots numbering: the wrapping
// paper, then the item
void WorldPackets::Item::WrapItem::Read()
{
    uint32 count = _worldPacket.ReadBits(2);
    _worldPacket.ResetBitReader();
    uint8 pos[2][2] = { };
    for (uint32 i = 0; i < count; ++i)
    {
        uint8 container = _worldPacket.read<uint8>();
        uint8 slot = _worldPacket.read<uint8>();
        if (i < 2)
            ServerItemPos(container, slot, pos[i][0], pos[i][1]);
    }
    GiftBag = pos[0][0];
    GiftSlot = pos[0][1];
    ItemBag = pos[1][0];
    ItemSlot = pos[1][1];
}

// CMSG_SOCKET_GEMS, 3.4.3 (writer FUN_140760b70): PackedGuid128 item, 3 x PackedGuid128 gem (empty: none)
void WorldPackets::Item::SocketGems::Read()
{
    ItemGuid = Guid128::Read(_worldPacket);
    for (int i = 0; i < MAX_GEM_SOCKETS; ++i)
        GemGuids[i] = Guid128::Read(_worldPacket);
}

void WorldPackets::Item::CancelTempEnchantment::Read()
{
    _worldPacket >> EquipmentSlot;
}

// CMSG_GET_ITEM_PURCHASE_DATA, 3.4.3 (writer FUN_14075d5e0): PackedGuid128 ItemGUID
void WorldPackets::Item::ItemRefundInfo::Read()
{
    ItemGuid = Guid128::Read(_worldPacket);
}

// CMSG_ITEM_PURCHASE_REFUND, 3.4.3 (writer FUN_14075e300): PackedGuid128 ItemGUID
void WorldPackets::Item::ItemRefund::Read()
{
    ItemGuid = Guid128::Read(_worldPacket);
}
