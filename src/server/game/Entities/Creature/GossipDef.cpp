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

#include "GossipDef.h"
#include "Formulas.h"
#include "ItemPackets.h"
#include "Object.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

GossipMenu::GossipMenu()
{
    _menuId = 0;
    _locale = DEFAULT_LOCALE;
    _senderGUID.Clear();
}

GossipMenu::~GossipMenu()
{
    ClearMenu();
}

void GossipMenu::AddMenuItem(int32 menuItemId, uint8 icon, std::string const& message, uint32 sender, uint32 action, std::string const& boxMessage, uint32 boxMoney, bool coded /*= false*/)
{
    ASSERT(_menuItems.size() <= GOSSIP_MAX_MENU_ITEMS);

    // Find a free new id - script case
    if (menuItemId == -1)
    {
        menuItemId = 0;
        if (!_menuItems.empty())
        {
            for (GossipMenuItemContainer::const_iterator itr = _menuItems.begin(); itr != _menuItems.end(); ++itr)
            {
                if (int32(itr->first) > menuItemId)
                    break;

                menuItemId = itr->first + 1;
            }
        }
    }

    GossipMenuItem& menuItem = _menuItems[menuItemId];

    menuItem.MenuItemIcon    = icon;
    menuItem.Message         = message;
    menuItem.IsCoded         = coded;
    menuItem.Sender          = sender;
    menuItem.OptionType      = action;
    menuItem.BoxMessage      = boxMessage;
    menuItem.BoxMoney        = boxMoney;
}

/**
 * @name AddMenuItem
 * @brief Adds a localized gossip menu item from db by menu id and menu item id.
 * @param menuId Gossip menu id.
 * @param menuItemId Gossip menu item id.
 * @param sender Identifier of the current menu.
 * @param action Custom action given to OnGossipHello.
 * @param boxMoney Custom price for pop-up box. If > 0, it will replace DB value.
 */
void GossipMenu::AddMenuItem(uint32 menuId, uint32 menuItemId, uint32 sender, uint32 action, uint32 boxMoney)
{
    /// Find items for given menu id.
    GossipMenuItemsMapBounds bounds = sObjectMgr->GetGossipMenuItemsMapBounds(menuId);
    /// Return if there are none.
    if (bounds.first == bounds.second)
        return;

    /// Iterate over each of them.
    for (GossipMenuItemsContainer::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        /// Find the one with the given menu item id.
        if (itr->second.OptionID != menuItemId)
            continue;

        /// Store texts for localization.
        std::string strOptionText, strBoxText;
        BroadcastText const* optionBroadcastText = sObjectMgr->GetBroadcastText(itr->second.OptionBroadcastTextID);
        BroadcastText const* boxBroadcastText = sObjectMgr->GetBroadcastText(itr->second.BoxBroadcastTextID);

        /// OptionText
        if (optionBroadcastText)
            ObjectMgr::GetLocaleString(optionBroadcastText->MaleText, GetLocale(), strOptionText);
        else
            strOptionText = itr->second.OptionText;

        /// BoxText
        if (boxBroadcastText)
            ObjectMgr::GetLocaleString(boxBroadcastText->MaleText, GetLocale(), strBoxText);
        else
            strBoxText = itr->second.BoxText;

        /// Check need of localization.
        if (GetLocale() != DEFAULT_LOCALE)
        {
            if (!optionBroadcastText)
            {
                /// Find localizations from database.
                if (GossipMenuItemsLocale const* gossipMenuLocale = sObjectMgr->GetGossipMenuItemsLocale(MAKE_PAIR32(menuId, menuItemId)))
                    ObjectMgr::GetLocaleString(gossipMenuLocale->OptionText, GetLocale(), strOptionText);
            }

            if (!boxBroadcastText)
            {
                /// Find localizations from database.
                if (GossipMenuItemsLocale const* gossipMenuLocale = sObjectMgr->GetGossipMenuItemsLocale(MAKE_PAIR32(menuId, menuItemId)))
                    ObjectMgr::GetLocaleString(gossipMenuLocale->BoxText, GetLocale(), strBoxText);
            }
        }

        /// Add menu item with existing method. Menu item id -1 is also used in ADD_GOSSIP_ITEM macro.
        AddMenuItem(-1, itr->second.OptionIcon, strOptionText, sender, action, strBoxText, boxMoney ? boxMoney : itr->second.BoxMoney, itr->second.BoxCoded);
    }
}

void GossipMenu::AddGossipMenuItemData(uint32 menuItemId, uint32 gossipActionMenuId, uint32 gossipActionPoi)
{
    GossipMenuItemData& itemData = _menuItemData[menuItemId];

    itemData.GossipActionMenuId  = gossipActionMenuId;
    itemData.GossipActionPoi     = gossipActionPoi;
}

uint32 GossipMenu::GetMenuItemSender(uint32 menuItemId) const
{
    GossipMenuItemContainer::const_iterator itr = _menuItems.find(menuItemId);
    if (itr == _menuItems.end())
        return 0;

    return itr->second.Sender;
}

uint32 GossipMenu::GetMenuItemAction(uint32 menuItemId) const
{
    GossipMenuItemContainer::const_iterator itr = _menuItems.find(menuItemId);
    if (itr == _menuItems.end())
        return 0;

    return itr->second.OptionType;
}

bool GossipMenu::IsMenuItemCoded(uint32 menuItemId) const
{
    GossipMenuItemContainer::const_iterator itr = _menuItems.find(menuItemId);
    if (itr == _menuItems.end())
        return false;

    return itr->second.IsCoded;
}

void GossipMenu::ClearMenu()
{
    _menuItems.clear();
    _menuItemData.clear();
}

