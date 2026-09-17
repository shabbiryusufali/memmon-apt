/*
 * memmon - Linux memory usage monitor
 *
 * Reads /proc/meminfo (and hostname/uname/loadavg) and renders a
 * scrollable, man-page-styled report either as an interactive ncurses
 * TUI (with a "watch"-style auto refresh) or, in --daemon mode, as
 * periodic plain-text snapshots appended to a daily log file. The
 * daemon mode is intended to be run under systemd. Daily log files
 * older than --log-retention-days (default 30, 0 disables) are
 * deleted automatically whenever the log rolls over to a new day.
 *
 * Build:   make
 * Run:     ./memmon                     (interactive, 5s refresh)
 *          ./memmon -i 5m                (interactive, 5 minute refresh)
 *          ./memmon --once               (single snapshot to stdout, no ncurses)
 *          ./memmon --daemon -i 5m       (headless logger, for systemd)
 *
 * License: do whatever you want with it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <ctype.h>
#include <locale.h>
#include <dirent.h>

#ifdef USE_NCURSES
#include <ncurses.h>
#endif

#define PROGNAME        "memmon"
#define VERSION         "1.1"
#define MEMINFO_PATH    "/proc/meminfo"
#define LOADAVG_PATH    "/proc/loadavg"
#define MAX_ENTRIES     64
#define KEY_LEN         48
#define DEFAULT_LOGDIR  "/var/log/memmon"
#define FALLBACK_LOGDIR ".local/share/memmon"   /* under $HOME */
#define DEFAULT_LOG_RETENTION_DAYS 30            /* 0 disables auto-delete */
#define PAD_LINES        400
#define PAD_COLS         220

/* ---------------------------------------------------------------------
 * /proc/meminfo parsing
 * ------------------------------------------------------------------- */

typedef struct {
    char key[KEY_LEN];
    unsigned long long kb;
} MeminfoEntry;

typedef struct {
    MeminfoEntry entries[MAX_ENTRIES];
    int count;
    time_t sampled_at;
} Meminfo;

static int meminfo_read(Meminfo *mi)
{
    FILE *f = fopen(MEMINFO_PATH, "r");
    if (!f) return -1;

    mi->count = 0;
    mi->sampled_at = time(NULL);

    char line[256];
    while (fgets(line, sizeof(line), f) && mi->count < MAX_ENTRIES) {
        char key[KEY_LEN];
        unsigned long long val;
        /* Lines look like: "MemTotal:       16384000 kB" or
         * "HugePages_Total:       0" (no unit) */
        if (sscanf(line, "%47[^:]: %llu", key, &val) == 2) {
            snprintf(mi->entries[mi->count].key, KEY_LEN, "%s", key);
            mi->entries[mi->count].kb = val;
            mi->count++;
        }
    }
    fclose(f);
    return 0;
}

static unsigned long long mi_get(const Meminfo *mi, const char *key)
{
    for (int i = 0; i < mi->count; i++) {
        if (strcmp(mi->entries[i].key, key) == 0)
            return mi->entries[i].kb;
    }
    return 0;
}

static int mi_has(const Meminfo *mi, const char *key)
{
    for (int i = 0; i < mi->count; i++) {
        if (strcmp(mi->entries[i].key, key) == 0)
            return 1;
    }
    return 0;
}

/* read the 1/5/15 min load average, returns 0 on success */
static int loadavg_read(double *l1, double *l5, double *l15)
{
    FILE *f = fopen(LOADAVG_PATH, "r");
    if (!f) return -1;
    int rc = fscanf(f, "%lf %lf %lf", l1, l5, l15);
    fclose(f);
    return (rc == 3) ? 0 : -1;
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
    long secs = (long)(val * mult);
    if (secs < 1) secs = 1;
    return secs;
}

static void format_interval(long secs, char *buf, size_t buflen)
{
    /* buf is expected to be reasonably sized (>=24 bytes) by callers */
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
 * Derived stats
 * ------------------------------------------------------------------- */

typedef struct {
    unsigned long long total, free, available, buffers, cached, shmem;
    unsigned long long used;                 /* total - available (fallback variants) */
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
        unsigned long long est = d->free + d->buffers + d->cached + sreclaim;
        d->available = est;
    }
    if (d->available > d->total) d->available = d->total; /* clamp */

    d->used = (d->total >= d->available) ? d->total - d->available : 0;
    d->swap_used = (d->swap_total >= d->swap_free) ? d->swap_total - d->swap_free : 0;

    d->mem_pct = d->total ? (100.0 * (double)d->used / (double)d->total) : 0.0;
    d->swap_pct = d->swap_total ? (100.0 * (double)d->swap_used / (double)d->swap_total) : 0.0;
}

/* ---------------------------------------------------------------------
 * Human readable byte formatting (input is kB from /proc/meminfo)
 * ------------------------------------------------------------------- */

static void human_kb(unsigned long long kb, char *buf, size_t buflen)
{
    const char *units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    double val = (double)kb;
    int u = 0;
    while (val >= 1024.0 && u < 4) {
        val /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, buflen, "%.0f %s", val, units[u]);
    else
        snprintf(buf, buflen, "%.2f %s", val, units[u]);
}

/* ---------------------------------------------------------------------
 * Logging - plain text snapshots, one file per calendar day
 * ------------------------------------------------------------------- */

