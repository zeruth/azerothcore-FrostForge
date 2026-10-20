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

#include "Common.h"
#include "GameTime.h"
#include "Log.h"
#include "MapMgr.h"
#include "NPCHandler.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Pet.h"
#include "Player.h"
#include "QueryPackets.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

/// One player's entry of SMSG_QUERY_PLAYER_NAMES_RESPONSE (Result 1 when the name is unknown).
static WorldPackets::Query::NameCacheLookupResult BuildNameQueryResult(ObjectGuid guid)
{
    WorldPackets::Query::NameCacheLookupResult result;
    result.Player = guid;

    CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!playerData)
        return result;

    Player* player = ObjectAccessor::FindConnectedPlayer(guid);

    result.Result = 0;
    result.HasData = true;
    result.Name = playerData->Name;
    result.Race = player ? player->getRace() : playerData->Race;
    result.Sex = player ? player->getGender() : playerData->Sex;
    result.Class = player ? player->getClass() : playerData->Class;
    result.Level = player ? player->GetLevel() : playerData->Level;
    result.DeclinedNames = player ? player->GetDeclinedNames() : nullptr;
    return result;
}

void WorldSession::SendNameQueryOpcode(ObjectGuid guid)
{
    WorldPackets::Query::NameQueryResponse response;
    response.Players.push_back(BuildNameQueryResult(guid));
    SendPacket(response.Write());
}

void WorldSession::HandleNameQueryOpcode(WorldPackets::Query::NameQuery& packet)
{
    // 3.4.3 asks for several players at once and takes one answer for all of them
    WorldPackets::Query::NameQueryResponse response;
    response.Players.reserve(packet.Players.size());
    for (ObjectGuid const& guid : packet.Players)
        response.Players.push_back(BuildNameQueryResult(guid));
    SendPacket(response.Write());
}

void WorldSession::HandleTimeQueryOpcode(WorldPackets::Query::TimeQuery& /*packet*/)
{
    SendTimeQueryResponse();
}

void WorldSession::SendTimeQueryResponse()
{
    // 3.4.3 carries the server time alone; 3.3.5's time to the daily reset is gone
    WorldPackets::Query::TimeQueryResponse timeQueryResponse;
    timeQueryResponse.CurrentTime = GameTime::GetGameTime().count();
    SendPacket(timeQueryResponse.Write());
}

/// A string the 3.4.3 client reads with a length that counts its NUL (FUN_14155f220): a length of 1
/// stands for the empty string and sends no bytes.
static void WriteCreatureStatsString(ByteBuffer& data, std::string const& value)
{
    if (!value.empty())
        data << value;                                  // the bytes and the NUL
}