PlayerMenu::PlayerMenu(WorldSession* session) : _session(session)
{
    _gossipMenu.SetLocale(session->GetSessionDbLocaleIndex());
}

PlayerMenu::~PlayerMenu()
{
    ClearMenus();
}

void PlayerMenu::ClearMenus()
{
    _gossipMenu.ClearMenu();
    _questMenu.ClearMenu();
}

/// ClientGossipText, 3.4.3 (FUN_1406fdb90): u32 QuestID, u32 ContentTuningID, u32 QuestType (the quest's
/// icon), u32 QuestLevel, u32 QuestMaxScalingLevel, u32 QuestFlags, u32 QuestFlagsEx, bit Repeatable, bit
/// (unused), 9-bit title length, flush, the title. Shared by the gossip menu and the quest list.
static void WriteClientGossipText(ByteBuffer& data, Quest const* quest, uint8 icon, WorldSession* session)
{
    std::string title = quest->GetTitle();
    LocaleConstant locale = session->GetSessionDbLocaleIndex();
    if (locale != LOCALE_enUS)
        if (QuestLocale const* localeData = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
            ObjectMgr::GetLocaleString(localeData->Title, locale, title);
    std::string_view titleView = std::string_view(title).substr(0, std::min<std::size_t>(title.size(), 0x1FF));

    data << uint32(quest->GetQuestId());
    data << uint32(0);                                      // ContentTuningID
    data << uint32(icon);
    data << int32(quest->GetQuestLevel());
    data << uint32(0);                                      // QuestMaxScalingLevel
    data << uint32(quest->GetFlags());
    data << uint32(0);                                      // QuestFlagsEx
    // 3.3.3: repeatable quests show a blue question mark or yellow exclamation mark
    data.WriteBit(quest->IsRepeatable() && !quest->IsDailyOrWeekly() && !quest->IsMonthly());
    data.WriteBit(false);
    data.WriteBits(titleView.size(), 9);
    data.FlushBits();
    data.WriteString(titleView);
}

void PlayerMenu::SendGossipMenu(uint32 titleTextId, ObjectGuid objectGUID)
{
    _gossipMenu.SetSenderGUID(objectGUID);

    // SMSG_GOSSIP_MESSAGE, 3.4.3 (FUN_1406fd730): PackedGuid128 GossipGUID, u32 GossipID, u32
    // FriendshipFactionID, u32 option count, u32 quest count, bits HasTextID, HasBroadcastTextID, flush,
    // the options, [u32 TextID], [u32 BroadcastTextID], the quests (ClientGossipText). The text is the
    // npc_text id the client queries.
    std::vector<Quest const*> quests;
    std::vector<uint8> icons;
    for (uint32 i = 0; i < _questMenu.GetMenuItemCount(); ++i)
    {
        QuestMenuItem const& item = _questMenu.GetItem(i);
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(item.QuestId))
        {
            quests.push_back(quest);
            icons.push_back(item.QuestIcon);
        }
    }

    WorldPacket data(SMSG_GOSSIP_MESSAGE, 40 + _gossipMenu.GetMenuItemCount() * 100 + quests.size() * 75);
    Guid128::Write(data, objectGUID);
    data << uint32(_gossipMenu.GetMenuId());
    data << uint32(0);                                      // FriendshipFactionID
    data << uint32(_gossipMenu.GetMenuItemCount());
    data << uint32(quests.size());
    data.WriteBit(true);                                    // HasTextID
    data.WriteBit(false);                                   // HasBroadcastTextID
    data.FlushBits();

    // ClientGossipOptions (FUN_1406fd8d0): u32 GossipOptionID, u8 OptionNPC (3.3.5's icons 0-10 keep their
    // meaning), u8 OptionFlags (1: asks for a code), i32 OptionCost, u32 OptionLanguage, u32 Flags, u32
    // OrderIndex, 12-bit text length, 12-bit confirm length, 2-bit Status, bits HasSpellID,
    // HasOverrideIconID, flush, u32 treasure count, the text, the confirm text
    uint32 orderIndex = 0;
    for (GossipMenuItemContainer::const_iterator itr = _gossipMenu.GetMenuItems().begin(); itr != _gossipMenu.GetMenuItems().end(); ++itr)
    {
        GossipMenuItem const& item = itr->second;
        std::string_view text = std::string_view(item.Message).substr(0, std::min<std::size_t>(item.Message.size(), 0xFFF));
        std::string_view confirm = std::string_view(item.BoxMessage).substr(0, std::min<std::size_t>(item.BoxMessage.size(), 0xFFF));
        data << uint32(itr->first);
        data << uint8(item.MenuItemIcon);
        data << uint8(item.IsCoded ? 1 : 0);
        data << int32(item.BoxMoney);                       // money required to open menu, 2.0.3
        data << uint32(0);                                  // OptionLanguage
        data << uint32(0);                                  // Flags
        data << uint32(orderIndex++);
        data.WriteBits(text.size(), 12);
        data.WriteBits(confirm.size(), 12);
        data.WriteBits(0, 2);                               // Status
        data.WriteBit(false);                               // HasSpellID
        data.WriteBit(false);                               // HasOverrideIconID
        data.FlushBits();
        data << uint32(0);                                  // Treasure items
        data.WriteString(text);
        data.WriteString(confirm);
    }

    data << uint32(titleTextId);                            // TextID

    for (std::size_t i = 0; i < quests.size(); ++i)
        WriteClientGossipText(data, quests[i], icons[i], _session);

    _session->SendPacket(&data);
}

void PlayerMenu::SendCloseGossip()
{
    _gossipMenu.SetSenderGUID(ObjectGuid::Empty);

    // SMSG_GOSSIP_COMPLETE, 3.4.3: bit SuppressSound, flush
    WorldPacket data(SMSG_GOSSIP_COMPLETE, 1);
    data.WriteBit(false);
    data.FlushBits();
    _session->SendPacket(&data);
}

