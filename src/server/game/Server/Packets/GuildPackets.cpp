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

#include "GuildPackets.h"
#include "ItemPackets.h"
#include "PacketGuid128.h"

namespace
{
    std::string ReadBitString(ByteBuffer& data, uint32 length, std::size_t maxLength)
    {
        std::string text = data.ReadString(length);
        if (text.size() > maxLength)
            text.resize(maxLength);
        return text;
    }
}

// CMSG_QUERY_GUILD_INFO, 3.4.3 (writer FUN_140768930): PackedGuid128 GuildGuid, PackedGuid128 PlayerGuid
void WorldPackets::Guild::QueryGuildInfo::Read()
{
    Guid128::ReadPacked(_worldPacket, GuildGuidLow, GuildGuidHigh);
    GuildId = (GuildGuidHigh >> 58) == 28 ? uint32(GuildGuidLow) : 0;
    Guid128::Skip(_worldPacket);
}

WorldPackets::Guild::QueryGuildInfoResponse::QueryGuildInfoResponse()
        : ServerPacket(SMSG_QUERY_GUILD_INFO_RESPONSE) { }

// SMSG_QUERY_GUILD_INFO_RESPONSE, 3.4.3 (FUN_140702af0, handler FUN_14034e3c0): PackedGuid128 GuildGUID (the cache key),
// bit HasGuildInfo, flush, [PackedGuid128 GuildGuid, u32 VirtualRealmAddress, u32 RankCount, u32 EmblemStyle,
// EmblemColor, BorderStyle, BorderColor, BackgroundColor, 7-bit name length, flush, RankCount x {u32 RankID, u32
// RankOrder, 7-bit name length, flush, the name}, the guild name]
WorldPacket const* WorldPackets::Guild::QueryGuildInfoResponse::Write()
{
    if (!HasGuildInfo)
    {
        Guid128::WritePacked(_worldPacket, GuildGuidLow, GuildGuidHigh);
        _worldPacket.WriteBit(false);
        _worldPacket.FlushBits();
        return &_worldPacket;
    }

    std::string_view name = std::string_view(Info.GuildName).substr(0, 127);
    Guid128::WriteGuild(_worldPacket, GuildId);
    _worldPacket.WriteBit(true);
    _worldPacket.FlushBits();
    Guid128::WriteGuild(_worldPacket, GuildId);
    _worldPacket << uint32(Guid128::VirtualRealmAddress());
    _worldPacket << uint32(Info.RankCount);
    _worldPacket << uint32(Info.EmblemStyle);
    _worldPacket << uint32(Info.EmblemColor);
    _worldPacket << uint32(Info.BorderStyle);
    _worldPacket << uint32(Info.BorderColor);
    _worldPacket << uint32(Info.BackgroundColor);
    _worldPacket.WriteBits(name.size(), 7);
    _worldPacket.FlushBits();
    for (uint32 i = 0; i < Info.RankCount && i < Info.Ranks.size(); ++i)
    {
        std::string_view rankName = std::string_view(Info.Ranks[i]).substr(0, 127);
        _worldPacket << uint32(i);                          // RankID
        _worldPacket << uint32(i);                          // RankOrder: AC's rank ids are their order
        _worldPacket.WriteBits(rankName.size(), 7);
        _worldPacket.FlushBits();
        _worldPacket.WriteString(rankName);
    }
    _worldPacket.WriteString(name);

    return &_worldPacket;
}

// SMSG_GUILD_ROSTER, 3.4.3 (FUN_1406c1b70, handler FUN_141c22d20): u32 NumAccounts, u32 CreateDate (packed), u32
// GuildFlags, u32 member count, 11-bit MOTD length, 11-bit info length, flush, the members, the MOTD, the info.
// The ranks are no longer in it: SMSG_GUILD_RANKS.
WorldPacket const* WorldPackets::Guild::GuildRoster::Write()
{
    std::string_view welcome = std::string_view(WelcomeText).substr(0, 0x7FF);
    std::string_view info = std::string_view(InfoText).substr(0, 0x7FF);
    _worldPacket << uint32(NumAccounts);
    _worldPacket.AppendPackedTime(CreateDate);
    _worldPacket << uint32(GuildFlags);
    _worldPacket << uint32(MemberData.size());
    _worldPacket.WriteBits(welcome.size(), 11);
    _worldPacket.WriteBits(info.size(), 11);
    _worldPacket.FlushBits();

    for (GuildRosterMemberData const& member : MemberData)
        _worldPacket << member;

    _worldPacket.WriteString(welcome);
    _worldPacket.WriteString(info);

    return &_worldPacket;
}

