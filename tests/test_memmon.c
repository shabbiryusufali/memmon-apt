/*
 * Unit tests for memmon. The source is included directly (built without
 * main() and without ncurses) so its static helpers can be exercised.
 *
 * Run with `make check`. Fixture /proc and /sys trees live in
 * tests/fixtures/root and are selected via g_sysroot.
 */
#define MEMMON_NO_MAIN
#include "../src/memmon.c"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(a, b) do { \
    g_checks++; \
    const char *a_ = (a), *b_ = (b); \
    if (strcmp(a_, b_) != 0) { g_failures++; \
        fprintf(stderr, "%s:%d: CHECK_STR failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_); } \
} while (0)

#define CHECK_NEAR(a, b) do { \
    g_checks++; \
    double a_ = (a), b_ = (b); \
    if (a_ - b_ > 0.001 || b_ - a_ > 0.001) { g_failures++; \
        fprintf(stderr, "%s:%d: CHECK_NEAR failed: %f != %f\n", __FILE__, __LINE__, a_, b_); } \
} while (0)

static void use_fixture_root(void)
{
    const char *dir = getenv("MEMMON_TEST_FIXTURES");
    snprintf(g_sysroot, sizeof(g_sysroot), "%s/root", dir ? dir : "tests/fixtures");
}

static void test_parse_interval(void)
{
    char err[128];
    CHECK(parse_interval("5", err, sizeof(err)) == 5);
    CHECK(parse_interval("5s", err, sizeof(err)) == 5);
    CHECK(parse_interval("5m", err, sizeof(err)) == 300);
    CHECK(parse_interval("1h", err, sizeof(err)) == 3600);
    CHECK(parse_interval("2d", err, sizeof(err)) == 172800);
    CHECK(parse_interval("1.5m", err, sizeof(err)) == 90);
    CHECK(parse_interval("0.1", err, sizeof(err)) == 1);      /* clamped to 1s */
    CHECK(parse_interval("", err, sizeof(err)) == -1);
    CHECK(parse_interval("abc", err, sizeof(err)) == -1);
    CHECK(parse_interval("5x", err, sizeof(err)) == -1);
    CHECK(strstr(err, "suffix") != NULL);
    CHECK(parse_interval("5mm", err, sizeof(err)) == -1);
    CHECK(parse_interval("-5", err, sizeof(err)) == -1);
    CHECK(parse_interval("400d", err, sizeof(err)) == -1);
}

static void test_format_interval(void)
{
    char b[32];
    format_interval(5, b, sizeof(b));      CHECK_STR(b, "5s");
    format_interval(300, b, sizeof(b));    CHECK_STR(b, "5m");
    format_interval(3600, b, sizeof(b));   CHECK_STR(b, "1h");
    format_interval(86400, b, sizeof(b));  CHECK_STR(b, "1d");
    format_interval(90, b, sizeof(b));     CHECK_STR(b, "90s");
}

static void test_human_kb(void)
{
    char b[32];
    double kb;
    g_units = UNITS_AUTO;
    human_kb(512, b, sizeof(b));           CHECK_STR(b, "512 KiB");
    human_kb(1024, b, sizeof(b));          CHECK_STR(b, "1.00 MiB");
    human_kb(1536 * 1024, b, sizeof(b));   CHECK_STR(b, "1.50 GiB");
    g_units = UNITS_MIB;
    human_kb(1536 * 1024, b, sizeof(b));   CHECK_STR(b, "1536.00 MiB");
    g_units = UNITS_KIB;
    human_kb(1536 * 1024, b, sizeof(b));   CHECK_STR(b, "1572864 KiB");
    g_units = UNITS_GIB;
    human_kb(512 * 1024, b, sizeof(b));    CHECK_STR(b, "0.50 GiB");
    g_units = UNITS_AUTO;

    CHECK(parse_human_kb("292.49 MiB", &kb) == 0);
    CHECK_NEAR(kb, 292.49 * 1024);
    CHECK(parse_human_kb("3 KiB", &kb) == 0);
    CHECK_NEAR(kb, 3);
    CHECK(parse_human_kb("1.00 GiB", &kb) == 0);
    CHECK_NEAR(kb, 1024.0 * 1024);
    CHECK(parse_human_kb("12 bananas", &kb) == -1);
}

static void test_log_filenames(void)
{
    struct tm tmv;
    int gz = -1;
    CHECK(parse_log_filename("2026-09-16.log", &tmv, &gz) == 0);
    CHECK(gz == 0);
    CHECK(tmv.tm_year == 126 && tmv.tm_mon == 8 && tmv.tm_mday == 16);
    CHECK(parse_log_filename("2026-09-16.jsonl", NULL, &gz) == 0 && gz == 0);
    CHECK(parse_log_filename("2026-09-16.csv.gz", NULL, &gz) == 0 && gz == 1);
    CHECK(parse_log_filename("2026-09-16.log.gz", NULL, &gz) == 0 && gz == 1);
    CHECK(parse_log_filename("2026-09-16.log.bak", NULL, NULL) == -1);
    CHECK(parse_log_filename("2026-09-16.log.gz.tmp", NULL, NULL) == -1);
    CHECK(parse_log_filename("2026-09-16.txt", NULL, NULL) == -1);
    CHECK(parse_log_filename("notes.log", NULL, NULL) == -1);
    CHECK(parse_log_filename("2026-13-01.log", NULL, NULL) == -1);
    CHECK(parse_log_filename("26-09-16.log", NULL, NULL) == -1);
}

static void test_config_parsing(void)
{
    Options o;
    options_default(&o);
    char err[160];
    char line[256], *k, *v;

    snprintf(line, sizeof(line), "  # a comment\n");
    CHECK(config_parse_line(line, &k, &v) == 0);
    snprintf(line, sizeof(line), "\n");
    CHECK(config_parse_line(line, &k, &v) == 0);
    snprintf(line, sizeof(line), "MEMMON_INTERVAL=5m\n");
    CHECK(config_parse_line(line, &k, &v) == 1);
    CHECK_STR(k, "MEMMON_INTERVAL");
    CHECK_STR(v, "5m");
    snprintf(line, sizeof(line), "export MEMMON_ALERT_CMD=\"logger -t memmon 'hi #1'\"\n");
    CHECK(config_parse_line(line, &k, &v) == 1);
    CHECK_STR(k, "MEMMON_ALERT_CMD");
    CHECK_STR(v, "logger -t memmon 'hi #1'");
    snprintf(line, sizeof(line), "no equals sign\n");
    CHECK(config_parse_line(line, &k, &v) == -1);

    CHECK(apply_setting(&o, "MEMMON_INTERVAL", "10m", SRC_CONFIG, err, sizeof(err)) == 0);
    CHECK(o.interval_secs == 600);
    CHECK(apply_setting(&o, "MEMMON_LOG_RETENTION_DAYS", "0", SRC_CONFIG, err, sizeof(err)) == 0);
    CHECK(o.retention_days == 0);
    CHECK(apply_setting(&o, "MEMMON_LOG_RETENTION_DAYS", "-3", SRC_CONFIG, err, sizeof(err)) == -1);
    CHECK(apply_setting(&o, "MEMMON_LOG_FORMAT", "JSON", SRC_CONFIG, err, sizeof(err)) == 0);
    CHECK(o.log_format == FMT_JSON);
    CHECK(apply_setting(&o, "MEMMON_LOG_FORMAT", "xml", SRC_CONFIG, err, sizeof(err)) == -1);
    CHECK(apply_setting(&o, "MEMMON_LOG_COMPRESS", "no", SRC_CONFIG, err, sizeof(err)) == 0);
    CHECK(o.compress_logs == 0);
    CHECK(apply_setting(&o, "MEMMON_LOG_COMPRESS", "maybe", SRC_CONFIG, err, sizeof(err)) == -1);
    CHECK(apply_setting(&o, "MEMMON_WARN_PERCENT", "70%", SRC_CONFIG, err, sizeof(err)) == 0);
    CHECK_NEAR(o.warn_pct, 70);
    CHECK(apply_setting(&o, "MEMMON_CRIT_PERCENT", "150", SRC_CONFIG, err, sizeof(err)) == -1);
    CHECK(apply_setting(&o, "MEMMON_TOP_PROCESSES", "51", SRC_CONFIG, err, sizeof(err)) == -1);
    CHECK(apply_setting(&o, "MEMMON_UNITS", "m", SRC_CONFIG, err, sizeof(err)) == 0);
    CHECK(o.units == UNITS_MIB);
    CHECK(apply_setting(&o, "MEMMON_LOG_DIR", "/srv/logs", SRC_ENV, err, sizeof(err)) == 0);
    CHECK(o.logdir_src == SRC_ENV);
    CHECK_STR(o.logdir, "/srv/logs");
    CHECK(apply_setting(&o, "MEMMON_BOGUS", "1", SRC_CONFIG, err, sizeof(err)) == 1);
}

static void test_alert_levels(void)
{
    CHECK(alert_level_for(10, 60, 85, ALERT_OK) == ALERT_OK);
    CHECK(alert_level_for(60, 60, 85, ALERT_OK) == ALERT_WARN);
    CHECK(alert_level_for(90, 60, 85, ALERT_OK) == ALERT_CRIT);
    /* hysteresis: stays critical until 5 points below the threshold */
    CHECK(alert_level_for(82, 60, 85, ALERT_CRIT) == ALERT_CRIT);
    CHECK(alert_level_for(79, 60, 85, ALERT_CRIT) == ALERT_WARN);
    CHECK(alert_level_for(57, 60, 85, ALERT_WARN) == ALERT_WARN);
    CHECK(alert_level_for(54, 60, 85, ALERT_WARN) == ALERT_OK);
    CHECK(alert_level_for(10, 60, 85, ALERT_CRIT) == ALERT_OK);
}

static void test_scheduling(void)
{
    /* build a local-time 12:03:17 and expect alignment to 12:05:00 */
    struct tm tmv;
    time_t now = time(NULL);
    localtime_r(&now, &tmv);
    tmv.tm_hour = 12; tmv.tm_min = 3; tmv.tm_sec = 17; tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    time_t n = next_aligned(t, 300);
    struct tm out;
    localtime_r(&n, &out);
    CHECK(out.tm_hour == 12 && out.tm_min == 5 && out.tm_sec == 0);

    /* exactly on a boundary moves to the next one */
    n = next_aligned(n, 300);
    localtime_r(&n, &out);
    CHECK(out.tm_hour == 12 && out.tm_min == 10 && out.tm_sec == 0);

    /* intervals that don't divide a day aren't aligned */
    CHECK(next_aligned(t, 420) == t + 420);

    /* unaligned schedule doesn't drift: due times advance by exactly
     * the interval regardless of when we woke up */
    CHECK(next_due(1000, 1003, 60, 0) == 1060);
    /* ...but skips ahead after falling far behind */
    CHECK(next_due(1000, 5000, 60, 0) == 5060);
}

static void test_meminfo_fixture(void)
{
    Meminfo mi;
    CHECK(meminfo_read(&mi) == 0);
    CHECK(mi_get(&mi, "MemTotal") == 8000000);
    CHECK(mi_has(&mi, "HugePages_Total"));
    const MeminfoEntry *e = mi_find(&mi, "HugePages_Total");
    CHECK(e && !e->is_kb && e->val == 16);
    e = mi_find(&mi, "MemFree");
    CHECK(e && e->is_kb);

    Derived d;
    derive(&mi, &d);
    CHECK(d.used == 6000000);
    CHECK_NEAR(d.mem_pct, 75.0);
    CHECK(d.swap_used == 500000);
    CHECK_NEAR(d.swap_pct, 25.0);
}

static void test_sources_fixture(void)
{
    Psi p;
    CHECK(psi_read(&p) == 0);
    CHECK(p.some_valid && p.full_valid);
    CHECK_NEAR(p.some10, 12.5);
    CHECK_NEAR(p.full300, 0.5);
    CHECK(p.some_total == 123456);

    Vmstat v;
    CHECK(vmstat_read(&v) == 0);
    CHECK(v.has_oom && v.oom_kill == 3);
    CHECK(v.pswpin == 100 && v.pswpout == 200 && v.pgmajfault == 4242);

    Zram z[MAX_ZRAM];
    int nz = zram_read(z, MAX_ZRAM);
    CHECK(nz == 1); /* zram1 has disksize 0 and is skipped */
    CHECK_STR(z[0].name, "zram0");
    CHECK_STR(z[0].algo, "zstd");
    CHECK(z[0].orig_kb == 307200 && z[0].compr_kb == 102400);

    Proc top[MAX_TOP];
    int nt = top_read(top, 5);
    CHECK(nt == 2); /* the kernel thread (no VmRSS) is skipped */
    CHECK(top[0].pid == 42);
    CHECK_STR(top[0].name, "big app");
    CHECK(top[0].rss_kb == 2000000 && top[0].swap_kb == 300000);
    CHECK(top[1].pid == 1);
    CHECK(top_read(top, 1) == 1 && top[0].pid == 42);
    CHECK(top_read(top, 0) == 0);

    Cgroup cg;
    /* auto-detect walks up from web.service (no limit) to app.slice */
    CHECK(cgroup_read(&cg, NULL) == 0);
    CHECK(cg.valid);
    CHECK_STR(cg.path, "/app.slice");
    CHECK(cg.has_max && cg.max_kb == 1048576);
    CHECK(cg.current_kb == 524288);
    CHECK(cg.has_swap && cg.swap_current_kb == 1024 && !cg.has_swap_max);
    CHECK(cg.has_oom && cg.oom_kill == 2);
    /* explicit path is used even without a limit */
    CHECK(cgroup_read(&cg, "/sys/fs/cgroup/app.slice/web.service") == 0);
    CHECK_STR(cg.path, "/app.slice/web.service");
    CHECK(!cg.has_max && cg.current_kb == 262144);
    CHECK(cgroup_read(&cg, "does/not/exist") == -1);
}

static int report_has_section(const Report *r, const char *title)
{
    for (int i = 0; i < r->n; i++)
        if (!strcmp(r->sec[i].title, title)) return 1;
    return 0;
}

static void test_report_model(void)
{
    Options o;
    options_default(&o);
    static Snapshot s;
    CHECK(snapshot_collect(&s, &o) == 0);
    History h;
    memset(&h, 0, sizeof(h));
    hist_push(&h, 10, 0);
    hist_push(&h, 50, 0);
    hist_push(&h, 100, 0);

    build_report(&g_report, &s, &h);
    CHECK(report_has_section(&g_report, "MEMORY"));
    CHECK(report_has_section(&g_report, "SWAP"));
    CHECK(report_has_section(&g_report, "TREND"));
    CHECK(report_has_section(&g_report, "MEMORY PRESSURE (PSI)"));
    CHECK(report_has_section(&g_report, "MEMORY EVENTS"));
    CHECK(report_has_section(&g_report, "CGROUP"));
    CHECK(report_has_section(&g_report, "ZRAM"));
    CHECK(report_has_section(&g_report, "TOP PROCESSES (by RSS)"));
    CHECK(report_has_section(&g_report, "HUGE PAGES"));

    /* HugePages_* are counts, not sizes, in every section incl. the raw dump */
    for (int i = 0; i < g_report.n; i++) {
        const Section *sec = &g_report.sec[i];
        for (int k = 0; k < sec->nrows; k++) {
            if (!strcmp(sec->rows[k].label, "HugePages_Total")) CHECK_STR(sec->rows[k].value, "16");
        }
    }

    build_report(&g_report, &s, NULL);
    CHECK(!report_has_section(&g_report, "TREND"));

    o.top_n = 0;
    CHECK(snapshot_collect(&s, &o) == 0);
    build_report(&g_report, &s, NULL);
    CHECK(!report_has_section(&g_report, "TOP PROCESSES (by RSS)"));
}

static void test_sparkline(void)
{
    History h;
    memset(&h, 0, sizeof(h));
    char out[HIST_LEN * 4 + 1];
    g_utf8 = 0;
    hist_push(&h, 0, 0);
    hist_push(&h, 100, 0);
    sparkline(&h, 0, out, sizeof(out));
    CHECK_STR(out, " #");
    for (int i = 0; i < HIST_LEN + 5; i++) hist_push(&h, 50, 0);
    sparkline(&h, 0, out, sizeof(out));
    CHECK(strlen(out) == HIST_LEN);
    g_utf8 = 1;
    sparkline(&h, 0, out, sizeof(out));
    CHECK(strlen(out) == HIST_LEN * 3); /* each block glyph is 3 bytes of UTF-8 */
    g_utf8 = 0;
}

static void test_json_escape(void)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    json_str(f, "a\"b\\c\nd\x01");
    fclose(f);
    CHECK_STR(buf, "\"a\\\"b\\\\c\\nd\\u0001\"");
    free(buf);
}

