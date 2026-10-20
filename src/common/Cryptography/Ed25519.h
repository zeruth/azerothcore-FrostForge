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

#ifndef AZEROTHCORE_ED25519_H
#define AZEROTHCORE_ED25519_H

#include "Define.h"
#include <array>
#include <string>

struct evp_pkey_st;

namespace Acore::Crypto
{
    // Ed25519 and Ed25519ctx (RFC 8032) signing with a private key loaded from PEM. The 3.4.3
    // client checks SMSG_ENTER_ENCRYPTED_MODE with Ed25519ctx and a 16-byte context.
    class AC_COMMON_API Ed25519
    {
    public:
        static constexpr std::size_t SIGNATURE_LENGTH = 64;
        static constexpr std::size_t PUBLIC_KEY_LENGTH = 32;
        using Signature = std::array<uint8, SIGNATURE_LENGTH>;
        using PublicKey = std::array<uint8, PUBLIC_KEY_LENGTH>;

        Ed25519();
        ~Ed25519();
        Ed25519(Ed25519 const&) = delete;
        Ed25519& operator=(Ed25519 const&) = delete;

        bool LoadPrivateKeyFromFile(std::string const& fileName);
        bool IsLoaded() const { return _key != nullptr; }
        PublicKey GetPublicKey() const;

        // Ed25519ctx when context is given (and non-empty), pure Ed25519 otherwise.
        bool Sign(uint8 const* message, std::size_t messageLength, uint8 const* context, std::size_t contextLength,
            Signature& signature) const;

    private:
        evp_pkey_st* _key;
    };
}

#endif
