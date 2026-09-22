/*
 * memmon - Linux memory usage monitor
 *
 * Reads /proc/meminfo, /proc/loadavg, memory pressure (PSI), /proc/vmstat,
 * zram devices, cgroup v2 limits and per-process RSS, and renders a
 * scrollable, man-page-styled report either as an interactive ncurses
 * TUI (with a "watch"-style auto refresh), as a plain ANSI watch, as a
 * one-shot report (text, JSON or CSV), or, in --daemon mode, as periodic
 * snapshots appended to a daily log file. The daemon mode is intended to
 * be run under systemd. Daily log files older than --log-retention-days
 * (default 30, 0 disables) are deleted automatically, and previous days'
 * logs are gzip-compressed, whenever the log rolls over to a new day.
 *
 * All output modes are driven from one Report model (build_report), so
 * the TUI, --plain, --once and the text log always show the same data.
 *
 * Build:   make
 * Run:     ./memmon                     (interactive, 5s refresh)
 *          ./memmon -i 5m                (interactive, 5 minute refresh)
 *          ./memmon --once               (single snapshot to stdout, no ncurses)
 *          ./memmon --once --format json (single snapshot as JSON)
 *          ./memmon --daemon -i 5m       (headless logger, for systemd)
 *          ./memmon --report yesterday   (summarise a day's log)
 *
 * License: The Unlicense (public domain) - see LICENSE.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <ctype.h>
#include <locale.h>
#include <langinfo.h>
#include <dirent.h>
#include <zlib.h>

#ifdef USE_NCURSES
#include <ncurses.h>
#endif

#define PROGNAME        "memmon"
#ifndef VERSION
/* Normally supplied by the Makefile from the top-level VERSION file
 * (-DVERSION="..."); this is only a fallback for ad hoc `cc memmon.c`
 * builds that bypass the Makefile. See VERSION for the single place
 * to bump the app version. */
#define VERSION         "0.0.0-unversioned"
#endif
#ifndef SYSCONFDIR
#define SYSCONFDIR      "/etc"
#endif
#define SYSTEM_CONFIG   SYSCONFDIR "/memmon/memmon.conf"
#define MAX_ENTRIES     128
#define KEY_LEN         48
#define DEFAULT_LOGDIR  "/var/log/memmon"
#define FALLBACK_LOGDIR ".local/share/memmon"   /* under $HOME */
#define DEFAULT_INTERVAL           5
#define DEFAULT_LOG_RETENTION_DAYS 30            /* 0 disables auto-delete */
#define DEFAULT_TOP_PROCESSES      5
#define MAX_TOP         50
#define DEFAULT_WARN_PCT 60.0
#define DEFAULT_CRIT_PCT 85.0
#define ALERT_HYSTERESIS 5.0                     /* percentage points */
#define MAX_INTERVAL    (366L * 86400L)
#define HIST_LEN        60
#define MAX_ZRAM        8
#define MAX_SECTIONS    20
#define PAD_LINES       600
#define PAD_COLS        240

/* ---------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void copy_str(char *dst, size_t len, const char *src)
{
    if (len == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= len) n = len - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static int str_is_digits(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* All /proc and /sys reads go through here. MEMMON_SYSROOT (used by the
 * test suite) prefixes every path so fixtures can stand in for a real
 * kernel. */
static char g_sysroot[256] = "";

static void root_path(char *out, size_t len, const char *path)
{
    size_t a = strlen(g_sysroot), b = strlen(path);
    if (a + b + 1 > len) { out[0] = '\0'; return; } /* too long: open fails with ENOENT */
    memcpy(out, g_sysroot, a);
    memcpy(out + a, path, b + 1);
}

static FILE *root_fopen(const char *path)
{
    char p[PATH_MAX];
    root_path(p, sizeof(p), path);
    return fopen(p, "r");
}

static DIR *root_opendir(const char *path)
{
    char p[PATH_MAX];
    root_path(p, sizeof(p), path);
    return opendir(p);
}

/* Reads the first line of a (sysroot-relative) file, newline stripped. */
static int read_first_line(const char *path, char *buf, size_t len)
{
    FILE *f = root_fopen(path);
    if (!f) return -1;
    if (!fgets(buf, (int)len, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/* Reads an unsigned number from a file. Returns 0 on success, 1 if the
 * file contains the cgroup "max" (unlimited) marker, -1 on error. */
static int read_ull_file(const char *path, unsigned long long *out)
{
    char buf[64];
    if (read_first_line(path, buf, sizeof(buf)) != 0) return -1;
    char *t = trim(buf);
    if (strcmp(t, "max") == 0) return 1;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(t, &end, 10);
    if (end == t || errno) return -1;
    *out = v;
    return 0;
}

/* ---------------------------------------------------------------------
 * /proc/meminfo parsing
 * ------------------------------------------------------------------- */

typedef struct {
    char key[KEY_LEN];
    unsigned long long val;
    int is_kb;          /* line carried a "kB" unit (HugePages_* counts don't) */
} MeminfoEntry;

typedef struct {
    MeminfoEntry entries[MAX_ENTRIES];
    int count;
} Meminfo;

static int meminfo_read(Meminfo *mi)
{
    FILE *f = root_fopen("/proc/meminfo");
    if (!f) return -1;

    mi->count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && mi->count < MAX_ENTRIES) {
        char key[KEY_LEN], unit[8] = "";
        unsigned long long val;
        /* Lines look like: "MemTotal:       16384000 kB" or
         * "HugePages_Total:       0" (no unit) */
        int n = sscanf(line, "%47[^:]: %llu %7s", key, &val, unit);
        if (n >= 2) {
            MeminfoEntry *e = &mi->entries[mi->count++];
            copy_str(e->key, sizeof(e->key), key);
            e->val = val;
            e->is_kb = (n == 3 && strcmp(unit, "kB") == 0);
        }
    }
    fclose(f);
    return mi->count > 0 ? 0 : -1;
}

static const MeminfoEntry *mi_find(const Meminfo *mi, const char *key)
{
    for (int i = 0; i < mi->count; i++) {
        if (strcmp(mi->entries[i].key, key) == 0)
            return &mi->entries[i];
    }
    return NULL;
}

static unsigned long long mi_get(const Meminfo *mi, const char *key)
{
    const MeminfoEntry *e = mi_find(mi, key);
    return e ? e->val : 0;
}

static int mi_has(const Meminfo *mi, const char *key)
{
    return mi_find(mi, key) != NULL;
}

/* read the 1/5/15 min load average, returns 0 on success */
static int loadavg_read(double *l1, double *l5, double *l15)
{
    FILE *f = root_fopen("/proc/loadavg");
    if (!f) return -1;
    int rc = fscanf(f, "%lf %lf %lf", l1, l5, l15);
    fclose(f);
    return (rc == 3) ? 0 : -1;
}

/* ---------------------------------------------------------------------
 * Memory pressure (PSI), /proc/vmstat counters, zram, cgroup v2 and
 * per-process RSS
 * ------------------------------------------------------------------- */

typedef struct {
    int some_valid, full_valid;
    double some10, some60, some300, full10, full60, full300;
    unsigned long long some_total, full_total;   /* microseconds stalled */
} Psi;

static int psi_read(Psi *p)
{
    memset(p, 0, sizeof(*p));
    FILE *f = root_fopen("/proc/pressure/memory");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char kind[8];
        double a10, a60, a300;
        unsigned long long tot;
        if (sscanf(line, "%7s avg10=%lf avg60=%lf avg300=%lf total=%llu",
                   kind, &a10, &a60, &a300, &tot) != 5)
            continue;
        if (strcmp(kind, "some") == 0) {
            p->some_valid = 1;
            p->some10 = a10; p->some60 = a60; p->some300 = a300; p->some_total = tot;
        } else if (strcmp(kind, "full") == 0) {
            p->full_valid = 1;
            p->full10 = a10; p->full60 = a60; p->full300 = a300; p->full_total = tot;
        }
    }
    fclose(f);
    return p->some_valid ? 0 : -1;
}

typedef struct {
    int valid, has_oom;
    unsigned long long oom_kill, pswpin, pswpout, pgmajfault;
} Vmstat;

static int vmstat_read(Vmstat *v)
{
    memset(v, 0, sizeof(*v));
    FILE *f = root_fopen("/proc/vmstat");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        unsigned long long val;
        if (sscanf(line, "%63s %llu", key, &val) != 2) continue;
        if (strcmp(key, "oom_kill") == 0) { v->oom_kill = val; v->has_oom = 1; }
        else if (strcmp(key, "pswpin") == 0) v->pswpin = val;
        else if (strcmp(key, "pswpout") == 0) v->pswpout = val;
        else if (strcmp(key, "pgmajfault") == 0) v->pgmajfault = val;
    }
    fclose(f);
    v->valid = 1;
    return 0;
}

typedef struct {
    char name[32];
    char algo[24];
    unsigned long long disksize_kb, orig_kb, compr_kb, mem_used_kb;
} Zram;

static int zram_cmp(const void *a, const void *b)
{
    return strcmp(((const Zram *)a)->name, ((const Zram *)b)->name);
}

static int zram_read(Zram *out, int max)
{
    DIR *d = root_opendir("/sys/block");
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        if (strncmp(ent->d_name, "zram", 4) != 0) continue;
        char path[PATH_MAX], buf[256];
        unsigned long long disksize = 0;
        snprintf(path, sizeof(path), "/sys/block/%.64s/disksize", ent->d_name);
        if (read_ull_file(path, &disksize) != 0 || disksize == 0) continue;

        Zram *z = &out[n];
        memset(z, 0, sizeof(*z));
        copy_str(z->name, sizeof(z->name), ent->d_name);
        z->disksize_kb = disksize / 1024;

        snprintf(path, sizeof(path), "/sys/block/%.64s/mm_stat", ent->d_name);
        if (read_first_line(path, buf, sizeof(buf)) == 0) {
            unsigned long long orig = 0, compr = 0, used = 0;
            if (sscanf(buf, "%llu %llu %llu", &orig, &compr, &used) == 3) {
                z->orig_kb = orig / 1024;
                z->compr_kb = compr / 1024;
                z->mem_used_kb = used / 1024;
            }
        }
        /* "lzo lzo-rle [lz4] zstd" - the active one is bracketed */
        snprintf(path, sizeof(path), "/sys/block/%.64s/comp_algorithm", ent->d_name);
        if (read_first_line(path, buf, sizeof(buf)) == 0) {
            char *lb = strchr(buf, '['), *rb = lb ? strchr(lb, ']') : NULL;
            if (lb && rb) { *rb = '\0'; copy_str(z->algo, sizeof(z->algo), lb + 1); }
            else copy_str(z->algo, sizeof(z->algo), trim(buf));
        }
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(Zram), zram_cmp);
    return n;
}

typedef struct {
    int pid;
    char name[32];
    unsigned long long rss_kb, swap_kb;
} Proc;

/* Scans /proc/<pid>/status and keeps the n processes with the largest
 * resident set, sorted largest first. Kernel threads (no VmRSS) are
 * skipped. */
static int top_read(Proc *out, int n)
{
    if (n <= 0) return 0;
    if (n > MAX_TOP) n = MAX_TOP;
    DIR *d = root_opendir("/proc");
    if (!d) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!str_is_digits(ent->d_name)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%.32s/status", ent->d_name);
        FILE *f = root_fopen(path);
        if (!f) continue; /* process exited while we were looking */
        Proc p;
        memset(&p, 0, sizeof(p));
        p.pid = atoi(ent->d_name);
        int has_rss = 0;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Name:", 5) == 0) {
                char *v = trim(line + 5);
                copy_str(p.name, sizeof(p.name), v);
            } else if (sscanf(line, "VmRSS: %llu", &p.rss_kb) == 1) {
                has_rss = 1;
            } else {
                sscanf(line, "VmSwap: %llu", &p.swap_kb);
            }
        }
        fclose(f);
        if (!has_rss) continue;

        /* insertion into the sorted top-n list */
        int pos = count;
        while (pos > 0 && out[pos - 1].rss_kb < p.rss_kb) pos--;
        if (pos >= n) continue;
        int last = count < n ? count : n - 1;
        for (int i = last; i > pos; i--) out[i] = out[i - 1];
        out[pos] = p;
        if (count < n) count++;
    }
    closedir(d);
    return count;
}

typedef struct {
    int valid;
    char path[256];                   /* relative to /sys/fs/cgroup */
    unsigned long long current_kb;
    int has_max; unsigned long long max_kb;
    int has_swap; unsigned long long swap_current_kb;
    int has_swap_max; unsigned long long swap_max_kb;
    int has_oom; unsigned long long oom_kill;
} Cgroup;

static void cgroup_dir(const char *rel, char *out, size_t len)
{
    if (strcmp(rel, "/") == 0 || !*rel)
        snprintf(out, len, "/sys/fs/cgroup");
    else
        snprintf(out, len, "/sys/fs/cgroup%s", rel);
}

/* Finds the cgroup v2 memory controller to report on. With an explicit
 * path it's used as-is. Otherwise we start from our own cgroup and walk
 * up to the nearest ancestor that actually has a memory limit - that's
 * the one that constrains us (e.g. inside a container). When nothing is
 * limited the section is simply omitted. */
static int cgroup_read(Cgroup *cg, const char *requested)
{
    memset(cg, 0, sizeof(*cg));
    char rel[256];
    int explicit_path = requested && *requested;

    if (explicit_path) {
        const char *r = requested;
        if (strncmp(r, "/sys/fs/cgroup", 14) == 0) r += 14;
        snprintf(rel, sizeof(rel), "%s%s", *r == '/' ? "" : "/", r);
    } else {
        FILE *f = root_fopen("/proc/self/cgroup");
        if (!f) return -1;
        char line[512];
        int found = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "0::", 3) == 0) {
                copy_str(rel, sizeof(rel), trim(line + 3));
                found = 1;
                break;
            }
        }
        fclose(f);
        if (!found) return -1;
    }

    char dir[PATH_MAX], path[PATH_MAX + 32];
    for (;;) {
        unsigned long long v;
        cgroup_dir(rel, dir, sizeof(dir));
        snprintf(path, sizeof(path), "%s/memory.max", dir);
        int rc = read_ull_file(path, &v);
        if (explicit_path || rc == 0) break;           /* found a limit */
        if (strcmp(rel, "/") == 0 || !*rel) return -1;   /* hit the root */
        char *slash = strrchr(rel, '/');
        if (slash == rel) rel[1] = '\0';
        else if (slash) *slash = '\0';
        else return -1;
    }

    unsigned long long v;
    snprintf(path, sizeof(path), "%s/memory.current", dir);
    if (read_ull_file(path, &v) != 0) return -1;
    cg->current_kb = v / 1024;
    copy_str(cg->path, sizeof(cg->path), rel);

    snprintf(path, sizeof(path), "%s/memory.max", dir);
    if (read_ull_file(path, &v) == 0) { cg->has_max = 1; cg->max_kb = v / 1024; }
    snprintf(path, sizeof(path), "%s/memory.swap.current", dir);
    if (read_ull_file(path, &v) == 0) { cg->has_swap = 1; cg->swap_current_kb = v / 1024; }
    snprintf(path, sizeof(path), "%s/memory.swap.max", dir);
    if (read_ull_file(path, &v) == 0) { cg->has_swap_max = 1; cg->swap_max_kb = v / 1024; }

    snprintf(path, sizeof(path), "%s/memory.events", dir);
    FILE *f = root_fopen(path);
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "oom_kill %llu", &v) == 1) { cg->has_oom = 1; cg->oom_kill = v; }
        }
        fclose(f);
    }
    cg->valid = 1;
    return 0;
}

