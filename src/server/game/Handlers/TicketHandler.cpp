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

#include "CharacterCache.h"
#include "Chat.h"
#include "GameTime.h"
#include "Language.h"
#include "Opcodes.h"
#include "PacketGuid128.h"
#include "Player.h"
#include "TicketMgr.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <zlib.h>

/// 3.4.3 has no ticket of its own to open, read or update: the help frame submits bugs, suggestions and complaints and
/// forgets them. Each becomes (or adds to) the player's GM ticket, so the .ticket commands still see them.
void WorldSession::CreateSupportTicket(uint32 mapId, float x, float y, float z, std::string const& message)
{
    if (!sTicketMgr->GetStatus() || !ValidateHyperlinksAndMaybeKick(message))
        return;

    if (GmTicket* ticket = sTicketMgr->GetTicketByPlayer(GetPlayer()->GetGUID()))
    {
        ticket->SetMessage(ticket->GetMessage() + "\n" + message);
        ticket->SetPosition(mapId, x, y, z);
        CharacterDatabaseTransaction trans = CharacterDatabaseTransaction(nullptr);
        ticket->SaveToDB(trans);
        sTicketMgr->UpdateLastChange(ticket);
        return;
    }

    GmTicket* ticket = new GmTicket(GetPlayer());
    ticket->SetPosition(mapId, x, y, z);
    ticket->SetMessage(message);
    sTicketMgr->AddTicket(ticket);
    sTicketMgr->UpdateLastChange(ticket);
}

/// SupportTicketHeader, 3.4.3: i32 MapID, f32 X, f32 Y, f32 Z, f32 Facing, i32 Program
static void ReadSupportTicketHeader(WorldPacket& data, uint32& mapId, float& x, float& y, float& z)
{
    int32 map;
    data >> map >> x >> y >> z;
    data.read_skip<float>();                                // Facing
    data.read_skip<int32>();                                // Program
    mapId = uint32(map);
}

/// CMSG_SUPPORT_TICKET_SUBMIT_BUG / _SUGGESTION, 3.4.3 (FUN_140769a30 / FUN_140769b70, Lua GMSubmitBug /
/// GMSubmitSuggestion): SupportTicketHeader, bits(10) NoteLen, flush, Note
void WorldSession::HandleSupportTicketSubmitBug(WorldPacket& recvData)
{
    uint32 mapId;
    float x, y, z;
    ReadSupportTicketHeader(recvData, mapId, x, y, z);
    uint32 noteLength = recvData.ReadBits(10);
    std::string note = recvData.ReadString(noteLength);

    CreateSupportTicket(mapId, x, y, z, (recvData.GetOpcode() == CMSG_SUPPORT_TICKET_SUBMIT_BUG ? "[bug] " : "[suggestion] ") + note);
}

/// CMSG_SUPPORT_TICKET_SUBMIT_COMPLAINT, 3.4.3 (FUN_140757a00, from C_ReportSystem.SendReport): SupportTicketHeader,
/// PackedGuid128 TargetCharacterGUID, i32 ReportType, i32 MajorCategory, i32 MinorCategoryFlags, the chat log (u32
/// LineCount, bit HasReportLineIndex, flush, lines of u64 Timestamp, bits(12) TextLen, flush, Text, [u32 index]), bits(10)
/// NoteLen, nine Has* bits (mail, calendar, pet, guild, LFG list search result, LFG list applicant, club message, club
/// finder result, unused), flush, [bit, flush], the Horus chat log, Note, then the optional blocks (not read: the note is
/// what the GM needs). The client asks no answer.
void WorldSession::HandleSupportTicketSubmitComplaint(WorldPacket& recvData)
{
    uint32 mapId;
    float x, y, z;
    ReadSupportTicketHeader(recvData, mapId, x, y, z);
    ObjectGuid target = Guid128::Read(recvData);
    recvData.read_skip<int32>();                            // ReportType
    recvData.read_skip<int32>();                            // MajorCategory
    recvData.read_skip<int32>();                            // MinorCategoryFlags

    uint32 lineCount = recvData.read<uint32>();
    bool hasLineIndex = recvData.ReadBit();
    if (lineCount > 1000)
    {
        recvData.rfinish();
        return;
    }
    for (uint32 i = 0; i < lineCount; ++i)
    {
        recvData.read_skip<uint64>();                       // Timestamp
        recvData.ResetBitReader();
        uint32 length = recvData.ReadBits(12);
        recvData.read_skip(length);
    }
    if (hasLineIndex)
        recvData.read_skip<uint32>();

    recvData.ResetBitReader();
    uint32 noteLength = recvData.ReadBits(10);
    recvData.ReadBit();                                     // HasMailInfo
    recvData.ReadBit();                                     // HasCalendarInfo
    recvData.ReadBit();                                     // HasPetInfo
    recvData.ReadBit();                                     // HasGuildInfo
    recvData.ReadBit();                                     // HasLFGListSearchResult
    recvData.ReadBit();                                     // HasLFGListApplicant
    bool hasClubMessage = recvData.ReadBit();
    recvData.ReadBit();                                     // HasClubFinderResult
    recvData.ReadBit();                                     // HasUnused910
    if (hasClubMessage)
    {
        recvData.ResetBitReader();
        recvData.ReadBit();                                 // IsPlayerUsingVoice
    }

    uint32 horusCount = recvData.read<uint32>();
    if (horusCount > 1000)
    {
        recvData.rfinish();
        return;
    }
    for (uint32 i = 0; i < horusCount; ++i)
    {
        recvData.read_skip<uint64>();                       // Timestamp
        Guid128::Read(recvData);                            // AuthorGUID
        recvData.ResetBitReader();
        bool hasClubId = recvData.ReadBit();
        bool hasChannelGuid = recvData.ReadBit();
        bool hasRealmAddress = recvData.ReadBit();
        bool hasSlashCmd = recvData.ReadBit();
        uint32 length = recvData.ReadBits(12);
        if (hasClubId)
            recvData.read_skip<uint64>();
        if (hasChannelGuid)
            Guid128::Read(recvData);
        if (hasRealmAddress)
        {
            recvData.read_skip<uint32>();
            recvData.read_skip<uint16>();
            recvData.read_skip<uint8>();
        }
        if (hasSlashCmd)
            recvData.read_skip<int32>();
        recvData.read_skip(length);
    }

    std::string note = recvData.ReadString(noteLength);
    recvData.rfinish();                                     // the optional blocks

    std::string targetName;
    sCharacterCache->GetCharacterNameByGuid(target, targetName);
    CreateSupportTicket(mapId, x, y, z, "[complaint vs " + (targetName.empty() ? target.ToString() : targetName) + "] " + note);
}

