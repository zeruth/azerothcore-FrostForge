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

#include "GameTime.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Realm.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <map>
#include <vector>

namespace
{
    // The expansion a race or class needs: the Burning Crusade races and the death knight.
    uint8 RequiredExpansion(uint8 race, uint8 playerClass)
    {
        uint8 expansion = 0;
        if (race == RACE_BLOODELF || race == RACE_DRAENEI)
            expansion = 1;
        if (playerClass == CLASS_DEATH_KNIGHT)
            expansion = 2;
        return expansion;
    }
}

// SMSG_AUTH_RESPONSE, 3.4.3 (parser FUN_1406b8730, SuccessInfo FUN_140708650): a result of 0
// must carry SuccessInfo or the client turns it into an error (world-handshake.ANALYSIS.md 3.5).
void WorldSession::SendAuthResponse(uint8 code, bool shortForm, uint32 queuePos)
{
    bool const ok = code == AUTH_OK || code == AUTH_WAIT_QUEUE;
    bool const queued = code == AUTH_WAIT_QUEUE || (!shortForm && queuePos);

    uint8 exp = Expansion(); // 0 - normal, 1 - TBC, 2 - WotLK, must be set in database manually for each account
    if (exp >= MAX_EXPANSIONS)
        exp = MAX_EXPANSIONS - 1;

    WorldPacket packet(SMSG_AUTH_RESPONSE, 128);
    packet << uint32(ok ? 0 : code);
    packet.WriteBit(ok);
    packet.WriteBit(queued);
    packet.FlushBits();

    if (ok)
    {
        std::map<uint8, std::vector<uint8>> classesByRace;
        for (uint8 race = 1; race <= RACE_DRAENEI; ++race)
            for (uint8 playerClass = 1; playerClass < MAX_CLASSES; ++playerClass)
                if (sObjectMgr->GetPlayerInfo(race, playerClass))
                    classesByRace[race].push_back(playerClass);

        std::string const& realmName = realm.Name;
        std::string normalizedName = realmName;
        normalizedName.erase(std::remove(normalizedName.begin(), normalizedName.end(), ' '), normalizedName.end());

        packet << uint32(Guid128::VirtualRealmAddress());
        packet << uint32(1);                                   // VirtualRealms
        packet << uint32(0);                                   // TimeRested
        packet << uint8(exp);                                  // ActiveExpansionLevel
        packet << uint8(exp);                                  // AccountExpansionLevel
        packet << uint32(0);                                   // TimeSecondsUntilPCKick
        packet << uint32(classesByRace.size());                // AvailableClasses
        packet << uint32(0);                                   // Templates
        packet << uint32(0);                                   // CurrencyID
        packet << uint64(GameTime::GetGameTime().count());     // Time

        for (auto const& [race, classes] : classesByRace)
        {
            packet << uint8(race);
            packet << uint32(classes.size());
            for (uint8 playerClass : classes)
            {
                uint8 required = RequiredExpansion(race, playerClass);
                packet << uint8(playerClass);
                packet << uint8(required);                     // ActiveExpansionLevel
                packet << uint8(required);                     // AccountExpansionLevel
                packet << uint8(0);                            // MinActiveExpansionLevel
            }
        }

        packet.WriteBit(false);                                // IsExpansionTrial
        packet.WriteBit(false);                                // ForceCharacterTemplate
        packet.WriteBit(false);                                // NumPlayersHorde
        packet.WriteBit(false);                                // NumPlayersAlliance
        packet.WriteBit(false);                                // ExpansionTrialExpiration
        packet.WriteBit(false);                                // NewBuildKeys
        packet.FlushBits();

        // GameTime (FUN_140712640)
        packet << uint32(GetBillingPlanFlags());               // BillingPlan
        packet << uint32(0);                                   // TimeRemain
        packet << uint32(0);
        packet.WriteBit(false);
        packet.WriteBit(false);
        packet.WriteBit(false);
        packet.FlushBits();

        // VirtualRealmInfo (FUN_14071aab0)
        packet << uint32(Guid128::VirtualRealmAddress());
        packet.WriteBit(true);                                 // IsLocal
        packet.WriteBit(false);                                // IsInternalRealm
        packet.WriteBits(realmName.size(), 8);
        packet.WriteBits(normalizedName.size(), 8);
        packet.FlushBits();
        packet.append(realmName.data(), realmName.size());
        packet.append(normalizedName.data(), normalizedName.size());
    }

    if (queued)
    {
        packet << uint32(queuePos);                            // WaitCount
        packet << uint32(0);                                   // WaitTime
        packet.WriteBit(false);                                // HasFCM
        packet.FlushBits();
    }

    SendPacket(&packet);
}

