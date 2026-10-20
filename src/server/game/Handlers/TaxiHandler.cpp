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
#include "Player.h"
#include "ScriptMgr.h"
#include "WaypointMovementGenerator.h"
#include "WorldPacket.h"
#include "PacketGuid128.h"
#include "WorldSession.h"

void WorldSession::HandleTaxiNodeStatusQueryOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_140761100): PackedGuid128 UnitGUID
    SendTaxiStatus(Guid128::Read(recvData));
}

void WorldSession::BuildTaxiNodeStatus(WorldPacket& data, ObjectGuid unit, bool known)
{
    // SMSG_TAXI_NODE_STATUS, 3.4.3 (FUN_1406cc020, handler FUN_141a52cf0): PackedGuid128 Unit, 2-bit Status
    // (1 learned, 2 not learned), flush
    data.Initialize(SMSG_TAXI_NODE_STATUS, 18 + 1);
    Guid128::Write(data, unit);
    data.WriteBits(known ? 1 : 2, 2);
    data.FlushBits();
}

void WorldSession::SendTaxiStatus(ObjectGuid guid)
{
    Player* const player = GetPlayer();
    Creature* unit = ObjectAccessor::GetCreature(*player, guid);
    // reaction must be checked both ways: the Dark Portal flight masters are neutral toward opposite-faction players, while players are hostile toward them
    if (!unit || unit->GetReactionTo(player) <= REP_UNFRIENDLY || player->IsHostileTo(unit) || !unit->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER))
    {
        LOG_DEBUG("network", "WorldSession::SendTaxiStatus - Unit ({}) not found.", guid.ToString());
        return;
    }

    // find taxi node
    uint32 nearest = sObjectMgr->GetNearestTaxiNode(*unit, player->GetTeamId(true));
    if (!nearest)
    {
        return;
    }

    WorldPacket data;
    BuildTaxiNodeStatus(data, guid, player->m_taxi.IsTaximaskNodeKnown(nearest));
    SendPacket(&data);
    LOG_DEBUG("network", "WORLD: Sent SMSG_TAXI_NODE_STATUS");
}

void WorldSession::HandleTaxiQueryAvailableNodes(WorldPacket& recvData)
{
    // 3.4.3: CMSG_TAXI_QUERY_AVAILABLE_NODES (writer FUN_140761140) and CMSG_ENABLE_TAXI_NODE
    // (FUN_14075d4c0): PackedGuid128 Unit
    ObjectGuid guid = Guid128::Read(recvData);

    // cheating checks
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!unit)
    {
        LOG_DEBUG("network", "WORLD: HandleTaxiQueryAvailableNodes - Unit ({}) not found or you can't interact with him.", guid.ToString());
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    // unknown taxi node case
    if (SendLearnNewTaxiNode(unit))
        return;

    // known taxi node case
    SendTaxiMenu(unit);
}

void WorldSession::SendTaxiMenu(Creature* unit)
{
    // find current node
    uint32 curloc = sObjectMgr->GetNearestTaxiNode(*unit, GetPlayer()->GetTeamId(true));

    if (curloc == 0)
        return;

    bool lastTaxiCheaterState = GetPlayer()->isTaxiCheater();
    if (unit->GetEntry() == 29480) GetPlayer()->SetTaxiCheater(true); // Grimwing in Ebon Hold, special case. NOTE: Not perfect, Zul'Aman should not be included according to WoWhead, and I think taxicheat includes it.

    LOG_DEBUG("network", "WORLD: CMSG_TAXI_NODE_STATUS_QUERY {} ", curloc);

    // SMSG_SHOW_TAXI_NODES, 3.4.3 (FUN_1406cac10, handler FUN_141714f80): bit HasWindowInfo, flush, u32
    // CanLandNodes count and u32 CanUseNodes count in u64 words, [PackedGuid128 UnitGUID, u32
    // CurrentNode], the two masks (node N is bit (N-1) % 64 of word (N-1) / 64: 3.3.5's u32 mask read
    // two words at a time)
    ByteBuffer mask;
    GetPlayer()->m_taxi.AppendTaximaskTo(mask, GetPlayer()->isTaxiCheater());
    while (mask.size() % 8)
        mask << uint8(0);
    uint32 words = uint32(mask.size() / 8);

    WorldPacket data(SMSG_SHOW_TAXI_NODES, 1 + 8 + 18 + 4 + mask.size() * 2);
    data.WriteBit(true);
    data.FlushBits();
    data << uint32(words);
    data << uint32(words);
    Guid128::Write(data, unit->GetGUID());
    data << uint32(curloc);
    data.append(mask);
    data.append(mask);
    SendPacket(&data);

    LOG_DEBUG("network", "WORLD: Sent SMSG_SHOW_TAXI_NODES");

    GetPlayer()->SetTaxiCheater(lastTaxiCheaterState);
}