// GuildRosterMemberData, 3.4.3 (FUN_1406feeb0): PackedGuid128 Guid, u32 RankID, u32 AreaID, u32 achievement points,
// u32 guild reputation, f32 LastSave (days), 2 x {u32 DbID, u32 Rank, u32 Step} professions, u32
// VirtualRealmAddress, u8 Status, u8 Level, u8 ClassID, u8 Gender, u64 GuildClubMemberID, u8 RaceID, 6-bit name,
// 8-bit note and 8-bit officer note lengths, bits Authenticated, SorEligible, the strings
ByteBuffer& operator<<(ByteBuffer& data, WorldPackets::Guild::GuildRosterMemberData const& rosterMemberData)
{
    std::string_view name = std::string_view(rosterMemberData.Name).substr(0, 63);
    std::string_view note = std::string_view(rosterMemberData.Note).substr(0, 255);
    std::string_view officerNote = std::string_view(rosterMemberData.OfficerNote).substr(0, 255);

    Guid128::WritePlayer(data, rosterMemberData.Guid);
    data << uint32(rosterMemberData.RankID);
    data << uint32(rosterMemberData.AreaID);
    data << uint32(0);                                      // PersonalAchievementPoints
    data << uint32(0);                                      // GuildReputation
    data << float(rosterMemberData.LastSave);
    for (uint8 i = 0; i < 2; ++i)
        data << uint32(0) << uint32(0) << uint32(0);        // professions
    data << uint32(Guid128::VirtualRealmAddress());
    data << uint8(rosterMemberData.Status);
    data << uint8(rosterMemberData.Level);
    data << uint8(rosterMemberData.ClassID);
    data << uint8(rosterMemberData.Gender);
    data << uint64(0);                                      // GuildClubMemberID
    data << uint8(rosterMemberData.RaceID);
    data.WriteBits(name.size(), 6);
    data.WriteBits(note.size(), 8);
    data.WriteBits(officerNote.size(), 8);
    data.WriteBit(false);                                   // Authenticated
    data.WriteBit(false);                                   // SorEligible
    data.FlushBits();
    data.WriteString(name);
    data.WriteString(note);
    data.WriteString(officerNote);

    return data;
}

// CMSG_GUILD_GET_RANKS, 3.4.3 (writer FUN_14075b2e0): PackedGuid128 GuildGUID
void WorldPackets::Guild::GuildGetRanks::Read()
{
    GuildId = Guid128::ReadGuild(_worldPacket);
}

// SMSG_GUILD_RANKS, 3.4.3 (inline, handler FUN_141c227b0): u32 count, the ranks
WorldPacket const* WorldPackets::Guild::GuildRanks::Write()
{
    _worldPacket << uint32(Ranks.size());
    for (GuildRankData const& rank : Ranks)
        _worldPacket << rank;

    return &_worldPacket;
}

// The rank of SMSG_GUILD_RANKS: u8 RankID, u32 RankOrder, u32 Flags (GuildRankRights), u32 WithdrawGoldLimit, 6 x
// {u32 TabFlags, u32 TabWithdrawItemLimit}, 7-bit name length, flush, the name
ByteBuffer& operator<<(ByteBuffer& data, WorldPackets::Guild::GuildRankData const& rankData)
{
    std::string_view name = std::string_view(rankData.RankName).substr(0, 127);
    data << uint8(rankData.RankID);
    data << uint32(rankData.RankOrder);
    data << uint32(rankData.Flags);
    data << uint32(rankData.WithdrawGoldLimit);

    for (uint8 i = 0; i < GUILD_BANK_MAX_TABS; i++)
    {
        data << uint32(rankData.TabFlags[i]);
        data << uint32(rankData.TabWithdrawItemLimit[i]);
    }

    data.WriteBits(name.size(), 7);
    data.FlushBits();
    data.WriteString(name);
    return data;
}

