/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AZEROTHCORE_RSA_H
#define AZEROTHCORE_RSA_H

#include "Define.h"
#include <string>
#include <vector>

struct evp_pkey_st;

namespace Acore::Crypto
{
    // RSA PKCS#1 v1.5 signing over SHA-256 with a private key loaded from PEM. The 3.4.3 client
    // checks SMSG_CONNECT_TO with a 2048-bit key this way (its signature little-endian).
    class AC_COMMON_API RSA
    {
    public:
        RSA();
        ~RSA();
        RSA(RSA const&) = delete;
        RSA& operator=(RSA const&) = delete;

        bool LoadPrivateKeyFromFile(std::string const& fileName);
        bool IsLoaded() const { return _key != nullptr; }
        std::size_t GetSize() const;

        // The signature of SHA-256(message), big-endian, GetSize() bytes.
        bool SignSHA256(uint8 const* message, std::size_t messageLength, std::vector<uint8>& signature) const;

    private:
        evp_pkey_st* _key;
    };
}

#endif
