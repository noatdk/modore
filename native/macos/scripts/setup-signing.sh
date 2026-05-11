#!/bin/bash
# Create the self-signed code-signing identity used by `codesign` in the
# macOS host Makefile.
#
# Why this exists:
#   * Ad-hoc signing (`codesign --sign -`) ties the code identity to the
#     binary's content hash. macOS's TCC (Accessibility, Input Monitoring,
#     etc.) treats every rebuild as a new app → re-prompts forever.
#   * A stable self-signed identity gives the binary a stable designated
#     requirement, so the Accessibility permission grant survives rebuilds.
#
# This script is:
#   * Idempotent: if the identity already exists, exit 0 immediately.
#   * Self-contained: uses /usr/bin/openssl and /usr/bin/security only.
#   * Local: writes a cert to your login keychain, scoped to /usr/bin/codesign.

set -euo pipefail

IDENTITY_NAME="modore Dev"
LOGIN_KEYCHAIN="${HOME}/Library/Keychains/login.keychain-db"
# Arbitrary non-empty password for the throwaway PKCS12 bundle. macOS's
# `security import` rejects PKCS12 files protected by an empty password with
# "MAC verification failed (wrong password?)" — likely a libsecurity quirk.
# The value doesn't matter; the file is deleted seconds after creation.
P12_PASS="modore-internal"

if security find-certificate -c "$IDENTITY_NAME" "$LOGIN_KEYCHAIN" >/dev/null 2>&1; then
    exit 0
fi

echo "Creating self-signed code-signing identity '$IDENTITY_NAME' …"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

cat > "$WORKDIR/cert.cnf" <<EOF
[ req ]
distinguished_name = dn
prompt             = no
[ dn ]
CN = $IDENTITY_NAME
[ v3_ext ]
keyUsage         = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
basicConstraints = critical,CA:false
EOF

openssl genrsa -out "$WORKDIR/key.pem" 2048 >/dev/null 2>&1
openssl req -new -x509 -key "$WORKDIR/key.pem" -out "$WORKDIR/cert.pem" \
    -days 3650 -config "$WORKDIR/cert.cnf" -extensions v3_ext >/dev/null 2>&1
# `-legacy` forces the old-style PKCS12 format (RC2-40 + SHA1 MAC). OpenSSL
# 3.x's default uses SHA256 MAC + AES-256, which macOS's `security` tool
# rejects in some configurations.
openssl pkcs12 -export -legacy -inkey "$WORKDIR/key.pem" -in "$WORKDIR/cert.pem" \
    -name "$IDENTITY_NAME" -out "$WORKDIR/id.p12" -passout "pass:$P12_PASS" >/dev/null 2>&1

# Import the cert+key into the login keychain. -T grants /usr/bin/codesign
# access without a per-build trust prompt.
security import "$WORKDIR/id.p12" \
    -k "$LOGIN_KEYCHAIN" \
    -P "$P12_PASS" \
    -T /usr/bin/codesign \
    -T /usr/bin/security >/dev/null

# On Sonoma+ the partition list also needs to whitelist codesign. The -k ""
# uses the login keychain's password; if the keychain is unlocked (the normal
# state of an active session) this succeeds without a prompt. If it fails
# (e.g. running in a non-interactive context), the import still worked —
# codesign will just prompt for trust on its first run.
security set-key-partition-list \
    -S "apple-tool:,apple:,codesign:" \
    -s -k "" "$LOGIN_KEYCHAIN" >/dev/null 2>&1 || true

# Self-signed code-signing certs don't always show up in
# `security find-identity -v -p codesigning` (which only lists certs the
# system trusts for code signing) — but `codesign` will still use them by
# CN. Verify via the cert's presence in the keychain instead.
if ! security find-certificate -c "$IDENTITY_NAME" "$LOGIN_KEYCHAIN" >/dev/null 2>&1; then
    echo "Identity import appears to have failed. '$IDENTITY_NAME' not in" >&2
    echo "the login keychain after import." >&2
    exit 1
fi

echo "Identity '$IDENTITY_NAME' is ready."