/* ---------------------------------------------------------------------
 * Interval parsing: accepts plain seconds ("5"), or suffixed
 * "5s", "5m", "1h", "1d".
 * ------------------------------------------------------------------- */

static long parse_interval(const char *s, char *errbuf, size_t errbuf_len)
{
    if (!s || !*s) {
        snprintf(errbuf, errbuf_len, "empty interval");
        return -1;
    }
    char *end;
    double val = strtod(s, &end);
    if (end == s || val < 0) {
        snprintf(errbuf, errbuf_len, "invalid interval '%s'", s);
        return -1;
    }
    long mult = 1;
    if (*end != '\0') {
        switch (tolower((unsigned char)*end)) {
            case 's': mult = 1; break;
            case 'm': mult = 60; break;
            case 'h': mult = 3600; break;
            case 'd': mult = 86400; break;
            default:
                snprintf(errbuf, errbuf_len,
                         "unknown interval suffix '%c' (use s/m/h/d)", *end);
                return -1;
        }
        if (*(end + 1) != '\0') {
            snprintf(errbuf, errbuf_len, "trailing characters after interval");
            return -1;
        }
    }
    double secs = val * (double)mult;
    if (secs > (double)MAX_INTERVAL) {
        snprintf(errbuf, errbuf_len, "interval '%s' is too long (max 366d)", s);
        return -1;
    }
    if (secs < 1) secs = 1;
    return (long)secs;
}

static void format_interval(long secs, char *buf, size_t buflen)
{
    if (secs % 86400 == 0 && secs >= 86400)
        snprintf(buf, buflen, "%ldd", secs / 86400);
    else if (secs % 3600 == 0 && secs >= 3600)
        snprintf(buf, buflen, "%ldh", secs / 3600);
    else if (secs % 60 == 0 && secs >= 60)
        snprintf(buf, buflen, "%ldm", secs / 60);
    else
        snprintf(buf, buflen, "%lds", secs);
}

/* ---------------------------------------------------------------------
 * Options: defaults < config file < MEMMON_* environment < command line
 * ------------------------------------------------------------------- */

typedef enum { FMT_TEXT, FMT_JSON, FMT_CSV } Format;
typedef enum { SRC_DEFAULT, SRC_CONFIG, SRC_ENV, SRC_CLI } Source;
enum { UNITS_AUTO, UNITS_KIB, UNITS_MIB, UNITS_GIB };

typedef struct {
    long interval_secs;
    char logdir[PATH_MAX];
    Source logdir_src;
    int retention_days;
    int compress_logs;
    Format log_format;
    Format out_format;
    int top_n;
    double warn_pct, crit_pct;
    char alert_cmd[1024];
    char cgroup[256];
    int units;
    int align;
} Options;

static void options_default(Options *o)
{
    memset(o, 0, sizeof(*o));
    o->interval_secs = DEFAULT_INTERVAL;
    o->logdir_src = SRC_DEFAULT;
    o->retention_days = DEFAULT_LOG_RETENTION_DAYS;
    o->compress_logs = 1;
    o->log_format = FMT_TEXT;
    o->out_format = FMT_TEXT;
    o->top_n = DEFAULT_TOP_PROCESSES;
    o->warn_pct = DEFAULT_WARN_PCT;
    o->crit_pct = DEFAULT_CRIT_PCT;
    o->units = UNITS_AUTO;
    o->align = 1;
}

/* Every setting that can come from the config file or environment. */
static const char *const SETTING_KEYS[] = {
    "MEMMON_INTERVAL", "MEMMON_LOG_DIR", "MEMMON_LOG_RETENTION_DAYS",
    "MEMMON_LOG_COMPRESS", "MEMMON_LOG_FORMAT", "MEMMON_TOP_PROCESSES",
    "MEMMON_WARN_PERCENT", "MEMMON_CRIT_PERCENT", "MEMMON_ALERT_CMD",
    "MEMMON_CGROUP", "MEMMON_UNITS", "MEMMON_ALIGN", NULL
};

static int parse_bool(const char *v, int *out)
{
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcmp(v, "1"))
        { *out = 1; return 0; }
    if (!strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off") || !strcmp(v, "0"))
        { *out = 0; return 0; }
    return -1;
}

static int parse_format(const char *v, Format *out)
{
    if (!strcasecmp(v, "text")) { *out = FMT_TEXT; return 0; }
    if (!strcasecmp(v, "json")) { *out = FMT_JSON; return 0; }
    if (!strcasecmp(v, "csv"))  { *out = FMT_CSV;  return 0; }
    return -1;
}

static int parse_units(const char *v, int *out)
{
    if (!strcasecmp(v, "auto")) { *out = UNITS_AUTO; return 0; }
    if (!strcasecmp(v, "k") || !strcasecmp(v, "kib") || !strcasecmp(v, "kb")) { *out = UNITS_KIB; return 0; }
    if (!strcasecmp(v, "m") || !strcasecmp(v, "mib") || !strcasecmp(v, "mb")) { *out = UNITS_MIB; return 0; }
    if (!strcasecmp(v, "g") || !strcasecmp(v, "gib") || !strcasecmp(v, "gb")) { *out = UNITS_GIB; return 0; }
    return -1;
}

static int parse_nonneg_int(const char *v, long max, long *out)
{
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 0 || n > max || errno) return -1;
    *out = n;
    return 0;
}

static int parse_percent(const char *v, double *out)
{
    char *end;
    double d = strtod(v, &end);
    if (end == v) return -1;
    if (*end == '%') end++;
    if (*end != '\0' || d <= 0 || d > 100) return -1;
    *out = d;
    return 0;
}

/* Returns 0 on success, -1 for an invalid value (message in err), 1 for
 * an unknown key. */
static int apply_setting(Options *o, const char *key, const char *val, Source src,
                         char *err, size_t errlen)
{
    long n;
    if (!strcmp(key, "MEMMON_INTERVAL")) {
        long v = parse_interval(val, err, errlen);
        if (v < 0) return -1;
        o->interval_secs = v;
    } else if (!strcmp(key, "MEMMON_LOG_DIR")) {
        if (!*val) { snprintf(err, errlen, "empty log directory"); return -1; }
        copy_str(o->logdir, sizeof(o->logdir), val);
        o->logdir_src = src;
    } else if (!strcmp(key, "MEMMON_LOG_RETENTION_DAYS")) {
        if (parse_nonneg_int(val, 100000, &n) != 0) {
            snprintf(err, errlen, "invalid log retention days '%s'", val);
            return -1;
        }
        o->retention_days = (int)n;
    } else if (!strcmp(key, "MEMMON_LOG_COMPRESS")) {
        if (parse_bool(val, &o->compress_logs) != 0) {
            snprintf(err, errlen, "invalid yes/no value '%s'", val);
            return -1;
        }
    } else if (!strcmp(key, "MEMMON_LOG_FORMAT")) {
        if (parse_format(val, &o->log_format) != 0) {
            snprintf(err, errlen, "invalid format '%s' (use text, json or csv)", val);
            return -1;
        }
    } else if (!strcmp(key, "MEMMON_TOP_PROCESSES")) {
        if (parse_nonneg_int(val, MAX_TOP, &n) != 0) {
            snprintf(err, errlen, "invalid process count '%s' (0-%d)", val, MAX_TOP);
            return -1;
        }
        o->top_n = (int)n;
    } else if (!strcmp(key, "MEMMON_WARN_PERCENT")) {
        if (parse_percent(val, &o->warn_pct) != 0) {
            snprintf(err, errlen, "invalid warning percentage '%s'", val);
            return -1;
        }
    } else if (!strcmp(key, "MEMMON_CRIT_PERCENT")) {
        if (parse_percent(val, &o->crit_pct) != 0) {
            snprintf(err, errlen, "invalid critical percentage '%s'", val);
            return -1;
        }
    } else if (!strcmp(key, "MEMMON_ALERT_CMD")) {
        copy_str(o->alert_cmd, sizeof(o->alert_cmd), val);
    } else if (!strcmp(key, "MEMMON_CGROUP")) {
        copy_str(o->cgroup, sizeof(o->cgroup), val);
    } else if (!strcmp(key, "MEMMON_UNITS")) {
        if (parse_units(val, &o->units) != 0) {
            snprintf(err, errlen, "invalid units '%s' (use auto, k, m or g)", val);
            return -1;
        }
    } else if (!strcmp(key, "MEMMON_ALIGN")) {
        if (parse_bool(val, &o->align) != 0) {
            snprintf(err, errlen, "invalid yes/no value '%s'", val);
            return -1;
        }
    } else {
        return 1;
    }
    return 0;
}

/* Parses one "KEY=VALUE" config line in place. Returns 1 if the line
 * carries a setting, 0 for blank/comment lines, -1 if malformed. The
 * syntax matches what systemd's EnvironmentFile= and a POSIX shell accept
 * for simple assignments: optional "export ", optional matching quotes. */
static int config_parse_line(char *line, char **key, char **val)
{
    char *s = trim(line);
    if (!*s || *s == '#') return 0;
    if (strncmp(s, "export ", 7) == 0) s = trim(s + 7);
    char *eq = strchr(s, '=');
    if (!eq) return -1;
    *eq = '\0';
    *key = trim(s);
    char *v = trim(eq + 1);
    size_t len = strlen(v);
    if (len >= 2 && (v[0] == '"' || v[0] == '\'') && v[len - 1] == v[0]) {
        v[len - 1] = '\0';
        v++;
    }
    *val = v;
    return **key ? 1 : -1;
}

static int config_load(Options *o, const char *path, int must_exist)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (!must_exist && errno == ENOENT) return 0;
        fprintf(stderr, "%s: cannot read config file '%s': %s\n", PROGNAME, path, strerror(errno));
        return -1;
    }
    char line[2048];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *key, *val, err[160];
        int r = config_parse_line(line, &key, &val);
        if (r == 0) continue;
        if (r < 0) {
            fprintf(stderr, "%s: %s:%d: expected KEY=VALUE\n", PROGNAME, path, lineno);
            rc = -1;
            break;
        }
        r = apply_setting(o, key, val, SRC_CONFIG, err, sizeof(err));
        if (r < 0) {
            fprintf(stderr, "%s: %s:%d: %s: %s\n", PROGNAME, path, lineno, key, err);
            rc = -1;
            break;
        }
        if (r > 0)
            fprintf(stderr, "%s: %s:%d: warning: ignoring unknown setting '%s'\n",
                    PROGNAME, path, lineno, key);
    }
    fclose(f);
    return rc;
}

static void user_config_path(char *out, size_t len)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, len, "%s/memmon/memmon.conf", xdg);
    else if (home && *home)
        snprintf(out, len, "%s/.config/memmon/memmon.conf", home);
    else
        out[0] = '\0';
}

static int env_load(Options *o)
{
    for (int i = 0; SETTING_KEYS[i]; i++) {
        const char *v = getenv(SETTING_KEYS[i]);
        if (!v || !*v) continue;
        char err[160];
        if (apply_setting(o, SETTING_KEYS[i], v, SRC_ENV, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: $%s: %s\n", PROGNAME, SETTING_KEYS[i], err);
            return -1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * Derived stats
 * ------------------------------------------------------------------- */

typedef struct {
    unsigned long long total, free, available, buffers, cached, shmem;
    unsigned long long used;                 /* total - available */
    unsigned long long swap_total, swap_free, swap_cached, swap_used;
    double mem_pct, swap_pct;
} Derived;

static void derive(const Meminfo *mi, Derived *d)
{
    memset(d, 0, sizeof(*d));
    d->total     = mi_get(mi, "MemTotal");
    d->free      = mi_get(mi, "MemFree");
    d->buffers   = mi_get(mi, "Buffers");
    d->cached    = mi_get(mi, "Cached");
    d->shmem     = mi_get(mi, "Shmem");
    d->swap_total = mi_get(mi, "SwapTotal");
    d->swap_free  = mi_get(mi, "SwapFree");
    d->swap_cached = mi_get(mi, "SwapCached");

    if (mi_has(mi, "MemAvailable")) {
        d->available = mi_get(mi, "MemAvailable");
    } else {
        /* Fallback approximation for very old kernels lacking MemAvailable */
        unsigned long long sreclaim = mi_get(mi, "SReclaimable");
        d->available = d->free + d->buffers + d->cached + sreclaim;
    }
    if (d->available > d->total) d->available = d->total; /* clamp */

    d->used = (d->total >= d->available) ? d->total - d->available : 0;
    d->swap_used = (d->swap_total >= d->swap_free) ? d->swap_total - d->swap_free : 0;

    d->mem_pct = d->total ? (100.0 * (double)d->used / (double)d->total) : 0.0;
    d->swap_pct = d->swap_total ? (100.0 * (double)d->swap_used / (double)d->swap_total) : 0.0;
}

/* ---------------------------------------------------------------------
 * One complete sample of everything we monitor
 * ------------------------------------------------------------------- */

typedef struct {
    time_t sampled_at;
    Meminfo mi;
    Derived d;
    int load_valid;
    double l1, l5, l15;
    Psi psi;
    Vmstat vm;
    int rates_valid;
    double swapin_ps, swapout_ps, majfault_ps;   /* pages per second */
    unsigned long long oom_since_prev;
    Zram zram[MAX_ZRAM];
    int nzram;
    Proc top[MAX_TOP];
    int ntop;
    Cgroup cg;
} Snapshot;

static Vmstat g_prev_vm;
static struct timespec g_prev_vm_ts;
static int g_have_prev_vm = 0;

static int snapshot_collect(Snapshot *s, const Options *o)
{
    memset(s, 0, sizeof(*s));
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); /* same clock as the scheduler */
    s->sampled_at = ts.tv_sec;
    if (meminfo_read(&s->mi) != 0) return -1;
    derive(&s->mi, &s->d);
    s->load_valid = loadavg_read(&s->l1, &s->l5, &s->l15) == 0;
    psi_read(&s->psi);

    if (vmstat_read(&s->vm) == 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (g_have_prev_vm) {
            double dt = (double)(now.tv_sec - g_prev_vm_ts.tv_sec) +
                        (double)(now.tv_nsec - g_prev_vm_ts.tv_nsec) / 1e9;
            if (dt > 0.001 && s->vm.pswpin >= g_prev_vm.pswpin &&
                s->vm.pswpout >= g_prev_vm.pswpout && s->vm.pgmajfault >= g_prev_vm.pgmajfault) {
                s->rates_valid = 1;
                s->swapin_ps = (double)(s->vm.pswpin - g_prev_vm.pswpin) / dt;
                s->swapout_ps = (double)(s->vm.pswpout - g_prev_vm.pswpout) / dt;
                s->majfault_ps = (double)(s->vm.pgmajfault - g_prev_vm.pgmajfault) / dt;
                if (s->vm.oom_kill >= g_prev_vm.oom_kill)
                    s->oom_since_prev = s->vm.oom_kill - g_prev_vm.oom_kill;
            }
        }
        g_prev_vm = s->vm;
        g_prev_vm_ts = now;
        g_have_prev_vm = 1;
    }

    s->nzram = zram_read(s->zram, MAX_ZRAM);
    s->ntop = top_read(s->top, o->top_n);
    cgroup_read(&s->cg, o->cgroup);
    return 0;
}

/* ---------------------------------------------------------------------
 * Human readable byte formatting (input is kB from /proc/meminfo)
 * ------------------------------------------------------------------- */

static int g_units = UNITS_AUTO;

static void human_kb(unsigned long long kb, char *buf, size_t buflen)
{
    const char *units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    double val = (double)kb;
    int u = 0;
    if (g_units == UNITS_AUTO) {
        while (val >= 1024.0 && u < 4) {
            val /= 1024.0;
            u++;
        }
    } else {
        for (; u < g_units - 1; u++) val /= 1024.0;
    }
    if (u == 0)
        snprintf(buf, buflen, "%.0f %s", val, units[u]);
    else
        snprintf(buf, buflen, "%.2f %s", val, units[u]);
}

/* Inverse of human_kb, used when summarising text logs. */
static int parse_human_kb(const char *s, double *kb)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s) return -1;
    while (*end == ' ') end++;
    double mult;
    if (!strncmp(end, "KiB", 3)) mult = 1;
    else if (!strncmp(end, "MiB", 3)) mult = 1024.0;
    else if (!strncmp(end, "GiB", 3)) mult = 1024.0 * 1024;
    else if (!strncmp(end, "TiB", 3)) mult = 1024.0 * 1024 * 1024;
    else if (!strncmp(end, "PiB", 3)) mult = 1024.0 * 1024 * 1024 * 1024;
    else return -1;
    *kb = v * mult;
    return 0;
}

/* ---------------------------------------------------------------------
 * Trend history + sparklines
 * ------------------------------------------------------------------- */

typedef struct {
    double mem[HIST_LEN], swap[HIST_LEN];
    int n, head;      /* head = index of the next write */
} History;

static void hist_push(History *h, double mem, double swap)
{
    h->mem[h->head] = mem;
    h->swap[h->head] = swap;
    h->head = (h->head + 1) % HIST_LEN;
    if (h->n < HIST_LEN) h->n++;
}

static int g_utf8 = 0;

/* Renders the history (oldest first) as a 0-100% sparkline. */
static void sparkline(const History *h, int swap, char *out, size_t len)
{
    static const char *const blocks[] = {"▁", "▂", "▃", "▄",
                                          "▅", "▆", "▇", "█"};
    static const char ascii[] = " .:-=+*#";
    size_t pos = 0;
    out[0] = '\0';
    int start = (h->head - h->n + HIST_LEN) % HIST_LEN;
    for (int i = 0; i < h->n; i++) {
        double v = swap ? h->swap[(start + i) % HIST_LEN] : h->mem[(start + i) % HIST_LEN];
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        int lvl = (int)(v / 100.0 * 7.0 + 0.5);
        const char *glyph;
        char one[2] = { ascii[lvl], '\0' };
        glyph = g_utf8 ? blocks[lvl] : one;
        size_t gl = strlen(glyph);
        if (pos + gl + 1 > len) break;
        memcpy(out + pos, glyph, gl);
        pos += gl;
        out[pos] = '\0';
    }
}

/* ---------------------------------------------------------------------
 * Report model - one list of titled sections shared by every renderer
 * ------------------------------------------------------------------- */

enum { IN_TUI = 1, IN_PLAIN = 2, IN_ONCE = 4, IN_LOG = 8, IN_ALL = 15 };
enum { LAYOUT_TWO_COL, LAYOUT_ONE_COL, LAYOUT_WIDE };

typedef struct {
    char label[KEY_LEN];
    char value[64];
} Row;

typedef struct {
    char title[64];
    unsigned modes;
    int layout;
    int has_bar;
    double bar_pct;
    char note[768];          /* free text lines shown before the rows */
    Row rows[MAX_ENTRIES];
    int nrows;
} Section;

typedef struct {
    Section sec[MAX_SECTIONS];
    int n;
} Report;

static Report g_report;

static Section *sec_new(Report *r, const char *title, unsigned modes, int layout)
{
    if (r->n >= MAX_SECTIONS) r->n = MAX_SECTIONS - 1; /* never happens; stay in bounds */
    Section *s = &r->sec[r->n++];
    memset(s, 0, sizeof(*s));
    copy_str(s->title, sizeof(s->title), title);
    s->modes = modes;
    s->layout = layout;
    return s;
}

static void sec_row(Section *s, const char *label, const char *fmt, ...)
{
    if (s->nrows >= MAX_ENTRIES) return;
    Row *row = &s->rows[s->nrows++];
    copy_str(row->label, sizeof(row->label), label);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(row->value, sizeof(row->value), fmt, ap);
    va_end(ap);
}

static void sec_row_kb(Section *s, const char *label, unsigned long long kb)
{
    char h[32];
    human_kb(kb, h, sizeof(h));
    sec_row(s, label, "%s", h);
}

static void sec_note(Section *s, const char *fmt, ...)
{
    size_t used = strlen(s->note);
    if (used && used + 1 < sizeof(s->note)) s->note[used++] = '\n';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->note + used, sizeof(s->note) - used, fmt, ap);
    va_end(ap);
}

