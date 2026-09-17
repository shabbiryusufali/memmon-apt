# memmon

A Linux memory usage monitor for the terminal. It reads `/proc/meminfo`
(total/used/free/available, buffers, cache, **Shmem**, **swap**, active/inactive,
slab/reclaimable, dirty/writeback, commit, vmalloc, huge pages — every field the
kernel exposes) and shows it either:

- as a **scrollable, man-page-styled interactive display** (ncurses) that
  auto-refreshes on a "watch"-style interval, or
- as a **headless logger** (`--daemon`) that appends a formatted snapshot to a
  daily log file on the same interval — meant to run under systemd, with the
  daemon **enabled and started automatically** when installed via the `.deb`
  package.

Log files older than a configurable retention window (30 days by default) are
deleted automatically — see [Log retention](#log-retention).

## Project layout

```
VERSION               The version. Bump it (and debian/changelog + the man page)
                       in one step with scripts/bump-version.sh
src/memmon.c        Source (single-file C program)
man/memmon.1         Man page
systemd/memmon.service   systemd unit (installed by both `make install` and the .deb)
config/memmon.conf   Default runtime config, installed to /etc/memmon/memmon.conf
Makefile             Plain `make` / `make install` build, for a non-packaged install
debian/               Debian packaging (dpkg-buildpackage / debhelper)
scripts/build-deb.sh  Builds the .deb and publishes it into pool/ + Packages(.gz)
pool/, Packages, Packages.gz   The flat apt repository served from this repo
```

## Install via apt (recommended)

This repo doubles as a flat apt repository (`pool/`, `Packages`, `Packages.gz`
at the root). Once it's hosted somewhere apt can reach over HTTP(S) — see
[PACKAGING.md](PACKAGING.md) for hosting options — point apt at it and
install/upgrade normally:

```bash
echo "deb [trusted=yes] https://<wherever-you-host-this-repo>/ ./" | \
    sudo tee /etc/apt/sources.list.d/memmon.list
sudo apt update
sudo apt install memmon
```

Later releases just need:

```bash
sudo apt update
sudo apt upgrade memmon
```

Installing the package:
- puts the binary at `/usr/bin/memmon`
- installs `/etc/memmon/memmon.conf` (edit this to change the interval, log
  directory, or log retention — see below)
- installs and **enables + starts** `memmon.service` automatically, no manual
  `systemctl enable` step needed
- installs the man page and docs

See [PACKAGING.md](PACKAGING.md) for how the repo is built/published, or to
install a single `.deb` directly without adding a repo.

## Build from source

Requires a C compiler and ncurses development headers.

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libncursesw5-dev

make
```

This produces a single `memmon` binary at the repo root. `make` auto-detects
ncurses via `pkg-config`; if that's unavailable it falls back to linking
`-lncursesw`.

```bash
sudo make install     # installs to /usr/local/bin, plus the man page,
                       # systemd unit, and /etc/memmon/memmon.conf
```

## Usage

```
memmon                    Interactive TUI, refresh every 5 seconds (default)
memmon -i 5m               Interactive TUI, refresh every 5 minutes
memmon -i 1h               Interactive TUI, refresh every hour
memmon --once               One-shot plain-text report, no loop, no ncurses
memmon --plain               Interactive watch, plain ANSI redraw (no ncurses)
memmon --daemon -i 5m       Headless logger (for systemd), 5 minute interval
```

Interval syntax: a number with an optional suffix — `s` seconds (default if
omitted), `m` minutes, `h` hours, `d` days. `5`, `5s`, `5m`, `1h`, `2d` are all
valid.

Full option list:

| Flag | Meaning |
|---|---|
| `-i, --interval INTERVAL` | Refresh/log interval (default `5s`) |
| `-d, --daemon` | Headless mode: no TUI, just logs on each interval, handles SIGTERM/SIGINT cleanly. For systemd. |
| `-o, --once` | Print a single snapshot to stdout and exit (no log file written) |
| `-p, --plain` | Interactive watch without ncurses — plain ANSI clear/redraw. Use this if the default display looks garbled or doesn't redraw in place on your terminal |
| `-l, --log-dir DIR` | Where daily log files go (default `/var/log/memmon`, falling back to `~/.local/share/memmon` if that's not writable) |
| `-r, --log-retention-days N` | Auto-delete log files older than `N` days. `0` disables. Default `30` (also settable via `MEMMON_LOG_RETENTION_DAYS`) |
| `-n, --no-log` | Disable logging while in interactive mode |
| `-h, --help` | Usage help |
| `-v, --version` | Version |

### Interactive keys

| Key | Action |
|---|---|
| `q` | Quit |
| `↑`/`↓` or `j`/`k` | Scroll one line |
| `PgUp`/`PgDn` | Scroll one page |
| `g` / `G` | Jump to top / bottom |
| `r` | Refresh immediately (resets the countdown) |

The interactive screen is a scrollable pad, like `man` or `less` — the
content (overview, swap, load average, active/inactive breakdown, kernel
reclaimable memory, page-cache activity, commit/vmalloc, huge pages, and a
raw dump of every `/proc/meminfo` field) is usually taller than one screen,
so scroll down to see everything. The header/footer bars stay pinned.

## Logging

Every logged snapshot is appended to `<log-dir>/YYYY-MM-DD.log` as a plain
text block:

```
==================================================================
2026-09-16 04:39:54 UTC  host=myhost  kernel=6.8.0-generic
------------------------------------------------------------------
Mem   total=3.91 GiB   used=292.49 MiB (7.3%) avail=3.62 GiB   free=3.16 GiB
      buffers=16.72 MiB  cached=663.34 MiB shared=11.50 MiB
Swap  total=0 KiB      used=0 KiB      (0.0%) free=0 KiB      cached=0 KiB
Load  1m=0.00 5m=0.00 15m=0.00
------------------------------------------------------------------
  Active           120.13 MiB
  Inactive         563.77 MiB
  ...
```

The log file name rolls over automatically at local midnight — no restart
needed, whether running interactively or as the systemd daemon.

### Log retention

Log files are named `YYYY-MM-DD.log`, one per calendar day. Whenever the log
rolls over to a new day, memmon deletes any of its own log files older than
the configured retention window:

- `-r, --log-retention-days N` on the command line (default `30`)
- or the `MEMMON_LOG_RETENTION_DAYS` environment variable, which the systemd
  unit sets from `/etc/memmon/memmon.conf`

Set it to `0` to disable auto-deletion and keep logs forever. Only files
matching memmon's own `YYYY-MM-DD.log` pattern are ever touched.

## Running as a systemd service (auto-start, 5 minute interval, 30 day retention)

Installing the `.deb` package enables and starts `memmon.service`
automatically. If you built from source with `make install` instead, do it
yourself once:

```bash
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now memmon.service
```

Check it's running and look at recent snapshots:

```bash
systemctl status memmon.service
tail -f /var/log/memmon/$(date +%Y-%m-%d).log
```

Stop/disable:

```bash
sudo systemctl disable --now memmon.service
```

To change the interval, log directory, or log retention, edit
`/etc/memmon/memmon.conf` (installed by both the package and `make install`)
then apply it:

```bash
sudo systemctl restart memmon.service
```

## If the display looks garbled

A few terminal front-ends (some Windows/WSL setups in particular, or any
non-fully-interactive tty) don't fully support the cursor-addressing ncurses
relies on for in-place redraws — you might see mis-rendered symbols or each
refresh printing new lines instead of overwriting the old ones. If that
happens, run:

```bash
memmon --plain
```

This drives the same watch loop with plain ANSI clear-screen/redraw codes
instead of full ncurses cursor addressing, which works correctly on a much
wider range of terminals at the cost of a little visual polish (no
scrolling — it always shows the current snapshot in full).

## Notes

- "Used" memory is computed as `MemTotal - MemAvailable`, which matches the
  modern `free -h` / `htop` notion of memory actually unavailable for new
  allocations (i.e. it doesn't double-count reclaimable cache as "used").
- Memory bars are colored green under 60%, yellow 60–85%, red above 85%.
- If a kernel doesn't expose a given `/proc/meminfo` field (e.g. no huge
  pages, no swap), that section adapts (swap shows "no swap configured";
  fields simply don't appear in the "all fields" dump).