// SMSG_FEATURE_SYSTEM_STATUS_GLUE_SCREEN, 3.4.3 (parser FUN_1406bf640 -> FUN_1406fb3f0, handler
// FUN_14167b900). 31 flag bits, [11 bits RealmHiddenAlert length incl. NUL], flush, [EuropaTicketConfig],
// u32 TokenPollTimeSeconds, u32 KioskSessionMinutes, i64 TokenBalanceAmount, i32 MaxCharactersPerRealm,
// u32 LiveRegionCharacterCopySourceRegions count, u32 BpayStoreProductDeliveryDelay,
// i32 ActiveCharacterUpgradeBoostType, i32 ActiveClassTrialBoostType, i32 MinimumExpansionLevel,
// i32 MaximumExpansionLevel, i32 ActiveSeason, u32 GameRuleValues count, i16 MaxPlayerNameQueriesPerPacket,
// i16 PlayerNameQueryTelemetryInterval, u32 PlayerNameQueryInterval, u32 DebugTimeEvents count, u32,
// [i32 LaunchETA], [RealmHiddenAlert], the arrays. (TrinityCore's 4.4 EventRealmQueues u32 is not read.)
// Until it arrives the client keeps character creation off; after it, creation is allowed while the
// account has fewer than MaxCharactersPerRealm characters here.
void WorldSession::SendFeatureSystemStatusGlueScreen()
{
    uint8 exp = Expansion();
    if (exp >= MAX_EXPANSIONS)
        exp = MAX_EXPANSIONS - 1;

    WorldPacket data(SMSG_FEATURE_SYSTEM_STATUS_GLUE_SCREEN, 64);
    data.WriteBit(false);                                   // BpayStoreEnabled
    data.WriteBit(false);                                   // BpayStoreAvailable
    data.WriteBit(false);                                   // BpayStoreDisabledByParentalControls
    data.WriteBit(false);                                   // CharUndeleteEnabled
    data.WriteBit(false);                                   // CommerceSystemEnabled
    data.WriteBit(false);                                   // Unk14
    data.WriteBit(false);                                   // WillKickFromWorld
    data.WriteBit(false);                                   // IsExpansionPreorderInStore
    data.WriteBit(false);                                   // KioskModeEnabled
    data.WriteBit(false);                                   // CompetitiveModeEnabled
    data.WriteBit(false);                                   // IsBoostEnabled
    data.WriteBit(false);                                   // TrialBoostEnabled
    data.WriteBit(false);                                   // TokenBalanceEnabled
    data.WriteBit(false);                                   // LiveRegionCharacterListEnabled
    data.WriteBit(false);                                   // LiveRegionCharacterCopyEnabled
    data.WriteBit(false);                                   // LiveRegionAccountCopyEnabled
    data.WriteBit(false);                                   // LiveRegionKeyBindingsCopyEnabled
    data.WriteBit(false);                                   // Unknown901CheckoutRelated
    data.WriteBit(false);
    data.WriteBit(false);                                   // EuropaTicketSystemStatus
    data.WriteBit(false);                                   // IsNameReservationEnabled
    data.WriteBit(false);                                   // LaunchETA
    data.WriteBit(false);
    data.WriteBit(false);
    data.WriteBit(false);
    data.WriteBit(false);                                   // IsSoMNotificationEnabled
    data.WriteBit(false);                                   // AddonsDisabled
    data.WriteBit(false);                                   // Unused1000
    data.WriteBit(false);                                   // AccountSaveDataExportEnabled
    data.WriteBit(false);                                   // AccountLockedByExport
    data.WriteBit(false);                                   // RealmHiddenAlert
    data.FlushBits();

    data << uint32(300);                                    // TokenPollTimeSeconds
    data << uint32(0);                                      // KioskSessionMinutes
    data << int64(0);                                       // TokenBalanceAmount
    data << int32(sWorld->getIntConfig(CONFIG_CHARACTERS_PER_REALM)); // MaxCharactersPerRealm
    data << uint32(0);                                      // LiveRegionCharacterCopySourceRegions
    data << uint32(0);                                      // BpayStoreProductDeliveryDelay
    data << int32(0);                                       // ActiveCharacterUpgradeBoostType
    data << int32(0);                                       // ActiveClassTrialBoostType
    data << int32(0);                                       // MinimumExpansionLevel
    data << int32(exp);                                     // MaximumExpansionLevel
    data << int32(0);                                       // ActiveSeason
    data << uint32(0);                                      // GameRuleValues
    data << int16(50);                                      // MaxPlayerNameQueriesPerPacket
    data << int16(600);                                     // PlayerNameQueryTelemetryInterval
    data << uint32(10);                                     // PlayerNameQueryInterval
    data << uint32(0);                                      // DebugTimeEvents
    data << uint32(0);                                      // Unused1007
    SendPacket(&data);
}

// SMSG_SET_TIME_ZONE_INFORMATION, 3.4.3 (parser FUN_1406ca960, handler FUN_141658c60): 7 bits each for
// the ServerTimeTZ, GameTimeTZ and ServerRegionalTZ lengths, flush, the three strings (no NUL).
void WorldSession::SendSetTimeZoneInformation()
{
    std::string const timeZone = "Etc/UTC";                 /// @todo the realm's own time zone

    WorldPacket data(SMSG_SET_TIME_ZONE_INFORMATION, 3 + 3 * timeZone.size());
    data.WriteBits(timeZone.size(), 7);                     // ServerTimeTZ
    data.WriteBits(timeZone.size(), 7);                     // GameTimeTZ
    data.WriteBits(timeZone.size(), 7);                     // ServerRegionalTZ
    data.FlushBits();
    for (int i = 0; i < 3; ++i)
        data.append(timeZone.data(), timeZone.size());
    SendPacket(&data);
}

// SMSG_AVAILABLE_HOTFIXES, 3.4.3 (Cache protocol, handler FUN_14056fd20): u32 VirtualRealmAddress,
// u32 Count, {u32 PushID, u32 UniqueID}[Count]. With none the client closes its hotfix phase at once
// and asks for nothing (CMSG_HOTFIX_REQUEST would follow for hotfixes it lacks).
void WorldSession::SendAvailableHotfixes()
{
    WorldPacket data(SMSG_AVAILABLE_HOTFIXES, 8);
    data << uint32(Guid128::VirtualRealmAddress());
    data << uint32(0);
    SendPacket(&data);
}

void WorldSession::SendClientCacheVersion(uint32 version)
{
    WorldPacket data(SMSG_CACHE_VERSION, 4);
    data << uint32(version);
    SendPacket(&data);
}
