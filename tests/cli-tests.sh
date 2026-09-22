#!/bin/sh
# cli-tests.sh - end-to-end tests that drive the real memmon binary against
# the fixture /proc and /sys tree in tests/fixtures/root.
#
# Usage: tests/cli-tests.sh [path-to-memmon]   (run by `make check`)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
MEMMON="$(cd "$(dirname "${1:-./memmon}")" && pwd)/$(basename "${1:-./memmon}")"
FIX="$HERE/fixtures"
TMP="$HERE/tmp"
rm -rf "$TMP"
mkdir -p "$TMP"

export MEMMON_SYSROOT="$FIX/root"
export TZ=UTC
export LC_ALL=C
export HOME="$TMP/home"
unset XDG_CONFIG_HOME MEMMON_INTERVAL MEMMON_LOG_DIR MEMMON_LOG_RETENTION_DAYS \
      MEMMON_LOG_COMPRESS MEMMON_LOG_FORMAT MEMMON_TOP_PROCESSES MEMMON_WARN_PERCENT \
      MEMMON_CRIT_PERCENT MEMMON_ALERT_CMD MEMMON_CGROUP MEMMON_UNITS MEMMON_ALIGN
CONF="$FIX/empty.conf"

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $*" >&2; }

# expect_rc DESC EXPECTED_RC CMD...   (stdout+stderr captured in $TMP/out)
expect_rc() {
    desc="$1"; want="$2"; shift 2
    "$@" > "$TMP/out" 2>&1
    rc=$?
    if [ "$rc" -eq "$want" ]; then ok; else fail "$desc: exit $rc, expected $want"; sed 's/^/    /' "$TMP/out" >&2; fi
}

# expect_out DESC PATTERN   (grep -E against the last captured output)
expect_out() {
    if grep -Eq -- "$2" "$TMP/out"; then ok; else fail "$1: output lacks /$2/"; sed 's/^/    /' "$TMP/out" >&2; fi
}

expect_no_out() {
    if grep -Eq -- "$2" "$TMP/out"; then fail "$1: output unexpectedly has /$2/"; else ok; fi
}

expect_file() {
    if [ -e "$2" ]; then ok; else fail "$1: missing $2"; ls -la "$(dirname "$2")" >&2; fi
}

expect_no_file() {
    if [ -e "$2" ]; then fail "$1: $2 should not exist"; else ok; fi
}