void PlayerMenu::SendPointOfInterest(uint32 poiId) const
{
    PointOfInterest const* poi = sObjectMgr->GetPointOfInterest(poiId);
    if (!poi)
    {
        LOG_ERROR("sql.sql", "Request to send non-existing POI (Id: {}), ignored.", poiId);
        return;
    }

    std::string name = poi->Name;
    int32 locale = _session->GetSessionDbLocaleIndex();
    if (PointOfInterestLocale const* localeData = sObjectMgr->GetPointOfInterestLocale(poiId))
        ObjectMgr::GetLocaleString(localeData->Name, locale, name);
    std::string_view nameView = std::string_view(name).substr(0, std::min<std::size_t>(name.size(), 0x3F));

    // SMSG_GOSSIP_POI, 3.4.3 (FUN_1406c1880): u32 ID, u32 Flags, f32 X, Y, Z, u32 Icon, u32 Importance,
    // u32 WMOGroupID, 6-bit name length, flush, the name
    WorldPacket data(SMSG_GOSSIP_POI, 4 * 8 + 1 + nameView.size());
    data << uint32(poiId);
    data << uint32(poi->Flags);
    data << float(poi->PositionX);
    data << float(poi->PositionY);
    data << float(0.0f);
    data << uint32(poi->Icon);
    data << uint32(poi->Importance);
    data << uint32(0);                                      // WMOGroupID
    data.WriteBits(nameView.size(), 6);
    data.FlushBits();
    data.WriteString(nameView);

    _session->SendPacket(&data);
}

/*********************************************************/
/***                    QUEST SYSTEM                   ***/
/*********************************************************/

QuestMenu::QuestMenu()
{
    _questMenuItems.reserve(16);                                   // can be set for max from most often sizes to speedup push_back and less memory use
}

QuestMenu::~QuestMenu()
{
    ClearMenu();
}

void QuestMenu::AddMenuItem(uint32 QuestId, uint8 Icon)
{
    if (!sObjectMgr->GetQuestTemplate(QuestId))
        return;

    ASSERT(_questMenuItems.size() <= GOSSIP_MAX_MENU_ITEMS);

    QuestMenuItem questMenuItem;

    questMenuItem.QuestId        = QuestId;
    questMenuItem.QuestIcon      = Icon;

    _questMenuItems.push_back(questMenuItem);
}

bool QuestMenu::HasItem(uint32 questId) const
{
    for (QuestMenuItemList::const_iterator i = _questMenuItems.begin(); i != _questMenuItems.end(); ++i)
        if (i->QuestId == questId)
            return true;

    return false;
}

void QuestMenu::ClearMenu()
{
    _questMenuItems.clear();
}

void PlayerMenu::SendQuestGiverQuestList(QEmote const& eEmote, std::string const& Title, ObjectGuid guid)
{
    std::string greeting = Title;
    uint32 emoteDelay = eEmote._Delay;
    uint32 emoteType = eEmote._Emote;
    if (QuestGreeting const* questGreeting = sObjectMgr->GetQuestGreeting(guid.GetTypeId(), guid.GetEntry()))
    {
        greeting.clear();
        // Check if greeting exists. Blizzlike that some creatures have empty greeting
        if (!questGreeting->Greeting.empty())
        {
            LocaleConstant locale = _session->GetSessionDbLocaleIndex();
            if (questGreeting->Greeting.size() > size_t(locale))
                greeting = questGreeting->Greeting[locale];
            else
                greeting = questGreeting->Greeting[DEFAULT_LOCALE];
        }
        emoteDelay = questGreeting->EmoteDelay;
        emoteType = questGreeting->EmoteType;
    }
    std::string_view greetingView = std::string_view(greeting).substr(0, std::min<std::size_t>(greeting.size(), 0x7FF));

    std::vector<std::pair<Quest const*, uint8>> quests;
    for (uint32 i = 0; i < _questMenu.GetMenuItemCount(); ++i)
    {
        QuestMenuItem const& questMenuItem = _questMenu.GetItem(i);
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questMenuItem.QuestId))
            quests.push_back({ quest, questMenuItem.QuestIcon });
    }

    // SMSG_QUEST_GIVER_QUEST_LIST_MESSAGE, 3.4.3 (inline in the quest switch): PackedGuid128 QuestGiverGUID,
    // u32 GreetEmoteDelay, u32 GreetEmoteType, u32 quest count, 11-bit greeting length, flush, the quests
    // (ClientGossipText), the greeting
    WorldPacket data(SMSG_QUEST_GIVER_QUEST_LIST_MESSAGE, 32 + quests.size() * 64 + greetingView.size());
    Guid128::Write(data, guid);
    data << uint32(emoteDelay);
    data << uint32(emoteType);
    data << uint32(quests.size());
    data.WriteBits(greetingView.size(), 11);
    data.FlushBits();
    for (auto const& [quest, icon] : quests)
        WriteClientGossipText(data, quest, icon, _session);
    data.WriteString(greetingView);

    _session->SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Sent SMSG_QUEST_GIVER_QUEST_LIST_MESSAGE (QuestGiver: {})", guid.ToString());
}

void PlayerMenu::SendQuestGiverStatus(uint8 questStatus, ObjectGuid npcGUID) const
{
    // SMSG_QUEST_GIVER_STATUS, 3.4.3: PackedGuid128 QuestGiver, u64 Status
    WorldPacket data(SMSG_QUEST_GIVER_STATUS, 26);
    Guid128::Write(data, npcGUID);
    data << uint64(ClientQuestGiverStatus(questStatus));

    _session->SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Sent SMSG_QUEST_GIVER_STATUS NPC {}, status={}", npcGUID.ToString(), questStatus);
}

