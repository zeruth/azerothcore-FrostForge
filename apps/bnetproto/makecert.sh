#!/usr/bin/env bash
# Makes the certificate the authserver's Battle.net port serves (BattlenetCertificatesFile,
# BattlenetPrivateKeyFile): a local FrostForge root CA, and a server certificate it signs
# for CN=127.0.0.1 with SAN IP:127.0.0.1 and DNS:localhost.
#
#   apps/bnetproto/makecert.sh <out dir>
#
# The client validates against the Windows trusted roots, so the CA must be trusted by the
# user that runs it. That step changes the user's trust store and is left to you:
#   certutil -user -addstore Root <out dir>/frostforge-ca.cert.pem
# An existing CA in <out dir> is reused, so re-running only reissues the server certificate.
set -euo pipefail
# Git Bash would rewrite "/CN=..." into a Windows path.
export MSYS_NO_PATHCONV=1

out="${1:?usage: makecert.sh <out dir>}"
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$out"
cp "$here/bnetserver.ext" "$out/bnetserver.ext"
cd "$out"

if [ ! -f frostforge-ca.key.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
        -keyout frostforge-ca.key.pem -out frostforge-ca.cert.pem \
        -subj "/CN=FrostForge Local Root CA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"
fi

openssl req -newkey rsa:2048 -nodes -sha256 \
    -keyout bnetserver.key.pem -out bnetserver.csr.pem \
    -subj "/CN=127.0.0.1"
openssl x509 -req -sha256 -days 825 \
    -in bnetserver.csr.pem -CA frostforge-ca.cert.pem -CAkey frostforge-ca.key.pem -CAcreateserial \
    -extfile bnetserver.ext -out bnetserver.leaf.pem
rm -f bnetserver.csr.pem bnetserver.ext

# use_certificate_chain_file wants the leaf first, then the issuer.
cat bnetserver.leaf.pem frostforge-ca.cert.pem > bnetserver.cert.pem
rm -f bnetserver.leaf.pem
openssl verify -CAfile frostforge-ca.cert.pem bnetserver.cert.pem