void WorldSession::SendDoFlight(uint32 mountDisplayId, uint32 path, uint32 pathNode)
{
    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    while (GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
        GetPlayer()->GetMotionMaster()->MovementExpired(false);

    if (mountDisplayId)
        GetPlayer()->Mount(mountDisplayId);

    if (Creature* critter = ObjectAccessor::GetCreature(*GetPlayer(), GetPlayer()->GetCritterGUID()))
        critter->DespawnOrUnsummon();

    GetPlayer()->GetMotionMaster()->MoveTaxiFlight(path, pathNode);
}

bool WorldSession::SendLearnNewTaxiNode(Creature* unit)
{
    // find current node
    uint32 curloc = sObjectMgr->GetNearestTaxiNode(*unit, GetPlayer()->GetTeamId(true));

    if (curloc == 0)
        return true;                                        // `true` send to avoid WorldSession::SendTaxiMenu call with one more curlock seartch with same false result.

    if (GetPlayer()->m_taxi.SetTaximaskNode(curloc))
    {
        WorldPacket msg(SMSG_NEW_TAXI_PATH, 0);
        SendPacket(&msg);

        WorldPacket update;
        BuildTaxiNodeStatus(update, unit->GetGUID(), true);
        SendPacket(&update);

        sScriptMgr->OnPlayerLearnTaxiNode(GetPlayer(), curloc);

        return true;
    }
    else
        return false;
}

void WorldSession::SendDiscoverNewTaxiNode(uint32 nodeid)
{
    if (GetPlayer()->m_taxi.SetTaximaskNode(nodeid))
    {
        WorldPacket msg(SMSG_NEW_TAXI_PATH, 0);
        SendPacket(&msg);

        sScriptMgr->OnPlayerLearnTaxiNode(GetPlayer(), nodeid);
    }
}

void WorldSession::HandleActivateTaxiExpressOpcode(WorldPacket& recvData)
{
    ObjectGuid guid;
    uint32 node_count;

    recvData >> guid >> node_count;

    Creature* npc = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        LOG_DEBUG("network", "WORLD: HandleActivateTaxiExpressOpcode - Unit ({}) not found or you can't interact with it.", guid.ToString());
        SendActivateTaxiReply(ERR_TAXITOOFARAWAY);
        return;
    }
    std::vector<uint32> nodes;

    for (uint32 i = 0; i < node_count; ++i)
    {
        uint32 node;
        recvData >> node;

        if (!GetPlayer()->m_taxi.IsTaximaskNodeKnown(node) && !GetPlayer()->isTaxiCheater())
        {
            SendActivateTaxiReply(ERR_TAXINOTVISITED);
            recvData.rfinish();
            return;
        }

        nodes.push_back(node);
    }

    if (nodes.empty())
        return;

    LOG_DEBUG("network", "WORLD: Received CMSG_ACTIVATETAXIEXPRESS from {} to {}", nodes.front(), nodes.back());

    GetPlayer()->ActivateTaxiPathTo(nodes, npc, 0);
}

