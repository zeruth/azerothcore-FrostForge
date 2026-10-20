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
#include "Realm.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <map>
#include <vector>

namespace
{
    // The Battle.net realm address of this realm: region << 24 | site << 16 | realm id, as the
    // Battle.net service advertises it (BattlenetSession.cpp RealmAddress).
    uint32 VirtualRealmAddress()
    {
        return (1u << 24) | (1u << 16) | (realm.Id.Realm & 0xFFFF);
    }

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

        packet << uint32(VirtualRealmAddress());
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
        packet << uint32(VirtualRealmAddress());
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

void WorldSession::SendClientCacheVersion(uint32 version)
{
    WorldPacket data(SMSG_CLIENTCACHE_VERSION, 4);
    data << uint32(version);
    SendPacket(&data);
}
