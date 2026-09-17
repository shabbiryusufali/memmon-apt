# Packaging memmon as a .deb

There are two different things people mean by "install via apt", and both
are covered below:

1. **Install a `.deb` file directly with apt** — works today, no server
   needed, apt still resolves and installs dependencies for you.
2. **Set up a real apt repository** — lets you (and any other machine) do
   `apt update && apt install memmon` after adding one line to
   `/etc/apt/sources.list.d/`, and get upgrades the normal way.

This is a real Debian source package (`debian/` uses debhelper, compat 13),
built with the standard `dpkg-buildpackage` tooling — not a hand-rolled
`DEBIAN/control` tree. A pre-built package lives in `pool/main/`, with the
matching `Packages`/`Packages.gz` index already generated at the repo root.

**Before publishing a new build**, check `debian/control`'s `Maintainer:`
field is accurate for your setup.

## 1. Install the .deb directly (simplest)

```bash
sudo apt install ./pool/main/memmon_1.1-1_amd64.deb
```

Using `apt install ./file.deb` (not `dpkg -i`) is what you want: apt reads
the package's `Depends:` line and pulls in `libncursesw6`/`libtinfo6` from
your normal repos automatically if they're missing, whereas plain `dpkg -i`
will fail on missing dependencies and leave you to fix it with
`apt-get install -f`.

This gets you:
- `/usr/bin/memmon`
- `/etc/memmon/memmon.conf` — interval, log directory, and log retention,
  read by the systemd service (edit + `systemctl restart memmon` to apply;
  preserved across upgrades since it's a conffile)
- `/usr/lib/systemd/system/memmon.service` — **enabled and started
  automatically** on install (via `dh_installsystemd`'s postinst), no manual
  `systemctl enable --now` needed
- `man memmon`
- `/usr/share/doc/memmon/README.md`, `PACKAGING.md`

Remove it the normal way: `sudo apt remove memmon` (or `purge` to also drop
the systemd unit's enabled/disabled state and stop the service).

## 2. Building / re-versioning the package

Requires `debhelper`, `dpkg-dev`, a C compiler, and ncurses dev headers:

```bash
sudo apt-get install build-essential debhelper libncursesw5-dev pkg-config dpkg-dev
```

Bump the version by adding a new entry to `debian/changelog` (or run
`dch -i` if you have `devscripts` installed), then build:

```bash
./scripts/build-deb.sh
```

This runs `dpkg-buildpackage -us -uc -b`, copies the resulting
`.deb` into `pool/main/`, and regenerates `Packages`/`Packages.gz` at the
repo root so the flat repository (see below) is immediately up to date.

Prefer to drive the tools yourself?

```bash
dpkg-buildpackage -us -uc -b     # builds ../memmon_<version>_amd64.deb
```

## 3. Hosting a real apt repository

This is what lets `apt update && apt install memmon` work without anyone
downloading a file by hand. This repo already is a **flat repository**: a
directory of `.deb` files (`pool/`) plus an index (`Packages`,
`Packages.gz`), served over plain HTTP(S) — exactly what `scripts/build-deb.sh`
regenerates on every build.

Serve the checked-out repo directory (or the corresponding branch, e.g. via
GitHub Pages, an S3 bucket, or any static file host) over HTTPS, then on a
machine that should install from it:

```bash
echo "deb [trusted=yes] https://your-host/memmon-apt/ ./" | \
    sudo tee /etc/apt/sources.list.d/memmon.list
sudo apt update
sudo apt install memmon
```

`[trusted=yes]` skips GPG signature checking, which is fine for personal/
internal use but apt will nag about it and, more importantly, an
unsigned repo means anyone who can tamper with that URL or DNS can push
you a malicious package. For anything beyond personal use, sign the
repo instead of using `trusted=yes`:

```bash
gpg --full-generate-key                 # if you don't have a key yet

# Generate Release/InRelease from a clean copy containing only the repo
# files (Packages, Packages.gz, pool/) - NOT the full git checkout, which
# also has debian/, src/, .git/, etc. that don't belong in the index.
mkdir -p /tmp/memmon-repo && cp -r Packages Packages.gz pool /tmp/memmon-repo/
cd /tmp/memmon-repo
apt-ftparchive release . > Release
gpg --default-key YOUR_KEY_ID -abs -o Release.gpg Release   # detached sig
# or, for the modern inline form apt also accepts:
gpg --default-key YOUR_KEY_ID --clearsign -o InRelease Release
```

Once signed, drop `[trusted=yes]` and instead have users import your public
key (`sudo apt-key add your-key.pub` on older apt, or the modern
`signed-by=/usr/share/keyrings/...gpg` form in the sources.list entry).

For a larger/public-facing repo, `reprepro` or `aptly` manage multiple
package versions, distributions (stable/testing), and signing for you
rather than the manual `dpkg-scanpackages` flow above — worth it once
you're publishing updates regularly rather than a one-off package.

## Log retention and configuration

The daemon prunes its own daily log files (`YYYY-MM-DD.log`) once they're
older than a configurable retention window (30 days by default, `0` disables
it). This is controlled by `/etc/memmon/memmon.conf`, which the package
installs as a conffile — it survives `apt upgrade` and any local edits are
preserved (dpkg will prompt if a future package version changes the
shipped default and you've also modified it locally).

## Project layout

| Path | Purpose |
|---|---|
| `src/memmon.c` | Source |
| `man/memmon.1` | Man page source (gzipped into the package automatically) |
| `systemd/memmon.service` | systemd unit (symlinked from `debian/memmon.service` for the packaging build) |
| `config/memmon.conf` | Default config installed to `/etc/memmon/memmon.conf` |
| `Makefile` | Plain `make` / `make install`, for a non-packaged install |
| `debian/` | Debhelper packaging: `control`, `rules`, `changelog`, `copyright`, etc. |
| `scripts/build-deb.sh` | Builds the .deb and publishes it into `pool/main/` + regenerates `Packages`/`Packages.gz` |
| `pool/`, `Packages`, `Packages.gz` | The flat apt repository served from this repo |
