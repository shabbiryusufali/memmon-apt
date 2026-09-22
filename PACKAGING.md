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
`DEBIAN/control` tree. Pre-built amd64 packages live in `pool/main/`, with
the matching `Packages`/`Packages.gz`/`Release` index already generated at
the repo root. The GitHub Pages workflow additionally builds arm64.

**Before publishing a new build**, check `debian/control`'s `Maintainer:`
field is accurate for your setup.

## 1. Install the .deb directly (simplest)

```bash
sudo apt install ./pool/main/memmon_2.0-1_amd64.deb
```

Using `apt install ./file.deb` (not `dpkg -i`) is what you want: apt reads
the package's `Depends:` line and pulls in `libncursesw6`/`libtinfo6`/
`zlib1g` from your normal repos automatically if they're missing, whereas
plain `dpkg -i` will fail on missing dependencies and leave you to fix it
with `apt-get install -f`.

This gets you:
- `/usr/bin/memmon`
- `/etc/memmon/memmon.conf` — interval, log directory/format/retention,
  alert thresholds and hook, read by memmon itself (edit + `systemctl reload
  memmon` to apply; preserved across upgrades since it's a conffile)
- `/usr/lib/sysusers.d/memmon.conf` — creates the unprivileged `memmon`
  system user the service runs as
- `/usr/lib/systemd/system/memmon.service` — **enabled and started
  automatically** on install (via `dh_installsystemd`'s postinst), no manual
  `systemctl enable --now` needed
- `man memmon`, bash and zsh completions
- `/usr/share/doc/memmon/README.md`, `PACKAGING.md`

Remove it the normal way: `sudo apt remove memmon` (or `purge` to also drop
the systemd unit's enabled/disabled state and stop the service). Log files in
`/var/log/memmon` are left alone.

### Upgrading from 1.x

2.0 runs the service as the `memmon` user instead of root. systemd re-owns
the existing `/var/log/memmon` logs automatically on the first start
(`LogsDirectory=`). The unit no longer passes settings on its command line —
memmon reads `/etc/memmon/memmon.conf` itself, so an existing, edited
conffile keeps working unchanged. Old logs get gzip-compressed on the first
day rollover unless `MEMMON_LOG_COMPRESS=no` is set.

## 2. Building / re-versioning the package

Requires `debhelper`, `dpkg-dev`, a C compiler, and ncurses and zlib dev
headers:

```bash
sudo apt-get install build-essential debhelper libncurses-dev zlib1g-dev pkg-config dpkg-dev
```

### Bumping the version

The version lives in three places that all have to agree: the top-level
`VERSION` file (compiled into `memmon --version`), `debian/changelog`
(drives the package's version), and the `.TH` line in `man/memmon.1`.
Bump all three in one step:

```bash
./scripts/bump-version.sh 2.1 "Describe what changed."
```

This writes `2.1` to `VERSION`, prepends a `debian/changelog` entry for
`2.1-1` (edit it afterwards if the one-line summary isn't enough), and
updates the man page's version/date. Then build:

```bash
./scripts/build-deb.sh
```

This runs `dpkg-buildpackage -us -uc -b` (which also runs the test suite —
set `DEB_BUILD_OPTIONS=nocheck` to skip it), runs `lintian` if installed,
copies the resulting `.deb` into `pool/main/`, and regenerates
`Packages`/`Packages.gz`/`Release` at the repo root (via
`scripts/update-repo-index.sh`) so the flat repository (see below) is
immediately up to date.

Prefer to drive the tools yourself?

```bash
dpkg-buildpackage -us -uc -b     # builds ../memmon_<version>_amd64.deb
./scripts/update-repo-index.sh   # after copying it into pool/main/
```

## 3. Hosting a real apt repository

This is what lets `apt update && apt install memmon` work without anyone
downloading a file by hand. This repo already is a **flat repository**: a
directory of `.deb` files (`pool/`) plus an index (`Packages`,
`Packages.gz`, `Release`), served over plain HTTP(S).

### Option A: GitHub Pages (automated, amd64 + arm64)

`.github/workflows/pages.yml` publishes the repository to GitHub Pages on
every push to `main`: it builds amd64 and arm64 packages of the current
version, adds them to the committed `pool/`, regenerates the index, signs it
if a key is configured, and deploys. One-time setup:

1. Settings → Pages → Source: **GitHub Actions**.
2. (Recommended) create a signing key and store it as the `APT_SIGNING_KEY`
   repository secret:

   ```bash
   gpg --batch --passphrase '' --quick-gen-key "memmon apt repository" rsa4096 sign never
   gpg --armor --export-secret-keys "memmon apt repository"   # paste into the secret
   ```

Then on each machine:

```bash
curl -fsSL https://<owner>.github.io/<repo>/memmon-archive-keyring.gpg | \
    sudo tee /usr/share/keyrings/memmon-archive-keyring.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/memmon-archive-keyring.gpg] https://<owner>.github.io/<repo>/ ./" | \
    sudo tee /etc/apt/sources.list.d/memmon.list
sudo apt update
sudo apt install memmon
```

Without the secret the workflow publishes an unsigned repository (with a
warning), usable with `[trusted=yes]` as below.

### Option B: any static host

Serve the checked-out repo directory (or an S3 bucket, or any static file
host) over HTTPS, then on a machine that should install from it:

```bash
echo "deb [trusted=yes] https://your-host/memmon-apt/ ./" | \
    sudo tee /etc/apt/sources.list.d/memmon.list
sudo apt update
sudo apt install memmon
```

`[trusted=yes]` skips GPG signature checking, which is fine for personal/
internal use but apt will nag about it and, more importantly, an
unsigned repo means anyone who can tamper with that URL or DNS can push
you a malicious package. For anything beyond personal use, sign the repo:

```bash
gpg --full-generate-key                          # if you don't have a key yet
MEMMON_GPG_KEY=<key-id> ./scripts/build-deb.sh   # or ./scripts/update-repo-index.sh
```

That writes `InRelease` and `Release.gpg` next to `Release`, and exports
the public key as `memmon-archive-keyring.gpg`. Publish that file, and use
the `signed-by=` sources line shown in option A instead of `trusted=yes`.
(`Release` only covers the `Packages` indexes, so it's safe to generate in
the full git checkout.)

For a larger/public-facing repo, `reprepro` or `aptly` manage multiple
package versions, distributions (stable/testing), and signing for you —
worth it once you're publishing updates regularly.

## Log retention and configuration

The daemon compresses previous days' log files and prunes them once they're
older than a configurable retention window (30 days by default, `0`
disables it). This, the log format, alert thresholds and everything else is
controlled by `/etc/memmon/memmon.conf`, which the package installs as a
conffile — it survives `apt upgrade` and any local edits are preserved
(dpkg will prompt if a future package version changes the shipped default
and you've also modified it locally).

## Project layout

| Path | Purpose |
|---|---|
| `VERSION` | The version. Bump it (and everything derived from it) via `scripts/bump-version.sh` |
| `src/memmon.c` | Source |
| `man/memmon.1` | Man page source (gzipped into the package automatically) |
| `systemd/memmon.service` | systemd unit (symlinked from `debian/memmon.service` for the packaging build) |
| `systemd/memmon.sysusers` | sysusers.d entry for the `memmon` user (symlinked from `debian/memmon.sysusers`) |
| `config/memmon.conf` | Default config installed to `/etc/memmon/memmon.conf` |
| `completions/` | bash (`memmon.bash`) and zsh (`_memmon`) completions |
| `tests/` | `make check`: unit tests (`test_memmon.c`), CLI tests (`cli-tests.sh`), fixtures |
| `Makefile` | Plain `make` / `make check` / `make install`, for a non-packaged install |
| `debian/` | Debhelper packaging: `control`, `rules`, `changelog`, `copyright`, etc. |
| `scripts/build-deb.sh` | Builds the .deb and publishes it into `pool/main/` + regenerates the index |
| `scripts/update-repo-index.sh` | Regenerates `Packages`/`Packages.gz`/`Release`, signs with `MEMMON_GPG_KEY` |
| `.github/workflows/ci.yml` | Build, test, shellcheck, lintian, package install test — amd64 and arm64 |
| `.github/workflows/pages.yml` | Publishes the apt repository (both architectures) to GitHub Pages |
| `pool/`, `Packages`, `Packages.gz`, `Release` | The flat apt repository served from this repo |
