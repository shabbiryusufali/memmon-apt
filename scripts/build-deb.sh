#!/bin/sh
# build-deb.sh - build the memmon .deb via the real Debian packaging in
# debian/ (dpkg-buildpackage), then copy the result into pool/main/ and
# regenerate the flat apt repository index (Packages, Packages.gz,
# Release) at the repo root so `apt update && apt install memmon` works
# against this repo as-is.
#
# Requires: dpkg-dev, debhelper, libncursesw5-dev, pkg-config.
#   sudo apt-get install build-essential debhelper libncursesw5-dev pkg-config dpkg-dev
#
# Usage: ./scripts/build-deb.sh
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

echo "==> Publishing to pool/main/ and regenerating repo index"
mkdir -p "$HERE/pool/main"
cp "$DEB" "$HERE/pool/main/"

cd "$HERE"
dpkg-scanpackages --multiversion pool/ > Packages
gzip -9 -n -f -k Packages

echo ""
echo "Built and published: pool/main/memmon_${VERSION}_${ARCH}.deb"
echo "Repo index refreshed: Packages, Packages.gz"
echo ""
echo "Install locally with:  sudo apt install $HERE/pool/main/memmon_${VERSION}_${ARCH}.deb"