// CMSG_GUILD_UPDATE_MOTD_TEXT, 3.4.3 (writer FUN_14075b7d0): 11-bit length, flush, the text
void WorldPackets::Guild::GuildUpdateMotdText::Read()
{
    uint32 length = _worldPacket.ReadBits(11);
    _worldPacket.ResetBitReader();
    MotdText = ReadBitString(_worldPacket, length, 128);
}

// SMSG_GUILD_COMMAND_RESULT, 3.4.3 (inline, handler FUN_1417354c0): u32 Result (GuildCommandError), u32 Command
// (GuildCommandType), 8-bit name length, flush, the name
WorldPacket const* WorldPackets::Guild::GuildCommandResult::Write()
{
    std::string_view name = std::string_view(Name).substr(0, 255);
    _worldPacket << int32(Result);
    _worldPacket << int32(Command);
    _worldPacket.WriteBits(name.size(), 8);
    _worldPacket.FlushBits();
    _worldPacket.WriteString(name);

    return &_worldPacket;
}

// CMSG_GUILD_INVITE_BY_NAME, 3.4.3 (writer FUN_1407672a0): 9-bit name length, bit HasUnused910, flush, the name,
// [u32]. The name may carry "-Realm".
void WorldPackets::Guild::GuildInviteByName::Read()
{
    uint32 length = _worldPacket.ReadBits(9);
    bool hasUnused = _worldPacket.ReadBit();
    _worldPacket.ResetBitReader();
    Name = _worldPacket.ReadString(length);
    if (hasUnused)
        ArenaTeamId = _worldPacket.read<uint32>();
    if (std::size_t dash = Name.find('-'); dash != std::string::npos)
        Name.resize(dash);
}

// SMSG_GUILD_INVITE, 3.4.3 (FUN_1406fe770, handler FUN_141c21840): 6-bit inviter name, 7-bit guild name and 7-bit
// old guild name lengths, flush, u32 InviterVirtualRealmAddress, u32 GuildVirtualRealmAddress, PackedGuid128
// GuildGUID, u32 OldGuildVirtualRealmAddress, PackedGuid128 OldGuildGUID, u32 EmblemStyle, EmblemColor, BorderStyle,
// BorderColor, BackgroundColor, AchievementPoints, the names
WorldPacket const* WorldPackets::Guild::GuildInvite::Write()
{
    std::string_view inviter = std::string_view(InviterName).substr(0, 63);
    std::string_view guild = std::string_view(GuildName).substr(0, 127);
    _worldPacket.WriteBits(inviter.size(), 6);
    _worldPacket.WriteBits(guild.size(), 7);
    _worldPacket.WriteBits(0, 7);                           // OldGuildName
    _worldPacket.FlushBits();
    _worldPacket << uint32(Guid128::VirtualRealmAddress());
    _worldPacket << uint32(Guid128::VirtualRealmAddress());
    Guid128::WriteGuild(_worldPacket, GuildId);
    _worldPacket << uint32(0);                              // OldGuildVirtualRealmAddress
    Guid128::WriteGuild(_worldPacket, 0);                   // OldGuildGUID
    _worldPacket << uint32(EmblemStyle);
    _worldPacket << uint32(EmblemColor);
    _worldPacket << uint32(BorderStyle);
    _worldPacket << uint32(BorderColor);
    _worldPacket << uint32(BackgroundColor);
    _worldPacket << uint32(0);                              // AchievementPoints
    _worldPacket.WriteString(inviter);
    _worldPacket.WriteString(guild);

    return &_worldPacket;
}