/// CMSG_GM_TICKET_GET_CASE_STATUS, 3.4.3 (Lua GetWebTicket): empty. SMSG_GM_TICKET_CASE_STATUS (FUN_1406fb9b0): i32
/// Count, the web cases; there are none (UPDATE_WEB_TICKET "no case").
void WorldSession::HandleGMTicketGetCaseStatusOpcode(WorldPacket& /*recvData*/)
{
    WorldPacket data(SMSG_GM_TICKET_CASE_STATUS, 4);
    data << int32(0);
    SendPacket(&data);
}

/// SMSG_GM_TICKET_SYSTEM_STATUS, 3.4.3 (raw, handler FUN_141735430): i32 Status (1 enabled, 0 disabled)
void WorldSession::HandleGMTicketSystemStatusOpcode(WorldPacket& /*recv_data*/)
{
    // Note: This only disables the ticket UI at client side and is not fully reliable
    // are we sure this is a uint32? Should ask Zor
    WorldPacket data(SMSG_GM_TICKET_SYSTEM_STATUS, 4);
    data << uint32(sTicketMgr->GetStatus() ? GMTICKET_QUEUE_STATUS_ENABLED : GMTICKET_QUEUE_STATUS_DISABLED);
    SendPacket(&data);
}

void WorldSession::HandleGMSurveySubmit(WorldPacket& recv_data)
{
    uint32 nextSurveyID = sTicketMgr->GetNextSurveyID();
    // just put the survey into the database
    uint32 mainSurvey; // GMSurveyCurrentSurvey.dbc, column 1 (all 9) ref to GMSurveySurveys.dbc
    recv_data >> mainSurvey;

    std::unordered_set<uint32> surveyIds;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // sub_survey1, r1, comment1, sub_survey2, r2, comment2, sub_survey3, r3, comment3, sub_survey4, r4, comment4, sub_survey5, r5, comment5, sub_survey6, r6, comment6, sub_survey7, r7, comment7, sub_survey8, r8, comment8, sub_survey9, r9, comment9, sub_survey10, r10, comment10,
    for (uint8 i = 0; i < 10; i++)
    {
        uint32 subSurveyId; // ref to i'th GMSurveySurveys.dbc field (all fields in that dbc point to fields in GMSurveyQuestions.dbc)
        recv_data >> subSurveyId;
        if (!subSurveyId)
            break;

        uint8 rank; // probably some sort of ref to GMSurveyAnswers.dbc
        recv_data >> rank;
        std::string comment; // comment ("Usage: GMSurveyAnswerSubmit(question, rank, comment)")
        recv_data >> comment;

        if (!ValidateHyperlinksAndMaybeKick(comment))
        {
            return;
        }

        // make sure the same sub survey is not added to DB twice
        if (!surveyIds.insert(subSurveyId).second)
            continue;

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_GM_SUBSURVEY);
        stmt->SetData(0, nextSurveyID);
        stmt->SetData(1, subSurveyId);
        stmt->SetData(2, rank);
        stmt->SetData(3, comment);
        trans->Append(stmt);
    }

    std::string comment; // just a guess
    recv_data >> comment;

    if (!ValidateHyperlinksAndMaybeKick(comment))
    {
        return;
    }

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_GM_SURVEY);
    stmt->SetData(0, GetPlayer()->GetGUID().GetCounter());
    stmt->SetData(1, nextSurveyID);
    stmt->SetData(2, mainSurvey);
    stmt->SetData(3, comment);

    trans->Append(stmt);

    CharacterDatabase.CommitTransaction(trans);
}

/// CMSG_REPORT_SERVER_LAG, 3.4.3 (FUN_140760470): empty. The map and position are the player's own.
void WorldSession::HandleReportLag(WorldPacket& /*recv_data*/)
{
    // just put the lag report into the database...
    // can't think of anything else to do with it
    uint32 lagType = 0;
    uint32 mapId = GetPlayer()->GetMapId();
    float x = GetPlayer()->GetPositionX();
    float y = GetPlayer()->GetPositionY();
    float z = GetPlayer()->GetPositionZ();

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_LAG_REPORT);
    stmt->SetData(0, GetPlayer()->GetGUID().GetCounter());
    stmt->SetData (1, lagType);
    stmt->SetData(2, mapId);
    stmt->SetData (3, x);
    stmt->SetData (4, y);
    stmt->SetData (5, z);
    stmt->SetData(6, GetLatency());
    stmt->SetData(7, GameTime::GetGameTime().count());
    CharacterDatabase.Execute(stmt);
}