static void sec_fields(Section *s, const Meminfo *mi, const char *const *fields)
{
    for (int i = 0; fields[i]; i++) {
        const MeminfoEntry *e = mi_find(mi, fields[i]);
        if (!e) continue;
        if (e->is_kb) sec_row_kb(s, e->key, e->val);
        else sec_row(s, e->key, "%llu", e->val);
    }
}

static void build_report(Report *r, const Snapshot *snap, const History *h)
{
    const Derived *d = &snap->d;
    const Meminfo *mi = &snap->mi;
    r->n = 0;

    Section *s = sec_new(r, "MEMORY", IN_ALL, LAYOUT_TWO_COL);
    s->has_bar = 1;
    s->bar_pct = d->mem_pct;
    sec_row_kb(s, "Total", d->total);
    sec_row_kb(s, "Used", d->used);
    sec_row_kb(s, "Available", d->available);
    sec_row_kb(s, "Free", d->free);
    sec_row_kb(s, "Buffers", d->buffers);
    sec_row_kb(s, "Cached", d->cached);
    sec_row_kb(s, "Shared", d->shmem);

    s = sec_new(r, "SWAP", IN_ALL, LAYOUT_TWO_COL);
    if (d->swap_total == 0) {
        sec_note(s, "(no swap configured)");
    } else {
        s->has_bar = 1;
        s->bar_pct = d->swap_pct;
        sec_row_kb(s, "Total", d->swap_total);
        sec_row_kb(s, "Used", d->swap_used);
        sec_row_kb(s, "Free", d->swap_free);
        sec_row_kb(s, "Cached", d->swap_cached);
    }
    static const char *const zswap_fields[] = { "Zswap", "Zswapped", NULL };
    sec_fields(s, mi, zswap_fields);

    if (h && h->n > 1) {
        s = sec_new(r, "TREND", IN_TUI | IN_PLAIN, LAYOUT_ONE_COL);
        char line[HIST_LEN * 4 + 1];
        sparkline(h, 0, line, sizeof(line));
        sec_note(s, "Memory %s %5.1f%%", line, d->mem_pct);
        if (d->swap_total) {
            sparkline(h, 1, line, sizeof(line));
            sec_note(s, "Swap   %s %5.1f%%", line, d->swap_pct);
        }
        sec_note(s, "(last %d samples, 0-100%%)", h->n);
    }

    if (snap->load_valid) {
        s = sec_new(r, "LOAD AVERAGE", IN_ALL, LAYOUT_ONE_COL);
        sec_note(s, "1m: %.2f   5m: %.2f   15m: %.2f", snap->l1, snap->l5, snap->l15);
    }

    if (snap->psi.some_valid) {
        const Psi *p = &snap->psi;
        s = sec_new(r, "MEMORY PRESSURE (PSI)", IN_ALL, LAYOUT_ONE_COL);
        sec_row(s, "some", "avg10 %.2f%%  avg60 %.2f%%  avg300 %.2f%%", p->some10, p->some60, p->some300);
        if (p->full_valid)
            sec_row(s, "full", "avg10 %.2f%%  avg60 %.2f%%  avg300 %.2f%%", p->full10, p->full60, p->full300);
    }

    if (snap->vm.valid) {
        s = sec_new(r, "MEMORY EVENTS", IN_ALL, LAYOUT_TWO_COL);
        if (snap->vm.has_oom) {
            if (snap->oom_since_prev)
                sec_row(s, "OOM kills", "%llu (+%llu)", snap->vm.oom_kill, snap->oom_since_prev);
            else
                sec_row(s, "OOM kills", "%llu", snap->vm.oom_kill);
        }
        if (snap->rates_valid) {
            sec_row(s, "Major faults", "%.1f /s", snap->majfault_ps);
            sec_row(s, "Swap-in", "%.1f pg/s", snap->swapin_ps);
            sec_row(s, "Swap-out", "%.1f pg/s", snap->swapout_ps);
        } else {
            /* first sample: no rate yet, show the since-boot counters */
            sec_row(s, "Major faults", "%llu total", snap->vm.pgmajfault);
            sec_row(s, "Swap-in", "%llu pg total", snap->vm.pswpin);
            sec_row(s, "Swap-out", "%llu pg total", snap->vm.pswpout);
        }
    }

    if (snap->cg.valid) {
        const Cgroup *cg = &snap->cg;
        s = sec_new(r, "CGROUP", IN_ALL, LAYOUT_TWO_COL);
        sec_note(s, "path: %s", cg->path);
        sec_row_kb(s, "Usage", cg->current_kb);
        if (cg->has_max) {
            sec_row_kb(s, "Limit", cg->max_kb);
            s->has_bar = 1;
            s->bar_pct = cg->max_kb ? 100.0 * (double)cg->current_kb / (double)cg->max_kb : 0.0;
        } else {
            sec_row(s, "Limit", "unlimited");
        }
        if (cg->has_swap) sec_row_kb(s, "Swap usage", cg->swap_current_kb);
        if (cg->has_swap) {
            if (cg->has_swap_max) sec_row_kb(s, "Swap limit", cg->swap_max_kb);
            else sec_row(s, "Swap limit", "unlimited");
        }
        if (cg->has_oom) sec_row(s, "OOM kills", "%llu", cg->oom_kill);
    }

    if (snap->nzram > 0) {
        s = sec_new(r, "ZRAM", IN_ALL, LAYOUT_TWO_COL);
        for (int i = 0; i < snap->nzram; i++) {
            const Zram *z = &snap->zram[i];
            char label[KEY_LEN];
            snprintf(label, sizeof(label), "%.16s size", z->name);
            sec_row_kb(s, label, z->disksize_kb);
            snprintf(label, sizeof(label), "%.16s algo", z->name);
            sec_row(s, label, "%s", z->algo[0] ? z->algo : "?");
            snprintf(label, sizeof(label), "%.16s data", z->name);
            sec_row_kb(s, label, z->orig_kb);
            snprintf(label, sizeof(label), "%.16s compr", z->name);
            sec_row_kb(s, label, z->compr_kb);
            snprintf(label, sizeof(label), "%.16s mem used", z->name);
            sec_row_kb(s, label, z->mem_used_kb);
            snprintf(label, sizeof(label), "%.16s ratio", z->name);
            if (z->compr_kb) sec_row(s, label, "%.2fx", (double)z->orig_kb / (double)z->compr_kb);
            else sec_row(s, label, "-");
        }
    }

    if (snap->ntop > 0) {
        s = sec_new(r, "TOP PROCESSES (by RSS)", IN_ALL, LAYOUT_ONE_COL);
        for (int i = 0; i < snap->ntop; i++) {
            const Proc *p = &snap->top[i];
            char label[KEY_LEN], rss[32], swp[32];
            snprintf(label, sizeof(label), "%7d %.30s", p->pid, p->name);
            human_kb(p->rss_kb, rss, sizeof(rss));
            human_kb(p->swap_kb, swp, sizeof(swp));
            sec_row(s, label, "RSS %-12s swap %s", rss, swp);
        }
    }

    static const char *const ai_group[] = {
        "Active", "Inactive", "Active(anon)", "Inactive(anon)",
        "Active(file)", "Inactive(file)", "Unevictable", "Mlocked", NULL
    };
    static const char *const cache_group[] = { "Dirty", "Writeback", "AnonPages", "Mapped", NULL };
    static const char *const kernel_group[] = {
        "KReclaimable", "Slab", "SReclaimable", "SUnreclaim", "KernelStack", "PageTables", NULL
    };
    static const char *const commit_group[] = {
        "CommitLimit", "Committed_AS", "VmallocTotal", "VmallocUsed", NULL
    };
    static const char *const huge_group[] = {
        "AnonHugePages", "HugePages_Total", "HugePages_Free", "HugePages_Rsvd",
        "HugePages_Surp", "Hugepagesize", NULL
    };
    struct { const char *title; const char *const *fields; } groups[] = {
        { "ACTIVE / INACTIVE", ai_group },
        { "PAGE CACHE ACTIVITY", cache_group },
        { "KERNEL & RECLAIMABLE", kernel_group },
        { "COMMIT & VIRTUAL ADDRESS SPACE", commit_group },
        { "HUGE PAGES", huge_group },
    };
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        s = sec_new(r, groups[g].title, IN_ALL, LAYOUT_TWO_COL);
        sec_fields(s, mi, groups[g].fields);
        if (s->nrows == 0) r->n--; /* nothing on this kernel - drop it */
    }

    s = sec_new(r, "ALL /proc/meminfo FIELDS", IN_TUI | IN_ONCE, LAYOUT_WIDE);
    for (int i = 0; i < mi->count; i++) {
        const MeminfoEntry *e = &mi->entries[i];
        if (e->is_kb) sec_row_kb(s, e->key, e->val);
        else sec_row(s, e->key, "%llu", e->val);
    }
}

/* ---------------------------------------------------------------------
 * Text rendering (log files, --once, --plain)
 * ------------------------------------------------------------------- */

static double g_warn_pct = DEFAULT_WARN_PCT;
static double g_crit_pct = DEFAULT_CRIT_PCT;

static void print_bar_plain(FILE *out, double pct, int width, int ansi)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)((pct / 100.0) * width + 0.5);
    const char *color = pct < g_warn_pct ? "\x1b[32m" : (pct < g_crit_pct ? "\x1b[33m" : "\x1b[31m");
    fputc('[', out);
    if (ansi) fputs(color, out);
    for (int i = 0; i < width; i++)
        fputc(i < filled ? '#' : '-', out);
    if (ansi) fputs("\x1b[0m", out);
    fprintf(out, "] %5.1f%%", pct);
}

/* Prints "Label1: value1   Label2: value2" (or just the first pair when
 * lbl2 is NULL), column-aligned so daily log files scan as easily as the
 * interactive display. */
static void text_two_col(FILE *f, const char *lbl1, const char *val1,
                         const char *lbl2, const char *val2, int lw, int vw)
{
    char l1[KEY_LEN + 2];
    snprintf(l1, sizeof(l1), "%s:", lbl1);
    if (lbl2) {
        char l2[KEY_LEN + 2];
        snprintf(l2, sizeof(l2), "%s:", lbl2);
        fprintf(f, "  %-*s%-*s%-*s%s\n", lw, l1, vw, val1, lw, l2, val2);
    } else {
        fprintf(f, "  %-*s%s\n", lw, l1, val1);
    }
}