// SMSG_GUILD_EVENT_LOG_QUERY_RESULTS, 3.4.3 (FUN_1406fe5c0): u32 count, {PackedGuid128 PlayerGUID, PackedGuid128
// OtherGUID, u8 TransactionType, u8 RankID, u32 TransactionDate (seconds ago)}; every field always present
WorldPacket const* WorldPackets::Guild::GuildEventLogQueryResults::Write()
{
    _worldPacket.reserve(4 + Entry.size() * 42);

    _worldPacket << uint32(Entry.size());

    for (GuildEventEntry const& entry : Entry)
    {
        Guid128::WritePlayer(_worldPacket, entry.PlayerGUID);
        Guid128::WritePlayer(_worldPacket, entry.OtherGUID);
        _worldPacket << uint8(entry.TransactionType);
        _worldPacket << uint8(entry.RankID);
        _worldPacket << uint32(entry.TransactionDate);
    }

    return &_worldPacket;
}

// SMSG_GUILD_PERMISSIONS_QUERY_RESULTS, 3.4.3 (inline, handler FUN_141c226d0): u32 RankID, u32 Flags, u32
// WithdrawGoldLimit, u32 NumTabs (purchased), u32 tab count, {u32 Flags, u32 WithdrawItemLimit}
WorldPacket const* WorldPackets::Guild::GuildPermissionsQueryResults::Write()
{
    _worldPacket << uint32(RankID);
    _worldPacket << int32(Flags);
    _worldPacket << int32(WithdrawGoldLimit);
    _worldPacket << int32(NumTabs);
    _worldPacket << uint32(Tab.size());

    for (GuildRankTabPermissions const& tab : Tab)
    {
        _worldPacket << int32(tab.Flags);
        _worldPacket << int32(tab.WithdrawItemLimit);
    }

    return &_worldPacket;
}

// CMSG_GUILD_SET_RANK_PERMISSIONS, 3.4.3 (writer FUN_14075b5b0): u32 RankID, u32 RankOrder, u32 Flags, u32
// WithdrawGoldLimit, 6 x {u32 TabFlags, u32 TabWithdrawItemLimit}, u32 OldFlags, 7-bit name length, flush, the name
void WorldPackets::Guild::GuildSetRankPermissions::Read()
{
    _worldPacket >> RankID;
    _worldPacket >> RankOrder;
    _worldPacket >> Flags;
    _worldPacket >> WithdrawGoldLimit;

    for (uint8 i = 0; i < GUILD_BANK_MAX_TABS; i++)
    {
        _worldPacket >> TabFlags[i];
        _worldPacket >> TabWithdrawItemLimit[i];
    }

    _worldPacket >> OldFlags;
    uint32 length = _worldPacket.ReadBits(7);
    _worldPacket.ResetBitReader();
    RankName = ReadBitString(_worldPacket, length, 15);
}

// CMSG_GUILD_ADD_RANK, 3.4.3 (writer FUN_14075af00): 7-bit name length, flush, u32 RankOrder, the name
void WorldPackets::Guild::GuildAddRank::Read()
{
    uint32 length = _worldPacket.ReadBits(7);
    _worldPacket.ResetBitReader();
    _worldPacket >> RankOrder;
    Name = ReadBitString(_worldPacket, length, 15);
}

// CMSG_GUILD_DELETE_RANK, 3.4.3 (writer FUN_14075b240): u32 RankOrder
void WorldPackets::Guild::GuildDeleteRank::Read()
{
    _worldPacket >> RankOrder;
}

// CMSG_GUILD_UPDATE_INFO_TEXT, 3.4.3 (writer FUN_14075b720): 11-bit length, flush, the text
void WorldPackets::Guild::GuildUpdateInfoText::Read()
{
    uint32 length = _worldPacket.ReadBits(11);
    _worldPacket.ResetBitReader();
    InfoText = ReadBitString(_worldPacket, length, 500);
}

// CMSG_GUILD_SET_MEMBER_NOTE, 3.4.3 (writer FUN_14075b4f0): PackedGuid128 NoteeGUID, 8-bit note length, bit IsPublic,
// flush, the note. Public and officer notes are one message.
void WorldPackets::Guild::GuildSetMemberNote::Read()
{
    NoteeGUID = Guid128::ReadPlayer(_worldPacket);
    uint32 length = _worldPacket.ReadBits(8);
    IsPublic = _worldPacket.ReadBit();
    _worldPacket.ResetBitReader();
    Note = ReadBitString(_worldPacket, length, 31);
}

