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

#include "Ed25519.h"
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/pem.h>

Acore::Crypto::Ed25519::Ed25519() : _key(nullptr) { }

Acore::Crypto::Ed25519::~Ed25519()
{
    EVP_PKEY_free(_key);
}

bool Acore::Crypto::Ed25519::LoadPrivateKeyFromFile(std::string const& fileName)
{
    // through a BIO OpenSSL opens itself: a FILE* from this module's CRT handed to the OpenSSL DLL
    // needs OPENSSL_Applink on Windows, and without it the process exits
    BIO* file = BIO_new_file(fileName.c_str(), "r");
    if (!file)
        return false;

    EVP_PKEY* key = PEM_read_bio_PrivateKey(file, nullptr, nullptr, nullptr);
    BIO_free(file);
    if (!key || EVP_PKEY_get_base_id(key) != EVP_PKEY_ED25519)
    {
        EVP_PKEY_free(key);
        return false;
    }

    EVP_PKEY_free(_key);
    _key = key;
    return true;
}

Acore::Crypto::Ed25519::PublicKey Acore::Crypto::Ed25519::GetPublicKey() const
{
    PublicKey publicKey{};
    std::size_t length = publicKey.size();
    if (_key)
        EVP_PKEY_get_raw_public_key(_key, publicKey.data(), &length);
    return publicKey;
}

bool Acore::Crypto::Ed25519::Sign(uint8 const* message, std::size_t messageLength, uint8 const* context,
    std::size_t contextLength, Signature& signature) const
{
    if (!_key)
        return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return false;

    OSSL_PARAM params[3];
    std::size_t count = 0;
    char instance[] = "Ed25519ctx";
    if (context && contextLength)
    {
        params[count++] = OSSL_PARAM_construct_utf8_string(OSSL_SIGNATURE_PARAM_INSTANCE, instance, 0);
        params[count++] = OSSL_PARAM_construct_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING,
            const_cast<uint8*>(context), contextLength);
    }
    params[count] = OSSL_PARAM_construct_end();

    std::size_t length = signature.size();
    bool ok = EVP_DigestSignInit_ex(ctx, nullptr, nullptr, nullptr, nullptr, _key, params) == 1
        && EVP_DigestSign(ctx, signature.data(), &length, message, messageLength) == 1
        && length == signature.size();
    EVP_MD_CTX_free(ctx);
    return ok;
}