static void render_text(FILE *f, const Report *r, unsigned mode, int ansi)
{
    for (int i = 0; i < r->n; i++) {
        const Section *s = &r->sec[i];
        if (!(s->modes & mode)) continue;
        if (ansi) fprintf(f, "\x1b[1;36m%s\x1b[0m\n", s->title);
        else fprintf(f, "%s\n", s->title);

        if (s->note[0]) {
            const char *p = s->note;
            while (*p) {
                size_t n = strcspn(p, "\n");
                fprintf(f, "  %.*s\n", (int)n, p);
                p += n;
                if (*p == '\n') p++;
            }
        }
        if (s->has_bar) {
            fputs("  ", f);
            print_bar_plain(f, s->bar_pct, 40, ansi);
            fputc('\n', f);
        }
        if (s->layout == LAYOUT_ONE_COL) {
            int lw = 18;
            for (int k = 0; k < s->nrows; k++) {
                int l = (int)strlen(s->rows[k].label) + 2;
                if (l > lw) lw = l;
            }
            for (int k = 0; k < s->nrows; k++)
                text_two_col(f, s->rows[k].label, s->rows[k].value, NULL, NULL, lw, 0);
        } else {
            int lw = s->layout == LAYOUT_WIDE ? 20 : 18;
            int vw = s->layout == LAYOUT_WIDE ? 18 : 14;
            for (int k = 0; k < s->nrows; k += 2) {
                if (k + 1 < s->nrows)
                    text_two_col(f, s->rows[k].label, s->rows[k].value,
                                 s->rows[k + 1].label, s->rows[k + 1].value, lw, vw);
                else
                    text_two_col(f, s->rows[k].label, s->rows[k].value, NULL, NULL, lw, vw);
            }
        }
        fputc('\n', f);
    }
}

static void local_time_str(time_t t, const char *fmt, char *buf, size_t len)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, len, fmt, &tmv);
}

static void write_text_snapshot(FILE *f, const Snapshot *snap, const char *hostname, const char *kernel)
{
    char ts[64];
    local_time_str(snap->sampled_at, "%Y-%m-%d %H:%M:%S %Z", ts, sizeof(ts));
    build_report(&g_report, snap, NULL);
    fprintf(f, "======================================================================\n");
    fprintf(f, "%s   host=%s   kernel=%s\n", ts, hostname, kernel);
    fprintf(f, "======================================================================\n");
    render_text(f, &g_report, IN_LOG, 0);
}

static void print_once_text(const Snapshot *snap, const char *hostname, const char *kernel)
{
    char ts[64];
    local_time_str(snap->sampled_at, "%Y-%m-%d %H:%M:%S", ts, sizeof(ts));
    printf("%s(1)                 Memory Monitor                 %s(1)\n\n", PROGNAME, PROGNAME);
    printf("HOST\n  %s   kernel %s   %s\n\n", hostname, kernel, ts);
    build_report(&g_report, snap, NULL);
    render_text(stdout, &g_report, IN_ONCE, 0);
}

/* ---------------------------------------------------------------------
 * JSON / CSV output
 * ------------------------------------------------------------------- */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void json_dbl(FILE *f, const char *key, int valid, double v)
{
    fprintf(f, ",\"%s\":", key);
    if (valid) fprintf(f, "%.2f", v);
    else fputs("null", f);
}

static void json_ull(FILE *f, const char *key, int valid, unsigned long long v)
{
    fprintf(f, ",\"%s\":", key);
    if (valid) fprintf(f, "%llu", v);
    else fputs("null", f);
}

static void iso_time(time_t t, char *buf, size_t len)
{
    local_time_str(t, "%Y-%m-%dT%H:%M:%S%z", buf, len);
}

/* One JSON object per line (JSON Lines). The flat mem_* / swap_* / psi_*
 * keys are stable and what --report reads back. */
static void write_json(FILE *f, const Snapshot *s, const char *hostname, const char *kernel)
{
    const Derived *d = &s->d;
    char ts[40];
    iso_time(s->sampled_at, ts, sizeof(ts));
    fprintf(f, "{\"timestamp\":\"%s\",\"epoch\":%lld,\"host\":", ts, (long long)s->sampled_at);
    json_str(f, hostname);
    fputs(",\"kernel\":", f);
    json_str(f, kernel);
    fprintf(f, ",\"version\":\"%s\"", VERSION);
    json_ull(f, "mem_total_kb", 1, d->total);
    json_ull(f, "mem_used_kb", 1, d->used);
    json_ull(f, "mem_available_kb", 1, d->available);
    json_ull(f, "mem_free_kb", 1, d->free);
    json_ull(f, "mem_buffers_kb", 1, d->buffers);
    json_ull(f, "mem_cached_kb", 1, d->cached);
    json_ull(f, "mem_shared_kb", 1, d->shmem);
    json_dbl(f, "mem_pct", 1, d->mem_pct);
    json_ull(f, "swap_total_kb", 1, d->swap_total);
    json_ull(f, "swap_used_kb", 1, d->swap_used);
    json_ull(f, "swap_free_kb", 1, d->swap_free);
    json_ull(f, "swap_cached_kb", 1, d->swap_cached);
    json_dbl(f, "swap_pct", 1, d->swap_pct);
    json_dbl(f, "load1", s->load_valid, s->l1);
    json_dbl(f, "load5", s->load_valid, s->l5);
    json_dbl(f, "load15", s->load_valid, s->l15);
    json_dbl(f, "psi_some_avg10", s->psi.some_valid, s->psi.some10);
    json_dbl(f, "psi_some_avg60", s->psi.some_valid, s->psi.some60);
    json_dbl(f, "psi_some_avg300", s->psi.some_valid, s->psi.some300);
    json_ull(f, "psi_some_total_us", s->psi.some_valid, s->psi.some_total);
    json_dbl(f, "psi_full_avg10", s->psi.full_valid, s->psi.full10);
    json_dbl(f, "psi_full_avg60", s->psi.full_valid, s->psi.full60);
    json_dbl(f, "psi_full_avg300", s->psi.full_valid, s->psi.full300);
    json_ull(f, "psi_full_total_us", s->psi.full_valid, s->psi.full_total);
    json_ull(f, "oom_kill", s->vm.has_oom, s->vm.oom_kill);
    json_ull(f, "pswpin", s->vm.valid, s->vm.pswpin);
    json_ull(f, "pswpout", s->vm.valid, s->vm.pswpout);
    json_ull(f, "pgmajfault", s->vm.valid, s->vm.pgmajfault);

    fputs(",\"cgroup\":", f);
    if (s->cg.valid) {
        const Cgroup *cg = &s->cg;
        fputs("{\"path\":", f);
        json_str(f, cg->path);
        json_ull(f, "current_kb", 1, cg->current_kb);
        json_ull(f, "max_kb", cg->has_max, cg->max_kb);
        json_dbl(f, "pct", cg->has_max && cg->max_kb,
                 cg->max_kb ? 100.0 * (double)cg->current_kb / (double)cg->max_kb : 0.0);
        json_ull(f, "swap_current_kb", cg->has_swap, cg->swap_current_kb);
        json_ull(f, "swap_max_kb", cg->has_swap_max, cg->swap_max_kb);
        json_ull(f, "oom_kill", cg->has_oom, cg->oom_kill);
        fputc('}', f);
    } else {
        fputs("null", f);
    }

    fputs(",\"zram\":[", f);
    for (int i = 0; i < s->nzram; i++) {
        const Zram *z = &s->zram[i];
        fputs(i ? ",{\"device\":" : "{\"device\":", f);
        json_str(f, z->name);
        fputs(",\"algorithm\":", f);
        json_str(f, z->algo);
        json_ull(f, "disksize_kb", 1, z->disksize_kb);
        json_ull(f, "orig_data_kb", 1, z->orig_kb);
        json_ull(f, "compr_data_kb", 1, z->compr_kb);
        json_ull(f, "mem_used_kb", 1, z->mem_used_kb);
        fputc('}', f);
    }
    fputs("],\"top\":[", f);
    for (int i = 0; i < s->ntop; i++) {
        const Proc *p = &s->top[i];
        fprintf(f, "%s{\"pid\":%d,\"name\":", i ? "," : "", p->pid);
        json_str(f, p->name);
        json_ull(f, "rss_kb", 1, p->rss_kb);
        json_ull(f, "swap_kb", 1, p->swap_kb);
        fputc('}', f);
    }
    fputs("],\"meminfo\":{", f);
    for (int i = 0; i < s->mi.count; i++) {
        if (i) fputc(',', f);
        json_str(f, s->mi.entries[i].key);
        fprintf(f, ":%llu", s->mi.entries[i].val);
    }
    fputs("}}\n", f);
}

static const char *const CSV_COLUMNS[] = {
    "timestamp", "host", "mem_total_kb", "mem_used_kb", "mem_available_kb",
    "mem_free_kb", "mem_buffers_kb", "mem_cached_kb", "mem_shared_kb", "mem_pct",
    "swap_total_kb", "swap_used_kb", "swap_free_kb", "swap_cached_kb", "swap_pct",
    "load1", "load5", "load15", "psi_some_avg10", "psi_some_avg60", "psi_some_avg300",
    "psi_full_avg10", "psi_full_avg60", "psi_full_avg300", "oom_kill", "pswpin",
    "pswpout", "pgmajfault", "cgroup_current_kb", "cgroup_max_kb", NULL
};

static void write_csv_header(FILE *f)
{
    for (int i = 0; CSV_COLUMNS[i]; i++)
        fprintf(f, "%s%s", i ? "," : "", CSV_COLUMNS[i]);
    fputc('\n', f);
}

static void csv_dbl(FILE *f, int valid, double v)
{
    fputc(',', f);
    if (valid) fprintf(f, "%.2f", v);
}

static void csv_ull(FILE *f, int valid, unsigned long long v)
{
    fputc(',', f);
    if (valid) fprintf(f, "%llu", v);
}

static void write_csv(FILE *f, const Snapshot *s, const char *hostname)
{
    const Derived *d = &s->d;
    char ts[40];
    iso_time(s->sampled_at, ts, sizeof(ts));
    fputs(ts, f);
    fputc(',', f);
    /* hostnames can't contain commas or quotes, but stay safe */
    for (const char *p = hostname; *p; p++)
        fputc((*p == ',' || *p == '"' || *p == '\n') ? '_' : *p, f);
    csv_ull(f, 1, d->total);
    csv_ull(f, 1, d->used);
    csv_ull(f, 1, d->available);
    csv_ull(f, 1, d->free);
    csv_ull(f, 1, d->buffers);
    csv_ull(f, 1, d->cached);
    csv_ull(f, 1, d->shmem);
    csv_dbl(f, 1, d->mem_pct);
    csv_ull(f, 1, d->swap_total);
    csv_ull(f, 1, d->swap_used);
    csv_ull(f, 1, d->swap_free);
    csv_ull(f, 1, d->swap_cached);
    csv_dbl(f, 1, d->swap_pct);
    csv_dbl(f, s->load_valid, s->l1);
    csv_dbl(f, s->load_valid, s->l5);
    csv_dbl(f, s->load_valid, s->l15);
    csv_dbl(f, s->psi.some_valid, s->psi.some10);
    csv_dbl(f, s->psi.some_valid, s->psi.some60);
    csv_dbl(f, s->psi.some_valid, s->psi.some300);
    csv_dbl(f, s->psi.full_valid, s->psi.full10);
    csv_dbl(f, s->psi.full_valid, s->psi.full60);
    csv_dbl(f, s->psi.full_valid, s->psi.full300);
    csv_ull(f, s->vm.has_oom, s->vm.oom_kill);
    csv_ull(f, s->vm.valid, s->vm.pswpin);
    csv_ull(f, s->vm.valid, s->vm.pswpout);
    csv_ull(f, s->vm.valid, s->vm.pgmajfault);
    csv_ull(f, s->cg.valid, s->cg.current_kb);
    csv_ull(f, s->cg.valid && s->cg.has_max, s->cg.max_kb);
    fputc('\n', f);
}

/* ---------------------------------------------------------------------
 * Logging - one file per calendar day, text (.log), JSON Lines (.jsonl)
 * or CSV (.csv)
 * ------------------------------------------------------------------- */

static const char *log_ext(Format f)
{
    return f == FMT_JSON ? "jsonl" : f == FMT_CSV ? "csv" : "log";
}