// CMSG_GUILD_DEMOTE_MEMBER / PROMOTE_MEMBER / OFFICER_REMOVE_MEMBER, 3.4.3: PackedGuid128 member
void WorldPackets::Guild::GuildDemoteMember::Read()
{
    Demotee = Guid128::ReadPlayer(_worldPacket);
}

void WorldPackets::Guild::GuildPromoteMember::Read()
{
    Promotee = Guid128::ReadPlayer(_worldPacket);
}

void WorldPackets::Guild::GuildOfficerRemoveMember::Read()
{
    Removee = Guid128::ReadPlayer(_worldPacket);
}

// CMSG_GUILD_BANK_ACTIVATE, 3.4.3 (writer FUN_14075d720): PackedGuid128 Banker, bit FullUpdate, flush
void WorldPackets::Guild::GuildBankActivate::Read()
{
    Banker = Guid128::Read(_worldPacket);
    FullUpdate = _worldPacket.ReadBit();
}

// CMSG_GUILD_BANK_BUY_TAB, 3.4.3 (writer FUN_14075d790): PackedGuid128 Banker, u8 BankTab
void WorldPackets::Guild::GuildBankBuyTab::Read()
{
    Banker = Guid128::Read(_worldPacket);
    _worldPacket >> BankTab;
}

// CMSG_GUILD_BANK_UPDATE_TAB, 3.4.3 (writer FUN_14075dd30): PackedGuid128 Banker, u8 BankTab, 7-bit name and 9-bit
// icon lengths, flush, the name, the icon
void WorldPackets::Guild::GuildBankUpdateTab::Read()
{
    Banker = Guid128::Read(_worldPacket);
    _worldPacket >> BankTab;
    uint32 nameLength = _worldPacket.ReadBits(7);
    uint32 iconLength = _worldPacket.ReadBits(9);
    _worldPacket.ResetBitReader();
    Name = ReadBitString(_worldPacket, nameLength, 16);
    Icon = ReadBitString(_worldPacket, iconLength, 100);
}

// CMSG_GUILD_BANK_DEPOSIT_MONEY / WITHDRAW_MONEY, 3.4.3 (writers FUN_14075d7e0 / FUN_14075dec0): PackedGuid128
// Banker, u64 Money (AC's money is 32-bit: larger amounts are refused)
void WorldPackets::Guild::GuildBankDepositMoney::Read()
{
    Banker = Guid128::Read(_worldPacket);
    uint64 money = _worldPacket.read<uint64>();
    Money = money > std::numeric_limits<uint32>::max() ? 0 : uint32(money);
}

// CMSG_GUILD_BANK_QUERY_TAB, 3.4.3 (writer FUN_14075dc40): PackedGuid128 Banker, u8 Tab, bit FullUpdate, flush
void WorldPackets::Guild::GuildBankQueryTab::Read()
{
    Banker = Guid128::Read(_worldPacket);
    _worldPacket >> Tab;
    FullUpdate = _worldPacket.ReadBit();
}

// SMSG_GUILD_BANK_REMAINING_WITHDRAW_MONEY, 3.4.3 (raw, handler FUN_141bd3ea0): i64
WorldPacket const* WorldPackets::Guild::GuildBankRemainingWithdrawMoney::Write()
{
    _worldPacket << int64(RemainingWithdrawMoney);

    return &_worldPacket;
}

void WorldPackets::Guild::GuildBankWithdrawMoney::Read()
{
    Banker = Guid128::Read(_worldPacket);
    uint64 money = _worldPacket.read<uint64>();
    Money = money > std::numeric_limits<uint32>::max() ? 0 : uint32(money);
}

