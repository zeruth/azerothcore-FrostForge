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

#include "QuestPackets.h"
#include "ItemPackets.h"
#include "PacketGuid128.h"

// The quest progress messages without a parser in 3.4.3 (QUEST_UPDATE_COMPLETE, _FAILED_TIMER,
// _ADD_PVP_CREDIT, QUEST_GIVER_QUEST_FAILED, QUEST_LOG_FULL) are read by their handlers as fixed
// structs over the raw body, with no length check: they must carry every byte.

WorldPacket const* WorldPackets::Quest::QuestUpdateComplete::Write()
{
    _worldPacket << uint32(QuestId);                        // raw: u32 QuestID

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestGiverQuestComplete::Write()
{
    // 3.4.3 (quest switch, inline): u32 QuestID, u32 XPReward, u64 MoneyReward, u32 SkillLineIDReward,
    // u32 NumSkillUpsReward, bits UseQuestReward, LaunchGossip, LaunchQuest, HideChatMessage, flush,
    // ItemInstance ItemReward
    _worldPacket << uint32(QuestId);
    _worldPacket << uint32(Experience);
    _worldPacket << uint64(RewardMoney);
    _worldPacket << uint32(0);
    _worldPacket << uint32(0);
    _worldPacket.WriteBit(false);
    _worldPacket.WriteBit(false);
    _worldPacket.WriteBit(false);
    _worldPacket.WriteBit(false);
    _worldPacket.FlushBits();
    WorldPackets::Item::WriteItemInstance(_worldPacket, 0);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestGiverQuestFailed::Write()
{
    // raw: u32 QuestID, u32 Reason (the client's InventoryResult numbering: handler FUN_141740cb0)
    _worldPacket << uint32(QuestId);
    _worldPacket << uint32(ClientInventoryResult(InventoryResult(FailureReason)));

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestUpdateFailedTimer::Write()
{
    _worldPacket << uint32(QuestId);                        // raw: u32 QuestID

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestGiverQuestInvalid::Write()
{
    // 3.4.3 (quest switch, inline): u32 Reason, u32 ContributionRewardID, bit SendErrorMessage, 9-bit
    // reason text length, flush, the text. The handler (FUN_141740970) knows 1, 6, 7, 12, 13, 16, 18,
    // 21, 23, 26 (ERR_QUEST_FAILED_CAIS), 28 (ERR_QUEST_ALREADY_DONE_DAILY), 30, 32: 3.3.5's CAIS (27)
    // and daily-done (29) are one lower; the 25-daily cap has no reason of its own and goes as reason 0
    // with its text, which the client shows through ERR_QUEST_NEED_PREREQS_CUSTOM.
    uint32 reason = FailureReason;
    std::string_view text;
    switch (FailureReason)
    {
        case INVALIDREASON_QUEST_FAILED_CAIS:           reason = 26; break;
        case INVALIDREASON_DAILY_QUEST_COMPLETED_TODAY: reason = 28; break;
        case INVALIDREASON_DAILY_QUESTS_REMAINING:
            reason = INVALIDREASON_DONT_HAVE_REQ;
            text = "You have already completed 25 daily quests today.";
            break;
        default: break;
    }
    _worldPacket << uint32(reason);
    _worldPacket << uint32(0);                              // ContributionRewardID
    _worldPacket.WriteBit(true);                            // SendErrorMessage
    _worldPacket.WriteBits(text.size(), 9);
    _worldPacket.FlushBits();
    _worldPacket.WriteString(text);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestConfirmAccept::Write()
{
    // 3.4.3: u32 QuestID, PackedGuid128 InitiatedBy, 10-bit title length, flush, the title
    std::string_view title = QuestTitle.substr(0, std::min<std::size_t>(QuestTitle.size(), 0x3FF));
    _worldPacket << uint32(QuestId);
    Guid128::Write(_worldPacket, PlayerGuid);
    _worldPacket.WriteBits(title.size(), 10);
    _worldPacket.FlushBits();
    _worldPacket.WriteString(title);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestPushResult::Write()
{
    // 3.4.3 (handler FUN_141740e70): PackedGuid128 SenderGUID, u8 Result (ERR_QUEST_PUSH_*: 0 success,
    // 1 invalid, 2 accepted, 3 declined, 4 too far, 5 busy, 6 dead, 7 log full, 8 on quest, 9 already
    // done, 10 not daily, 11 timer expired, 12 not in party, ...), 9-bit title length, flush, the title
    static uint8 const clientResult[] = { 0, 1, 2, 3, 5, 7, 8, 9, 10, 11, 12 };
    Guid128::Write(_worldPacket, PlayerGuid);
    _worldPacket << uint8(QuestShareMessage < std::size(clientResult) ? clientResult[QuestShareMessage] : 1);
    _worldPacket.WriteBits(0, 9);
    _worldPacket.FlushBits();

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestUpdateAddKill::Write()
{
    // SMSG_QUEST_UPDATE_ADD_CREDIT, 3.4.3: PackedGuid128 VictimGUID, u32 QuestID, u32 ObjectID, u16 Count,
    // u16 Required, u8 ObjectiveType (0 creature, 2 game object)
    Guid128::Write(_worldPacket, ObjectiveGuid);
    _worldPacket << uint32(QuestId);
    _worldPacket << uint32(CreatureEntry);
    _worldPacket << uint16(CurrentCount);
    _worldPacket << uint16(RequiredCount);
    _worldPacket << uint8(ObjectiveType);

    return &_worldPacket;
}

WorldPacket const* WorldPackets::Quest::QuestUpdateAddPvPKill::Write()
{
    // raw: u32 QuestID, u16 Count
    _worldPacket << uint32(QuestId);
    _worldPacket << uint16(CurrentCount);

    return &_worldPacket;
}

void WorldPackets::Quest::QuestPushResultClient::Read()
{
    // 3.4.3 (writer FUN_14075fbc0): PackedGuid128 SenderGUID, u32 QuestID, u8 Result
    PlayerGuid = Guid128::Read(_worldPacket);
    _worldPacket >> QuestId;
    uint8 Message;
    _worldPacket >> Message;
    QuestShareMessage = static_cast<QuestShareMessages>(Message);
}

void WorldPackets::Quest::QuestLogRemoveQuest::Read()
{
    _worldPacket >> Slot;
}

void WorldPackets::Quest::QuestConfirmAcceptClient::Read()
{
    _worldPacket >> QuestId;
}

void WorldPackets::Quest::PushQuestToParty::Read()
{
    _worldPacket >> QuestId;
}