static void today_str(char *buf, size_t buflen)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, buflen, "%Y-%m-%d", &tmv);
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    /* Try to create it (and parents, best-effort single level) */
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void resolve_logdir(const char *requested, char *out, size_t outlen)
{
    if (requested && *requested) {
        strncpy(out, requested, outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    /* Prefer /var/log/memmon if it exists or we can create/write to it */
    if (ensure_dir(DEFAULT_LOGDIR) == 0 && access(DEFAULT_LOGDIR, W_OK) == 0) {
        strncpy(out, DEFAULT_LOGDIR, outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(out, outlen, "%s/%s", home, FALLBACK_LOGDIR);
        /* create parent-ish best effort */
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.local", home);
        ensure_dir(parent);
        snprintf(parent, sizeof(parent), "%s/.local/share", home);
        ensure_dir(parent);
        ensure_dir(out);
        return;
    }
    strncpy(out, "/tmp/memmon", outlen - 1);
    out[outlen - 1] = '\0';
    ensure_dir(out);
}

static void write_snapshot(FILE *f, const Meminfo *mi, const Derived *d,
                            const char *hostname, const char *kernel)
{
    char ts[64];
    struct tm tmv;
    localtime_r(&mi->sampled_at, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", &tmv);

    char used_h[32], total_h[32], avail_h[32], free_h[32], buf_h[32], cache_h[32], shmem_h[32];
    char sused_h[32], stotal_h[32], sfree_h[32], scache_h[32];
    human_kb(d->used, used_h, sizeof(used_h));
    human_kb(d->total, total_h, sizeof(total_h));
    human_kb(d->available, avail_h, sizeof(avail_h));
    human_kb(d->free, free_h, sizeof(free_h));
    human_kb(d->buffers, buf_h, sizeof(buf_h));
    human_kb(d->cached, cache_h, sizeof(cache_h));
    human_kb(d->shmem, shmem_h, sizeof(shmem_h));
    human_kb(d->swap_used, sused_h, sizeof(sused_h));
    human_kb(d->swap_total, stotal_h, sizeof(stotal_h));
    human_kb(d->swap_free, sfree_h, sizeof(sfree_h));
    human_kb(d->swap_cached, scache_h, sizeof(scache_h));

    double l1 = -1, l5 = -1, l15 = -1;
    loadavg_read(&l1, &l5, &l15);

    fprintf(f, "==================================================================\n");
    fprintf(f, "%s  host=%s  kernel=%s\n", ts, hostname, kernel);
    fprintf(f, "------------------------------------------------------------------\n");
    fprintf(f, "Mem   total=%-10s used=%-10s (%.1f%%) avail=%-10s free=%-10s\n",
            total_h, used_h, d->mem_pct, avail_h, free_h);
    fprintf(f, "      buffers=%-10s cached=%-10s shared=%-10s\n",
            buf_h, cache_h, shmem_h);
    fprintf(f, "Swap  total=%-10s used=%-10s (%.1f%%) free=%-10s cached=%-10s\n",
            stotal_h, sused_h, d->swap_pct, sfree_h, scache_h);
    if (l1 >= 0)
        fprintf(f, "Load  1m=%.2f 5m=%.2f 15m=%.2f\n", l1, l5, l15);

    /* Extended fields, if present in this kernel's /proc/meminfo */
    static const char *extra_fields[] = {
        "Active", "Inactive", "Active(anon)", "Inactive(anon)",
        "Active(file)", "Inactive(file)", "Unevictable", "Mlocked",
        "Dirty", "Writeback", "AnonPages", "Mapped", "KReclaimable",
        "Slab", "SReclaimable", "SUnreclaim", "KernelStack", "PageTables",
        "CommitLimit", "Committed_AS", "VmallocTotal", "VmallocUsed",
        "AnonHugePages", "HugePages_Total", "HugePages_Free", "Hugepagesize",
        NULL
    };
    fprintf(f, "------------------------------------------------------------------\n");
    for (int i = 0; extra_fields[i]; i++) {
        if (!mi_has(mi, extra_fields[i])) continue;
        unsigned long long v = mi_get(mi, extra_fields[i]);
        char h[32];
        /* HugePages_* and Hugepagesize/VmallocTotal etc are still in kB from
         * /proc/meminfo except the *_Total/_Free counts, which are raw counts. */
        if (strncmp(extra_fields[i], "HugePages_", 10) == 0)
            fprintf(f, "  %-16s %llu\n", extra_fields[i], v);
        else {
            human_kb(v, h, sizeof(h));
            fprintf(f, "  %-16s %s\n", extra_fields[i], h);
        }
    }
    fprintf(f, "\n");
    fflush(f);
}

static FILE *g_logfile = NULL;
static char g_log_date[16] = "";
static char g_logdir[512] = "";
static int g_log_retention_days = DEFAULT_LOG_RETENTION_DAYS;

static void prune_old_logs(const char *logdir, int retention_days);

static int log_open_for_today(void)
{
    char date[16];
    today_str(date, sizeof(date));
    if (g_logfile && strcmp(date, g_log_date) == 0)
        return 0; /* already open for today */

    if (g_logfile) {
        fclose(g_logfile);
        g_logfile = NULL;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.log", g_logdir, date);
    g_logfile = fopen(path, "a");
    if (!g_logfile) {
        fprintf(stderr, "%s: cannot open log file '%s': %s\n",
                PROGNAME, path, strerror(errno));
        return -1;
    }
    snprintf(g_log_date, sizeof(g_log_date), "%s", date);
    /* Rolling to a new day's file is a natural, cheap point to sweep
     * out anything older than the retention window. */
    prune_old_logs(g_logdir, g_log_retention_days);
    return 0;
}

/* ---------------------------------------------------------------------
 * Log retention - delete daily log files (YYYY-MM-DD.log) older than
 * retention_days. A retention_days value <= 0 disables pruning
 * entirely. Only ever touches files matching the exact name pattern
 * memmon itself writes, so a log-dir shared with other files is safe.
 * ------------------------------------------------------------------- */

static int parse_log_filename_date(const char *name, struct tm *out)
{
    /* Expect exactly "YYYY-MM-DD.log" */
    int y, mo, d;
    char suffix[8];
    if (sscanf(name, "%4d-%2d-%2d.log%7s", &y, &mo, &d, suffix) != 3)
        return -1;
    if (strlen(name) != 14) /* "YYYY-MM-DD.log" == 14 chars, rejects trailing junk */
        return -1;
    memset(out, 0, sizeof(*out));
    out->tm_year = y - 1900;
    out->tm_mon  = mo - 1;
    out->tm_mday = d;
    out->tm_hour = 12; /* noon, to sidestep any DST edge effects in mktime */
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
        if (parse_log_filename_date(ent->d_name, &file_tm) != 0)
            continue;

        time_t file_time = mktime(&file_tm);
        if (file_time == (time_t)-1) continue;

        double age_days = difftime(now, file_time) / 86400.0;
        if (age_days <= (double)retention_days) continue;

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", logdir, ent->d_name);
        if (unlink(path) != 0) {
            fprintf(stderr, "%s: warning: could not delete expired log '%s': %s\n",
                    PROGNAME, path, strerror(errno));
        }
    }
    closedir(dir);
}

/* ---------------------------------------------------------------------
 * Plain-text (non-ncurses) single snapshot, for --once and as a
 * fallback if ncurses isn't compiled in.
 * ------------------------------------------------------------------- */

static void print_bar_plain(FILE *out, double pct, int width)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)((pct / 100.0) * width + 0.5);
    fputc('[', out);
    for (int i = 0; i < width; i++)
        fputc(i < filled ? '#' : '-', out);
    fprintf(out, "] %5.1f%%", pct);
}

static void print_once(const Meminfo *mi, const Derived *d,
                        const char *hostname, const char *kernel)
{
    char ts[64];
    struct tm tmv;
    localtime_r(&mi->sampled_at, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    printf("%s(1)                 Memory Monitor                 %s(1)\n\n", PROGNAME, PROGNAME);
    printf("HOST\n    %s   kernel %s   %s\n\n", hostname, kernel, ts);

    char used_h[32], total_h[32], avail_h[32], free_h[32], buf_h[32], cache_h[32], shmem_h[32];
    human_kb(d->used, used_h, sizeof(used_h));
    human_kb(d->total, total_h, sizeof(total_h));
    human_kb(d->available, avail_h, sizeof(avail_h));
    human_kb(d->free, free_h, sizeof(free_h));
    human_kb(d->buffers, buf_h, sizeof(buf_h));
    human_kb(d->cached, cache_h, sizeof(cache_h));
    human_kb(d->shmem, shmem_h, sizeof(shmem_h));

    printf("MEMORY\n");
    printf("    ");
    print_bar_plain(stdout, d->mem_pct, 40);
    printf("\n    total %-10s used %-10s avail %-10s free %-10s\n",
           total_h, used_h, avail_h, free_h);
    printf("    buffers %-10s cached %-10s shared %-10s\n\n", buf_h, cache_h, shmem_h);

    char sused_h[32], stotal_h[32], sfree_h[32], scache_h[32];
    human_kb(d->swap_used, sused_h, sizeof(sused_h));
    human_kb(d->swap_total, stotal_h, sizeof(stotal_h));
    human_kb(d->swap_free, sfree_h, sizeof(sfree_h));
    human_kb(d->swap_cached, scache_h, sizeof(scache_h));

    printf("SWAP\n");
    printf("    ");
    print_bar_plain(stdout, d->swap_pct, 40);
    printf("\n    total %-10s used %-10s free %-10s cached %-10s\n\n",
           stotal_h, sused_h, sfree_h, scache_h);

    static const char *extra_fields[] = {
        "Active", "Inactive", "Active(anon)", "Inactive(anon)",
        "Active(file)", "Inactive(file)", "Unevictable", "Mlocked",
        "Dirty", "Writeback", "AnonPages", "Mapped", "KReclaimable",
        "Slab", "SReclaimable", "SUnreclaim", "KernelStack", "PageTables",
        "CommitLimit", "Committed_AS", "VmallocTotal", "VmallocUsed",
        "AnonHugePages", "HugePages_Total", "HugePages_Free", "Hugepagesize",
        NULL
    };
    printf("DETAILS\n");
    for (int i = 0; extra_fields[i]; i++) {
        if (!mi_has(mi, extra_fields[i])) continue;
        unsigned long long v = mi_get(mi, extra_fields[i]);
        char h[32];
        if (strncmp(extra_fields[i], "HugePages_", 10) == 0)
            printf("    %-18s %llu\n", extra_fields[i], v);
        else {
            human_kb(v, h, sizeof(h));
            printf("    %-18s %s\n", extra_fields[i], h);
        }
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

static volatile sig_atomic_t g_plain_stop = 0;
static void handle_plain_signal(int sig) { (void)sig; g_plain_stop = 1; }

static void print_bar_ansi(double pct, int width)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (int)((pct / 100.0) * width + 0.5);
    const char *color = pct < 60.0 ? "\x1b[32m" : (pct < 85.0 ? "\x1b[33m" : "\x1b[31m");
    printf("[%s", color);
    for (int i = 0; i < width; i++)
        putchar(i < filled ? '#' : '-');
    printf("\x1b[0m] %5.1f%%", pct);
}

static int run_plain_watch(long interval_secs, int logging_enabled,
                            const char *hostname, const char *kernel)
{
    signal(SIGINT, handle_plain_signal);
    signal(SIGTERM, handle_plain_signal);

    while (!g_plain_stop) {
        Meminfo mi; Derived d;
        if (meminfo_read(&mi) != 0) {
            fprintf(stderr, "%s: failed to read %s: %s\n", PROGNAME, MEMINFO_PATH, strerror(errno));
            return 1;
        }
        derive(&mi, &d);

        char ibuf[32];
        format_interval(interval_secs, ibuf, sizeof(ibuf));
        char ts[64];
        struct tm tmv;
        localtime_r(&mi.sampled_at, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

        fputs("\x1b[H\x1b[2J", stdout); /* cursor home + clear screen */
        printf("\x1b[1;7m memmon \x1b[0m  System Memory Monitor - %s   refresh: %s   %s\n\n",
               hostname, ibuf, ts);

        char used_h[32], total_h[32], avail_h[32], free_h[32], buf_h[32], cache_h[32], shmem_h[32];
        human_kb(d.used, used_h, sizeof(used_h));
        human_kb(d.total, total_h, sizeof(total_h));
        human_kb(d.available, avail_h, sizeof(avail_h));
        human_kb(d.free, free_h, sizeof(free_h));
        human_kb(d.buffers, buf_h, sizeof(buf_h));
        human_kb(d.cached, cache_h, sizeof(cache_h));
        human_kb(d.shmem, shmem_h, sizeof(shmem_h));

        printf("\x1b[1;36mMEMORY\x1b[0m\n  ");
        print_bar_ansi(d.mem_pct, 40);
        printf("\n  total %-10s used %-10s avail %-10s free %-10s\n",
               total_h, used_h, avail_h, free_h);
        printf("  buffers %-10s cached %-10s shared %-10s\n\n", buf_h, cache_h, shmem_h);

        char sused_h[32], stotal_h[32], sfree_h[32], scache_h[32];
        human_kb(d.swap_used, sused_h, sizeof(sused_h));
        human_kb(d.swap_total, stotal_h, sizeof(stotal_h));
        human_kb(d.swap_free, sfree_h, sizeof(sfree_h));
        human_kb(d.swap_cached, scache_h, sizeof(scache_h));

        printf("\x1b[1;36mSWAP\x1b[0m\n  ");
        print_bar_ansi(d.swap_pct, 40);
        printf("\n  total %-10s used %-10s free %-10s cached %-10s\n\n",
               stotal_h, sused_h, sfree_h, scache_h);

        double l1, l5, l15;
        if (loadavg_read(&l1, &l5, &l15) == 0)
            printf("\x1b[1;36mLOAD AVERAGE\x1b[0m\n  1m: %.2f   5m: %.2f   15m: %.2f\n\n", l1, l5, l15);

        static const char *extra_fields[] = {
            "Active", "Inactive", "Active(anon)", "Inactive(anon)",
            "Active(file)", "Inactive(file)", "Dirty", "Writeback",
            "AnonPages", "Mapped", "Slab", "SReclaimable", "SUnreclaim",
            "KernelStack", "PageTables", "Committed_AS", "VmallocUsed",
            "HugePages_Total", "HugePages_Free", NULL
        };
        printf("\x1b[1;36mDETAILS\x1b[0m\n");
        int col = 0;
        for (int i = 0; extra_fields[i]; i++) {
            if (!mi_has(&mi, extra_fields[i])) continue;
            unsigned long long v = mi_get(&mi, extra_fields[i]);
            char h[32];
            if (strncmp(extra_fields[i], "HugePages_", 10) == 0)
                printf("  %-16s %-14llu", extra_fields[i], v);
            else {
                human_kb(v, h, sizeof(h));
                printf("  %-16s %-14s", extra_fields[i], h);
            }
            col++;
            if (col == 2) { col = 0; putchar('\n'); }
        }
        if (col != 0) putchar('\n');

        printf("\n(Ctrl-C to quit -- plain mode, refresh every %s)\n", ibuf);
        fflush(stdout);

        if (logging_enabled) {
            if (log_open_for_today() == 0)
                write_snapshot(g_logfile, &mi, &d, hostname, kernel);
        }

        long remaining = interval_secs;
        while (remaining > 0 && !g_plain_stop) {
            long chunk = remaining > 1 ? 1 : remaining;
            sleep((unsigned int)chunk);
            remaining -= chunk;
        }
    }
    if (g_logfile) fclose(g_logfile);
    printf("\n");
    return 0;
}

/* ---------------------------------------------------------------------
 * ncurses TUI
 * ------------------------------------------------------------------- */

#ifdef USE_NCURSES

enum { CP_HEADER = 1, CP_TITLE, CP_LABEL, CP_BAR_OK, CP_BAR_WARN, CP_BAR_CRIT, CP_VALUE, CP_DIM };

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
}

static int bar_color_for(double pct)
{
    if (pct < 60.0) return CP_BAR_OK;
    if (pct < 85.0) return CP_BAR_WARN;
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

static void kv_row(WINDOW *w, int y, int x, const char *label, const char *value, int width)
{
    wattron(w, COLOR_PAIR(CP_LABEL));
    mvwprintw(w, y, x, "%-16s", label);
    wattroff(w, COLOR_PAIR(CP_LABEL));
    wattron(w, COLOR_PAIR(CP_VALUE) | A_BOLD);
    mvwprintw(w, y, x + 16, "%-*s", width, value);
    wattroff(w, COLOR_PAIR(CP_VALUE) | A_BOLD);
}

static int build_pad(WINDOW *pad, const Meminfo *mi, const Derived *d,
                      const char *hostname, const char *kernel)
{
    werase(pad);
    int y = 1;

    char ts[64];
    struct tm tmv;
    localtime_r(&mi->sampled_at, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    wattron(pad, A_BOLD);
    mvwprintw(pad, y, 0, "System Memory Report");
    wattroff(pad, A_BOLD);
    wattron(pad, COLOR_PAIR(CP_DIM));
    mvwprintw(pad, y, 24, "host: %s   kernel: %s   sampled: %s", hostname, kernel, ts);
    wattroff(pad, COLOR_PAIR(CP_DIM));
    y += 2;

    /* ---- Overview ---- */
    section_title(pad, &y, "MEMORY OVERVIEW");
    char used_h[32], total_h[32], avail_h[32], free_h[32], buf_h[32], cache_h[32], shmem_h[32];
    human_kb(d->used, used_h, sizeof(used_h));
    human_kb(d->total, total_h, sizeof(total_h));
    human_kb(d->available, avail_h, sizeof(avail_h));
    human_kb(d->free, free_h, sizeof(free_h));
    human_kb(d->buffers, buf_h, sizeof(buf_h));
    human_kb(d->cached, cache_h, sizeof(cache_h));
    human_kb(d->shmem, shmem_h, sizeof(shmem_h));

    draw_bar(pad, y, 4, 40, d->mem_pct); y += 2;
    kv_row(pad, y, 4, "Total:", total_h, 14);
    kv_row(pad, y, 34, "Used:", used_h, 14); y++;
    kv_row(pad, y, 4, "Available:", avail_h, 14);
    kv_row(pad, y, 34, "Free:", free_h, 14); y++;
    kv_row(pad, y, 4, "Buffers:", buf_h, 14);
    kv_row(pad, y, 34, "Cached:", cache_h, 14); y++;
    kv_row(pad, y, 4, "Shared (Shmem):", shmem_h, 14); y += 2;

    /* ---- Swap ---- */
    section_title(pad, &y, "SWAP");
    char sused_h[32], stotal_h[32], sfree_h[32], scache_h[32];
    human_kb(d->swap_used, sused_h, sizeof(sused_h));
    human_kb(d->swap_total, stotal_h, sizeof(stotal_h));
    human_kb(d->swap_free, sfree_h, sizeof(sfree_h));
    human_kb(d->swap_cached, scache_h, sizeof(scache_h));

    if (d->swap_total == 0) {
        mvwprintw(pad, y, 4, "(no swap configured)");
        y += 2;
    } else {
        draw_bar(pad, y, 4, 40, d->swap_pct); y += 2;
        kv_row(pad, y, 4, "Total:", stotal_h, 14);
        kv_row(pad, y, 34, "Used:", sused_h, 14); y++;
        kv_row(pad, y, 4, "Free:", sfree_h, 14);
        kv_row(pad, y, 34, "Cached:", scache_h, 14); y += 2;
    }

    /* ---- Load average ---- */
    double l1, l5, l15;
    if (loadavg_read(&l1, &l5, &l15) == 0) {
        section_title(pad, &y, "LOAD AVERAGE");
        mvwprintw(pad, y, 4, "1m: %.2f   5m: %.2f   15m: %.2f", l1, l5, l15);
        y += 2;
    }

    /* ---- Active / Inactive breakdown ---- */
    section_title(pad, &y, "ACTIVE / INACTIVE");
    const char *ai_fields[][2] = {
        {"Active", "Inactive"},
        {"Active(anon)", "Inactive(anon)"},
        {"Active(file)", "Inactive(file)"},
    };
    for (size_t i = 0; i < sizeof(ai_fields)/sizeof(ai_fields[0]); i++) {
        char h1[32], h2[32];
        human_kb(mi_get(mi, ai_fields[i][0]), h1, sizeof(h1));
        human_kb(mi_get(mi, ai_fields[i][1]), h2, sizeof(h2));
        kv_row(pad, y, 4, ai_fields[i][0], h1, 14);
        kv_row(pad, y, 34, ai_fields[i][1], h2, 14);
        y++;
    }
    y++;

    /* ---- Kernel / reclaimable ---- */
    section_title(pad, &y, "KERNEL & RECLAIMABLE");
    const char *kfields[] = {"Slab", "SReclaimable", "SUnreclaim", "KernelStack",
                              "PageTables", "VmallocUsed", NULL};
    int col = 0;
    for (int i = 0; kfields[i]; i++) {
        char h[32];
        human_kb(mi_get(mi, kfields[i]), h, sizeof(h));
        kv_row(pad, y, 4 + col * 30, kfields[i], h, 14);
        col++;
        if (col == 2) { col = 0; y++; }
    }
    if (col != 0) y++;
    y++;

    /* ---- Writeback / dirty / mapped ---- */
    section_title(pad, &y, "PAGE CACHE ACTIVITY");
    const char *wfields[] = {"Dirty", "Writeback", "AnonPages", "Mapped", NULL};
    col = 0;
    for (int i = 0; wfields[i]; i++) {
        char h[32];
        human_kb(mi_get(mi, wfields[i]), h, sizeof(h));
        kv_row(pad, y, 4 + col * 30, wfields[i], h, 14);
        col++;
        if (col == 2) { col = 0; y++; }
    }
    if (col != 0) y++;
    y++;

    /* ---- Commit & vmalloc ---- */
    section_title(pad, &y, "COMMIT & VIRTUAL ADDRESS SPACE");
    const char *cfields[] = {"CommitLimit", "Committed_AS", "VmallocTotal", NULL};
    col = 0;
    for (int i = 0; cfields[i]; i++) {
        char h[32];
        human_kb(mi_get(mi, cfields[i]), h, sizeof(h));
        kv_row(pad, y, 4 + col * 30, cfields[i], h, 14);
        col++;
        if (col == 2) { col = 0; y++; }
    }
    if (col != 0) y++;
    y++;

    /* ---- Huge pages ---- */
    if (mi_has(mi, "HugePages_Total")) {
        section_title(pad, &y, "HUGE PAGES");
        char hsz[32];
        human_kb(mi_get(mi, "Hugepagesize"), hsz, sizeof(hsz));
        mvwprintw(pad, y, 4, "Total: %-8llu Free: %-8llu Rsvd: %-8llu Surplus: %-8llu Size: %s",
                  mi_get(mi, "HugePages_Total"), mi_get(mi, "HugePages_Free"),
                  mi_get(mi, "HugePages_Rsvd"), mi_get(mi, "HugePages_Surp"), hsz);
        y += 2;
    }

    /* ---- Everything else, raw dump ---- */
    section_title(pad, &y, "ALL /proc/meminfo FIELDS");
    col = 0;
    for (int i = 0; i < mi->count; i++) {
        char h[32];
        human_kb(mi->entries[i].kb, h, sizeof(h));
        kv_row(pad, y, 4 + col * 36, mi->entries[i].key, h, 18);
        col++;
        if (col == 2) { col = 0; y++; }
    }
    if (col != 0) y++;
    y++;

    return y; /* total content height used */
}

/* Header/footer are drawn directly onto stdscr (row 0 and row LINES-1)
 * rather than via per-frame subwin()/delwin(), which is unnecessary churn
 * and was one more thing that could go sideways on quirky terminals. */
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
    snprintf(mid, sizeof(mid), "System Memory Monitor - %s", hostname);
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
}

static void draw_footer(int rows, int cols, int logging_enabled, int secs_to_refresh)
{
    int cp = has_colors() ? CP_HEADER : 0;
    attr_t extra = has_colors() ? A_BOLD : (A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(cp) | extra);
    char msg[256];
    snprintf(msg, sizeof(msg),
             " q:quit  Up/Dn or j/k:scroll  PgUp/PgDn  g/G:top/bottom  r:refresh now   log:%s   next refresh in %ds ",
             logging_enabled ? "on" : "off", secs_to_refresh);
    mvprintw(rows - 1, 0, "%-*.*s", cols, cols, msg);
    attroff(COLOR_PAIR(cp) | extra);
}

static int run_interactive(long interval_secs, int logging_enabled, const char *hostname,
                            const char *kernel)
{
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr,
            "%s: stdout is not a terminal; the interactive display needs a real tty.\n"
            "Use --once for a single snapshot or --daemon for headless logging instead.\n",
            PROGNAME);
        return 1;
    }

    initscr();
    if (has_colors()) init_colors();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(200); /* ms poll interval so we can update countdown + handle keys */
    /* Some terminals (notably several found under Windows/WSL front-ends)
     * misreport or only partially support cursor-addressing capabilities,
     * which makes ncurses' normal incremental-diff redraw either leave
     * stale content on screen or scroll instead of repainting in place.
     * Forcing a full repaint every cycle costs a little more output but
     * is far more likely to render correctly everywhere. */
    clearok(stdscr, TRUE);

    int scroll_y = 0;
    WINDOW *pad = newpad(PAD_LINES, PAD_COLS);
    if (!pad) { endwin(); fprintf(stderr, "%s: failed to allocate display pad\n", PROGNAME); return 1; }

    Meminfo mi; Derived d;
    meminfo_read(&mi); derive(&mi, &d);
    int content_h = build_pad(pad, &mi, &d, hostname, kernel);

    if (logging_enabled) {
        if (log_open_for_today() == 0)
            write_snapshot(g_logfile, &mi, &d, hostname, kernel);
    }

    struct timespec last_refresh, now;
    clock_gettime(CLOCK_MONOTONIC, &last_refresh);

    int running = 1;
    while (running) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        (void)cols;
        int content_rows = rows - 2;
        if (content_rows < 1) content_rows = 1;

        int max_scroll = content_h - content_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_y > max_scroll) scroll_y = max_scroll;
        if (scroll_y < 0) scroll_y = 0;

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - last_refresh.tv_sec) +
                          (now.tv_nsec - last_refresh.tv_nsec) / 1e9;
        int secs_to_refresh = (int)(interval_secs - elapsed);
        if (secs_to_refresh < 0) secs_to_refresh = 0;

        erase();
        draw_header(cols, hostname, interval_secs);
        draw_footer(rows, cols, logging_enabled, secs_to_refresh);
        wnoutrefresh(stdscr);
        pnoutrefresh(pad, scroll_y, 0, 1, 0, rows - 2, cols - 1);
        doupdate();

        int ch = getch();
        switch (ch) {
            case 'q': case 'Q': running = 0; break;
            case KEY_UP: case 'k': if (scroll_y > 0) scroll_y--; break;
            case KEY_DOWN: case 'j': if (scroll_y < max_scroll) scroll_y++; break;
            case KEY_NPAGE: scroll_y += content_rows; break;
            case KEY_PPAGE: scroll_y -= content_rows; break;
            case 'g': scroll_y = 0; break;
            case 'G': scroll_y = max_scroll; break;
            case KEY_RESIZE: resizeterm(0, 0); clearok(stdscr, TRUE); break;
            case 'r': case 'R': elapsed = interval_secs + 1; break;
            default: break;
        }

        if (elapsed >= interval_secs) {
            meminfo_read(&mi);
            derive(&mi, &d);
            content_h = build_pad(pad, &mi, &d, hostname, kernel);
            if (logging_enabled) {
                if (log_open_for_today() == 0)
                    write_snapshot(g_logfile, &mi, &d, hostname, kernel);
            }
            clock_gettime(CLOCK_MONOTONIC, &last_refresh);
        }
    }

    delwin(pad);
    endwin();
    return 0;
}

