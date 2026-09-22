#!/bin/sh
# update-repo-index.sh - regenerate the flat apt repository index for a
# directory containing pool/: Packages, Packages.gz and Release, and sign
# it (InRelease + Release.gpg, plus the public key as
# memmon-archive-keyring.gpg) when a GPG key is configured.
#
# Usage: ./scripts/update-repo-index.sh [repo-dir]      (default: repo root)
#
# Signing: set MEMMON_GPG_KEY to the key ID/fingerprint to sign with (the
# secret key must be in the gpg keyring). Without it the repo is left
# unsigned, which apt only accepts with [trusted=yes].
#
# Requires: dpkg-scanpackages (dpkg-dev), gzip, md5sum/sha1sum/sha256sum.
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "${1:-$HERE}" && pwd)"
cd "$REPO"

if [ ! -d pool ]; then
    echo "error: $REPO has no pool/ directory" >&2
    exit 1
fi

echo "==> Regenerating Packages / Packages.gz in $REPO"
dpkg-scanpackages --multiversion pool/ > Packages
gzip -9 -n -f -k Packages

# Release lists every index with its size and hashes. Only Packages*
# files are covered, so it's safe to run in a full git checkout.
ARCHES="$(sed -n 's/^Architecture: //p' Packages | sort -u | tr '\n' ' ' | sed 's/ $//')"
hashes() {
    # $1 = checksum tool
    for f in Packages Packages.gz; do
        printf ' %s %s %s\n' "$($1 "$f" | cut -d' ' -f1)" "$(wc -c < "$f" | tr -d ' ')" "$f"
    done
}
{
    echo "Origin: memmon"
    echo "Label: memmon"
    echo "Suite: stable"
    echo "Codename: memmon"
    echo "Date: $(LC_ALL=C date -u -R)"
    echo "Architectures: ${ARCHES:-amd64}"
    echo "Description: memmon flat apt repository"
    echo "MD5Sum:"
    hashes md5sum
    echo "SHA1:"
    hashes sha1sum
    echo "SHA256:"
    hashes sha256sum
} > Release
echo "==> Wrote Release (architectures: ${ARCHES:-amd64})"

rm -f InRelease Release.gpg
if [ -n "${MEMMON_GPG_KEY:-}" ]; then
    echo "==> Signing with key $MEMMON_GPG_KEY"
    gpg --batch --yes --default-key "$MEMMON_GPG_KEY" --clearsign -o InRelease Release
    gpg --batch --yes --default-key "$MEMMON_GPG_KEY" -abs -o Release.gpg Release
    gpg --batch --yes --export "$MEMMON_GPG_KEY" > memmon-archive-keyring.gpg
    echo "==> Signed: InRelease, Release.gpg; public key: memmon-archive-keyring.gpg"
else
    echo "==> MEMMON_GPG_KEY not set: repository left unsigned"
fi