/// Only _static_ data is sent in this packet !!!
void WorldSession::HandleCreatureQueryOpcode(WorldPacket& recvData)
{
    // 3.4.3: u32 CreatureID (writer FUN_14075f440)
    uint32 entry;
    recvData >> entry;

    // 3.4.3 (cache switch FUN_140721aa0 case 0x2914): u32 CreatureID, bit Allow, flush, [CreatureStats]
    WorldPacket data(SMSG_QUERY_CREATURE_RESPONSE, 200);
    data << uint32(entry);

    CreatureTemplate const* ci = sObjectMgr->GetCreatureTemplate(entry);
    data.WriteBit(ci != nullptr);
    data.FlushBits();
    if (!ci)
    {
        LOG_DEBUG("network", "WORLD: CMSG_QUERY_CREATURE - NO CREATURE INFO! ({})", entry);
        SendPacket(&data);
        return;
    }

    std::string Name = ci->Name, Title = ci->SubName;
    LocaleConstant loc_idx = GetSessionDbLocaleIndex();
    if (loc_idx >= 0)
    {
        if (CreatureLocale const* cl = sObjectMgr->GetCreatureLocale(entry))
        {
            ObjectMgr::GetLocaleString(cl->Name, loc_idx, Name);
            ObjectMgr::GetLocaleString(cl->Title, loc_idx, Title);
        }
    }

    // CreatureStats (FUN_14070b9b0). Bits: 11 Title length, 11 TitleAlt, 6 CursorName, Civilian,
    // Leader, then 11+11 per name slot (Name, NameAlt) x4; flush. Every length counts the NUL. Then the
    // eight names, u32 Flags[2], CreatureType, CreatureFamily, Classification, PetSpellDataID,
    // ProxyCreatureID[2], u32 display count, f32 TotalProbability, {u32 CreatureDisplayID, f32 Scale,
    // f32 Probability}, f32 HpMulti, f32 EnergyMulti, u32 quest item count, u32 CreatureMovementInfoID,
    // HealthScalingExpansion, RequiredExpansion, VignetteID, Class, CreatureDifficultyID, WidgetSetID,
    // WidgetSetUnitConditionID, then Title, TitleAlt, CursorName, the quest items.
    std::array<std::string, 4> names = { Name, "", "", "" };
    std::string const empty;
    auto length = [](std::string const& s) { return s.empty() ? 0 : s.size() + 1; };

    data.WriteBits(length(Title), 11);
    data.WriteBits(0, 11);                              // TitleAlt
    data.WriteBits(length(ci->IconName), 6);
    data.WriteBit((ci->flags_extra & CREATURE_FLAG_EXTRA_CIVILIAN) != 0);
    data.WriteBit(ci->RacialLeader);
    for (std::string const& name : names)
    {
        data.WriteBits(length(name), 11);
        data.WriteBits(0, 11);                          // NameAlt
    }
    data.FlushBits();

    for (std::string const& name : names)
        WriteCreatureStatsString(data, name);           // NameAlt is empty and sends nothing

    data << uint32(ci->type_flags);
    data << uint32(0);                                  // Flags[1]
    data << int32(ci->type);
    data << int32(ci->family);
    data << int32(ci->rank);
    data << int32(ci->PetSpellDataId);
    data << int32(ci->KillCredit[0]);                   // ProxyCreatureID
    data << int32(ci->KillCredit[1]);

    float totalProbability = 0.0f;
    for (CreatureModel const& model : ci->Models)
        totalProbability += model.Probability;
    data << uint32(ci->Models.size());
    data << float(totalProbability);
    for (CreatureModel const& model : ci->Models)
    {
        data << uint32(model.CreatureDisplayID);
        data << float(model.DisplayScale);
        data << float(model.Probability);
    }

    data << float(ci->ModHealth);
    data << float(ci->ModMana);

    CreatureQuestItemList const* items = sObjectMgr->GetCreatureQuestItemList(entry);
    data << uint32(items ? items->size() : 0);
    data << uint32(ci->movementId);
    data << int32(ci->expansion);                       // HealthScalingExpansion
    data << uint32(0);                                  // RequiredExpansion
    data << uint32(0);                                  // VignetteID
    data << int32(ci->unit_class);
    data << int32(0);                                   // CreatureDifficultyID
    data << int32(0);                                   // WidgetSetID
    data << int32(0);                                   // WidgetSetUnitConditionID

    WriteCreatureStatsString(data, Title);
    WriteCreatureStatsString(data, empty);              // TitleAlt
    WriteCreatureStatsString(data, ci->IconName);

    if (items)
        for (uint32 item : *items)
            data << uint32(item);

    SendPacket(&data);
}

/// Only _static_ data is sent in this packet !!!
void WorldSession::HandleGameObjectQueryOpcode(WorldPacket& recvData)
{
    // 3.4.3: u32 GameObjectID, PackedGuid128 Guid (writer FUN_14075f480)
    uint32 entry;
    recvData >> entry;
    ObjectGuid guid = Guid128::Read(recvData);

    // 3.4.3 (cache switch FUN_140721aa0 case 0x2915): u32 GameObjectID, PackedGuid128 Guid, bit Allow,
    // flush, u32 size, the GameObjectStats bytes (read by the handler, FUN_141706050)
    WorldPacket data(SMSG_QUERY_GAME_OBJECT_RESPONSE, 200);
    data << uint32(entry);
    Guid128::Write(data, guid);

    GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
    data.WriteBit(info != nullptr);
    data.FlushBits();
    if (!info)
    {
        LOG_DEBUG("network", "WORLD: CMSG_QUERY_GAME_OBJECT - Missing gameobject info for ({})", guid.ToString());
        data << uint32(0);
        SendPacket(&data);
        return;
    }

    std::string Name = info->name;
    std::string IconName = info->IconName;
    std::string CastBarCaption = info->castBarCaption;

    LocaleConstant localeConstant = GetSessionDbLocaleIndex();
    if (localeConstant >= LOCALE_enUS)
        if (GameObjectLocale const* gameObjectLocale = sObjectMgr->GetGameObjectLocale(entry))
        {
            ObjectMgr::GetLocaleString(gameObjectLocale->Name, localeConstant, Name);
            ObjectMgr::GetLocaleString(gameObjectLocale->CastBarCaption, localeConstant, CastBarCaption);
        }

    // GameObjectStats: u32 Type, u32 DisplayID, four NUL-terminated names, IconName, CastBarCaption,
    // UnkString, i32 Data[35], f32 Size, u8 quest item count, u32 quest items, u32 ContentTuningID
    ByteBuffer stats(200);
    stats << uint32(info->type);
    stats << uint32(info->displayId);
    stats << Name;
    stats << uint8(0) << uint8(0) << uint8(0);         // name2, name3, name4
    stats << IconName;
    stats << CastBarCaption;
    stats << info->unk1;
    for (uint32 i = 0; i < 35; ++i)                     // 3.4.3 has 35 data fields, AC's templates 24
        stats << uint32(i < MAX_GAMEOBJECT_DATA ? info->raw.data[i] : 0);
    stats << float(info->size);

    GameObjectQuestItemList const* items = sObjectMgr->GetGameObjectQuestItemList(entry);
    uint8 itemCount = items ? uint8(std::min<std::size_t>(items->size(), 255)) : 0;
    stats << uint8(itemCount);
    for (uint8 i = 0; i < itemCount; ++i)
        stats << uint32((*items)[i]);
    stats << uint32(0);                                 // ContentTuningID

    data << uint32(stats.size());
    data.append(stats);

    SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Sent SMSG_QUERY_GAME_OBJECT_RESPONSE");
}

