# memmon

A Linux memory usage monitor for the terminal. It reads `/proc/meminfo`
(total/used/free/available, buffers, cache, **Shmem**, **swap**, zswap,
active/inactive, slab/reclaimable, dirty/writeback, commit, vmalloc, huge
pages — every field the kernel exposes) plus:

- **memory pressure** (PSI, `/proc/pressure/memory`) — how much time tasks
  spend stalled waiting for memory, which "used %" alone doesn't show
- **OOM kills**, swap-in/out and major-fault rates (`/proc/vmstat`)
- **zram** devices (size, data, compressed size, ratio)
- the **cgroup v2 memory limit** that applies to it — inside a container
  `/proc/meminfo` shows the host's numbers, this shows the container's
- the **largest processes** by resident memory

and shows it either:

- as a **scrollable, searchable, man-page-styled interactive display**
  (ncurses) with trend sparklines that auto-refreshes on a "watch"-style
  interval,
- as a **one-shot report** in text, **JSON** or **CSV** (`--once`),
- as a **headless logger** (`--daemon`) that appends a snapshot to a daily
  log file (text, JSON Lines or CSV) on the same interval and raises
  **warning/critical alerts** — meant to run under systemd, with the daemon
  **enabled and started automatically** when installed via the `.deb`
  package, or
- as a **daily summary** of those logs (`--report yesterday`).

