#!/bin/sh
# build-deb.sh - build the memmon .deb via the real Debian packaging in
# debian/ (dpkg-buildpackage, which also runs the test suite), then copy
# the result into pool/main/ and regenerate the flat apt repository index
# (Packages, Packages.gz, Release - signed if MEMMON_GPG_KEY is set) at
# the repo root so `apt update && apt install memmon` works against this
# repo as-is.
#
# Requires: dpkg-dev, debhelper, libncurses-dev, zlib1g-dev, pkg-config.
#   sudo apt-get install build-essential debhelper libncurses-dev zlib1g-dev pkg-config dpkg-dev
#
# Usage: ./scripts/build-deb.sh
#        MEMMON_GPG_KEY=<key-id> ./scripts/build-deb.sh     (also sign the repo)
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

echo "==> Building package with dpkg-buildpackage"
dpkg-buildpackage -us -uc -b

VERSION="$(dpkg-parsechangelog -SVersion)"
ARCH="$(dpkg --print-architecture)"
DEB="../memmon_${VERSION}_${ARCH}.deb"

if [ ! -f "$DEB" ]; then
    echo "error: expected build output '$DEB' not found" >&2
    exit 1
fi

if command -v lintian >/dev/null 2>&1; then
    echo "==> Running lintian"
    lintian --fail-on error "$DEB"
fi

echo "==> Publishing to pool/main/ and regenerating repo index"
mkdir -p "$HERE/pool/main"
cp "$DEB" "$HERE/pool/main/"
"$HERE/scripts/update-repo-index.sh" "$HERE"

echo ""
echo "Built and published: pool/main/memmon_${VERSION}_${ARCH}.deb"
echo "Repo index refreshed: Packages, Packages.gz, Release"
echo ""
echo "Install locally with:  sudo apt install $HERE/pool/main/memmon_${VERSION}_${ARCH}.deb"