void WorldSession::HandleMoveSplineDoneOpcode(WorldPacket& recvData)
{
    ObjectGuid guid;           // used only for proper packet read
    MovementInfo movementInfo; // used only for proper packet read
    uint32 movementCounter;    // spline counter

    Unit* mover = _player->m_mover;

    // 3.4.3 (writer FUN_140763550): the status, u32 SplineID
    ReadMovementInfo(recvData, &movementInfo);
    guid = movementInfo.guid;
    recvData >> movementCounter;

    if (_player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_TAXI_FLIGHT)) // taxi spline case
    {
        // in taxi flight packet received in 2 case:
        // 1) end taxi path in far (multi-node) flight
        // 2) switch from one map to other in case multim-map taxi path
        // we need process only (1)

        uint32 curDest = GetPlayer()->m_taxi.GetTaxiDestination();
        if (curDest)
        {
            TaxiNodesEntry const* curDestNode = sTaxiNodesStore.LookupEntry(curDest);

            // far teleport case
            if (curDestNode && curDestNode->map_id != GetPlayer()->GetMapId() && GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
            {
                if (FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(GetPlayer()->GetMotionMaster()->top()))
                {
                    // short preparations to continue flight
                    flight->SetCurrentNodeAfterTeleport();
                    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
                    flight->SkipCurrentNode();

                    GetPlayer()->TeleportTo(curDestNode->map_id, node->x, node->y, node->z, GetPlayer()->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
                }
            }

            return;
        }

        // at this point only 1 node is expected (final destination)
        if (GetPlayer()->m_taxi.GetPath().size() != 1)
        {
            return;
        }

        GetPlayer()->CleanupAfterTaxiFlight();
        GetPlayer()->SetFallInformation(GameTime::GetGameTime().count(), GetPlayer()->GetPositionZ());
        if (GetPlayer()->pvpInfo.IsHostile)
        {
            GetPlayer()->CastSpell(GetPlayer(), 2479, true);
        }
        return;
    }

    if (mover->GetGUID() != guid)
        return;

    if (!_player->IsExpectingChangeTransport() || !mover->movespline || mover->movespline->GetId() != movementCounter)
        return;

    _player->SetExpectingChangeTransport(false);
    WorldPacket data(_player->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_ROOT) ? SMSG_MOVE_UPDATE : SMSG_MOVE_UPDATE, recvData.size());
    WriteMovementInfo(&data, &movementInfo);
    mover->SendMessageToSet(&data, _player);
}

void WorldSession::HandleActivateTaxiOpcode(WorldPacket& recvData)
{
    // 3.4.3 (writer FUN_14075b950): PackedGuid128 Vendor, u32 Node (the destination), u32 GroundMountID, u32
    // FlyingMountID. There is no source node: it is the vendor's (the nearest). The sender never sets the
    // two mount ids (FUN_141745dd0): they are stack garbage.
    ObjectGuid guid = Guid128::Read(recvData);
    std::vector<uint32> nodes;
    nodes.resize(2);
    GetPlayer()->SetCanTeleport(true);
    recvData >> nodes[1];
    recvData.read_skip<uint32>();
    recvData.read_skip<uint32>();
    Creature* npc = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        LOG_DEBUG("network", "WORLD: HandleActivateTaxiOpcode - Unit ({}) not found or you can't interact with it.", guid.ToString());
        SendActivateTaxiReply(ERR_TAXITOOFARAWAY);
        return;
    }

    nodes[0] = sObjectMgr->GetNearestTaxiNode(*npc, GetPlayer()->GetTeamId(true));
    LOG_DEBUG("network", "WORLD: Received CMSG_ACTIVATE_TAXI from {} to {}", nodes[0], nodes[1]);
    if (!nodes[0] || nodes[0] == nodes[1])
    {
        SendActivateTaxiReply(nodes[0] ? ERR_TAXISAMENODE : ERR_TAXINOVENDORNEARBY);
        return;
    }

    if (!GetPlayer()->isTaxiCheater())
    {
        if (!GetPlayer()->m_taxi.IsTaximaskNodeKnown(nodes[0]) || !GetPlayer()->m_taxi.IsTaximaskNodeKnown(nodes[1]))
        {
            SendActivateTaxiReply(ERR_TAXINOTVISITED);
            return;
        }
    }

    GetPlayer()->ActivateTaxiPathTo(nodes, npc, 0);
}

void WorldSession::SendActivateTaxiReply(ActivateTaxiReply reply)
{
    GetPlayer()->SetCanTeleport(true);
    // 3.4.3 (FUN_1406b6520, handler FUN_141a52bd0): 4-bit Reply, flush (3.3.5's numbering, 13 in combat)
    WorldPacket data(SMSG_ACTIVATE_TAXI_REPLY, 1);
    data.WriteBits(uint32(reply), 4);
    data.FlushBits();
    SendPacket(&data);

    LOG_DEBUG("network", "WORLD: Sent SMSG_ACTIVATE_TAXI_REPLY");
}
