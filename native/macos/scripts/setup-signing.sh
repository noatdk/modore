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

IDENTITY_NAME="ModelessIMEHost Dev"
LOGIN_KEYCHAIN="${HOME}/Library/Keychains/login.keychain-db"

if security find-identity -p codesigning -v 2>/dev/null \
        | grep -q "\"$IDENTITY_NAME\""; then
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
openssl pkcs12 -export -inkey "$WORKDIR/key.pem" -in "$WORKDIR/cert.pem" \
    -name "$IDENTITY_NAME" -out "$WORKDIR/id.p12" -passout pass: >/dev/null 2>&1

# Import the cert+key into the login keychain. -T grants /usr/bin/codesign
# access without a per-build trust prompt.
security import "$WORKDIR/id.p12" \
    -k "$LOGIN_KEYCHAIN" \
    -P "" \
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

if ! security find-identity -p codesigning -v 2>/dev/null \
        | grep -q "\"$IDENTITY_NAME\""; then
    echo "Identity import appears to have failed. 'security find-identity'" >&2
    echo "did not list '$IDENTITY_NAME' after import." >&2
    exit 1
fi

echo "Identity '$IDENTITY_NAME' is ready."