Previous days' logs are gzip-compressed and logs older than a configurable
retention window (30 days by default) are deleted automatically — see
[Log retention](#log-retention).

## Project layout

```
VERSION                The version. Bump it (and debian/changelog + the man page)
                       in one step with scripts/bump-version.sh
src/memmon.c           Source (single-file C program)
man/memmon.1           Man page
config/memmon.conf     Default config, installed to /etc/memmon/memmon.conf
systemd/               systemd unit + sysusers.d entry for the "memmon" user
completions/           bash and zsh completions
tests/                 Unit tests, CLI tests and fixture /proc + /sys trees
Makefile               `make`, `make check`, `make install` for a non-packaged install
debian/                Debian packaging (dpkg-buildpackage / debhelper)
scripts/build-deb.sh   Builds the .deb and publishes it into pool/ + the repo index
scripts/update-repo-index.sh  Regenerates Packages/Release, signs them if configured
.github/workflows/     CI (amd64 + arm64) and GitHub Pages apt-repo publishing
pool/, Packages, Packages.gz, Release   The flat apt repository served from this repo
```

## Install via apt (recommended)

This repo doubles as a flat apt repository (`pool/`, `Packages`,
`Packages.gz`, `Release` at the root). Once it's hosted somewhere apt can
reach over HTTP(S) — the included GitHub Pages workflow does this, see
[PACKAGING.md](PACKAGING.md) — point apt at it and install/upgrade normally.

If the repository is signed (recommended, see PACKAGING.md):

```bash
curl -fsSL https://<wherever-you-host-this-repo>/memmon-archive-keyring.gpg | \
    sudo tee /usr/share/keyrings/memmon-archive-keyring.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/memmon-archive-keyring.gpg] https://<wherever-you-host-this-repo>/ ./" | \
    sudo tee /etc/apt/sources.list.d/memmon.list
sudo apt update
sudo apt install memmon
```

Unsigned (personal use only):

```bash
echo "deb [trusted=yes] https://<wherever-you-host-this-repo>/ ./" | \
    sudo tee /etc/apt/sources.list.d/memmon.list
sudo apt update
sudo apt install memmon
```

Later releases just need `sudo apt update && sudo apt upgrade memmon`.

Installing the package:
- puts the binary at `/usr/bin/memmon`
- installs `/etc/memmon/memmon.conf` (interval, log directory/format/
  retention, alert thresholds and hook — see [Configuration](#configuration))
- creates the unprivileged `memmon` system user and installs and **enables +
  starts** `memmon.service` automatically, no manual `systemctl enable` step
  needed
- installs the man page, bash/zsh completions and docs

See [PACKAGING.md](PACKAGING.md) for how the repo is built/published, or to
install a single `.deb` directly without adding a repo.

## Build from source

Requires a C compiler, ncurses and zlib development headers.

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libncurses-dev zlib1g-dev pkg-config

make
make check      # unit + CLI tests
```

This produces a single `memmon` binary at the repo root. `make` auto-detects
ncurses via `pkg-config`; if that's unavailable it falls back to linking
`-lncursesw`. `make NO_NCURSES=1` builds without ncurses (plain, once,
daemon and report modes only).

```bash
sudo make install     # installs to /usr/local/bin, plus the man page,
                      # completions, systemd unit, the memmon system user
                      # and /etc/memmon/memmon.conf (kept if it exists)
sudo make uninstall   # stops the service and removes all of the above
                      # (logs and the memmon user are left in place)
```

## Usage

```
memmon                        Interactive TUI, refresh every 5 seconds (default)
memmon -i 5m                  Interactive TUI, refresh every 5 minutes
memmon --once                 One-shot plain-text report, no loop, no ncurses
memmon --once --format json   One-shot JSON (also: --format csv)
memmon --plain                Interactive watch, plain ANSI redraw (no ncurses)
memmon --daemon -i 5m         Headless logger (for systemd), 5 minute interval
memmon --report yesterday     Summary of yesterday's log
```

Interval syntax: a number with an optional suffix — `s` seconds (default if
omitted), `m` minutes, `h` hours, `d` days. `5`, `5s`, `5m`, `1h`, `2d` are all
valid.

Full option list (each setting option can also go in the config file or
environment — the variable name is in brackets):

| Flag | Meaning |
|---|---|
| `-d, --daemon` | Headless mode: no TUI, logs on each interval, raises alerts. SIGTERM/SIGINT stop it cleanly, SIGHUP reloads the config. For systemd. |
| `-o, --once` | Print a single snapshot to stdout and exit (no log file written) |
| `-p, --plain` | Interactive watch without ncurses — plain ANSI clear/redraw. Use this if the default display looks garbled or doesn't redraw in place on your terminal |
| `-R, --report DAY\|FILE` | Summarise a day's log (`YYYY-MM-DD`, `today`, `yesterday`, or a log file path) |
| `-i, --interval INTERVAL` | Refresh/log interval (default `5s`) [`MEMMON_INTERVAL`] |
| `--no-align` | Don't align daemon/plain samples to wall-clock multiples of the interval [`MEMMON_ALIGN=no`] |
| `-f, --format FMT` | `text`, `json` or `csv` output for `--once` (`text`/`json` for `--report`) |
| `-l, --log-dir DIR` | Where daily log files go (default `/var/log/memmon`, falling back to `~/.local/share/memmon` for interactive runs if that's not writable) [`MEMMON_LOG_DIR`] |
| `-F, --log-format FMT` | Log format: `text` (`.log`), `json` (`.jsonl`) or `csv` (`.csv`) [`MEMMON_LOG_FORMAT`] |
| `-r, --log-retention-days N` | Auto-delete log files older than `N` days. `0` disables. Default `30` [`MEMMON_LOG_RETENTION_DAYS`] |
| `--no-compress` | Don't gzip previous days' logs [`MEMMON_LOG_COMPRESS=no`] |
| `-n, --no-log` | Disable logging while in interactive mode |
| `-t, --top N` | Show/log the `N` largest processes by RSS (default `5`, `0` hides) [`MEMMON_TOP_PROCESSES`] |
| `-w, --warn PCT` / `-C, --crit PCT` | Warning/critical memory-used thresholds (default `60`/`85`) [`MEMMON_WARN_PERCENT`, `MEMMON_CRIT_PERCENT`] |
| `-a, --alert-cmd CMD` | Command the daemon runs when the alert level changes [`MEMMON_ALERT_CMD`] |
| `-g, --cgroup PATH` | cgroup v2 group to report on (default: nearest memory-limited ancestor of memmon's own) [`MEMMON_CGROUP`] |
| `-u, --units UNITS` | `auto`, `k`, `m` or `g` [`MEMMON_UNITS`] |
| `-c, --config FILE` | Read this config file instead of the system and user ones |
| `-h, --help` | Usage help |
| `-v, --version` | Version |

### Interactive keys

| Key | Action |
|---|---|
| `q` | Quit |
| `↑`/`↓`, `j`/`k`, mouse wheel | Scroll |
| `PgUp`/`PgDn`, `Space` | Scroll one page |
| `g` / `G` | Jump to top / bottom |
| `r` | Refresh immediately (resets the countdown) |
| `p` | Pause / resume sampling |
| `u` | Cycle units: auto, KiB, MiB, GiB |
| `/`, `n`, `N` | Search, next / previous match |
| `?` or `h` | Key help |

The interactive screen is a scrollable pad, like `man` or `less` — the
content (overview, swap, trend, load average, memory pressure, OOM/swap
activity, cgroup, zram, top processes, active/inactive breakdown, kernel
reclaimable memory, page-cache activity, commit/vmalloc, huge pages, and a
raw dump of every `/proc/meminfo` field) is usually taller than one screen,
so scroll down to see everything. The header/footer bars stay pinned; the
header shows **WARNING**/**CRITICAL** when memory use crosses a threshold.

## Configuration

memmon reads its settings from, in increasing order of precedence:

1. built-in defaults
2. `/etc/memmon/memmon.conf` (installed by the package and `make install`)
3. `~/.config/memmon/memmon.conf` (per user)
4. `MEMMON_*` environment variables
5. command-line options

`-c FILE` replaces both files. The format is simple `KEY=VALUE` lines — see
the commented [config/memmon.conf](config/memmon.conf) for every setting. An
invalid value stops memmon with an error naming the file and line.

The daemon picks up changes with `sudo systemctl reload memmon`.

## Alerts

Memory use is classified as ok, **warning** (`--warn`, default 60%) or
**critical** (`--crit`, default 85%), with 5 points of hysteresis so a value
hovering around a threshold doesn't flap. In daemon mode every level change
is written to the journal and, if configured, `MEMMON_ALERT_CMD` runs with
`MEMMON_ALERT_LEVEL`, `MEMMON_MEM_PCT`, `MEMMON_SWAP_PCT`,
`MEMMON_PSI_SOME_AVG10`, `MEMMON_HOST` etc. in its environment:

```bash
# /etc/memmon/memmon.conf
MEMMON_ALERT_CMD=logger -p user.warning "memmon: memory $MEMMON_ALERT_LEVEL ($MEMMON_MEM_PCT%)"
```

## Logging

Every logged snapshot is appended to `<log-dir>/YYYY-MM-DD.log` as a plain
text block (the same sections as `--once`, minus the raw field dump):

```
======================================================================
2026-09-16 04:39:54 UTC   host=myhost   kernel=6.8.0-generic
======================================================================
MEMORY
  [###-------------------------------------]   7.3%
  Total:            3.91 GiB      Used:             292.49 MiB
  Available:        3.62 GiB      Free:             3.16 GiB
  ...
MEMORY PRESSURE (PSI)
  some:             avg10 0.00%  avg60 0.00%  avg300 0.00%
  ...
```

With `MEMMON_LOG_FORMAT=json` each snapshot is one JSON object per line in
`YYYY-MM-DD.jsonl` (every `/proc/meminfo` field, PSI, vmstat counters,
cgroup, zram and top processes included); with `csv` it's one row per
snapshot in `YYYY-MM-DD.csv` with a header line — easy to load into a
spreadsheet, pandas or Grafana.

The log file name rolls over automatically at local midnight — no restart
needed, whether running interactively or as the systemd daemon. Daemon
samples are aligned to the clock (`:00`, `:05`, `:10` ... for a 5 minute
interval) and don't drift.

Summarise a day:

```
$ memmon --report yesterday
memmon report: 2026-09-21
  source:   /var/log/memmon/2026-09-21.log.gz

  samples:         288  (00:00:00 - 23:55:00)
  memory used:     min  31.2%   avg  44.0%   max  71.9% at 14:35:00
                   min 2.44 GiB   avg 3.44 GiB   max 5.62 GiB
  swap used:       avg   0.4%   max   2.1%
  PSI some avg10:  avg 0.02%   max 3.10% at 14:35:00
  OOM kills:       0
```

### Log retention

Log files are named `YYYY-MM-DD.{log,jsonl,csv}`, one per calendar day.
Whenever the log rolls over to a new day, memmon gzips the previous days'
files and deletes any of its own log files older than the configured
retention window:

- `-r, --log-retention-days N` on the command line (default `30`)
- or `MEMMON_LOG_RETENTION_DAYS` in the config file / environment

Set it to `0` to disable auto-deletion and keep logs forever, and
`MEMMON_LOG_COMPRESS=no` (or `--no-compress`) to keep old logs uncompressed.
Only files matching memmon's own `YYYY-MM-DD.*` log patterns are ever touched.

## Running as a systemd service (auto-start, 5 minute interval, 30 day retention)

Installing the `.deb` package enables and starts `memmon.service`
automatically. It runs as the unprivileged `memmon` user in a tight systemd
sandbox. If you built from source with `make install` instead, do it
yourself once:

```bash
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now memmon.service
```

Check it's running and look at recent snapshots:

```bash
systemctl status memmon.service
journalctl -u memmon            # startup line, alerts
tail -f /var/log/memmon/$(date +%Y-%m-%d).log
```

Stop/disable:

```bash
sudo systemctl disable --now memmon.service
```

To change the interval, log directory, format, retention or alerts, edit
`/etc/memmon/memmon.conf` then apply it:

```bash
sudo systemctl reload memmon.service
```

**Custom log directory:** the service may only write to `/var/log/memmon`.
To log elsewhere, make the directory writable by the `memmon` user and allow
it in a drop-in:

```bash
sudo install -d -o memmon -g memmon /srv/memmon-logs
sudo systemctl edit memmon       # add:  [Service]
                                 #       ReadWritePaths=/srv/memmon-logs
sudo systemctl restart memmon
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

## Development

```bash
make WERROR=1 check            # build with -Werror, run unit + CLI tests
```

The tests run memmon against the fixture `/proc` and `/sys` trees in
`tests/fixtures/root` (via the `MEMMON_SYSROOT` environment variable), so
they're deterministic on any machine. CI (`.github/workflows/ci.yml`) builds,
tests, lints and packages for amd64 and arm64 on every push.

## Notes

- "Used" memory is computed as `MemTotal - MemAvailable`, which matches the
  modern `free -h` / `htop` notion of memory actually unavailable for new
  allocations (i.e. it doesn't double-count reclaimable cache as "used").
- Memory bars are colored green under the warning threshold, yellow up to
  the critical threshold, red above (60% / 85% by default).
- If a kernel doesn't expose a given source (no PSI, no swap, no zram, no
  cgroup limit), that section adapts or is simply omitted.

## License

[The Unlicense](LICENSE) — public domain.