static void today_str(char *buf, size_t buflen)
{
    local_time_str(time(NULL), "%Y-%m-%d", buf, buflen);
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    if (strlen(path) >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    copy_str(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (stat(tmp, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static void fallback_logdir(char *out, size_t outlen)
{
    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(out, outlen, "%s/%s", home, FALLBACK_LOGDIR);
    else
        snprintf(out, outlen, "/tmp/memmon-%u", (unsigned)getuid());
}

/* A log dir given on the command line (or, for the daemon, in the config
 * file/environment) is used as-is so misconfiguration is visible rather
 * than logs silently landing somewhere else. Otherwise the configured or
 * default dir is preferred, falling back to ~/.local/share/memmon when
 * it isn't writable (e.g. a normal user running the interactive view). */
static void resolve_logdir(const Options *o, int daemon_mode, char *out, size_t outlen)
{
    const char *want = o->logdir[0] ? o->logdir : DEFAULT_LOGDIR;
    int strict = o->logdir_src == SRC_CLI || (daemon_mode && o->logdir_src != SRC_DEFAULT);
    if (strict) {
        copy_str(out, outlen, want);
        mkdir_p(out);
        return;
    }
    if (mkdir_p(want) == 0 && access(want, W_OK) == 0) {
        copy_str(out, outlen, want);
        return;
    }
    fallback_logdir(out, outlen);
    mkdir_p(out);
}

/* When root writes into a log dir owned by someone else (the daemon's
 * "memmon" user), hand new files to that owner so the daemon can keep
 * appending to them. */
static void match_dir_owner(const char *dir, int fd, const char *path)
{
    if (geteuid() != 0) return;
    struct stat st;
    if (stat(dir, &st) != 0 || st.st_uid == 0) return;
    int rc = fd >= 0 ? fchown(fd, st.st_uid, st.st_gid) : chown(path, st.st_uid, st.st_gid);
    (void)rc;
}

static FILE *g_logfile = NULL;
static char g_log_date[16] = "";
static char g_log_path[PATH_MAX + 32] = "";
static char g_logdir[PATH_MAX] = "";
static int g_log_quiet = 0;        /* TUI: don't scribble errors over the screen */
static char g_log_error[160] = "";

static void log_error(const char *fmt, ...)
{
    char msg[sizeof(g_log_error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    int repeated = strcmp(msg, g_log_error) == 0;
    copy_str(g_log_error, sizeof(g_log_error), msg);
    if (g_log_quiet || repeated) return; /* don't spam the journal every interval */
    fprintf(stderr, "%s: %s\n", PROGNAME, msg);
}

/* ---------------------------------------------------------------------
 * Log retention + compression. Only ever touches files matching the
 * exact name pattern memmon itself writes (YYYY-MM-DD.{log,jsonl,csv},
 * optionally .gz), so a log dir shared with other files is safe.
 * ------------------------------------------------------------------- */

static int parse_log_filename(const char *name, struct tm *out, int *is_gz)
{
    int y, mo, d, consumed = 0;
    if (sscanf(name, "%4d-%2d-%2d.%n", &y, &mo, &d, &consumed) != 3 || consumed != 11)
        return -1;
    if (!isdigit((unsigned char)name[0]) || name[4] != '-' || name[7] != '-')
        return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
    const char *ext = name + 11;
    static const char *const exts[] = { "log", "jsonl", "csv", NULL };
    int gz = -1;
    for (int i = 0; exts[i]; i++) {
        size_t n = strlen(exts[i]);
        if (strncmp(ext, exts[i], n) != 0) continue;
        if (ext[n] == '\0') gz = 0;
        else if (strcmp(ext + n, ".gz") == 0) gz = 1;
        if (gz >= 0) break;
    }
    if (gz < 0) return -1;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->tm_year = y - 1900;
        out->tm_mon  = mo - 1;
        out->tm_mday = d;
        out->tm_hour = 12; /* noon, to sidestep any DST edge effects in mktime */
        out->tm_isdst = -1;
    }
    if (is_gz) *is_gz = gz;
    return 0;
}

static void prune_old_logs(const char *logdir, int retention_days)
{
    if (retention_days <= 0) return;

    DIR *dir = opendir(logdir);
    if (!dir) return;

    time_t now = time(NULL);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        struct tm file_tm;
        if (parse_log_filename(ent->d_name, &file_tm, NULL) != 0)
            continue;

        time_t file_time = mktime(&file_tm);
        if (file_time == (time_t)-1) continue;

        double age_days = difftime(now, file_time) / 86400.0;
        if (age_days <= (double)retention_days) continue;

        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", logdir, ent->d_name);
        if (unlink(path) != 0) {
            fprintf(stderr, "%s: warning: could not delete expired log '%s': %s\n",
                    PROGNAME, path, strerror(errno));
        }
    }
    closedir(dir);
}

/* gzip src into src.gz and remove src. If src.gz already exists the data
 * is appended as a new gzip member (still one valid .gz stream). */
static int gzip_file(const char *dir, const char *src)
{
    char dst[PATH_MAX + 8], tmp[PATH_MAX + 16];
    snprintf(dst, sizeof(dst), "%s.gz", src);
    int append = access(dst, F_OK) == 0;
    snprintf(tmp, sizeof(tmp), "%s.gz.tmp", src);
    const char *target = append ? dst : tmp;

    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    struct stat st;
    fstat(fileno(in), &st);
    gzFile out = gzopen(target, append ? "ab9" : "wb9");
    if (!out) { fclose(in); return -1; }

    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (gzwrite(out, buf, (unsigned)n) != (int)n) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (gzclose(out) != Z_OK) ok = 0;
    if (!ok) {
        if (!append) unlink(tmp);
        return -1;
    }
    if (!append && rename(tmp, dst) != 0) { unlink(tmp); return -1; }
    struct timespec times[2] = { st.st_atim, st.st_mtim };
    utimensat(AT_FDCWD, dst, times, 0);
    match_dir_owner(dir, -1, dst);
    return unlink(src);
}

static void compress_old_logs(const char *logdir)
{
    DIR *dir = opendir(logdir);
    if (!dir) return;
    char today[16];
    today_str(today, sizeof(today));
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int gz;
        if (parse_log_filename(ent->d_name, NULL, &gz) != 0 || gz) continue;
        if (strncmp(ent->d_name, today, 10) == 0) continue; /* still being written */
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", logdir, ent->d_name);
        if (gzip_file(logdir, path) != 0)
            fprintf(stderr, "%s: warning: could not compress '%s': %s\n",
                    PROGNAME, path, strerror(errno));
    }
    closedir(dir);
}

static void log_close(void)
{
    if (g_logfile) fclose(g_logfile);
    g_logfile = NULL;
    g_log_date[0] = '\0';
}

static int log_open_for_today(const Options *o)
{
    char date[16];
    today_str(date, sizeof(date));
    if (g_logfile && strcmp(date, g_log_date) == 0)
        return 0; /* already open for today */

    log_close();
    snprintf(g_log_path, sizeof(g_log_path), "%s/%s.%s", g_logdir, date, log_ext(o->log_format));
    g_logfile = fopen(g_log_path, "a");
    if (!g_logfile) {
        int e = errno;
        if (e == EROFS || e == EACCES)
            log_error("cannot open log file '%s': %s (under systemd, a custom MEMMON_LOG_DIR "
                      "must be writable by the memmon user and listed in ReadWritePaths=; "
                      "see `systemctl edit memmon`)", g_log_path, strerror(e));
        else
            log_error("cannot open log file '%s': %s", g_log_path, strerror(e));
        return -1;
    }
    g_log_error[0] = '\0';
    match_dir_owner(g_logdir, fileno(g_logfile), g_log_path);
    struct stat st;
    if (o->log_format == FMT_CSV && fstat(fileno(g_logfile), &st) == 0 && st.st_size == 0)
        write_csv_header(g_logfile);
    copy_str(g_log_date, sizeof(g_log_date), date);
    /* Rolling to a new day's file is a natural, cheap point to compress
     * yesterday's log and sweep out anything past the retention window. */
    if (o->compress_logs) compress_old_logs(g_logdir);
    prune_old_logs(g_logdir, o->retention_days);
    return 0;
}

static void log_write(const Options *o, const Snapshot *s, const char *hostname, const char *kernel)
{
    if (log_open_for_today(o) != 0) return;
    switch (o->log_format) {
        case FMT_JSON: write_json(g_logfile, s, hostname, kernel); break;
        case FMT_CSV:  write_csv(g_logfile, s, hostname); break;
        default:       write_text_snapshot(g_logfile, s, hostname, kernel); break;
    }
    if (fflush(g_logfile) != 0)
        log_error("cannot write log file '%s': %s", g_log_path, strerror(errno));
}

/* ---------------------------------------------------------------------
 * Alerts - warning/critical levels with hysteresis. In daemon mode a
 * level change is written to stderr (the journal) and MEMMON_ALERT_CMD
 * runs with the details in its environment.
 * ------------------------------------------------------------------- */

enum { ALERT_OK, ALERT_WARN, ALERT_CRIT };
static const char *const ALERT_NAMES[] = { "ok", "warning", "critical" };
static int g_alert_level = ALERT_OK;

static int alert_level_for(double pct, double warn, double crit, int prev)
{
    int lvl = pct >= crit ? ALERT_CRIT : pct >= warn ? ALERT_WARN : ALERT_OK;
    if (lvl < prev) {
        /* only step down once clearly below the threshold, so a value
         * hovering around it doesn't flap */
        double thr = prev == ALERT_CRIT ? crit : warn;
        if (pct > thr - ALERT_HYSTERESIS) lvl = prev;
    }
    return lvl;
}

static void setenv_fmt(const char *key, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    setenv(key, buf, 1);
}

static void run_alert_cmd(const Options *o, const Snapshot *s, int lvl, int prev, const char *hostname)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: cannot run alert command: %s\n", PROGNAME, strerror(errno));
        return;
    }
    if (pid > 0) return; /* SIGCHLD is ignored in daemon mode, so no zombie */

    setsid();
    signal(SIGCHLD, SIG_DFL);
    setenv("MEMMON_ALERT_LEVEL", ALERT_NAMES[lvl], 1);
    setenv("MEMMON_ALERT_PREVIOUS", ALERT_NAMES[prev], 1);
    setenv("MEMMON_HOST", hostname, 1);
    setenv_fmt("MEMMON_MEM_PCT", "%.1f", s->d.mem_pct);
    setenv_fmt("MEMMON_MEM_USED_KB", "%llu", s->d.used);
    setenv_fmt("MEMMON_MEM_TOTAL_KB", "%llu", s->d.total);
    setenv_fmt("MEMMON_SWAP_PCT", "%.1f", s->d.swap_pct);
    setenv_fmt("MEMMON_WARN_PERCENT", "%.1f", o->warn_pct);
    setenv_fmt("MEMMON_CRIT_PERCENT", "%.1f", o->crit_pct);
    if (s->psi.some_valid) setenv_fmt("MEMMON_PSI_SOME_AVG10", "%.2f", s->psi.some10);
    int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) { dup2(fd, STDIN_FILENO); if (fd > 2) close(fd); }
    execl("/bin/sh", "sh", "-c", o->alert_cmd, (char *)NULL);
    _exit(127);
}

static void alerts_process(const Options *o, const Snapshot *s, const char *hostname, int notify)
{
    int prev = g_alert_level;
    int lvl = alert_level_for(s->d.mem_pct, o->warn_pct, o->crit_pct, prev);
    if (lvl == prev) return;
    g_alert_level = lvl;
    if (!notify) return;
    fprintf(stderr, "%s: memory %s -> %s: %.1f%% used (warning at %.0f%%, critical at %.0f%%)\n",
            PROGNAME, ALERT_NAMES[prev], ALERT_NAMES[lvl], s->d.mem_pct, o->warn_pct, o->crit_pct);
    if (o->alert_cmd[0]) run_alert_cmd(o, s, lvl, prev, hostname);
}

/* ---------------------------------------------------------------------
 * Scheduling. Sampling is anchored to a schedule rather than "sleep N
 * seconds after the last sample", so it doesn't drift. With alignment on
 * (the default) and an interval that divides a day, samples land on
 * wall-clock multiples of the interval (:00, :05, :10 ... for 5m).
 * ------------------------------------------------------------------- */

static time_t next_aligned(time_t now, long interval)
{
    if (interval <= 0 || 86400 % interval != 0) return now + interval;
    struct tm tmv;
    localtime_r(&now, &tmv);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    time_t midnight = mktime(&tmv);
    if (midnight == (time_t)-1 || midnight > now) return now + interval;
    long since = (long)(now - midnight);
    time_t next = midnight + (since / interval + 1) * interval;
    return next > now ? next : now + interval;
}

static time_t next_due(time_t prev_due, time_t now, long interval, int align)
{
    if (align) return next_aligned(now, interval);
    time_t due = prev_due + interval;
    if (due <= now) due = now + interval; /* fell behind (suspend, clock jump) */
    return due;
}

/* The scheduler must read the same clock sleep_until() waits on: time()
 * uses the coarse clock, which can lag CLOCK_REALTIME by a few ms, and
 * computing the next slot from a stale second makes the loop fire
 * repeatedly right after a boundary. */
static time_t realtime_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

/* Sleeps until wall-clock time `due`, returning early if *stop becomes set. */
static void sleep_until(time_t due, volatile sig_atomic_t *stop, volatile sig_atomic_t *stop2)
{
    for (;;) {
        if ((stop && *stop) || (stop2 && *stop2)) return;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        double left = (double)(due - now.tv_sec) - (double)now.tv_nsec / 1e9;
        if (left <= 0) return;
        if (left > 1.0) left = 1.0;
        struct timespec ts = { (time_t)left, (long)((left - (double)(time_t)left) * 1e9) };
        nanosleep(&ts, NULL);
    }
}

/* ---------------------------------------------------------------------
 * Plain ANSI watch mode - no ncurses at all. Uses only "clear screen /
 * cursor home" (\x1b[H\x1b[2J) and SGR color codes, which even quirky
 * or non-fully-VT100 terminals (some Windows/WSL front-ends, serial
 * consoles, etc.) tend to support when full curses cursor-addressing
 * does not render correctly. This is the recommended fallback if the
 * ncurses TUI (the default mode) looks garbled or doesn't redraw in
 * place on your terminal.
 * ------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;
static void handle_stop(int sig) { (void)sig; g_stop = 1; }
static void handle_reload(int sig) { (void)sig; g_reload = 1; }

static void install_handler(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: interrupt sleeps promptly */
    sigaction(sig, &sa, NULL);
}

static int run_plain_watch(const Options *o, int logging_enabled,
                           const char *hostname, const char *kernel)
{
    install_handler(SIGINT, handle_stop);
    install_handler(SIGTERM, handle_stop);

    static Snapshot snap;
    static History hist;
    char ibuf[32];
    format_interval(o->interval_secs, ibuf, sizeof(ibuf));
    time_t due = realtime_now();

    while (!g_stop) {
        if (snapshot_collect(&snap, o) != 0) {
            fprintf(stderr, "%s: failed to read /proc/meminfo: %s\n", PROGNAME, strerror(errno));
            return 1;
        }
        hist_push(&hist, snap.d.mem_pct, snap.d.swap_pct);
        alerts_process(o, &snap, hostname, 0);

        char ts[64];
        local_time_str(snap.sampled_at, "%Y-%m-%d %H:%M:%S", ts, sizeof(ts));
        fputs("\x1b[H\x1b[2J", stdout); /* cursor home + clear screen */
        printf("\x1b[1;7m memmon \x1b[0m  System Memory Monitor - %s   refresh: %s   %s",
               hostname, ibuf, ts);
        if (g_alert_level == ALERT_CRIT) printf("   \x1b[1;41;37m CRITICAL \x1b[0m");
        else if (g_alert_level == ALERT_WARN) printf("   \x1b[1;43;30m WARNING \x1b[0m");
        printf("\n\n");

        build_report(&g_report, &snap, &hist);
        render_text(stdout, &g_report, IN_PLAIN, 1);

        if (logging_enabled) log_write(o, &snap, hostname, kernel);
        printf("(Ctrl-C to quit -- plain mode, refresh every %s%s%s)\n", ibuf,
               g_log_error[0] ? " -- log error: " : "", g_log_error);
        fflush(stdout);

        due = next_due(due, realtime_now(), o->interval_secs, o->align);
        sleep_until(due, &g_stop, NULL);
    }
    log_close();
    printf("\n");
    return 0;
}

/* ---------------------------------------------------------------------
 * ncurses TUI
 * ------------------------------------------------------------------- */

#ifdef USE_NCURSES

static const char *const UNIT_NAMES[] = { "auto", "KiB", "MiB", "GiB" };

enum { CP_HEADER = 1, CP_TITLE, CP_LABEL, CP_BAR_OK, CP_BAR_WARN, CP_BAR_CRIT, CP_VALUE,
       CP_DIM, CP_ALERT_WARN, CP_ALERT_CRIT };

static void init_colors(void)
{
    start_color();
    use_default_colors();
    init_pair(CP_HEADER,  COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_TITLE,   COLOR_CYAN,  -1);
    init_pair(CP_LABEL,   COLOR_WHITE, -1);
    init_pair(CP_BAR_OK,  COLOR_GREEN, -1);
    init_pair(CP_BAR_WARN,COLOR_YELLOW,-1);
    init_pair(CP_BAR_CRIT,COLOR_RED,   -1);
    init_pair(CP_VALUE,   COLOR_WHITE, -1);
    init_pair(CP_DIM,     COLOR_BLUE,  -1);
    init_pair(CP_ALERT_WARN, COLOR_BLACK, COLOR_YELLOW);
    init_pair(CP_ALERT_CRIT, COLOR_WHITE, COLOR_RED);
}

static int bar_color_for(double pct)
{
    if (pct < g_warn_pct) return CP_BAR_OK;
    if (pct < g_crit_pct) return CP_BAR_WARN;
    return CP_BAR_CRIT;
}

static void draw_bar(WINDOW *w, int y, int x, int width, double pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)((pct / 100.0) * width + 0.5);
    int cp = bar_color_for(pct);
    mvwaddch(w, y, x, '[');
    wattron(w, COLOR_PAIR(cp) | A_BOLD);
    for (int i = 0; i < width; i++)
        mvwaddch(w, y, x + 1 + i, i < filled ? ACS_CKBOARD : ' ');
    wattroff(w, COLOR_PAIR(cp) | A_BOLD);
    mvwaddch(w, y, x + 1 + width, ']');
    wattron(w, COLOR_PAIR(cp) | A_BOLD);
    mvwprintw(w, y, x + width + 3, "%5.1f%%", pct);
    wattroff(w, COLOR_PAIR(cp) | A_BOLD);
}

static void section_title(WINDOW *w, int *y, const char *title)
{
    wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD | A_UNDERLINE);
    mvwprintw(w, *y, 0, "%s", title);
    wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD | A_UNDERLINE);
    (*y)++;
}

static void kv_row(WINDOW *w, int y, int x, const char *label, int lw, const char *value, int vw)
{
    wattron(w, COLOR_PAIR(CP_LABEL));
    mvwprintw(w, y, x, "%-*.*s", lw, lw, label);
    wattroff(w, COLOR_PAIR(CP_LABEL));
    wattron(w, COLOR_PAIR(CP_VALUE) | A_BOLD);
    mvwprintw(w, y, x + lw, "%-*s", vw, value);
    wattroff(w, COLOR_PAIR(CP_VALUE) | A_BOLD);
}