// SMSG_GUILD_BANK_QUERY_RESULTS, 3.4.3 (FUN_1406fdec0, handler FUN_141bd3790): u64 Money, u32 Tab, u32
// WithdrawalsRemaining, u32 tab info count, u32 item count, bit FullUpdate, flush, {u32 TabIndex, 7-bit name and
// 9-bit icon lengths, flush, the strings}, {u32 Slot, u32 Count, u32 EnchantmentID, u32 Charges, u32
// OnUseEnchantmentID, u32 Flags, ItemInstance, 2-bit gem count, bit Locked, flush, {u8 SocketIndex, ItemInstance gem}}
WorldPacket const* WorldPackets::Guild::GuildBankQueryResults::Write()
{
    _worldPacket << uint64(Money);
    _worldPacket << uint32(Tab);
    _withdrawalsRemainingPos = _worldPacket.wpos();
    _worldPacket << int32(WithdrawalsRemaining);
    _worldPacket << uint32(TabInfo.size());
    _worldPacket << uint32(ItemInfo.size());
    _worldPacket.WriteBit(FullUpdate);
    _worldPacket.FlushBits();

    for (GuildBankTabInfo const& tab : TabInfo)
    {
        std::string_view name = std::string_view(tab.Name).substr(0, 127);
        std::string_view icon = std::string_view(tab.Icon).substr(0, 511);
        _worldPacket << uint32(tab.TabIndex);
        _worldPacket.WriteBits(name.size(), 7);
        _worldPacket.WriteBits(icon.size(), 9);
        _worldPacket.FlushBits();
        _worldPacket.WriteString(name);
        _worldPacket.WriteString(icon);
    }

    for (GuildBankItemInfo const& item : ItemInfo)
    {
        _worldPacket << uint32(item.Slot);
        _worldPacket << int32(item.Count);
        _worldPacket << int32(item.EnchantmentID);
        _worldPacket << int32(item.Charges);
        _worldPacket << int32(0);                           // OnUseEnchantmentID
        _worldPacket << int32(item.Flags);
        WorldPackets::Item::WriteItemInstance(_worldPacket, item.ItemID, item.RandomPropertiesSeed, item.RandomPropertiesID);
        _worldPacket.WriteBits(std::min<std::size_t>(item.SocketEnchant.size(), 3), 2);
        _worldPacket.WriteBit(false);                       // Locked
        _worldPacket.FlushBits();
        for (std::size_t i = 0; i < item.SocketEnchant.size() && i < 3; ++i)
        {
            _worldPacket << uint8(item.SocketEnchant[i].SocketIndex);
            WorldPackets::Item::WriteItemInstance(_worldPacket, item.SocketEnchant[i].GemItemID);
        }
    }

    return &_worldPacket;
}

void WorldPackets::Guild::GuildBankQueryResults::SetWithdrawalsRemaining(int32 withdrawalsRemaining)
{
    WithdrawalsRemaining = withdrawalsRemaining;
    _worldPacket.put<int32>(_withdrawalsRemainingPos, withdrawalsRemaining);
}

// CMSG_GUILD_BANK_LOG_QUERY, 3.4.3 (writer FUN_14075b010): u32 Tab (6: the money log)
void WorldPackets::Guild::GuildBankLogQuery::Read()
{
    Tab = uint8(std::min<uint32>(_worldPacket.read<uint32>(), 0xFF));
}