# wait (up to ~5s) for a file to contain a pattern
wait_for() {
    i=0
    while [ $i -lt 50 ]; do
        if [ -e "$1" ] && grep -Eq -- "$2" "$1"; then return 0; fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

today="$(date +%Y-%m-%d)"
yesterday="$(date -d yesterday +%Y-%m-%d 2>/dev/null || echo 2000-01-02)"

# --- basic CLI ------------------------------------------------------------
expect_rc "--version" 0 "$MEMMON" --version
expect_out "--version" "^memmon [0-9]"
expect_rc "--help" 0 "$MEMMON" --help
expect_out "--help lists --report" "--report"
expect_rc "missing value" 2 "$MEMMON" -i
expect_out "missing value message" "option '-i' requires a value"
expect_rc "missing value (long)" 2 "$MEMMON" --log-dir
expect_out "missing value message (long)" "option '--log-dir' requires a value"
expect_rc "unknown option" 2 "$MEMMON" --bogus
expect_out "unknown option message" "unknown option '--bogus'"
expect_rc "bad interval" 2 "$MEMMON" -c "$CONF" --once -i 5x
expect_rc "flag with value" 2 "$MEMMON" --once=yes
expect_rc "two modes" 2 "$MEMMON" -c "$CONF" --once --daemon
expect_rc "--format without --once" 2 "$MEMMON" -c "$CONF" --format json
expect_rc "warn >= crit" 2 "$MEMMON" -c "$CONF" --once -w 90 -C 80

# --- --once -----------------------------------------------------------------
expect_rc "--once" 0 "$MEMMON" -c "$CONF" --once
for s in "^MEMORY$" "^SWAP$" "^LOAD AVERAGE$" "^MEMORY PRESSURE \(PSI\)$" "^MEMORY EVENTS$" \
         "^CGROUP$" "^ZRAM$" "^TOP PROCESSES" "^HUGE PAGES$" "^ALL /proc/meminfo FIELDS$"; do
    expect_out "--once section $s" "$s"
done
expect_out "--once memory bar" "75\.0%"
expect_out "--once swap bar" "25\.0%"
expect_out "--once PSI" "avg10 12\.50%"
expect_out "--once cgroup path" "path: /app.slice"
expect_out "--once zram algorithm" "zstd"
expect_out "--once top process" "42 big app"
expect_out "HugePages are counts" "HugePages_Total: +16( |$)"
expect_no_out "HugePages not sizes" "HugePages_Total: +16 KiB"
expect_rc "--once no trend" 0 "$MEMMON" -c "$CONF" --once
expect_no_out "--once has no TREND" "^TREND"
expect_rc "--once units" 0 "$MEMMON" -c "$CONF" --once -u m
expect_out "--units m" "Total: +7812\.50 MiB"

expect_rc "--once json" 0 "$MEMMON" -c "$CONF" --once --format json
expect_out "json mem_pct" '"mem_pct":75\.00'
expect_out "json cgroup" '"cgroup":\{"path":"/app.slice"'
expect_out "json top" '"top":\[\{"pid":42,"name":"big app"'
if command -v python3 >/dev/null 2>&1; then
    if python3 -m json.tool < "$TMP/out" > /dev/null; then ok; else fail "json output doesn't parse"; fi
fi

expect_rc "--once csv" 0 "$MEMMON" -c "$CONF" --once -f csv
if [ "$(wc -l < "$TMP/out")" -eq 2 ]; then ok; else fail "csv should be header + 1 row"; fi
hdr_cols="$(head -1 "$TMP/out" | tr ',' '\n' | wc -l)"
row_cols="$(tail -1 "$TMP/out" | tr ',' '\n' | wc -l)"
if [ "$hdr_cols" -eq "$row_cols" ] && [ "$hdr_cols" -eq 30 ]; then ok; else fail "csv columns: header $hdr_cols row $row_cols"; fi

# --- config precedence: file < env < CLI ------------------------------------
printf 'MEMMON_TOP_PROCESSES=1\n' > "$TMP/top.conf"
expect_rc "config file" 0 "$MEMMON" -c "$TMP/top.conf" --once
expect_no_out "config limits top to 1" "  1 systemd"
MEMMON_TOP_PROCESSES=2 expect_rc "env overrides config" 0 "$MEMMON" -c "$TMP/top.conf" --once
expect_out "env shows 2 processes" "  1 systemd"
MEMMON_TOP_PROCESSES=2 expect_rc "cli overrides env" 0 "$MEMMON" -c "$TMP/top.conf" --once -t 0
expect_no_out "cli hides top" "TOP PROCESSES"
printf '# comment\nMEMMON_INTERVAL=5m\nMEMMON_LOG_RETENTION_DAYS=lots\n' > "$TMP/bad.conf"
expect_rc "bad config value" 2 "$MEMMON" -c "$TMP/bad.conf" --once
expect_out "bad config names file:line" "bad.conf:3: MEMMON_LOG_RETENTION_DAYS"
printf 'MEMMON_FUTURE_SETTING=1\n' > "$TMP/unknown.conf"
expect_rc "unknown config key is a warning" 0 "$MEMMON" -c "$TMP/unknown.conf" --once
expect_out "unknown key warning" "ignoring unknown setting 'MEMMON_FUTURE_SETTING'"
MEMMON_LOG_RETENTION_DAYS=-1 expect_rc "bad env value" 2 "$MEMMON" -c "$CONF" --once
expect_out "bad env message" "MEMMON_LOG_RETENTION_DAYS"
expect_rc "missing -c file" 2 "$MEMMON" -c "$TMP/nope.conf" --once
mkdir -p "$HOME/.config/memmon"
printf 'MEMMON_UNITS=k\n' > "$HOME/.config/memmon/memmon.conf"
expect_rc "user config" 0 "$MEMMON" --once
expect_out "user config applied" "Total: +8000000 KiB"
rm -f "$HOME/.config/memmon/memmon.conf"

# --- daemon: logging, retention, compression, alerts -----------------------
LOGS="$TMP/logs"
mkdir -p "$LOGS"
echo old > "$LOGS/2000-01-01.log"
echo old > "$LOGS/2000-01-01.jsonl.gz"
cp "$FIX/logs/2026-09-16.log" "$LOGS/$yesterday.log"
echo keep > "$LOGS/notes.txt"
ALERT_OUT="$TMP/alert.out"

"$MEMMON" -c "$CONF" --daemon -i 1 --no-align -l "$LOGS" -r 30 -w 50 -C 70 \
    --alert-cmd "echo \"\$MEMMON_ALERT_LEVEL \$MEMMON_ALERT_PREVIOUS \$MEMMON_MEM_PCT\" > '$ALERT_OUT'" \
    2> "$TMP/daemon.err" &
pid=$!
if wait_for "$LOGS/$today.log" "host=" && wait_for "$ALERT_OUT" "critical"; then ok; else fail "daemon didn't log/alert"; fi
kill -HUP "$pid"
wait_for "$TMP/daemon.err" "reloading configuration" || true
sleep 1.2
if [ "$(grep -c 'starting daemon mode' "$TMP/daemon.err")" -eq 2 ]; then ok; else fail "SIGHUP didn't re-exec"; cat "$TMP/daemon.err" >&2; fi
if kill -0 "$pid" 2>/dev/null; then ok; else fail "daemon died after SIGHUP"; fi
kill -TERM "$pid"
wait "$pid"
rc=$?
if [ "$rc" -eq 0 ]; then ok; else fail "daemon exit status $rc after SIGTERM"; fi
cp "$TMP/daemon.err" "$TMP/out"
expect_out "daemon startup line" "starting daemon mode, interval=1s"
expect_out "daemon alert transition" "memory ok -> critical: 75\.0% used"
if [ "$(grep -c "memory ok -> critical" "$TMP/out")" -eq 1 ]; then ok; else fail "alert re-fired after reload"; fi
expect_out "daemon clean stop" "stopping \(signal received\)"
cp "$ALERT_OUT" "$TMP/out"
expect_out "alert hook environment" "^critical ok 75\.0$"
cp "$LOGS/$today.log" "$TMP/out"
expect_out "text log section" "^MEMORY PRESSURE \(PSI\)$"
expect_no_out "text log has no raw dump" "ALL /proc/meminfo"
expect_no_file "retention pruned old .log" "$LOGS/2000-01-01.log"
expect_no_file "retention pruned old .jsonl.gz" "$LOGS/2000-01-01.jsonl.gz"
expect_file "yesterday compressed" "$LOGS/$yesterday.log.gz"
expect_no_file "yesterday original removed" "$LOGS/$yesterday.log"
expect_file "unrelated file untouched" "$LOGS/notes.txt"
if gzip -dc "$LOGS/$yesterday.log.gz" | cmp -s - "$FIX/logs/2026-09-16.log"; then ok; else fail "compressed log content differs"; fi

# no compression when disabled
LOGS2="$TMP/logs2"
mkdir -p "$LOGS2"
cp "$FIX/logs/2026-09-16.log" "$LOGS2/$yesterday.log"
"$MEMMON" -c "$CONF" --daemon -i 1 -l "$LOGS2" -r 0 --no-compress -F json 2>/dev/null &
pid=$!
wait_for "$LOGS2/$today.jsonl" "mem_pct" || fail "json daemon log not written"
kill -TERM "$pid"; wait "$pid"
expect_file "--no-compress keeps plain log" "$LOGS2/$yesterday.log"
if command -v python3 >/dev/null 2>&1; then
    if python3 -c 'import json,sys; [json.loads(l) for l in open(sys.argv[1])]' "$LOGS2/$today.jsonl"; then ok; else fail "jsonl log invalid"; fi
fi

# csv log gets exactly one header across restarts
LOGS3="$TMP/logs3"
for _ in 1 2; do
    "$MEMMON" -c "$CONF" --daemon -i 1 -l "$LOGS3" -F csv 2>/dev/null &
    pid=$!
    wait_for "$LOGS3/$today.csv" "^[0-9]{4}-" || fail "csv daemon log not written"
    kill -TERM "$pid"; wait "$pid"
done
if [ "$(grep -c '^timestamp,' "$LOGS3/$today.csv")" -eq 1 ]; then ok; else fail "csv header repeated"; fi

# aligned scheduling: one sample per slot, no bursts at second boundaries
LOGS4="$TMP/logs4"
"$MEMMON" -c "$CONF" --daemon -i 1 -l "$LOGS4" 2>/dev/null &
pid=$!
sleep 3.5
kill -TERM "$pid"; wait "$pid"
n="$(grep -c 'host=' "$LOGS4/$today.log")"
dups="$(grep 'host=' "$LOGS4/$today.log" | cut -c1-19 | uniq -d | wc -l)"
if [ "$n" -ge 3 ] && [ "$n" -le 5 ] && [ "$dups" -eq 0 ]; then ok; else fail "aligned 1s daemon wrote $n samples ($dups duplicate seconds) in 3.5s"; fi

# --- --report ---------------------------------------------------------------
expect_rc "report on legacy text log" 0 "$MEMMON" -c "$CONF" --report "$FIX/logs/2026-09-16.log"
expect_out "report samples" "samples: +3 +\(04:39:54 - 04:49:54\)"
expect_out "report max" "max +25\.0% at 04:44:54"
expect_out "report used range" "min 292\.49 MiB .* max 1\.00 GiB"
expect_rc "report by date (gz, log dir)" 0 "$MEMMON" -c "$CONF" -l "$LOGS" --report yesterday
expect_out "report gz samples" "samples: +3"
expect_rc "report today" 0 "$MEMMON" -c "$CONF" -l "$LOGS" --report today
expect_out "report today PSI" "PSI some avg10: +avg 12\.50%"
expect_out "report today OOM" "OOM kills: +0"
expect_rc "report jsonl" 0 "$MEMMON" -c "$CONF" -l "$LOGS2" --report "$today" -f json
expect_out "report json" '"mem_pct_max":75\.00'
expect_rc "report csv log" 0 "$MEMMON" -c "$CONF" -l "$LOGS3" --report today
expect_out "report csv samples" "samples: +[0-9]+"
expect_rc "report bad date" 2 "$MEMMON" -c "$CONF" --report 2026-99-99
expect_rc "report no logs" 1 "$MEMMON" -c "$CONF" -l "$TMP/empty" --report 1999-01-01

# --- plain watch ------------------------------------------------------------
timeout -s INT 3 "$MEMMON" -c "$CONF" --plain -n -i 1 --no-align > "$TMP/out" 2>&1
expect_out "plain header" "System Memory Monitor"
expect_out "plain trend after 2 samples" "^.*TREND"
expect_out "plain alert tag (75% vs 60/85)" " WARNING "

# --- ncurses TUI (needs a pseudo-terminal; skipped without python3) ---------
if command -v python3 >/dev/null 2>&1 && "$MEMMON" --help >/dev/null && \
   python3 - "$MEMMON" "$CONF" <<'EOF'
import os, pty, sys, time, select
memmon, conf = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.environ["LINES"] = "40"; os.environ["COLUMNS"] = "120"
    os.execv(memmon, [memmon, "-c", conf, "-n", "-i", "1"])
out = b""
def pump(t):
    global out
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: out += os.read(fd, 65536)
            except OSError: return
pump(0.8)
for keys in [b"?", b"x", b"/big app\n", b"n", b"u", b"p", b"p", b"G", b"g", b"r", b"q"]:
    os.write(fd, keys); pump(0.3)
_, status = os.waitpid(pid, 0)
text = out.decode("utf-8", "replace")
ok = os.WEXITSTATUS(status) == 0 and "memmon keys" in text and "MEMORY" in text and "PAUSED" in text
if not ok:
    sys.stderr.write("exit=%d\n%s\n" % (os.WEXITSTATUS(status), text[-2000:]))
sys.exit(0 if ok else 1)
EOF
then ok; else
    if command -v python3 >/dev/null 2>&1; then fail "interactive TUI smoke test"; fi
fi

rm -rf "$TMP"
echo "cli tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