static int tui_build_pad(WINDOW *pad, const Report *r, const Snapshot *snap,
                         const char *hostname, const char *kernel)
{
    werase(pad);
    int y = 1;

    char ts[64];
    local_time_str(snap->sampled_at, "%Y-%m-%d %H:%M:%S", ts, sizeof(ts));
    wattron(pad, A_BOLD);
    mvwprintw(pad, y, 0, "System Memory Report");
    wattroff(pad, A_BOLD);
    wattron(pad, COLOR_PAIR(CP_DIM));
    mvwprintw(pad, y, 24, "host: %s   kernel: %s   sampled: %s", hostname, kernel, ts);
    wattroff(pad, COLOR_PAIR(CP_DIM));
    y += 2;

    for (int i = 0; i < r->n && y < PAD_LINES - 4; i++) {
        const Section *s = &r->sec[i];
        if (!(s->modes & IN_TUI)) continue;
        section_title(pad, &y, s->title);

        const char *p = s->note;
        while (*p) {
            size_t n = strcspn(p, "\n");
            mvwaddnstr(pad, y++, 4, p, (int)n);
            p += n;
            if (*p == '\n') p++;
        }
        if (s->has_bar) {
            draw_bar(pad, y, 4, 40, s->bar_pct);
            y += s->nrows ? 2 : 1;
        }
        for (int k = 0; k < s->nrows && y < PAD_LINES - 2; k++) {
            const Row *row = &s->rows[k];
            switch (s->layout) {
                case LAYOUT_ONE_COL:
                    kv_row(pad, y++, 4, row->label, 24, row->value, 40);
                    break;
                case LAYOUT_WIDE:
                    kv_row(pad, y, 4 + (k % 2) * 40, row->label, 20, row->value, 16);
                    if (k % 2 == 1 || k == s->nrows - 1) y++;
                    break;
                default:
                    kv_row(pad, y, 4 + (k % 2) * 32, row->label, 16, row->value, 14);
                    if (k % 2 == 1 || k == s->nrows - 1) y++;
                    break;
            }
        }
        y++;
    }
    return y; /* total content height used */
}

/* Header/footer are drawn directly onto stdscr (row 0 and row LINES-1)
 * rather than via per-frame subwin()/delwin(). */
static void draw_header(int cols, const char *hostname, long interval_secs)
{
    int cp = has_colors() ? CP_HEADER : 0;
    attr_t extra = has_colors() ? A_BOLD : (A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(cp) | extra);
    move(0, 0);
    for (int i = 0; i < cols; i++) addch(' ');

    char ibuf[32];
    format_interval(interval_secs, ibuf, sizeof(ibuf));
    mvprintw(0, 0, " %s(1)", PROGNAME);
    char mid[128];
    snprintf(mid, sizeof(mid), "System Memory Monitor - %.80s", hostname);
    int midlen = (int)strlen(mid);
    int midpos = (cols - midlen) / 2;
    if (midpos > 0 && midpos + midlen < cols)
        mvprintw(0, midpos, "%s", mid);
    char right[48];
    snprintf(right, sizeof(right), "refresh: %s ", ibuf);
    int rightlen = (int)strlen(right);
    if (cols - rightlen - 1 > 0)
        mvprintw(0, cols - rightlen - 1, "%s", right);
    attroff(COLOR_PAIR(cp) | extra);

    if (g_alert_level != ALERT_OK) {
        const char *tag = g_alert_level == ALERT_CRIT ? " CRITICAL " : " WARNING ";
        int acp = has_colors() ? (g_alert_level == ALERT_CRIT ? CP_ALERT_CRIT : CP_ALERT_WARN) : 0;
        int x = (int)strlen(PROGNAME) + 6;
        if (x + 10 < midpos) {
            attron(COLOR_PAIR(acp) | A_BOLD | (has_colors() ? 0 : A_BLINK));
            mvprintw(0, x, "%s", tag);
            attroff(COLOR_PAIR(acp) | A_BOLD | (has_colors() ? 0 : A_BLINK));
        }
    }
}

static void draw_footer(int rows, int cols, int logging_enabled, int secs_to_refresh,
                        int paused, const char *status)
{
    int cp = has_colors() ? CP_HEADER : 0;
    attr_t extra = has_colors() ? A_BOLD : (A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(cp) | extra);
    char msg[512], when[40];
    if (paused) snprintf(when, sizeof(when), "PAUSED (p to resume)");
    else snprintf(when, sizeof(when), "next refresh in %ds", secs_to_refresh);
    if (status && *status)
        snprintf(msg, sizeof(msg), " %s", status);
    else
        snprintf(msg, sizeof(msg),
                 " q:quit  ?:help  /:search  p:pause  u:units(%s)  log:%s   %s ",
                 UNIT_NAMES[g_units],
                 !logging_enabled ? "off" : g_log_error[0] ? "ERROR" : "on", when);
    mvprintw(rows - 1, 0, "%-*.*s", cols, cols, msg);
    attroff(COLOR_PAIR(cp) | extra);
}

static void draw_help(int rows, int cols)
{
    static const char *const lines[] = {
        "memmon keys",
        "",
        "q            quit",
        "Up/Down j/k  scroll one line (mouse wheel scrolls too)",
        "PgUp/PgDn    scroll one page (Space = page down)",
        "g / G        top / bottom",
        "r            refresh now",
        "p            pause / resume sampling",
        "u            cycle units: auto, KiB, MiB, GiB",
        "/            search (case-insensitive)",
        "n / N        next / previous match",
        "? or h       this help",
        "",
        "press any key to close",
        NULL
    };
    int h = 0, w = 0;
    for (; lines[h]; h++) {
        int l = (int)strlen(lines[h]);
        if (l > w) w = l;
    }
    h += 2;
    w += 4;
    if (h > rows || w > cols) return;
    WINDOW *win = newwin(h, w, (rows - h) / 2, (cols - w) / 2);
    if (!win) return;
    werase(win);
    box(win, 0, 0);
    for (int i = 0; lines[i]; i++) {
        if (i == 0) wattron(win, A_BOLD);
        mvwprintw(win, i + 1, 2, "%s", lines[i]);
        if (i == 0) wattroff(win, A_BOLD);
    }
    wnoutrefresh(win);
    delwin(win);
}

/* Finds the next pad line containing q (case-insensitive), starting at
 * `from` and moving in direction dir (+1/-1), wrapping around. */
static int pad_search(WINDOW *pad, int height, const char *q, int from, int dir)
{
    if (!q[0] || height <= 0) return -1;
    char line[PAD_COLS + 1];
    for (int k = 0; k < height; k++) {
        int y = ((from + dir * k) % height + height) % height;
        if (mvwinnstr(pad, y, 0, line, PAD_COLS) == ERR) continue;
        if (strcasestr(line, q)) return y;
    }
    return -1;
}

static int run_interactive(const Options *o, int logging_enabled, const char *hostname,
                           const char *kernel)
{
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr,
            "%s: stdout is not a terminal; the interactive display needs a real tty.\n"
            "Use --once for a single snapshot or --daemon for headless logging instead.\n",
            PROGNAME);
        return 1;
    }

    static Snapshot snap;
    static History hist;
    if (snapshot_collect(&snap, o) != 0) {
        fprintf(stderr, "%s: failed to read /proc/meminfo: %s\n", PROGNAME, strerror(errno));
        return 1;
    }

    g_log_quiet = 1;
    initscr();
    if (has_colors()) init_colors();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(200); /* ms poll interval so we can update countdown + handle keys */
    mouseinterval(0);
#ifdef BUTTON5_PRESSED
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
#else
    mousemask(BUTTON4_PRESSED, NULL);
#endif
    /* Some terminals (notably several found under Windows/WSL front-ends)
     * misreport or only partially support cursor-addressing capabilities,
     * which makes ncurses' normal incremental-diff redraw either leave
     * stale content on screen or scroll instead of repainting in place.
     * Forcing a full repaint whenever the content or scroll position
     * changes (see clearok(curscr) in the loop - clearok is one-shot, so
     * it has to be re-armed) costs a little more output but is far more
     * likely to render correctly everywhere. */
    clearok(stdscr, TRUE);

    int scroll_y = 0, drawn_scroll_y = -1;
    WINDOW *pad = newpad(PAD_LINES, PAD_COLS);
    if (!pad) { endwin(); fprintf(stderr, "%s: failed to allocate display pad\n", PROGNAME); return 1; }

    hist_push(&hist, snap.d.mem_pct, snap.d.swap_pct);
    alerts_process(o, &snap, hostname, 0);
    if (logging_enabled) log_write(o, &snap, hostname, kernel);

    char query[64] = "";
    int match_line = -1;
    char status[160] = "";
    time_t status_until = 0;
    int paused = 0, show_help = 0, rebuild = 1;

    struct timespec last_refresh, now;
    clock_gettime(CLOCK_MONOTONIC, &last_refresh);
    int content_h = 0;

    int running = 1;
    while (running) {
        int repaint = rebuild;
        if (rebuild) {
            build_report(&g_report, &snap, &hist);
            content_h = tui_build_pad(pad, &g_report, &snap, hostname, kernel);
            if (query[0] && match_line >= 0) {
                match_line = pad_search(pad, content_h, query, match_line, 1);
                if (match_line >= 0) mvwchgat(pad, match_line, 0, -1, A_REVERSE, 0, NULL);
            }
            rebuild = 0;
        }

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int content_rows = rows - 2;
        if (content_rows < 1) content_rows = 1;

        int max_scroll = content_h - content_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_y > max_scroll) scroll_y = max_scroll;
        if (scroll_y < 0) scroll_y = 0;

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - last_refresh.tv_sec) +
                         (double)(now.tv_nsec - last_refresh.tv_nsec) / 1e9;
        int secs_to_refresh = (int)((double)o->interval_secs - elapsed);
        if (secs_to_refresh < 0) secs_to_refresh = 0;
        if (status[0] && time(NULL) > status_until) status[0] = '\0';

        if (repaint || scroll_y != drawn_scroll_y) clearok(curscr, TRUE);
        drawn_scroll_y = scroll_y;
        erase();
        draw_header(cols, hostname, o->interval_secs);
        draw_footer(rows, cols, logging_enabled, secs_to_refresh, paused, status);
        wnoutrefresh(stdscr);
        pnoutrefresh(pad, scroll_y, 0, 1, 0, rows - 2, cols - 1);
        if (show_help) draw_help(rows, cols);
        doupdate();

        int force = 0;
        int ch = getch();
        if (show_help && ch != ERR && ch != KEY_RESIZE && ch != KEY_MOUSE) {
            show_help = 0;
            continue;
        }
        switch (ch) {
            case 'q': case 'Q': running = 0; break;
            case KEY_UP: case 'k': if (scroll_y > 0) scroll_y--; break;
            case KEY_DOWN: case 'j': if (scroll_y < max_scroll) scroll_y++; break;
            case KEY_NPAGE: case ' ': scroll_y += content_rows; break;
            case KEY_PPAGE: scroll_y -= content_rows; break;
            case 'g': case KEY_HOME: scroll_y = 0; break;
            case 'G': case KEY_END: scroll_y = max_scroll; break;
            case KEY_RESIZE: resizeterm(0, 0); clearok(stdscr, TRUE); break;
            case 'r': case 'R': force = 1; break;
            case 'p': case 'P':
                paused = !paused;
                if (!paused) force = 1;
                break;
            case 'u': case 'U':
                g_units = (g_units + 1) % 4;
                rebuild = 1;
                break;
            case '?': case 'h': show_help = 1; break;
            case '/': {
                char buf[sizeof(query)] = "";
                move(rows - 1, 0);
                clrtoeol();
                printw("/");
                echo();
                curs_set(1);
                timeout(-1);
                getnstr(buf, (int)sizeof(buf) - 1);
                noecho();
                curs_set(0);
                timeout(200);
                if (buf[0]) {
                    copy_str(query, sizeof(query), buf);
                    match_line = pad_search(pad, content_h, query, scroll_y, 1);
                    if (match_line < 0) {
                        snprintf(status, sizeof(status), "pattern not found: %s", query);
                        status_until = time(NULL) + 2;
                    }
                    rebuild = 1;
                }
                break;
            }
            case 'n': case 'N':
                if (query[0]) {
                    int dir = ch == 'n' ? 1 : -1;
                    int from = match_line >= 0 ? match_line + dir : scroll_y;
                    match_line = pad_search(pad, content_h, query, from, dir);
                    if (match_line < 0) {
                        snprintf(status, sizeof(status), "pattern not found: %s", query);
                        status_until = time(NULL) + 2;
                    }
                    rebuild = 1;
                }
                break;
            case KEY_MOUSE: {
                MEVENT ev;
                if (getmouse(&ev) == OK) {
                    if (ev.bstate & BUTTON4_PRESSED) scroll_y -= 3;
#ifdef BUTTON5_PRESSED
                    else if (ev.bstate & BUTTON5_PRESSED) scroll_y += 3;
#endif
                }
                break;
            }
            default: break;
        }

        /* keep the current match on screen */
        if (match_line >= 0 && (ch == '/' || ch == 'n' || ch == 'N') &&
            (match_line < scroll_y || match_line >= scroll_y + content_rows))
            scroll_y = match_line - content_rows / 3;

        if (force || (!paused && elapsed >= (double)o->interval_secs)) {
            if (snapshot_collect(&snap, o) == 0) {
                hist_push(&hist, snap.d.mem_pct, snap.d.swap_pct);
                alerts_process(o, &snap, hostname, 0);
                if (logging_enabled) log_write(o, &snap, hostname, kernel);
            }
            rebuild = 1;
            clock_gettime(CLOCK_MONOTONIC, &last_refresh);
        }
    }

    delwin(pad);
    endwin();
    log_close();
    return 0;
}

#endif /* USE_NCURSES */

/* ---------------------------------------------------------------------
 * Daemon mode (no ncurses) - for systemd
 * ------------------------------------------------------------------- */

static int run_daemon(const Options *o, const char *hostname, const char *kernel, char **argv)
{
    install_handler(SIGTERM, handle_stop);
    install_handler(SIGINT, handle_stop);
    install_handler(SIGHUP, handle_reload);
    signal(SIGCHLD, SIG_IGN); /* alert hooks are fire-and-forget */

    char ibuf[32], retention[32];
    format_interval(o->interval_secs, ibuf, sizeof(ibuf));
    if (o->retention_days > 0) snprintf(retention, sizeof(retention), "%dd", o->retention_days);
    else snprintf(retention, sizeof(retention), "disabled");
    fprintf(stderr, "%s %s: starting daemon mode, interval=%s%s, logdir=%s, format=%s, "
            "log-retention=%s, compress=%s, warn=%.0f%%, crit=%.0f%%%s\n",
            PROGNAME, VERSION, ibuf, o->align ? " (aligned)" : "", g_logdir,
            log_ext(o->log_format), retention, o->compress_logs ? "yes" : "no",
            o->warn_pct, o->crit_pct, o->alert_cmd[0] ? ", alert-cmd set" : "");

    const char *carried = getenv("MEMMON_RELOAD_ALERT_LEVEL");
    if (carried) {
        int lvl = atoi(carried);
        if (lvl >= ALERT_OK && lvl <= ALERT_CRIT) g_alert_level = lvl;
        unsetenv("MEMMON_RELOAD_ALERT_LEVEL");
    }

    static Snapshot snap;
    time_t due = realtime_now();
    while (!g_stop && !g_reload) {
        if (snapshot_collect(&snap, o) == 0) {
            alerts_process(o, &snap, hostname, 1);
            log_write(o, &snap, hostname, kernel);
        } else {
            fprintf(stderr, "%s: failed to read /proc/meminfo: %s\n", PROGNAME, strerror(errno));
        }
        due = next_due(due, realtime_now(), o->interval_secs, o->align);
        sleep_until(due, &g_stop, &g_reload);
    }
    log_close();
    if (g_reload && !g_stop) {
        /* SIGHUP (systemctl reload memmon): re-exec so the config file,
         * environment and command line are all re-read from scratch. */
        fprintf(stderr, "%s: SIGHUP received, reloading configuration\n", PROGNAME);
        fflush(stderr);
        /* carry the alert level over so a reload doesn't re-fire the hook */
        setenv_fmt("MEMMON_RELOAD_ALERT_LEVEL", "%d", g_alert_level);
        execv("/proc/self/exe", argv);
        fprintf(stderr, "%s: reload failed: %s\n", PROGNAME, strerror(errno));
        return 1;
    }
    fprintf(stderr, "%s: stopping (signal received)\n", PROGNAME);
    return 0;
}