/// One quest objective as 3.4.3 lists them (QuestObjective in SMSG_QUERY_QUEST_INFO_RESPONSE and in
/// SMSG_QUEST_GIVER_QUEST_DETAILS). AC's quests keep 3.3.5's fixed slots; they become objectives here:
/// the creature/game object slots 0-3 with storage index 0-3 (the quest log's ObjectiveProgress
/// counters, PlayerData QuestLog), the items, the player kills (the PvP slot), the reputations and
/// the money with storage index -1 (the client counts those itself). An objective's id is the quest
/// id << 4 | its position.
struct QuestObjective343
{
    uint32 ID;
    uint8 Type;                 // QuestObjectiveType [names inferred]: 0 monster, 1 item, 2 game object,
                                // 6 minimum reputation, 7 maximum reputation, 8 money, 9 player kills
    int8 StorageIndex;
    int32 ObjectID;
    int32 Amount;
    std::string Description;
};

static std::vector<QuestObjective343> BuildQuestObjectives(Quest const* quest, std::string const* objectiveTexts)
{
    std::vector<QuestObjective343> objectives;
    uint32 position = 0;
    auto add = [&](uint8 type, int8 storageIndex, int32 objectId, int32 amount, std::string description)
    {
        objectives.push_back({ (quest->GetQuestId() << 4) | (position++ & 0xF), type, storageIndex, objectId, amount, std::move(description) });
    };

    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        if (!quest->RequiredNpcOrGo[i])
            continue;
        if (quest->RequiredNpcOrGo[i] > 0)
            add(0, int8(i), quest->RequiredNpcOrGo[i], int32(quest->RequiredNpcOrGoCount[i]), objectiveTexts[i]);
        else
            add(2, int8(i), -quest->RequiredNpcOrGo[i], int32(quest->RequiredNpcOrGoCount[i]), objectiveTexts[i]);
    }
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i])
            add(1, -1, int32(quest->RequiredItemId[i]), int32(quest->RequiredItemCount[i]), "");
    if (quest->GetPlayersSlain())
        add(9, int8(QUEST_PVP_KILL_SLOT), 0, int32(quest->GetPlayersSlain()), "");
    if (quest->GetRepObjectiveFaction())
        add(6, -1, int32(quest->GetRepObjectiveFaction()), quest->GetRepObjectiveValue(), "");
    if (quest->GetRepObjectiveFaction2())
        add(6, -1, int32(quest->GetRepObjectiveFaction2()), quest->GetRepObjectiveValue2(), "");
    if (quest->GetRewOrReqMoney() < 0)
        add(8, -1, 0, -quest->GetRewOrReqMoney(), "");
    return objectives;
}

/// QuestRewards, 3.4.3 (FUN_14071d9b0): u32 ChoiceItemCount, u32 ItemCount, {u32 ItemID, u32 Qty} x4, u32
/// Money, u32 XP, u64 ArtifactXP, u32 ArtifactCategoryID, u32 Honor, u32 Title, u32 FactionFlags, {u32
/// FactionID, u32 Value, u32 Override, u32 CapIn} x5, u32 SpellCompletionDisplayID x3, u32
/// SpellCompletionID, {u32 CurrencyID, u32 Qty} x4, u32 SkillLineID, u32 NumSkillUps, u32
/// TreasurePickerID, {2-bit LootItemType, flush, ItemInstance, u32 Quantity} x6, bit IsBoostSpell, flush.
static void WriteQuestRewards(ByteBuffer& data, Quest const* quest, Player* player)
{
    bool hidden = quest->HasFlag(QUEST_FLAGS_HIDDEN_REWARDS);

    uint32 money = 0;
    uint32 xp = 0;
    if (!hidden)
    {
        if (player && (player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) || sScriptMgr->OnPlayerShouldBeRewardedWithMoneyInsteadOfExp(player)))
            money = quest->GetRewMoneyMaxLevel();
        int32 levelMoney = quest->GetRewOrReqMoney(player ? player->GetLevel() : 0);   // reward money (below max lvl)
        if (levelMoney > 0)
            money += uint32(levelMoney);

        if (player && !sScriptMgr->OnPlayerShouldBeRewardedWithMoneyInsteadOfExp(player))
            xp = player->CalculateQuestRewardXP(quest);
        sScriptMgr->OnPlayerQuestComputeXP(player, quest, xp);
    }

    data << uint32(hidden ? 0 : quest->GetRewChoiceItemsCount());
    data << uint32(hidden ? 0 : quest->GetRewItemsCount());
    for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
    {
        data << uint32(hidden ? 0 : quest->RewardItemId[i]);
        data << uint32(hidden ? 0 : quest->RewardItemIdCount[i]);
    }
    data << uint32(money);
    data << uint32(xp);
    data << uint64(0);                                      // ArtifactXP
    data << uint32(0);                                      // ArtifactCategoryID
    data << uint32(quest->CalculateHonorGain(player ? player->GetQuestLevel(quest) : 0));
    data << uint32(quest->GetCharTitleId());
    data << uint32(0);                                      // FactionFlags
    for (uint32 i = 0; i < QUEST_REPUTATIONS_COUNT; ++i)
    {
        data << uint32(quest->RewardFactionId[i]);
        data << int32(quest->RewardFactionValueId[i]);
        data << int32(quest->RewardFactionValueIdOverride[i]);
        data << uint32(0);                                  // FactionCapIn
    }
    data << uint32(quest->GetRewSpell());                   // SpellCompletionDisplayID: shown (cast if no SpellCompletionID)
    data << uint32(0);
    data << uint32(0);
    data << int32(quest->GetRewSpellCast());                // SpellCompletionID
    for (uint32 i = 0; i < 4; ++i)
        data << uint32(0) << uint32(0);                     // currencies
    data << uint32(0);                                      // SkillLineID
    data << uint32(0);                                      // NumSkillUps
    data << uint32(0);                                      // TreasurePickerID
    for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
    {
        data.WriteBits(0, 2);                               // LootItemType: item
        data.FlushBits();
        WorldPackets::Item::WriteItemInstance(data, hidden ? 0 : quest->RewardChoiceItemId[i]);
        data << uint32(hidden ? 0 : quest->RewardChoiceItemCount[i]);
    }
    data.WriteBit(false);                                   // IsBoostSpell
    data.FlushBits();
}