void WorldSession::HandleCorpseQueryOpcode(WorldPacket& /*recvData*/)
{
    if (!_player->HasCorpse())
    {
        WorldPacket data(SMSG_CORPSE_LOCATION, 1);
        data << uint8(0);                                   // corpse not found
        SendPacket(&data);
        return;
    }

    WorldLocation corpseLocation = _player->GetCorpseLocation();
    uint32 corpseMapID = corpseLocation.GetMapId();
    uint32 mapID = corpseLocation.GetMapId();
    float x = corpseLocation.GetPositionX();
    float y = corpseLocation.GetPositionY();
    float z = corpseLocation.GetPositionZ();

    // if corpse at different map
    if (mapID != _player->GetMapId())
    {
        // search entrance map for proper show entrance
        if (MapEntry const* corpseMapEntry = sMapStore.LookupEntry(mapID))
        {
            if (corpseMapEntry->IsDungeon() && corpseMapEntry->entrance_map >= 0)
            {
                // if corpse map have entrance
                if (Map const* entranceMap = sMapMgr->CreateBaseMap(corpseMapEntry->entrance_map))
                {
                    mapID = corpseMapEntry->entrance_map;
                    x = corpseMapEntry->entrance_x;
                    y = corpseMapEntry->entrance_y;
                    z = entranceMap->GetHeight(GetPlayer()->GetPhaseMask(), x, y, MAX_HEIGHT);
                }
            }
        }
    }

    WorldPacket data(SMSG_CORPSE_LOCATION, 1 + (6 * 4));
    data << uint8(1);                                       // corpse found
    data << int32(mapID);
    data << float(x);
    data << float(y);
    data << float(z);
    data << int32(corpseMapID);
    data << uint32(0);                                      // unknown
    SendPacket(&data);
}