/* ---------------------------------------------------------------------
 * --report: summarise a day's log (text, JSON Lines or CSV, optionally
 * gzip-compressed)
 * ------------------------------------------------------------------- */

typedef struct {
    int valid;
    char ts[24];                           /* "YYYY-MM-DD HH:MM:SS" */
    double mem_pct;
    int has_used; double used_kb;
    int has_swap; double swap_pct;
    int has_psi; double psi;
    int has_oom; unsigned long long oom;
} Sample;

typedef struct {
    long n;
    char first_ts[24], last_ts[24];
    double mem_min, mem_max, mem_sum; char mem_max_ts[24];
    long used_n; double used_min, used_max, used_sum;
    long swap_n; double swap_max, swap_sum;
    long psi_n; double psi_max, psi_sum; char psi_max_ts[24];
    int oom_seen; unsigned long long oom_prev, oom_kills;
} ReportStats;

static void stats_add(ReportStats *st, const Sample *s)
{
    if (!s->valid) return;
    if (st->n == 0) {
        copy_str(st->first_ts, sizeof(st->first_ts), s->ts);
        st->mem_min = st->mem_max = s->mem_pct;
        copy_str(st->mem_max_ts, sizeof(st->mem_max_ts), s->ts);
    }
    st->n++;
    copy_str(st->last_ts, sizeof(st->last_ts), s->ts);
    if (s->mem_pct < st->mem_min) st->mem_min = s->mem_pct;
    if (s->mem_pct > st->mem_max) {
        st->mem_max = s->mem_pct;
        copy_str(st->mem_max_ts, sizeof(st->mem_max_ts), s->ts);
    }
    st->mem_sum += s->mem_pct;
    if (s->has_used) {
        if (st->used_n == 0 || s->used_kb < st->used_min) st->used_min = s->used_kb;
        if (st->used_n == 0 || s->used_kb > st->used_max) st->used_max = s->used_kb;
        st->used_sum += s->used_kb;
        st->used_n++;
    }
    if (s->has_swap) {
        if (st->swap_n == 0 || s->swap_pct > st->swap_max) st->swap_max = s->swap_pct;
        st->swap_sum += s->swap_pct;
        st->swap_n++;
    }
    if (s->has_psi) {
        if (st->psi_n == 0 || s->psi > st->psi_max) {
            st->psi_max = s->psi;
            copy_str(st->psi_max_ts, sizeof(st->psi_max_ts), s->ts);
        }
        st->psi_sum += s->psi;
        st->psi_n++;
    }
    if (s->has_oom) {
        /* the counter resets on reboot, so sum the increases */
        if (st->oom_seen)
            st->oom_kills += s->oom >= st->oom_prev ? s->oom - st->oom_prev : s->oom;
        st->oom_prev = s->oom;
        st->oom_seen = 1;
    }
}

static void ts_from_iso(const char *iso, char *out, size_t len)
{
    copy_str(out, len, iso);
    if (strlen(out) > 19) out[19] = '\0';
    if (out[0] && strlen(out) > 10 && out[10] == 'T') out[10] = ' ';
}

static int json_find_num(const char *line, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    char *end;
    double v = strtod(p, &end);
    if (end == p) return -1; /* null */
    *out = v;
    return 0;
}

static void parse_json_line(const char *line, Sample *s)
{
    memset(s, 0, sizeof(*s));
    const char *p = strstr(line, "\"timestamp\":\"");
    double v;
    if (!p || json_find_num(line, "mem_pct", &s->mem_pct) != 0) return;
    char iso[40];
    copy_str(iso, sizeof(iso), p + 13);
    iso[strcspn(iso, "\"")] = '\0';
    ts_from_iso(iso, s->ts, sizeof(s->ts));
    if (json_find_num(line, "mem_used_kb", &v) == 0) { s->has_used = 1; s->used_kb = v; }
    double swap_total = 0;
    if (json_find_num(line, "swap_total_kb", &swap_total) == 0 && swap_total > 0 &&
        json_find_num(line, "swap_pct", &v) == 0) { s->has_swap = 1; s->swap_pct = v; }
    if (json_find_num(line, "psi_some_avg10", &v) == 0) { s->has_psi = 1; s->psi = v; }
    if (json_find_num(line, "oom_kill", &v) == 0) { s->has_oom = 1; s->oom = (unsigned long long)v; }
    s->valid = 1;
}

/* Splits a CSV line in place (no quoting - memmon never writes any). */
static int csv_split(char *line, char **fields, int max)
{
    int n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    return n;
}

enum { CI_TS, CI_MEMPCT, CI_USED, CI_SWAPTOTAL, CI_SWAPPCT, CI_PSI, CI_OOM, CI_COUNT };
static const char *const CSV_WANTED[CI_COUNT] = {
    "timestamp", "mem_pct", "mem_used_kb", "swap_total_kb", "swap_pct", "psi_some_avg10", "oom_kill"
};

static void parse_csv_line(char *line, const int *idx, Sample *s)
{
    memset(s, 0, sizeof(*s));
    char *f[64];
    int n = csv_split(line, f, 64);
    #define CSV_FIELD(ci) ((idx[ci] >= 0 && idx[ci] < n && *f[idx[ci]]) ? f[idx[ci]] : NULL)
    const char *ts = CSV_FIELD(CI_TS), *mem = CSV_FIELD(CI_MEMPCT);
    if (!ts || !mem) return;
    ts_from_iso(ts, s->ts, sizeof(s->ts));
    s->mem_pct = atof(mem);
    const char *v;
    if ((v = CSV_FIELD(CI_USED))) { s->has_used = 1; s->used_kb = atof(v); }
    const char *st = CSV_FIELD(CI_SWAPTOTAL);
    if (st && atof(st) > 0 && (v = CSV_FIELD(CI_SWAPPCT))) { s->has_swap = 1; s->swap_pct = atof(v); }
    if ((v = CSV_FIELD(CI_PSI))) { s->has_psi = 1; s->psi = atof(v); }
    if ((v = CSV_FIELD(CI_OOM))) { s->has_oom = 1; s->oom = strtoull(v, NULL, 10); }
    #undef CSV_FIELD
    s->valid = 1;
}

/* The text log is meant for humans, but its layout is fixed enough to
 * read the key numbers back: the timestamp header, the MEMORY/SWAP bars,
 * the "Used:" rows, PSI "some:" and "OOM kills:". Works on logs written
 * by older memmon versions too. */
enum { TS_NONE, TS_MEM, TS_SWAP, TS_PSI, TS_EVENTS };

static void text_line(const char *line, Sample *cur, int *sec, ReportStats *st)
{
    if (isdigit((unsigned char)line[0]) && strlen(line) > 19 && line[4] == '-' && strstr(line, "host=")) {
        stats_add(st, cur);
        memset(cur, 0, sizeof(*cur));
        copy_str(cur->ts, sizeof(cur->ts), line);
        cur->ts[19] = '\0';
        *sec = TS_NONE;
        return;
    }
    if (!cur->ts[0]) return;
    if (isupper((unsigned char)line[0])) {
        char title[64];
        copy_str(title, sizeof(title), line);
        char *t = trim(title);
        *sec = !strcmp(t, "MEMORY") ? TS_MEM : !strcmp(t, "SWAP") ? TS_SWAP :
               !strncmp(t, "MEMORY PRESSURE", 15) ? TS_PSI : !strcmp(t, "MEMORY EVENTS") ? TS_EVENTS : TS_NONE;
        return;
    }
    const char *p;
    if (!strncmp(line, "  [", 3) && (p = strchr(line, ']')) != NULL) {
        double pct = atof(p + 1);
        if (*sec == TS_MEM) { cur->mem_pct = pct; cur->valid = 1; }
        else if (*sec == TS_SWAP) { cur->swap_pct = pct; cur->has_swap = 1; }
    } else if (*sec == TS_MEM && (p = strstr(line, "Used:")) != NULL) {
        double kb;
        char buf[64];
        copy_str(buf, sizeof(buf), p + 5);
        if (parse_human_kb(trim(buf), &kb) == 0) { cur->has_used = 1; cur->used_kb = kb; }
    } else if (*sec == TS_PSI && (p = strstr(line, "some:")) != NULL) {
        const char *a = strstr(p, "avg10 ");
        if (a) { cur->has_psi = 1; cur->psi = atof(a + 6); }
    } else if (*sec == TS_EVENTS && (p = strstr(line, "OOM kills:")) != NULL) {
        cur->has_oom = 1;
        cur->oom = strtoull(p + 10, NULL, 10);
    }
}

