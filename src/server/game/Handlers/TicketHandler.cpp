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

#include "Chat.h"
#include "GameTime.h"
#include "Language.h"
#include "Opcodes.h"
#include "Player.h"
#include "TicketMgr.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <zlib.h>

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

void WorldSession::HandleReportLag(WorldPacket& recv_data)
{
    // just put the lag report into the database...
    // can't think of anything else to do with it
    uint32 lagType, mapId;
    recv_data >> lagType;
    recv_data >> mapId;
    float x, y, z;
    recv_data >> x;
    recv_data >> y;
    recv_data >> z;

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