void WorldSession::HandleNpcTextQueryOpcode(WorldPacket& recvData)
{
    // 3.4.3: u32 TextID, PackedGuid128 Guid (writer FUN_14075f510)
    uint32 textID;
    recvData >> textID;
    Guid128::Skip(recvData);
    LOG_DEBUG("network", "WORLD: CMSG_QUERY_NPC_TEXT TextId: {}", textID);

    // 3.4.3 (cache switch FUN_140721aa0 case 0x2916): u32 TextID, bit Allow, flush, u32 size, then
    // f32 Probabilities[8] and u32 BroadcastTextID[8] (FUN_141706300). The texts themselves come from
    // the client's BroadcastText.db2, so an option without a BroadcastTextID shows nothing.
    GossipText const* gossip = sObjectMgr->GetGossipText(textID);

    WorldPacket data(SMSG_QUERY_NPC_TEXT_RESPONSE, 4 + 1 + 4 + 64);
    data << uint32(textID);
    data.WriteBit(gossip != nullptr);
    data.FlushBits();
    data << uint32(gossip ? MAX_GOSSIP_TEXT_OPTIONS * 8 : 0);
    if (gossip)
    {
        for (uint8 i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
            data << float(gossip->Options[i].Probability);
        for (uint8 i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
            data << uint32(gossip->Options[i].BroadcastTextID);
    }

    SendPacket(&data);

    LOG_DEBUG("network", "WORLD: Sent SMSG_QUERY_NPC_TEXT_RESPONSE");
}

/// Only _static_ data is sent in this packet !!!
void WorldSession::HandlePageTextQueryOpcode(WorldPacket& recvData)
{
    // 3.4.3: u32 PageTextID, PackedGuid128 ItemGUID (writer FUN_14075f580)
    uint32 pageID;
    recvData >> pageID;
    Guid128::Skip(recvData);

    // 3.4.3 (cache switch FUN_140721aa0 case 0x2917): u32 PageTextID, bit Allow, flush, then if Allow
    // u32 count and per page {u32 ID, u32 NextPageID, u32 PlayerConditionID, u8 Flags, 12-bit text
    // length, flush, text}. The whole chain goes in one answer.
    struct Page { uint32 Id; uint32 Next; std::string Text; };
    std::vector<Page> pages;
    int loc_idx = GetSessionDbLocaleIndex();
    for (uint32 id = pageID; id && pages.size() < 64; )
    {
        PageText const* pageText = sObjectMgr->GetPageText(id);
        if (!pageText)
            break;

        std::string text = pageText->Text;
        if (loc_idx >= 0)
            if (PageTextLocale const* locale = sObjectMgr->GetPageTextLocale(id))
                ObjectMgr::GetLocaleString(locale->Text, loc_idx, text);
        if (text.size() > 0xFFF)
            text.resize(0xFFF);

        pages.push_back({ id, pageText->NextPage, std::move(text) });
        id = pageText->NextPage;
        if (std::any_of(pages.begin(), pages.end(), [id](Page const& p) { return p.Id == id; }))
            break;                                      // a loop in the chain
    }

    WorldPacket data(SMSG_QUERY_PAGE_TEXT_RESPONSE, 50);
    data << uint32(pageID);
    data.WriteBit(!pages.empty());
    data.FlushBits();
    if (!pages.empty())
    {
        data << uint32(pages.size());
        for (Page const& page : pages)
        {
            data << uint32(page.Id);
            data << uint32(page.Next);
            data << uint32(0);                          // PlayerConditionID
            data << uint8(0);                           // Flags
            data.WriteBits(page.Text.size(), 12);
            data.FlushBits();
            data.WriteString(page.Text);
        }
    }

    SendPacket(&data);

    LOG_DEBUG("network", "WORLD: Sent SMSG_QUERY_PAGE_TEXT_RESPONSE");
}

void WorldSession::HandleCorpseMapPositionQuery(WorldPackets::Query::CorpseMapPositionQuery& /*packet*/)
{
    LOG_DEBUG("network", "WORLD: Recv CMSG_QUERY_CORPSE_TRANSPORT");

    WorldPacket data(SMSG_CORPSE_TRANSPORT_QUERY, 4 + 4 + 4 + 4);
    data << float(0);
    data << float(0);
    data << float(0);
    data << float(0);
    SendPacket(&data);
}

void WorldSession::HandleQuestPOIQuery(WorldPacket& recvData)
{
    uint32 count;
    recvData >> count; // quest count, max=25

    if (count > MAX_QUEST_LOG_SIZE)
    {
        recvData.rfinish();
        return;
    }

    // Read quest ids and add the in a unordered_set so we don't send POIs for the same quest multiple times
    std::unordered_set<uint32> questIds;
    for (uint32 i = 0; i < count; ++i)
        questIds.insert(recvData.read<uint32>()); // quest id

    WorldPacket data(SMSG_QUEST_POI_QUERY_RESPONSE, 4 + (4 + 4)*questIds.size());
    data << uint32(questIds.size()); // count

    for (std::unordered_set<uint32>::const_iterator itr = questIds.begin(); itr != questIds.end(); ++itr)
    {
        uint32 questId = *itr;
        bool questOk = false;

        uint16 questSlot = _player->FindQuestSlot(questId);

        if (questSlot != MAX_QUEST_LOG_SIZE)
            questOk = _player->GetQuestSlotQuestId(questSlot) == questId;

        if (questOk)
        {
            QuestPOIVector const* POI = sObjectMgr->GetQuestPOIVector(questId);

            if (POI)
            {
                data << uint32(questId); // quest ID
                data << uint32(POI->size()); // POI count

                for (QuestPOIVector::const_iterator itr = POI->begin(); itr != POI->end(); ++itr)
                {
                    data << uint32(itr->Id);                // POI index
                    data << int32(itr->ObjectiveIndex);     // objective index
                    data << uint32(itr->MapId);             // mapid
                    data << uint32(itr->AreaId);            // areaid
                    data << uint32(itr->FloorId);           // floorid
                    data << uint32(itr->Unk3);              // unknown
                    data << uint32(itr->Unk4);              // unknown
                    data << uint32(itr->points.size());     // POI points count

                    for (std::vector<QuestPOIPoint>::const_iterator itr2 = itr->points.begin(); itr2 != itr->points.end(); ++itr2)
                    {
                        data << int32(itr2->x); // POI point x
                        data << int32(itr2->y); // POI point y
                    }
                }
            }
            else
            {
                data << uint32(questId); // quest ID
                data << uint32(0); // POI count
            }
        }
        else
        {
            data << uint32(questId); // quest ID
            data << uint32(0); // POI count
        }
    }

    SendPacket(&data);
}
