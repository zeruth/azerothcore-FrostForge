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

#include "BankPackets.h"
#include "PacketGuid128.h"
#include "Player.h"

// 3.4.3: CMSG_AUTOBANK_ITEM (writer FUN_14075c190) and CMSG_AUTOSTORE_BANK_ITEM (FUN_14075c4a0) start with
// the client's InvUpdate (2-bit count, flush, pairs), then u8 Bag, u8 Slot in InvSlots numbering
static void ReadBankItemPos(ByteBuffer& data, uint8& bag, uint8& slot)
{
    uint32 count = data.ReadBits(2);
    data.ResetBitReader();
    data.read_skip(std::size_t(count) * 2);
    uint8 clientBag = data.read<uint8>();
    uint8 clientSlot = data.read<uint8>();
    ServerItemPos(clientBag, clientSlot, bag, slot);
}

void WorldPackets::Bank::AutoBankItem::Read()
{
    ReadBankItemPos(_worldPacket, Bag, Slot);
}

void WorldPackets::Bank::AutoStoreBankItem::Read()
{
    ReadBankItemPos(_worldPacket, Bag, Slot);
}

void WorldPackets::Bank::BuyBankSlot::Read()
{
    // 3.4.3 (writer FUN_14075ca50): PackedGuid128 Guid (the banker)
    Banker = Guid128::Read(_worldPacket);
}