static void test_report_parsers(void)
{
    Sample s;
    parse_json_line("{\"timestamp\":\"2026-09-20T13:05:00+0200\",\"mem_pct\":42.50,"
                    "\"mem_used_kb\":1000,\"swap_total_kb\":0,\"swap_pct\":0.00,"
                    "\"psi_some_avg10\":null,\"oom_kill\":7}", &s);
    CHECK(s.valid);
    CHECK_STR(s.ts, "2026-09-20 13:05:00");
    CHECK_NEAR(s.mem_pct, 42.5);
    CHECK(s.has_used && !s.has_swap && !s.has_psi);
    CHECK(s.has_oom && s.oom == 7);

    ReportStats st;
    memset(&st, 0, sizeof(st));
    Sample a = { .valid = 1, .ts = "2026-09-20 00:00:00", .mem_pct = 10, .has_oom = 1, .oom = 5 };
    Sample b = { .valid = 1, .ts = "2026-09-20 00:05:00", .mem_pct = 30, .has_oom = 1, .oom = 7 };
    Sample c = { .valid = 1, .ts = "2026-09-20 00:10:00", .mem_pct = 20, .has_oom = 1, .oom = 1 }; /* reboot */
    stats_add(&st, &a);
    stats_add(&st, &b);
    stats_add(&st, &c);
    CHECK(st.n == 3);
    CHECK_NEAR(st.mem_min, 10);
    CHECK_NEAR(st.mem_max, 30);
    CHECK_STR(st.mem_max_ts, "2026-09-20 00:05:00");
    CHECK_NEAR(st.mem_sum / (double)st.n, 20);
    CHECK(st.oom_kills == 3); /* +2, then +1 after the counter reset */
}

int main(void)
{
    setlocale(LC_ALL, "C");
    setenv("TZ", "UTC", 1);
    tzset();
    use_fixture_root();

    test_parse_interval();
    test_format_interval();
    test_human_kb();
    test_log_filenames();
    test_config_parsing();
    test_alert_levels();
    test_scheduling();
    test_meminfo_fixture();
    test_sources_fixture();
    test_report_model();
    test_sparkline();
    test_json_escape();
    test_report_parsers();

    if (g_failures) {
        fprintf(stderr, "%d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("unit tests: all %d checks passed\n", g_checks);
    return 0;
}