void PlayerMenu::SendQuestGiverQuestDetails(Quest const* quest, ObjectGuid npcGUID, bool activateAccept) const
{
    std::string questTitle           = quest->GetTitle();
    std::string questDetails         = quest->GetDetails();
    std::string questObjectives      = quest->GetObjectives();
    std::string questAreaDescription = quest->GetAreaDescription();

    int32 locale = _session->GetSessionDbLocaleIndex();
    if (QuestLocale const* localeData = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
    {
        ObjectMgr::GetLocaleString(localeData->Title, locale, questTitle);
        ObjectMgr::GetLocaleString(localeData->Details, locale, questDetails);
        ObjectMgr::GetLocaleString(localeData->Objectives, locale, questObjectives);
        ObjectMgr::GetLocaleString(localeData->AreaDescription, locale, questAreaDescription);
    }

    auto clamp = [](std::string const& s, std::size_t max) { return std::string_view(s).substr(0, std::min(s.size(), max)); };
    std::string_view title = clamp(questTitle, 0x1FF), details = clamp(questDetails, 0xFFF), objectivesText = clamp(questObjectives, 0xFFF);

    std::vector<QuestObjective343> objectives = BuildQuestObjectives(quest, quest->ObjectiveText);
    uint32 emoteCount = 0;
    for (uint32 i = 0; i < QUEST_EMOTE_COUNT; ++i)
        if (quest->DetailsEmote[i])
            emoteCount = i + 1;

    // SMSG_QUEST_GIVER_QUEST_DETAILS, 3.4.3 (FUN_140703860): PackedGuid128 QuestGiverGUID, PackedGuid128
    // InformUnit (who shared the quest), u32 QuestID, u32 QuestPackageID, u32 PortraitGiver, PortraitGiverMount,
    // PortraitGiverModelSceneID, PortraitTurnIn, u32 QuestFlags[3], u32 SuggestedPartyMembers, u32 counts of
    // learned spells, emotes and objectives, u32 QuestStartItemID, u32 QuestSessionBonus, u32
    // QuestGiverCreatureID, u32 conditional text count, the spells, the emotes {Type, Delay}, the objectives
    // {u32 ID, u32 ObjectID, u32 Amount, u8 Type}, bits: 9 title, 12 description, 12 log description, 10/8/10/8
    // portrait texts and names, AutoLaunched, FromContentPush, StartCheat, DisplayPopup; flush; QuestRewards;
    // the seven strings; the conditional texts
    WorldPacket data(SMSG_QUEST_GIVER_QUEST_DETAILS, 600 + title.size() + details.size() + objectivesText.size());
    Guid128::Write(data, npcGUID);
    Guid128::Write(data, _session->GetPlayer()->GetDivider());
    data << uint32(quest->GetQuestId());
    data << uint32(0);                                      // QuestPackageID
    data << uint32(0) << uint32(0) << uint32(0) << uint32(0);   // portraits
    data << uint32(quest->GetFlags());
    data << uint32(0);
    data << uint32(0);
    data << uint32(quest->GetSuggestedPlayers());
    data << uint32(0);                                      // LearnSpells
    data << uint32(emoteCount);
    data << uint32(objectives.size());
    data << uint32(0);                                      // QuestStartItemID
    data << uint32(0);                                      // QuestSessionBonus
    data << uint32(npcGUID.GetEntry());                     // QuestGiverCreatureID
    data << uint32(0);                                      // conditional descriptions
    for (uint32 i = 0; i < emoteCount; ++i)
    {
        data << uint32(quest->DetailsEmote[i]);
        data << uint32(quest->DetailsEmoteDelay[i]);       // DetailsEmoteDelay (in ms)
    }
    for (QuestObjective343 const& objective : objectives)
    {
        data << uint32(objective.ID);
        data << int32(objective.ObjectID);
        data << int32(objective.Amount);
        data << uint8(objective.Type);
    }
    data.WriteBits(title.size(), 9);
    data.WriteBits(details.size(), 12);
    data.WriteBits(objectivesText.size(), 12);
    data.WriteBits(0, 10);                                  // PortraitGiverText
    data.WriteBits(0, 8);                                   // PortraitGiverName
    data.WriteBits(0, 10);                                  // PortraitTurnInText
    data.WriteBits(0, 8);                                   // PortraitTurnInName
    data.WriteBit(activateAccept);                          // AutoLaunched
    data.WriteBit(false);                                   // FromContentPush
    data.WriteBit(false);                                   // StartCheat
    data.WriteBit(false);                                   // DisplayPopup (with flag 0x80000: accept at once)
    data.FlushBits();
    WriteQuestRewards(data, quest, _session->GetPlayer());
    data.WriteString(title);
    data.WriteString(details);
    data.WriteString(objectivesText);
    _session->SendPacket(&data);

    LOG_DEBUG("network", "WORLD: Sent SMSG_QUEST_GIVER_QUEST_DETAILS {}, questid={}", npcGUID.ToString(), quest->GetQuestId());
}

void PlayerMenu::SendQuestQueryResponse(Quest const* quest) const
{
    std::string questTitle           = quest->GetTitle();
    std::string questDetails         = quest->GetDetails();
    std::string questObjectives      = quest->GetObjectives();
    std::string questAreaDescription = quest->GetAreaDescription();
    std::string questCompletedText   = quest->GetCompletedText();

    std::string questObjectiveText[QUEST_OBJECTIVES_COUNT];
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        questObjectiveText[i] = quest->ObjectiveText[i];

    int32 locale = _session->GetSessionDbLocaleIndex();
    if (QuestLocale const* localeData = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
    {
        ObjectMgr::GetLocaleString(localeData->Title, locale, questTitle);
        ObjectMgr::GetLocaleString(localeData->Details, locale, questDetails);
        ObjectMgr::GetLocaleString(localeData->Objectives, locale, questObjectives);
        ObjectMgr::GetLocaleString(localeData->AreaDescription, locale, questAreaDescription);
        ObjectMgr::GetLocaleString(localeData->CompletedText, locale, questCompletedText);

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            ObjectMgr::GetLocaleString(localeData->ObjectiveText[i], locale, questObjectiveText[i]);
    }

    auto clamp = [](std::string const& s, std::size_t max) { return std::string_view(s).substr(0, std::min(s.size(), max)); };
    std::string_view logTitle = clamp(questTitle, 0x1FF), logDescription = clamp(questObjectives, 0xFFF);
    std::string_view description = clamp(questDetails, 0xFFF), areaDescription = clamp(questAreaDescription, 0x1FF);
    std::string_view completionLog = clamp(questCompletedText, 0x7FF);

    std::vector<QuestObjective343> objectives = BuildQuestObjectives(quest, questObjectiveText);
    bool hidden = quest->HasFlag(QUEST_FLAGS_HIDDEN_REWARDS);
    Player* player = _session->GetPlayer();

    // SMSG_QUERY_QUEST_INFO_RESPONSE, 3.4.3 (quest switch, inline): u32 QuestID, bit Allow, flush, then
    // QuestInfo (FUN_14070e470): 108 u32 of quest data in the order below, u64 TimeAllowed, u32 objective
    // count, u64 AllowableRaces, u32 TreasurePickerID, u32 Expansion, u32 ManagedWorldStateID, two u32
    // conditional text counts, nine string lengths (9/12/12/9/10/8/10/8/11 bits), flush, the objectives
    // {u32 ID, u8 Type, i8 StorageIndex, i32 ObjectID, i32 Amount, u32 Flags, u32 Flags2, f32
    // ProgressBarWeight, u32 VisualEffects count, the effects, u8 description length, description}, the
    // nine strings, the conditional texts
    WorldPacket data(SMSG_QUERY_QUEST_INFO_RESPONSE, 600 + logTitle.size() + logDescription.size() + description.size());
    data << uint32(quest->GetQuestId());
    data.WriteBit(true);                                    // Allow
    data.FlushBits();

    data << uint32(quest->GetQuestId());
    data << int32(quest->GetQuestMethod());                 // QuestType: 0 == IsAutoComplete() (skip objectives/details)
    data << int32(quest->GetQuestLevel());
    data << int32(0);                                       // QuestScalingFactionGroup
    data << int32(0);                                       // QuestMaxScalingLevel
    data << int32(0);                                       // QuestPackageID
    data << int32(quest->GetMinLevel());
    data << int32(quest->GetZoneOrSort());                  // QuestSortID
    data << int32(quest->GetType());                        // QuestInfoID
    data << uint32(quest->GetSuggestedPlayers());
    data << uint32(quest->GetNextQuestInChain());           // RewardNextQuest
    data << uint32(quest->GetXPId());                       // RewardXPDifficulty
    data << float(1.0f);                                    // RewardXPMultiplier
    int32 rewardMoney = 0;
    if (!hidden)
    {
        if (player && (player->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) || sScriptMgr->OnPlayerShouldBeRewardedWithMoneyInsteadOfExp(player)))
            rewardMoney = quest->GetRewMoneyMaxLevel();
        rewardMoney += quest->GetRewOrReqMoney(player ? player->GetLevel() : 0);
    }
    data << int32(rewardMoney);
    data << uint32(0);                                      // RewardMoneyDifficulty
    data << float(1.0f);                                    // RewardMoneyMultiplier
    data << uint32(quest->GetRewMoneyMaxLevel());           // RewardBonusMoney
    data << uint32(quest->GetRewSpell());                   // RewardDisplaySpell x3
    data << uint32(0);
    data << uint32(0);
    data << int32(quest->GetRewSpellCast());                // RewardSpell
    data << uint32(quest->GetRewHonorAddition());
    data << float(quest->GetRewHonorMultiplier());          // RewardKillHonor
    data << uint32(0);                                      // RewardArtifactXPDifficulty
    data << float(1.0f);                                    // RewardArtifactXPMultiplier
    data << uint32(0);                                      // RewardArtifactCategoryID
    data << uint32(quest->GetSrcItemId());                  // StartItem
    data << uint32(quest->GetFlags());
    data << uint32(0);                                      // FlagsEx
    data << uint32(0);                                      // FlagsEx2
    for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
    {
        data << uint32(hidden ? 0 : quest->RewardItemId[i]);
        data << uint32(hidden ? 0 : quest->RewardItemIdCount[i]);
        data << uint32(i < QUEST_SOURCE_ITEM_IDS_COUNT ? quest->ItemDrop[i] : 0);
        data << uint32(i < QUEST_SOURCE_ITEM_IDS_COUNT ? quest->ItemDropQuantity[i] : 0);
    }
    for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
    {
        uint32 item = hidden ? 0 : quest->RewardChoiceItemId[i];
        data << uint32(item);
        data << uint32(hidden ? 0 : quest->RewardChoiceItemCount[i]);
        ItemTemplate const* itemTemplate = item ? sObjectMgr->GetItemTemplate(item) : nullptr;
        data << uint32(itemTemplate ? itemTemplate->DisplayInfoID : 0);
    }
    data << uint32(quest->GetPOIContinent());
    data << float(quest->GetPOIx());
    data << float(quest->GetPOIy());
    data << uint32(quest->GetPointOpt());                   // POIPriority
    data << uint32(quest->GetCharTitleId());                // RewardTitle
    data << int32(quest->GetRewArenaPoints());
    data << uint32(0);                                      // RewardSkillLineID
    data << uint32(0);                                      // RewardNumSkillUps
    data << uint32(0) << uint32(0) << uint32(0) << uint32(0);   // portraits
    for (uint8 i = 0; i < QUEST_REPUTATIONS_COUNT; ++i)
    {
        data << uint32(quest->RewardFactionId[i]);
        data << int32(quest->RewardFactionValueId[i]);
        data << int32(quest->RewardFactionValueIdOverride[i]);
        data << int32(0);                                   // RewardFactionCapIn
    }
    data << uint32(0);                                      // RewardFactionFlags
    for (uint8 i = 0; i < 4; ++i)
        data << uint32(0) << uint32(0);                     // currencies
    data << uint32(0);                                      // AcceptedSoundKitID
    data << uint32(0);                                      // CompleteSoundKitID
    data << uint32(0);                                      // AreaGroupID
    data << uint64(quest->GetTimeAllowed());
    data << uint32(objectives.size());
    data << uint64(quest->GetAllowableRaces() ? quest->GetAllowableRaces() : uint64(-1));
    data << uint32(0);                                      // TreasurePickerID
    data << uint32(0);                                      // Expansion
    data << uint32(0);                                      // ManagedWorldStateID
    data << uint32(0);                                      // conditional descriptions
    data << uint32(0);                                      // conditional completion logs

    data.WriteBits(logTitle.size(), 9);
    data.WriteBits(logDescription.size(), 12);
    data.WriteBits(description.size(), 12);
    data.WriteBits(areaDescription.size(), 9);
    data.WriteBits(0, 10);                                  // PortraitGiverText
    data.WriteBits(0, 8);                                   // PortraitGiverName
    data.WriteBits(0, 10);                                  // PortraitTurnInText
    data.WriteBits(0, 8);                                   // PortraitTurnInName
    data.WriteBits(completionLog.size(), 11);
    data.FlushBits();

    for (QuestObjective343 const& objective : objectives)
    {
        std::string_view text = clamp(objective.Description, 0xFF);
        data << uint32(objective.ID);
        data << uint8(objective.Type);
        data << int8(objective.StorageIndex);
        data << int32(objective.ObjectID);
        data << int32(objective.Amount);
        data << uint32(0);                                  // Flags
        data << uint32(0);                                  // Flags2
        data << float(0.0f);                                // ProgressBarWeight
        data << uint32(0);                                  // VisualEffects
        data << uint8(text.size());
        data.WriteString(text);
    }

    data.WriteString(logTitle);
    data.WriteString(logDescription);
    data.WriteString(description);
    data.WriteString(areaDescription);
    data.WriteString(completionLog);

    _session->SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Sent SMSG_QUERY_QUEST_INFO_RESPONSE questid={}", quest->GetQuestId());
}

void PlayerMenu::SendQuestGiverOfferReward(Quest const* quest, ObjectGuid npcGUID, bool enableNext) const
{
    std::string questTitle = quest->GetTitle();
    std::string RewardText = quest->GetOfferRewardText();

    int32 locale = _session->GetSessionDbLocaleIndex();
    if (QuestLocale const* localeData = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
        ObjectMgr::GetLocaleString(localeData->Title, locale, questTitle);

    if (QuestOfferRewardLocale const* questOfferRewardLocale = sObjectMgr->GetQuestOfferRewardLocale(quest->GetQuestId()))
        ObjectMgr::GetLocaleString(questOfferRewardLocale->RewardText, locale, RewardText);

    auto clamp = [](std::string const& s, std::size_t max) { return std::string_view(s).substr(0, std::min(s.size(), max)); };
    std::string_view title = clamp(questTitle, 0x1FF), rewardText = clamp(RewardText, 0xFFF);

    uint32 emoteCount = 0;
    for (uint32 i = 0; i < QUEST_EMOTE_COUNT; ++i)
    {
        if (quest->OfferRewardEmote[i] <= 0)
            break;
        ++emoteCount;
    }

    // SMSG_QUEST_GIVER_OFFER_REWARD_MESSAGE, 3.4.3 (FUN_140703370): QuestGiverOfferReward {PackedGuid128
    // QuestGiverGUID, u32 QuestGiverCreatureID, u32 QuestID, u32 QuestFlags[3], u32 SuggestedPartyMembers,
    // u32 emote count, {u32 Type, u32 Delay}, bits AutoLaunched, unused, flush, QuestRewards}, u32
    // QuestPackageID, u32 portraits x4, u32 (unread), u32 conditional text count, bits: 9 title, 12 reward
    // text, 10/8/10/8 portrait texts and names; flush; the conditional texts; the six strings
    WorldPacket data(SMSG_QUEST_GIVER_OFFER_REWARD_MESSAGE, 500 + title.size() + rewardText.size());
    Guid128::Write(data, npcGUID);
    data << uint32(npcGUID.GetEntry());
    data << uint32(quest->GetQuestId());
    data << uint32(quest->GetFlags());
    data << uint32(0);
    data << uint32(0);
    data << uint32(quest->GetSuggestedPlayers());
    data << uint32(emoteCount);
    for (uint32 i = 0; i < emoteCount; ++i)
    {
        data << uint32(quest->OfferRewardEmote[i]);
        data << uint32(quest->OfferRewardEmoteDelay[i]);    // Delay Emote
    }
    data.WriteBit(enableNext);                              // AutoLaunched
    data.WriteBit(false);
    data.FlushBits();
    WriteQuestRewards(data, quest, _session->GetPlayer());
    data << uint32(0);                                      // QuestPackageID
    data << uint32(0) << uint32(0) << uint32(0) << uint32(0);   // portraits
    data << uint32(0);
    data << uint32(0);                                      // conditional reward texts
    data.WriteBits(title.size(), 9);
    data.WriteBits(rewardText.size(), 12);
    data.WriteBits(0, 10);
    data.WriteBits(0, 8);
    data.WriteBits(0, 10);
    data.WriteBits(0, 8);
    data.FlushBits();
    data.WriteString(title);
    data.WriteString(rewardText);

    _session->SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Sent SMSG_QUEST_GIVER_OFFER_REWARD_MESSAGE {}, questid={}", npcGUID.ToString(), quest->GetQuestId());
}

void PlayerMenu::SendQuestGiverRequestItems(Quest const* quest, ObjectGuid npcGUID, bool canComplete, bool closeOnCancel) const
{
    // We can always call to RequestItems, but this packet only goes out if there are actually
    // items.  Otherwise, we'll skip straight to the OfferReward

    std::string questTitle = quest->GetTitle();
    std::string requestItemsText = quest->GetRequestItemsText();

    int32 locale = _session->GetSessionDbLocaleIndex();
    if (QuestLocale const* localeData = sObjectMgr->GetQuestLocale(quest->GetQuestId()))
        ObjectMgr::GetLocaleString(localeData->Title, locale, questTitle);

    if (QuestRequestItemsLocale const* questRequestItemsLocale = sObjectMgr->GetQuestRequestItemsLocale(quest->GetQuestId()))
        ObjectMgr::GetLocaleString(questRequestItemsLocale->CompletionText, locale, requestItemsText);

    if (!quest->GetReqItemsCount() && canComplete)
    {
        SendQuestGiverOfferReward(quest, npcGUID, true);
        return;
    }

    // Xinef: recheck completion on reward display
    Player* _player = _session->GetPlayer();
    QuestStatusMap::iterator qsitr = _player->getQuestStatusMap().find(quest->GetQuestId());
    if (qsitr != _player->getQuestStatusMap().end() && qsitr->second.Status == QUEST_STATUS_INCOMPLETE)
    {
        for (uint8 i = 0; i < 6; ++i)
            if (quest->RequiredItemId[i] && qsitr->second.ItemCount[i] < quest->RequiredItemCount[i])
                if (_player->GetItemCount(quest->RequiredItemId[i], false) >= quest->RequiredItemCount[i])
                    qsitr->second.ItemCount[i] = quest->RequiredItemCount[i];

        if (_player->CanCompleteQuest(quest->GetQuestId()))
        {
            _player->CompleteQuest(quest->GetQuestId());
            canComplete = true;
        }
    }

    auto clamp = [](std::string const& s, std::size_t max) { return std::string_view(s).substr(0, std::min(s.size(), max)); };
    std::string_view title = clamp(questTitle, 0x1FF), completionText = clamp(requestItemsText, 0xFFF);

    // SMSG_QUEST_GIVER_REQUEST_ITEMS, 3.4.3 (FUN_1406c8550): PackedGuid128 QuestGiverGUID, u32
    // QuestGiverCreatureID, u32 QuestID, u32 CompEmoteDelay, u32 CompEmoteType, u32 QuestFlags[3], u32
    // SuggestPartyMembers, u32 MoneyToGet, u32 collect count, u32 currency count, u32 StatusFlags (the
    // client lets the quest be completed when (StatusFlags & 0xDE) == 0xDE), {u32 ObjectID, u32 Amount,
    // u32 Flags}, the currencies, bit AutoLaunched, flush, u32 (unread), u32 conditional text count, 9-bit
    // title and 12-bit text lengths, flush, the conditional texts, the title, the text
    WorldPacket data(SMSG_QUEST_GIVER_REQUEST_ITEMS, 128 + title.size() + completionText.size());
    Guid128::Write(data, npcGUID);
    data << uint32(npcGUID.GetEntry());
    data << uint32(quest->GetQuestId());
    data << uint32(0);                                      // CompEmoteDelay
    data << uint32(canComplete ? quest->GetCompleteEmote() : quest->GetIncompleteEmote());
    data << uint32(quest->GetFlags());
    data << uint32(0);
    data << uint32(0);
    data << uint32(quest->GetSuggestedPlayers());
    data << uint32(quest->GetRewOrReqMoney() < 0 ? -quest->GetRewOrReqMoney() : 0);   // Required Money
    data << uint32(quest->GetReqItemsCount());
    data << uint32(0);                                      // currencies
    data << uint32(canComplete ? 0xDF : 0x5F);              // StatusFlags
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        if (!quest->RequiredItemId[i])
            continue;

        data << uint32(quest->RequiredItemId[i]);
        data << uint32(quest->RequiredItemCount[i]);
        data << uint32(0);                                  // Flags
    }
    data.WriteBit(closeOnCancel);                           // AutoLaunched: close the window after cancel
    data.FlushBits();
    data << uint32(0);
    data << uint32(0);                                      // conditional completion texts
    data.WriteBits(title.size(), 9);
    data.WriteBits(completionText.size(), 12);
    data.FlushBits();
    data.WriteString(title);
    data.WriteString(completionText);

    _session->SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Sent SMSG_QUEST_GIVER_REQUEST_ITEMS {}, questid={}", npcGUID.ToString(), quest->GetQuestId());
}
