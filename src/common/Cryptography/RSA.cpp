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

#include "RSA.h"
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

Acore::Crypto::RSA::RSA() : _key(nullptr) { }

Acore::Crypto::RSA::~RSA()
{
    EVP_PKEY_free(_key);
}

bool Acore::Crypto::RSA::LoadPrivateKeyFromFile(std::string const& fileName)
{
    // through a BIO OpenSSL opens itself, as Ed25519 does (no OPENSSL_Applink on Windows)
    BIO* file = BIO_new_file(fileName.c_str(), "r");
    if (!file)
        return false;

    EVP_PKEY* key = PEM_read_bio_PrivateKey(file, nullptr, nullptr, nullptr);
    BIO_free(file);
    if (!key || EVP_PKEY_get_base_id(key) != EVP_PKEY_RSA)
    {
        EVP_PKEY_free(key);
        return false;
    }

    EVP_PKEY_free(_key);
    _key = key;
    return true;
}

std::size_t Acore::Crypto::RSA::GetSize() const
{
    return _key ? std::size_t(EVP_PKEY_get_size(_key)) : 0;
}

bool Acore::Crypto::RSA::SignSHA256(uint8 const* message, std::size_t messageLength, std::vector<uint8>& signature) const
{
    if (!_key)
        return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return false;

    EVP_PKEY_CTX* keyCtx = nullptr;
    std::size_t length = GetSize();
    signature.resize(length);
    bool ok = EVP_DigestSignInit(ctx, &keyCtx, EVP_sha256(), nullptr, _key) == 1
        && EVP_PKEY_CTX_set_rsa_padding(keyCtx, RSA_PKCS1_PADDING) == 1
        && EVP_DigestSign(ctx, signature.data(), &length, message, messageLength) == 1
        && length == signature.size();
    EVP_MD_CTX_free(ctx);
    return ok;
}