static int report_file(const char *path, ReportStats *st)
{
    gzFile gz = gzopen(path, "rb"); /* reads plain files transparently too */
    if (!gz) {
        fprintf(stderr, "%s: cannot read '%s': %s\n", PROGNAME, path, strerror(errno));
        return -1;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int is_json = strstr(base, ".jsonl") != NULL;
    int is_csv = strstr(base, ".csv") != NULL;

    static char line[65536];
    int idx[CI_COUNT];
    for (int i = 0; i < CI_COUNT; i++) idx[i] = -1;
    int header_done = 0, sec = TS_NONE;
    Sample cur;
    memset(&cur, 0, sizeof(cur));

    while (gzgets(gz, line, sizeof(line))) {
        if (is_json) {
            Sample s;
            parse_json_line(line, &s);
            stats_add(st, &s);
        } else if (is_csv) {
            if (!header_done || !strncmp(line, "timestamp,", 10)) {
                char *f[64];
                int n = csv_split(line, f, 64);
                for (int c = 0; c < CI_COUNT; c++) {
                    idx[c] = -1;
                    for (int k = 0; k < n; k++)
                        if (!strcmp(f[k], CSV_WANTED[c])) idx[c] = k;
                }
                header_done = 1;
                continue;
            }
            Sample s;
            parse_csv_line(line, idx, &s);
            stats_add(st, &s);
        } else {
            line[strcspn(line, "\r\n")] = '\0';
            text_line(line, &cur, &sec, st);
        }
    }
    if (!is_json && !is_csv) stats_add(st, &cur);
    gzclose(gz);
    return 0;
}

static int resolve_report_date(const char *arg, char *out, size_t len)
{
    time_t t = time(NULL);
    if (!strcmp(arg, "today")) { local_time_str(t, "%Y-%m-%d", out, len); return 0; }
    if (!strcmp(arg, "yesterday")) {
        struct tm tmv;
        localtime_r(&t, &tmv);
        tmv.tm_mday -= 1;
        tmv.tm_hour = 12;
        tmv.tm_isdst = -1;
        time_t y = mktime(&tmv);
        local_time_str(y, "%Y-%m-%d", out, len);
        return 0;
    }
    char probe[32];
    snprintf(probe, sizeof(probe), "%.10s.log", arg);
    if (strlen(arg) == 10 && parse_log_filename(probe, NULL, NULL) == 0) {
        copy_str(out, len, arg);
        return 0;
    }
    return -1;
}

static void print_report_text(const ReportStats *st, const char *label, char files[][PATH_MAX + 32], int nfiles)
{
    char a[32], b[32], c[32];
    printf("memmon report: %s\n", label);
    for (int i = 0; i < nfiles; i++)
        printf("  %s %s\n", i ? "         " : "source:  ", files[i]);
    printf("\n");
    printf("  %-16s %ld  (%s - %s)\n", "samples:", st->n, st->first_ts + 11, st->last_ts + 11);
    printf("  %-16s min %5.1f%%   avg %5.1f%%   max %5.1f%% at %s\n", "memory used:",
           st->mem_min, st->mem_sum / (double)st->n, st->mem_max, st->mem_max_ts + 11);
    if (st->used_n) {
        human_kb((unsigned long long)st->used_min, a, sizeof(a));
        human_kb((unsigned long long)(st->used_sum / (double)st->used_n), b, sizeof(b));
        human_kb((unsigned long long)st->used_max, c, sizeof(c));
        printf("  %-16s min %s   avg %s   max %s\n", "", a, b, c);
    }
    if (st->swap_n)
        printf("  %-16s avg %5.1f%%   max %5.1f%%\n", "swap used:",
               st->swap_sum / (double)st->swap_n, st->swap_max);
    else
        printf("  %-16s (no swap)\n", "swap used:");
    if (st->psi_n)
        printf("  %-16s avg %.2f%%   max %.2f%% at %s\n", "PSI some avg10:",
               st->psi_sum / (double)st->psi_n, st->psi_max, st->psi_max_ts + 11);
    if (st->oom_seen)
        printf("  %-16s %llu\n", "OOM kills:", st->oom_kills);
}

static void print_report_json(const ReportStats *st, const char *label, char files[][PATH_MAX + 32], int nfiles)
{
    printf("{\"report\":");
    json_str(stdout, label);
    printf(",\"files\":[");
    for (int i = 0; i < nfiles; i++) {
        if (i) putchar(',');
        json_str(stdout, files[i]);
    }
    printf("],\"samples\":%ld,\"first\":", st->n);
    json_str(stdout, st->first_ts);
    printf(",\"last\":");
    json_str(stdout, st->last_ts);
    printf(",\"mem_pct_min\":%.2f,\"mem_pct_avg\":%.2f,\"mem_pct_max\":%.2f,\"mem_pct_max_at\":",
           st->mem_min, st->mem_sum / (double)st->n, st->mem_max);
    json_str(stdout, st->mem_max_ts);
    json_ull(stdout, "mem_used_kb_min", st->used_n > 0, (unsigned long long)st->used_min);
    json_ull(stdout, "mem_used_kb_avg", st->used_n > 0,
             st->used_n ? (unsigned long long)(st->used_sum / (double)st->used_n) : 0);
    json_ull(stdout, "mem_used_kb_max", st->used_n > 0, (unsigned long long)st->used_max);
    json_dbl(stdout, "swap_pct_avg", st->swap_n > 0, st->swap_n ? st->swap_sum / (double)st->swap_n : 0);
    json_dbl(stdout, "swap_pct_max", st->swap_n > 0, st->swap_max);
    json_dbl(stdout, "psi_some_avg10_avg", st->psi_n > 0, st->psi_n ? st->psi_sum / (double)st->psi_n : 0);
    json_dbl(stdout, "psi_some_avg10_max", st->psi_n > 0, st->psi_max);
    json_ull(stdout, "oom_kills", st->oom_seen, st->oom_kills);
    printf("}\n");
}

static int run_report(const Options *o, const char *arg)
{
    static char files[16][PATH_MAX + 32];
    int nfiles = 0;
    char label[PATH_MAX];
    struct stat st;

    if (strchr(arg, '/') || stat(arg, &st) == 0) {
        copy_str(files[nfiles++], sizeof(files[0]), arg);
        copy_str(label, sizeof(label), arg);
    } else {
        char date[16];
        if (resolve_report_date(arg, date, sizeof(date)) != 0) {
            fprintf(stderr, "%s: --report expects YYYY-MM-DD, 'today', 'yesterday' or a log file path, not '%s'\n",
                    PROGNAME, arg);
            return 2;
        }
        copy_str(label, sizeof(label), date);
        char dirs[2][PATH_MAX];
        int ndirs = 0;
        copy_str(dirs[ndirs++], sizeof(dirs[0]), o->logdir[0] ? o->logdir : DEFAULT_LOGDIR);
        if (o->logdir_src != SRC_CLI) fallback_logdir(dirs[ndirs++], sizeof(dirs[0]));
        static const char *const exts[] = { "log", "log.gz", "jsonl", "jsonl.gz", "csv", "csv.gz", NULL };
        for (int d = 0; d < ndirs && nfiles == 0; d++) {
            for (int e = 0; exts[e]; e++) {
                int n = snprintf(files[nfiles], sizeof(files[0]), "%s/%s.%s", dirs[d], date, exts[e]);
                if (n < 0 || (size_t)n >= sizeof(files[0])) continue; /* path too long */
                if (access(files[nfiles], R_OK) == 0) nfiles++;
            }
        }
        if (nfiles == 0) {
            fprintf(stderr, "%s: no readable memmon logs for %s in %s%s%s\n", PROGNAME, date, dirs[0],
                    ndirs > 1 ? " or " : "", ndirs > 1 ? dirs[1] : "");
            return 1;
        }
    }

    ReportStats stats;
    memset(&stats, 0, sizeof(stats));
    for (int i = 0; i < nfiles; i++)
        if (report_file(files[i], &stats) != 0) return 1;
    if (stats.n == 0) {
        fprintf(stderr, "%s: no samples found in %s\n", PROGNAME, files[0]);
        return 1;
    }
    if (o->out_format == FMT_JSON) print_report_json(&stats, label, files, nfiles);
    else if (o->out_format == FMT_CSV) {
        fprintf(stderr, "%s: --report supports --format text or json\n", PROGNAME);
        return 2;
    } else print_report_text(&stats, label, files, nfiles);
    return 0;
}

/* ---------------------------------------------------------------------
 * CLI
 * ------------------------------------------------------------------- */

static void usage(void)
{
    printf(
"%s %s - Linux memory usage monitor\n\n"
"Usage: %s [options]\n\n"
"Modes (default: interactive ncurses display):\n"
"  -d, --daemon               Headless logger for systemd: no display, logs a\n"
"                             snapshot every interval, raises alerts. SIGTERM/\n"
"                             SIGINT stop it, SIGHUP reloads the config.\n"
"  -o, --once                 Print a single snapshot to stdout and exit (no\n"
"                             ncurses, no loop, no log file written).\n"
"  -p, --plain                Interactive watch without ncurses: plain ANSI\n"
"                             clear-screen/redraw. Try this if the default\n"
"                             display looks garbled or doesn't redraw in place\n"
"                             (common under some Windows/WSL terminals).\n"
"  -R, --report DAY|FILE      Summarise a day's log (min/avg/max, peak time,\n"
"                             OOM kills). DAY is YYYY-MM-DD, today or yesterday.\n\n"
"Options:\n"
"  -i, --interval INTERVAL    Refresh/log interval: plain seconds or a suffix\n"
"                             s, m, h, d (e.g. 5, 5s, 5m, 1h). Default: 5s\n"
"      --no-align             Don't align daemon/plain samples to wall-clock\n"
"                             multiples of the interval.\n"
"  -f, --format FMT           Output format for --once and --report:\n"
"                             text (default), json or csv.\n"
"  -l, --log-dir DIR          Directory for daily log files. Default: %s\n"
"                             (falls back to ~/%s if not writable).\n"
"  -F, --log-format FMT       Log file format: text (.log, default), json\n"
"                             (.jsonl, one object per line) or csv (.csv).\n"
"  -r, --log-retention-days N Delete log files older than N days (0 = keep\n"
"                             forever). Default: %d\n"
"      --no-compress          Don't gzip previous days' log files.\n"
"  -n, --no-log               Disable logging in interactive modes.\n"
"  -t, --top N                Show the N largest processes by RSS (0-%d, 0\n"
"                             hides the section). Default: %d\n"
"  -w, --warn PCT             Memory warning threshold. Default: %.0f\n"
"  -C, --crit PCT             Memory critical threshold. Default: %.0f\n"
"  -a, --alert-cmd CMD        Shell command run (daemon mode) whenever the\n"
"                             alert level changes; see memmon(1).\n"
"  -g, --cgroup PATH          Report this cgroup v2 group (default: the\n"
"                             nearest memory-limited ancestor of our own).\n"
"  -u, --units UNITS          auto (default), k, m or g.\n"
"  -c, --config FILE          Read settings from FILE instead of\n"
"                             %s and ~/.config/memmon/memmon.conf.\n"
"  -h, --help                 Show this help and exit.\n"
"  -v, --version              Show version and exit.\n\n"
"Every option except the modes can also be set in the config file or the\n"
"environment (MEMMON_INTERVAL, MEMMON_LOG_DIR, ...); see memmon(1).\n\n"
"Interactive keys:  q quit | up/down j/k scroll | PgUp/PgDn | g/G top/bottom\n"
"                   r refresh | p pause | u units | / search, n/N next/prev\n"
"                   ? help\n\n"
"Examples:\n"
"  %s                          Interactive TUI, refresh every 5 seconds\n"
"  %s -i 5m                    Interactive TUI, refresh every 5 minutes\n"
"  %s --once --format json     One-shot JSON snapshot\n"
"  %s --daemon -i 5m -r 14     Headless logger, keep 14 days of logs\n"
"  %s --report yesterday       Summary of yesterday's log\n",
    PROGNAME, VERSION, PROGNAME, DEFAULT_LOGDIR, FALLBACK_LOGDIR,
    DEFAULT_LOG_RETENTION_DAYS, MAX_TOP, DEFAULT_TOP_PROCESSES,
    DEFAULT_WARN_PCT, DEFAULT_CRIT_PCT, SYSTEM_CONFIG,
    PROGNAME, PROGNAME, PROGNAME, PROGNAME, PROGNAME);
}

enum {
    OPT_SETTING, OPT_DAEMON, OPT_ONCE, OPT_PLAIN, OPT_NOLOG, OPT_CONFIG, OPT_FORMAT,
    OPT_REPORT, OPT_NOCOMPRESS, OPT_NOALIGN, OPT_HELP, OPT_VERSION
};

typedef struct {
    char shrt;
    const char *lng;
    int has_arg;
    int id;
    const char *key;     /* for OPT_SETTING */
} OptDef;

static const OptDef OPTDEFS[] = {
    { 'i', "interval",           1, OPT_SETTING,    "MEMMON_INTERVAL" },
    { 'd', "daemon",             0, OPT_DAEMON,     NULL },
    { 'o', "once",               0, OPT_ONCE,       NULL },
    { 'p', "plain",              0, OPT_PLAIN,      NULL },
    { 'R', "report",             1, OPT_REPORT,     NULL },
    { 'l', "log-dir",            1, OPT_SETTING,    "MEMMON_LOG_DIR" },
    { 'r', "log-retention-days", 1, OPT_SETTING,    "MEMMON_LOG_RETENTION_DAYS" },
    { 'F', "log-format",         1, OPT_SETTING,    "MEMMON_LOG_FORMAT" },
    {  0,  "no-compress",        0, OPT_NOCOMPRESS, NULL },
    { 'n', "no-log",             0, OPT_NOLOG,      NULL },
    { 'f', "format",             1, OPT_FORMAT,     NULL },
    { 't', "top",                1, OPT_SETTING,    "MEMMON_TOP_PROCESSES" },
    { 'w', "warn",               1, OPT_SETTING,    "MEMMON_WARN_PERCENT" },
    { 'C', "crit",               1, OPT_SETTING,    "MEMMON_CRIT_PERCENT" },
    { 'a', "alert-cmd",          1, OPT_SETTING,    "MEMMON_ALERT_CMD" },
    { 'g', "cgroup",             1, OPT_SETTING,    "MEMMON_CGROUP" },
    { 'u', "units",              1, OPT_SETTING,    "MEMMON_UNITS" },
    {  0,  "no-align",           0, OPT_NOALIGN,    NULL },
    { 'c', "config",             1, OPT_CONFIG,     NULL },
    { 'h', "help",               0, OPT_HELP,       NULL },
    { 'v', "version",            0, OPT_VERSION,    NULL },
};
#define N_OPTDEFS ((int)(sizeof(OPTDEFS) / sizeof(OPTDEFS[0])))

typedef struct { const OptDef *def; const char *val; const char *spelled; } ParsedOpt;

/* Tokenises argv into (option, value) pairs without applying them, so the
 * config file named by -c can be loaded before command-line values
 * override it. Returns the count, or -1 after printing an error. */
static int parse_args(int argc, char **argv, ParsedOpt *out)
{
    int n = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const OptDef *def = NULL;
        const char *inline_val = NULL;
        if (a[0] == '-' && a[1] == '-' && a[2]) {
            const char *name = a + 2;
            size_t len = strcspn(name, "=");
            for (int k = 0; k < N_OPTDEFS; k++) {
                if (strlen(OPTDEFS[k].lng) == len && !strncmp(OPTDEFS[k].lng, name, len)) {
                    def = &OPTDEFS[k];
                    break;
                }
            }
            if (def && name[len] == '=') inline_val = name + len + 1;
        } else if (a[0] == '-' && a[1] && !a[2]) {
            for (int k = 0; k < N_OPTDEFS; k++)
                if (OPTDEFS[k].shrt == a[1]) { def = &OPTDEFS[k]; break; }
        }
        if (!def) {
            fprintf(stderr, "%s: unknown option '%s' (see --help)\n", PROGNAME, a);
            return -1;
        }
        const char *val = NULL;
        if (def->has_arg) {
            if (inline_val) val = inline_val;
            else if (i + 1 < argc) val = argv[++i];
            else {
                fprintf(stderr, "%s: option '%s' requires a value (see --help)\n", PROGNAME, a);
                return -1;
            }
        } else if (inline_val) {
            fprintf(stderr, "%s: option '--%s' doesn't take a value\n", PROGNAME, def->lng);
            return -1;
        }
        out[n].def = def;
        out[n].val = val;
        out[n].spelled = a;
        n++;
    }
    return n;
}

#ifndef MEMMON_NO_MAIN
int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
    const char *sysroot = getenv("MEMMON_SYSROOT");
    if (sysroot) copy_str(g_sysroot, sizeof(g_sysroot), sysroot);

    ParsedOpt *args = calloc((size_t)argc + 1, sizeof(ParsedOpt));
    if (!args) { perror(PROGNAME); return 1; }
    int nargs = parse_args(argc, argv, args);
    if (nargs < 0) return 2;

    const char *config_path = NULL;
    for (int i = 0; i < nargs; i++) {
        if (args[i].def->id == OPT_HELP) { usage(); return 0; }
        if (args[i].def->id == OPT_VERSION) { printf("%s %s\n", PROGNAME, VERSION); return 0; }
        if (args[i].def->id == OPT_CONFIG) config_path = args[i].val;
    }

    Options o;
    options_default(&o);
    if (config_path) {
        if (config_load(&o, config_path, 1) != 0) return 2;
    } else {
        char user_path[PATH_MAX];
        if (config_load(&o, SYSTEM_CONFIG, 0) != 0) return 2;
        user_config_path(user_path, sizeof(user_path));
        if (user_path[0] && config_load(&o, user_path, 0) != 0) return 2;
    }
    if (env_load(&o) != 0) return 2;

    int daemon_mode = 0, once_mode = 0, plain_mode = 0, no_log = 0, format_given = 0;
    const char *report_arg = NULL;
    for (int i = 0; i < nargs; i++) {
        const ParsedOpt *p = &args[i];
        char err[160];
        switch (p->def->id) {
            case OPT_SETTING:
                if (apply_setting(&o, p->def->key, p->val, SRC_CLI, err, sizeof(err)) != 0) {
                    fprintf(stderr, "%s: %s: %s\n", PROGNAME, p->spelled, err);
                    return 2;
                }
                break;
            case OPT_DAEMON: daemon_mode = 1; break;
            case OPT_ONCE: once_mode = 1; break;
            case OPT_PLAIN: plain_mode = 1; break;
            case OPT_NOLOG: no_log = 1; break;
            case OPT_NOCOMPRESS: o.compress_logs = 0; break;
            case OPT_NOALIGN: o.align = 0; break;
            case OPT_REPORT: report_arg = p->val; break;
            case OPT_FORMAT:
                if (parse_format(p->val, &o.out_format) != 0) {
                    fprintf(stderr, "%s: %s: invalid format '%s' (use text, json or csv)\n",
                            PROGNAME, p->spelled, p->val);
                    return 2;
                }
                format_given = 1;
                break;
            default: break;
        }
    }
    if (daemon_mode + once_mode + plain_mode + (report_arg != NULL) > 1) {
        fprintf(stderr, "%s: choose only one of --daemon, --once, --plain and --report\n", PROGNAME);
        return 2;
    }
    if (format_given && !once_mode && !report_arg) {
        fprintf(stderr, "%s: --format applies to --once and --report (use --log-format for log files)\n",
                PROGNAME);
        return 2;
    }
    if (o.warn_pct >= o.crit_pct) {
        fprintf(stderr, "%s: warning threshold (%.0f%%) must be below critical threshold (%.0f%%)\n",
                PROGNAME, o.warn_pct, o.crit_pct);
        return 2;
    }
    g_units = o.units;
    g_warn_pct = o.warn_pct;
    g_crit_pct = o.crit_pct;

    if (report_arg) return run_report(&o, report_arg);

    char hostname[256] = "unknown";
    gethostname(hostname, sizeof(hostname) - 1);

    struct utsname uts;
    char kernel[128] = "unknown";
    if (uname(&uts) == 0)
        copy_str(kernel, sizeof(kernel), uts.release);

    if (once_mode) {
        static Snapshot snap;
        if (snapshot_collect(&snap, &o) != 0) {
            fprintf(stderr, "%s: failed to read /proc/meminfo: %s\n", PROGNAME, strerror(errno));
            return 1;
        }
        switch (o.out_format) {
            case FMT_JSON: write_json(stdout, &snap, hostname, kernel); break;
            case FMT_CSV:  write_csv_header(stdout); write_csv(stdout, &snap, hostname); break;
            default:       print_once_text(&snap, hostname, kernel); break;
        }
        return 0;
    }

    int logging = daemon_mode || !no_log;
    if (logging) {
        resolve_logdir(&o, daemon_mode, g_logdir, sizeof(g_logdir));
        if (mkdir_p(g_logdir) != 0) {
            fprintf(stderr, "%s: warning: could not create log dir '%s': %s\n",
                    PROGNAME, g_logdir, strerror(errno));
        }
    }

    if (daemon_mode)
        return run_daemon(&o, hostname, kernel, argv);

    if (plain_mode)
        return run_plain_watch(&o, logging, hostname, kernel);

#ifdef USE_NCURSES
    return run_interactive(&o, logging, hostname, kernel);
#else
    fprintf(stderr,
        "%s: built without ncurses support; using --plain mode.\n",
        PROGNAME);
    return run_plain_watch(&o, logging, hostname, kernel);
#endif
}
#endif /* MEMMON_NO_MAIN */