#endif /* USE_NCURSES */

/* ---------------------------------------------------------------------
 * Daemon mode (no ncurses) - for systemd
 * ------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop = 0;
static void handle_signal(int sig) { (void)sig; g_stop = 1; }

static int run_daemon(long interval_secs, const char *hostname, const char *kernel)
{
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    if (g_log_retention_days > 0)
        fprintf(stderr, "%s: starting daemon mode, interval=%lds, logdir=%s, log-retention=%dd\n",
                PROGNAME, interval_secs, g_logdir, g_log_retention_days);
    else
        fprintf(stderr, "%s: starting daemon mode, interval=%lds, logdir=%s, log-retention=disabled\n",
                PROGNAME, interval_secs, g_logdir);
    prune_old_logs(g_logdir, g_log_retention_days);

    while (!g_stop) {
        Meminfo mi; Derived d;
        if (meminfo_read(&mi) == 0) {
            derive(&mi, &d);
            if (log_open_for_today() == 0)
                write_snapshot(g_logfile, &mi, &d, hostname, kernel);
        }
        /* Sleep in small chunks so SIGTERM is handled promptly */
        long remaining = interval_secs;
        while (remaining > 0 && !g_stop) {
            long chunk = remaining > 1 ? 1 : remaining;
            sleep((unsigned int)chunk);
            remaining -= chunk;
        }
    }
    if (g_logfile) fclose(g_logfile);
    fprintf(stderr, "%s: stopping (signal received)\n", PROGNAME);
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
"Options:\n"
"  -i, --interval INTERVAL   Refresh/log interval. Accepts plain seconds or a\n"
"                             suffix: s (sec), m (min), h (hour), d (day).\n"
"                             Examples: -i 5, -i 5s, -i 5m, -i 1h. Default: 5s\n"
"  -d, --daemon               Headless mode: no TUI, just logs snapshots on\n"
"                             each interval. Intended for systemd. Handles\n"
"                             SIGTERM/SIGINT for clean shutdown.\n"
"  -o, --once                 Print a single snapshot to stdout and exit\n"
"                             (no ncurses, no loop, no log file written).\n"
"  -p, --plain                 Interactive watch mode without ncurses: plain\n"
"                             ANSI clear-screen/redraw instead of full cursor\n"
"                             addressing. Try this if the default ncurses\n"
"                             display looks garbled or doesn't redraw in\n"
"                             place on your terminal (common under some\n"
"                             Windows/WSL terminal front-ends).\n"
"  -l, --log-dir DIR          Directory for daily log files (named\n"
"                             YYYY-MM-DD.log). Default: %s\n"
"                             (falls back to ~/%s if not writable).\n"
"  -r, --log-retention-days N  Auto-delete log files older than N days.\n"
"                             N=0 disables auto-delete. Default: %d\n"
"                             (also settable via MEMMON_LOG_RETENTION_DAYS).\n"
"  -n, --no-log                Disable logging in interactive mode.\n"
"  -h, --help                  Show this help and exit.\n"
"  -v, --version                Show version and exit.\n\n"
"Interactive keys:  q quit | up/down or j/k scroll | PgUp/PgDn | g/G top/bottom\n"
"                    r refresh now\n\n"
"Examples:\n"
"  %s                          Interactive TUI, refresh every 5 seconds\n"
"  %s -i 5m                    Interactive TUI, refresh every 5 minutes\n"
"  %s --once                   One-shot plain text report\n"
"  %s --daemon -i 5m           Headless logger for systemd (5 minute interval)\n"
"  %s --daemon -i 5m -r 14     Same, but only keep 14 days of logs\n",
    PROGNAME, VERSION, PROGNAME, DEFAULT_LOGDIR, FALLBACK_LOGDIR,
    DEFAULT_LOG_RETENTION_DAYS,
    PROGNAME, PROGNAME, PROGNAME, PROGNAME, PROGNAME);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    long interval_secs = 5;
    int daemon_mode = 0;
    int once_mode = 0;
    int plain_mode = 0;
    int no_log = 0;
    const char *logdir_arg = NULL;
    char errbuf[128];

    /* MEMMON_LOG_RETENTION_DAYS lets systemd's EnvironmentFile=/etc/memmon/memmon.conf
     * configure retention without editing the unit or passing -r explicitly;
     * -r/--log-retention-days on the command line still wins over it. */
    const char *retention_env = getenv("MEMMON_LOG_RETENTION_DAYS");
    if (retention_env && *retention_env) {
        char *end;
        long v = strtol(retention_env, &end, 10);
        if (end != retention_env && *end == '\0' && v >= 0)
            g_log_retention_days = (int)v;
    }

    static struct { const char *shrt; const char *lng; } opts; (void)opts;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((strcmp(a, "-i") == 0 || strcmp(a, "--interval") == 0) && i + 1 < argc) {
            long v = parse_interval(argv[++i], errbuf, sizeof(errbuf));
            if (v < 0) { fprintf(stderr, "%s: %s\n", PROGNAME, errbuf); return 2; }
            interval_secs = v;
        } else if (strncmp(a, "--interval=", 11) == 0) {
            long v = parse_interval(a + 11, errbuf, sizeof(errbuf));
            if (v < 0) { fprintf(stderr, "%s: %s\n", PROGNAME, errbuf); return 2; }
            interval_secs = v;
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--once") == 0) {
            once_mode = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--plain") == 0) {
            plain_mode = 1;
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--no-log") == 0) {
            no_log = 1;
        } else if ((strcmp(a, "-l") == 0 || strcmp(a, "--log-dir") == 0) && i + 1 < argc) {
            logdir_arg = argv[++i];
        } else if (strncmp(a, "--log-dir=", 10) == 0) {
            logdir_arg = a + 10;
        } else if ((strcmp(a, "-r") == 0 || strcmp(a, "--log-retention-days") == 0) && i + 1 < argc) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 0) {
                fprintf(stderr, "%s: invalid log-retention-days '%s'\n", PROGNAME, argv[i]);
                return 2;
            }
            g_log_retention_days = (int)v;
        } else if (strncmp(a, "--log-retention-days=", 21) == 0) {
            char *end;
            long v = strtol(a + 21, &end, 10);
            if (end == a + 21 || *end != '\0' || v < 0) {
                fprintf(stderr, "%s: invalid log-retention-days '%s'\n", PROGNAME, a + 21);
                return 2;
            }
            g_log_retention_days = (int)v;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("%s %s\n", PROGNAME, VERSION);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown option '%s' (see --help)\n", PROGNAME, a);
            return 2;
        }
    }

    char hostname[256] = "unknown";
    gethostname(hostname, sizeof(hostname) - 1);

    struct utsname uts;
    char kernel[128] = "unknown";
    if (uname(&uts) == 0)
        snprintf(kernel, sizeof(kernel), "%s", uts.release);

    resolve_logdir(logdir_arg, g_logdir, sizeof(g_logdir));
    if (ensure_dir(g_logdir) != 0 && daemon_mode) {
        fprintf(stderr, "%s: warning: could not create log dir '%s': %s\n",
                PROGNAME, g_logdir, strerror(errno));
    }

    if (once_mode) {
        Meminfo mi; Derived d;
        if (meminfo_read(&mi) != 0) {
            fprintf(stderr, "%s: failed to read %s: %s\n", PROGNAME, MEMINFO_PATH, strerror(errno));
            return 1;
        }
        derive(&mi, &d);
        print_once(&mi, &d, hostname, kernel);
        return 0;
    }

    if (daemon_mode) {
        return run_daemon(interval_secs, hostname, kernel);
    }

    if (plain_mode) {
        return run_plain_watch(interval_secs, !no_log, hostname, kernel);
    }

#ifdef USE_NCURSES
    return run_interactive(interval_secs, !no_log, hostname, kernel);
#else
    fprintf(stderr,
        "%s: built without ncurses support; use --plain, --once, or --daemon.\n",
        PROGNAME);
    return run_plain_watch(interval_secs, !no_log, hostname, kernel);
#endif
}