// SMSG_GUILD_BANK_LOG_QUERY_RESULTS, 3.4.3 (FUN_1406fdcd0): u32 Tab, u32 count, bit HasWeeklyBonusMoney, flush,
// {PackedGuid128 PlayerGUID, u32 TimeOffset, u8 EntryType, bits HasMoney, HasItemID, HasCount, HasOtherTab, flush,
// [u64 Money], [u32 ItemID], [u32 Count], [u8 OtherTab]}, [u64 WeeklyBonusMoney]
WorldPacket const* WorldPackets::Guild::GuildBankLogQueryResults::Write()
{
    _worldPacket << uint32(Tab);
    _worldPacket << uint32(Entry.size());
    _worldPacket.WriteBit(false);
    _worldPacket.FlushBits();

    for (GuildBankLogEntry const& logEntry : Entry)
    {
        bool hasItem = false, hasOtherTab = false;
        switch (logEntry.EntryType)
        {
            case GUILD_BANK_LOG_DEPOSIT_ITEM:
            case GUILD_BANK_LOG_WITHDRAW_ITEM:
                hasItem = true;
                break;
            case GUILD_BANK_LOG_MOVE_ITEM:
            case GUILD_BANK_LOG_MOVE_ITEM2:
                hasItem = true;
                hasOtherTab = true;
                break;
            default:
                break;
        }

        Guid128::WritePlayer(_worldPacket, logEntry.PlayerGUID);
        _worldPacket << uint32(logEntry.TimeOffset);
        _worldPacket << int8(logEntry.EntryType);
        _worldPacket.WriteBit(!hasItem);                    // HasMoney
        _worldPacket.WriteBit(hasItem);                     // HasItemID
        _worldPacket.WriteBit(hasItem);                     // HasCount
        _worldPacket.WriteBit(hasOtherTab);
        _worldPacket.FlushBits();
        if (!hasItem)
            _worldPacket << uint64(logEntry.Money);
        if (hasItem)
        {
            _worldPacket << int32(logEntry.ItemID);
            _worldPacket << int32(logEntry.Count);
        }
        if (hasOtherTab)
            _worldPacket << int8(logEntry.OtherTab);
    }

    return &_worldPacket;
}

// CMSG_GUILD_BANK_TEXT_QUERY, 3.4.3 (writer FUN_14075b130): u32 Tab
void WorldPackets::Guild::GuildBankTextQuery::Read()
{
    Tab = uint8(std::min<uint32>(_worldPacket.read<uint32>(), 0xFF));
}

// SMSG_GUILD_BANK_TEXT_QUERY_RESULT, 3.4.3 (inline, handler 0x141bd3ec0): u32 Tab, 14-bit length, flush, the text
WorldPacket const* WorldPackets::Guild::GuildBankTextQueryResult::Write()
{
    std::string_view text = std::string_view(Text).substr(0, 0x3FFF);
    _worldPacket << uint32(Tab);
    _worldPacket.WriteBits(text.size(), 14);
    _worldPacket.FlushBits();
    _worldPacket.WriteString(text);

    return &_worldPacket;
}

// CMSG_GUILD_BANK_SET_TAB_TEXT, 3.4.3 (writer FUN_14075b070): u32 Tab, 14-bit length, flush, the text
void WorldPackets::Guild::GuildBankSetTabText::Read()
{
    Tab = uint8(std::min<uint32>(_worldPacket.read<uint32>(), 0xFF));
    uint32 length = _worldPacket.ReadBits(14);
    _worldPacket.ResetBitReader();
    TabText = ReadBitString(_worldPacket, length, 500);
}

// CMSG_GUILD_SET_GUILD_MASTER, 3.4.3 (writer FUN_140767380): 9-bit name length, flush, the name
void WorldPackets::Guild::GuildSetGuildMaster::Read()
{
    uint32 length = _worldPacket.ReadBits(9);
    _worldPacket.ResetBitReader();
    NewMasterName = _worldPacket.ReadString(length);
    if (std::size_t dash = NewMasterName.find('-'); dash != std::string::npos)
        NewMasterName.resize(dash);
}

// CMSG_SAVE_GUILD_EMBLEM, 3.4.3 (writer FUN_140763b30): PackedGuid128 Vendor, u32 EmblemStyle, EmblemColor,
// BorderStyle, BorderColor, BackgroundColor
void WorldPackets::Guild::SaveGuildEmblem::Read()
{
    Vendor = Guid128::Read(_worldPacket);
    _worldPacket >> EStyle;
    _worldPacket >> EColor;
    _worldPacket >> BStyle;
    _worldPacket >> BColor;
    _worldPacket >> Bg;
}

// SMSG_PLAYER_SAVE_GUILD_EMBLEM, 3.4.3 (raw, handler FUN_141714960): u32 Error (AC's GuildEmblemError)
WorldPacket const* WorldPackets::Guild::PlayerSaveGuildEmblem::Write()
{
    _worldPacket << int32(Error);

    return &_worldPacket;
}
