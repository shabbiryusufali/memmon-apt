#!/bin/sh
# build-deb.sh - build memmon_<version>_amd64.deb from source.
#
# Usage: ./build-deb.sh [version]
#   version defaults to 1.0-1 (Debian-style upstream-debianrevision)
#
# Requires: gcc/cc, libncursesw5-dev (or ncurses-dev), dpkg-deb.
set -e

VERSION="${1:-1.0-1}"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
HERE="$(cd "$(dirname "$0")" && pwd)"
PKGNAME="memmon"
BUILDROOT="$HERE/build/${PKGNAME}_${VERSION}_${ARCH}"

echo "==> Building memmon binary"
cd "$HERE"
make clean >/dev/null 2>&1 || true
make

echo "==> Assembling package tree in $BUILDROOT"
rm -rf "$HERE/build"
mkdir -p "$BUILDROOT/DEBIAN"
mkdir -p "$BUILDROOT/usr/bin"
mkdir -p "$BUILDROOT/lib/systemd/system"
mkdir -p "$BUILDROOT/usr/share/doc/memmon"
mkdir -p "$BUILDROOT/usr/share/man/man1"

install -m755 memmon "$BUILDROOT/usr/bin/memmon"
install -m644 README.md "$BUILDROOT/usr/share/doc/memmon/README.md"
gzip -9 -n -c memmon.1 > "$BUILDROOT/usr/share/man/man1/memmon.1.gz"
chmod 644 "$BUILDROOT/usr/share/man/man1/memmon.1.gz"

# Ship the systemd unit with the FHS install path baked in.
sed 's#/usr/local/bin/memmon#/usr/bin/memmon#' memmon.service \
    > "$BUILDROOT/lib/systemd/system/memmon.service"
chmod 644 "$BUILDROOT/lib/systemd/system/memmon.service"

echo "==> Writing control file"
NCURSES_DEP=$(ldd "$BUILDROOT/usr/bin/memmon" | grep -q libncursesw && echo "libncursesw6 (>= 6), " || echo "")
cat > "$BUILDROOT/DEBIAN/control" <<EOF
Package: $PKGNAME
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: libc6 (>= 2.34), ${NCURSES_DEP}libtinfo6 (>= 6)
Maintainer: CHANGE ME <you@example.com>
Description: scrollable, man-page-styled Linux memory usage monitor
 memmon reads /proc/meminfo (total/used/free/available memory, buffers,
 cache, shared memory, swap, active/inactive, reclaimable slab,
 dirty/writeback, commit, vmalloc, huge pages, and every other field the
 running kernel exposes) along with the load average, and displays it as
 a scrollable, man-page-styled terminal report that auto-refreshes on a
 configurable interval (seconds, minutes, hours, or days).
 .
 It can also run headless via \`memmon --daemon\`, appending timestamped
 snapshots to a daily log file (YYYY-MM-DD.log); a systemd service unit
 is included to run it this way automatically at boot.
EOF
echo "Installed-Size: $(du -sk "$BUILDROOT" | cut -f1)" >> "$BUILDROOT/DEBIAN/control"

cat > "$BUILDROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ] && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
exit 0
EOF

cat > "$BUILDROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ -d /run/systemd/system ]; then
    case "$1" in
        remove)
            systemctl stop memmon.service >/dev/null 2>&1 || true
            systemctl daemon-reload >/dev/null 2>&1 || true
            ;;
        purge)
            systemctl disable --now memmon.service >/dev/null 2>&1 || true
            systemctl daemon-reload >/dev/null 2>&1 || true
            ;;
    esac
fi
exit 0
EOF
chmod 755 "$BUILDROOT/DEBIAN/postinst" "$BUILDROOT/DEBIAN/postrm"
find "$BUILDROOT" -type d -exec chmod 755 {} \;

echo "==> Building .deb"
OUT="$HERE/build/${PKGNAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$BUILDROOT" "$OUT"

echo ""
echo "Built: $OUT"
echo "Install with:  sudo apt install $OUT"
