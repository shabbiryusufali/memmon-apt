# Packaging memmon as a .deb

There are two different things people mean by "install via apt", and both
are covered below:

1. **Install a `.deb` file directly with apt** — works today, no server
   needed, apt still resolves and installs dependencies for you.
2. **Set up a real apt repository** — lets you (and any other machine) do
   `apt update && apt install memmon` after adding one line to
   `/etc/apt/sources.list.d/`, and get upgrades the normal way.

A ready-built package (`memmon_1.0-1_amd64.deb`) is included, plus
`build-deb.sh` to rebuild/re-version it yourself. **Before you ship or host
this anywhere, edit the `Maintainer:` line the script writes into
`DEBIAN/control`** — it's currently a placeholder (`CHANGE ME
<you@example.com>`).

## 1. Install the .deb directly (simplest)

```bash
sudo apt install ./memmon_1.0-x_amd64.deb
```

Using `apt install ./file.deb` (not `dpkg -i`) is what you want: apt reads
the package's `Depends:` line and pulls in `libncursesw6`/`libtinfo6` from
your normal repos automatically if they're missing, whereas plain `dpkg -i`
will fail on missing dependencies and leave you to fix it with
`apt-get install -f`.

This gets you:
- `/usr/bin/memmon`
- `/lib/systemd/system/memmon.service` (not enabled automatically — run
  `sudo systemctl enable --now memmon.service` yourself, same as before)
- `man memmon`
- `/usr/share/doc/memmon/README.md`

Remove it the normal way: `sudo apt remove memmon` (or `purge` to also drop
the systemd unit's enabled/disabled state).

## 2. Rebuilding / re-versioning the package

```bash
./build-deb.sh          # builds version 1.0-1 by default
./build-deb.sh 1.1-1     # or pass an explicit version
```

Output lands in `build/memmon_<version>_amd64.deb`. The script recompiles
from `memmon.c` via `make`, so edit the source, bump the version, and rerun
it for each release.

## 3. Hosting a real apt repository

This is what lets `apt update && apt install memmon` work without anyone
downloading a file by hand. The simplest form is a **flat repository**: a
directory of `.deb` files plus an index, served over plain HTTP(S).

```bash
mkdir -p repo/pool/main
cp build/memmon_1.0-1_amd64.deb repo/pool/main/

cd repo
dpkg-scanpackages --multiversion pool/ > Packages
gzip -9 -c Packages > Packages.gz
```

(`dpkg-scanpackages` is in the `dpkg-dev` package: `sudo apt install
dpkg-dev` if you don't have it.)

Upload the whole `repo/` directory (pool/, Packages, Packages.gz) to any web
host, S3 bucket, GitHub Pages, etc. — it's just static files.

On a machine that should install from it:

```bash
echo "deb [trusted=yes] https://your-host/repo/ ./" | \
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
cd repo
gpg --default-key YOUR_KEY_ID -abs -o Release.gpg Release   # detached sig
# or, for the modern inline form apt also accepts:
gpg --default-key YOUR_KEY_ID --clearsign -o InRelease Release
```

That requires a `Release` file (apt-ftparchive can generate one from
`Packages`/`Packages.gz`: `apt-ftparchive release repo/ > repo/Release`,
from the same `dpkg-dev` package). Once signed, drop `[trusted=yes]` and
instead have users import your public key
(`sudo apt-key add your-key.pub` on older apt, or the modern
`signed-by=/usr/share/keyrings/...gpg` form in the sources.list entry).

For a larger/public-facing repo, `reprepro` or `aptly` manage multiple
package versions, distributions (stable/testing), and signing for you
rather than the manual `dpkg-scanpackages` flow above — worth it once
you're publishing updates regularly rather than a one-off package.

## What's included here

| File | Purpose |
|---|---|
| `memmon_1.0-1_amd64.deb` | Pre-built package, ready to `apt install ./...` |
| `build-deb.sh` | Rebuilds the .deb from `memmon.c` at any version |
| `memmon.1` | Man page source (gzipped into the package automatically) |
| `memmon.c`, `Makefile`, `memmon.service`, `README.md` | Same source as before |
