#include "mq.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>
#include <time.h>
#include <sys/stat.h>

/* =========================================================================
 * Common argument scanning
 * ======================================================================= */

#define MQ_MAX_EPATS 32

typedef struct {
    Format  fmt;
    int     limit;       /* 0 = unlimited */
    int     preview;     /* chars for compact mode */
    bool    no_sort;
    char   *tmpl;        /* --template string, NULL if not set */
    bool    exit_status; /* --exit-status: exit 1 if no results */
    bool    word;        /* --word / -w: whole-word match in grep */
    char   *epats[MQ_MAX_EPATS]; /* -e patterns for grep */
    int     n_epats;
} Flags;

static Flags default_flags(Format default_fmt)
{
    Flags f = {0};
    f.fmt     = default_fmt;
    f.preview = 200;
    return f;
}

/*
 * Scan argv[0..argc) for known flags; compact non-flag entries to the front.
 * Returns new argc (non-flag count).
 */
static int scan_flags(int argc, char **argv, Flags *f)
{
    int out = 0;
    for (int i = 0; i < argc; i++) {
        char *a = argv[i];

        if ((!strcmp(a, "-f") || !strcmp(a, "--format")) && i + 1 < argc) {
            f->fmt = parse_format(argv[++i]);
        } else if (!strncmp(a, "--format=", 9)) {
            f->fmt = parse_format(a + 9);
        } else if ((!strcmp(a, "-n") || !strcmp(a, "--limit")) && i + 1 < argc) {
            f->limit = atoi(argv[++i]);
        } else if (!strncmp(a, "--limit=", 8)) {
            f->limit = atoi(a + 8);
        } else if (!strcmp(a, "--preview") && i + 1 < argc) {
            f->preview = atoi(argv[++i]);
        } else if (!strncmp(a, "--preview=", 10)) {
            f->preview = atoi(a + 10);
        } else if (!strcmp(a, "--no-sort")) {
            f->no_sort = true;
        } else if ((!strcmp(a, "-T") || !strcmp(a, "--template")) && i + 1 < argc) {
            f->tmpl = argv[++i];
        } else if (!strncmp(a, "--template=", 11)) {
            f->tmpl = a + 11;
        } else if (!strcmp(a, "--exit-status")) {
            f->exit_status = true;
        } else if (!strcmp(a, "--word") || !strcmp(a, "-w")) {
            f->word = true;
        } else if (!strcmp(a, "-e") && i + 1 < argc) {
            if (f->n_epats < MQ_MAX_EPATS) f->epats[f->n_epats++] = argv[++i];
        } else {
            argv[out++] = a;
        }
    }
    return out;
}

static void load_ra(RecordArray *ra, int n_files, char **files)
{
    load_files((const char **)files, n_files, ra);
}

/* Print a record using template if set, otherwise fmt_record.
 * Adds a blank separator line for human-readable formats (not TSV/JSONL). */
static void print_record(const Record *r, const Flags *f)
{
    if (f->tmpl) fmt_template(r, f->tmpl);
    else         fmt_record(r, f->fmt, f->preview);
    /* blank separator between records for human formats */
    if (f->fmt != FMT_TSV && f->fmt != FMT_JSONL && !f->tmpl)
        putchar('\n');
}

/* =========================================================================
 * Grep helpers — smart case + word-boundary wrapping
 * ======================================================================= */

/* smart case: all-lowercase → REG_ICASE, any uppercase → case-sensitive */
static int regex_cflags(const char *pat)
{
    int flags = REG_EXTENDED | REG_NOSUB;
    for (const char *p = pat; *p; p++)
        if (isupper((unsigned char)*p)) return flags;
    return flags | REG_ICASE;
}

/* wrap pattern with \b...\b for whole-word matching */
static char *word_wrap(const char *pat)
{
    size_t n = strlen(pat);
    char  *wp = malloc(n + 5);
    if (wp) sprintf(wp, "\\b%s\\b", pat);
    return wp;
}

/* =========================================================================
 * String comparator for qsort on char* arrays
 * ======================================================================= */

static int cmp_strp(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* =========================================================================
 * cmd_meta — show MINER_DUMP header per file
 * ======================================================================= */

static void usage_meta(void)
{
    fputs("usage: mq meta file|dir ...\n", stderr);
}

static int cmd_meta(int argc, char **argv)
{
    if (argc < 1) { usage_meta(); return 1; }
    MetaArray *ma = load_meta((const char **)argv, argc);
    for (int i = 0; i < ma->len; i++) {
        FileMeta *m = ma->data[i];
        printf("  %-55s %4drec  %s",
               m->fname,
               m->n_records,
               m->topic ? m->topic : "(no header)");
        if (m->status && strcmp(m->status, "COMPLETE") != 0)
            printf("  (%s)", m->status);
        if (m->depth)
            printf("  depth=%s", m->depth);
        putchar('\n');
    }
    return 0;
}

/* =========================================================================
 * cmd_top — top N records scored by query relevance
 * ======================================================================= */

typedef struct { int score; int idx; } Scored;

static int cmp_scored_desc(const void *a, const void *b)
{
    const Scored *sa = a, *sb = b;
    if (sb->score != sa->score) return sb->score - sa->score;
    return sa->idx - sb->idx;
}

static void usage_top(void)
{
    fputs("usage: mq top N query file ...\n", stderr);
}

static int cmd_top(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);

    if (argc < 3) { usage_top(); return 1; }
    int         n     = atoi(argv[0]);
    const char *query = argv[1];

    RecordArray *ra = ra_new();
    load_ra(ra, argc - 2, argv + 2);

    char *qcopy = strdup(query);
    const char *terms[64];
    int n_terms = 0;
    char *tok = strtok(qcopy, " \t");
    while (tok && n_terms < 64) { terms[n_terms++] = tok; tok = strtok(NULL, " \t"); }

    Scored *scored = malloc(ra->len * sizeof(Scored));
    for (int i = 0; i < ra->len; i++) {
        scored[i].score = score_record(ra->data[i], terms, n_terms);
        scored[i].idx   = i;
    }
    qsort(scored, ra->len, sizeof(Scored), cmp_scored_desc);

    int base_bonus[] = { 4, 2, 0, 1 };
    int seen = 0;
    for (int i = 0; i < ra->len && seen < n; i++) {
        Record *r = ra->data[scored[i].idx];
        int bonus = base_bonus[rel_order(r->relevance)];
        if (scored[i].score <= bonus) break;
        print_record(r, &f);
        seen++;
    }

    free(scored);
    free(qcopy);
    return f.exit_status && seen == 0 ? 1 : 0;
}

/* =========================================================================
 * cmd_filter — filter by expression (AND/OR supported)
 * ======================================================================= */

static void usage_filter(void)
{
    fputs("usage: mq filter [flags] 'expr' file ...\n"
          "  expr: key==val | key!=val | key~pat | EXPR AND EXPR | EXPR OR EXPR\n"
          "  e.g.: 'relevance == HIGH AND content ~ VLOOKUP'\n", stderr);
}

static int cmd_filter(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);
    if (argc < 2) { usage_filter(); return 1; }

    const char *expr = argv[0];
    RecordArray *ra  = ra_new();
    load_ra(ra, argc - 1, argv + 1);

    if (!f.no_sort) ra_sort_by_rel(ra);

    int seen = 0;
    for (int i = 0; i < ra->len; i++) {
        if (f.limit && seen >= f.limit) break;
        if (match_expr(ra->data[i], expr)) {
            print_record(ra->data[i], &f);
            seen++;
        }
    }
    return f.exit_status && seen == 0 ? 1 : 0;
}

/* =========================================================================
 * cmd_grep — regex search across content/title/keywords
 *   supports: -e pattern (multi, OR), --word (-w), smart case
 * ======================================================================= */

static void usage_grep(void)
{
    fputs("usage: mq grep [flags] [-e pat ...] [pattern] file ...\n"
          "  -e pat     additional pattern (OR semantics, repeatable)\n"
          "  -w/--word  whole-word match\n"
          "  smart case: all-lowercase = case-insensitive\n", stderr);
}

static int cmd_grep(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);

    /* collect patterns: -e flags + optional positional */
    if (f.n_epats == 0) {
        if (argc < 2) { usage_grep(); return 1; }
        f.epats[f.n_epats++] = argv[0];
        argv++; argc--;
    }
    if (argc < 1) { usage_grep(); return 1; }

    /* compile regexes */
    regex_t res[MQ_MAX_EPATS];
    char   *wps[MQ_MAX_EPATS];  /* word-wrapped pattern strings to free */
    for (int i = 0; i < f.n_epats; i++) {
        const char *pat = f.epats[i];
        wps[i] = f.word ? word_wrap(pat) : NULL;
        const char *use = wps[i] ? wps[i] : pat;
        if (regcomp(&res[i], use, regex_cflags(pat)) != 0) {
            fprintf(stderr, "mq: invalid pattern: %s\n", pat);
            for (int j = 0; j < i; j++) { regfree(&res[j]); free(wps[j]); }
            free(wps[i]);
            return 1;
        }
    }

    RecordArray *ra = ra_new();
    load_ra(ra, argc, argv);
    if (!f.no_sort) ra_sort_by_rel(ra);

    int seen = 0;
    for (int i = 0; i < ra->len; i++) {
        if (f.limit && seen >= f.limit) break;
        const Record *r = ra->data[i];
        const char *title    = r->title    ? r->title    : "";
        const char *content  = r->content  ? r->content  : "";
        const char *keywords = r->keywords ? r->keywords : "";

        bool matched = false;
        for (int p = 0; p < f.n_epats && !matched; p++) {
            if (regexec(&res[p], title,    0, NULL, 0) == 0 ||
                regexec(&res[p], content,  0, NULL, 0) == 0 ||
                regexec(&res[p], keywords, 0, NULL, 0) == 0)
                matched = true;
        }

        if (matched) {
            if (f.fmt != FMT_TITLE && f.fmt != FMT_JSONL && f.fmt != FMT_TSV && !f.tmpl)
                printf("# %s\n", r->file ? r->file : "");
            print_record(r, &f);
            seen++;
        }
    }

    for (int i = 0; i < f.n_epats; i++) { regfree(&res[i]); free(wps[i]); }
    return f.exit_status && seen == 0 ? 1 : 0;
}

/* =========================================================================
 * cmd_titles — list titles grouped by file
 * ======================================================================= */

static int cmd_titles(int argc, char **argv)
{
    Flags f = default_flags(FMT_TITLE);
    argc = scan_flags(argc, argv, &f);
    if (argc < 1) { fputs("usage: mq titles file ...\n", stderr); return 1; }

    RecordArray *ra = ra_new();
    load_ra(ra, argc, argv);

    const char *cur_file = NULL;
    int seen = 0;
    for (int i = 0; i < ra->len; i++) {
        if (f.limit && seen >= f.limit) break;
        const Record *r = ra->data[i];
        const char   *fn = r->file ? r->file : "";
        if (!cur_file || strcmp(fn, cur_file)) {
            if (cur_file) putchar('\n');
            printf("--- %s ---\n", fn);
            cur_file = fn;
        }
        printf("  %3s: ", r->id ? r->id : "?");
        fmt_record(r, FMT_TITLE, f.preview);
        seen++;
    }
    return 0;
}

/* =========================================================================
 * cmd_select — select specific fields
 * ======================================================================= */

static int cmd_select(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);
    if (argc < 2) { fputs("usage: mq select fields file ...\n", stderr); return 1; }

    char *flist = strdup(argv[0]);
    char *fields[32];
    int   n_fields = 0;
    char *tok = strtok(flist, ",");
    while (tok && n_fields < 32) {
        while (*tok == ' ') tok++;
        fields[n_fields++] = tok;
        tok = strtok(NULL, ",");
    }

    RecordArray *ra = ra_new();
    load_ra(ra, argc - 1, argv + 1);

    int seen = 0;
    for (int i = 0; i < ra->len; i++) {
        if (f.limit && seen >= f.limit) break;
        const Record *r = ra->data[i];

        if (f.fmt == FMT_TSV) {
            /* TSV: values tab-separated on one line */
            for (int j = 0; j < n_fields; j++) {
                if (j) putchar('\t');
                const char *v = get_field(r, fields[j]);
                if (v) {
                    for (const char *p = v; *p; p++)
                        putchar((*p == '\t' || *p == '\n') ? ' ' : *p);
                }
            }
            putchar('\n');
            seen++;
        } else {
            bool any = false;
            for (int j = 0; j < n_fields; j++) {
                const char *v = get_field(r, fields[j]);
                if (v) { printf("%s: %s\n", fields[j], v); any = true; }
            }
            if (any) { putchar('\n'); seen++; }
        }
    }
    free(flist);
    return 0;
}

/* =========================================================================
 * cmd_stats — per-file record count by relevance
 * ======================================================================= */

typedef struct {
    char *fname;
    int total, high, medium, low, unknown;
} FileStats;

static int cmd_stats(int argc, char **argv)
{
    if (argc < 1) { fputs("usage: mq stats file ...\n", stderr); return 1; }

    RecordArray *ra = ra_new();
    load_ra(ra, argc, argv);

    FileStats *stats = NULL;
    int n_stats = 0, cap_stats = 0;

    for (int i = 0; i < ra->len; i++) {
        const Record *r = ra->data[i];
        const char *fn = r->file ? r->file : "";

        FileStats *fs = NULL;
        for (int j = 0; j < n_stats; j++) {
            if (!strcmp(stats[j].fname, fn)) { fs = &stats[j]; break; }
        }
        if (!fs) {
            if (n_stats >= cap_stats) {
                cap_stats = cap_stats ? cap_stats * 2 : 16;
                stats = realloc(stats, cap_stats * sizeof(FileStats));
            }
            stats[n_stats] = (FileStats){ .fname = strdup(fn) };
            fs = &stats[n_stats++];
        }

        fs->total++;
        int ro = rel_order(r->relevance);
        if (ro == 0) fs->high++;
        else if (ro == 1) fs->medium++;
        else if (ro == 2) fs->low++;
        else fs->unknown++;
    }

    int total = 0;
    for (int i = 0; i < n_stats; i++) {
        FileStats *fs = &stats[i];
        total += fs->total;
        printf("  %s: %d records", fs->fname, fs->total);
        if (fs->high)    printf(", %d HIGH",    fs->high);
        if (fs->medium)  printf(", %d MEDIUM",  fs->medium);
        if (fs->low)     printf(", %d LOW",     fs->low);
        if (fs->unknown) printf(", %d UNKNOWN", fs->unknown);
        putchar('\n');
        free(fs->fname);
    }
    printf("\n  total: %d records across %d files\n", total, n_stats);
    free(stats);
    return 0;
}

/* =========================================================================
 * cmd_jsonl — export all records as JSONL
 * ======================================================================= */

static int cmd_jsonl(int argc, char **argv)
{
    if (argc < 1) { fputs("usage: mq jsonl file ...\n", stderr); return 1; }
    RecordArray *ra = ra_new();
    load_ra(ra, argc, argv);
    for (int i = 0; i < ra->len; i++)
        fmt_record(ra->data[i], FMT_JSONL, 0);
    return 0;
}

/* =========================================================================
 * cmd_count — count matching records
 * ======================================================================= */

static int cmd_count(int argc, char **argv)
{
    if (argc < 2) { fputs("usage: mq count 'expr' file ...\n", stderr); return 1; }
    const char *expr = argv[0];
    RecordArray *ra  = ra_new();
    load_ra(ra, argc - 1, argv + 1);
    int n = 0;
    for (int i = 0; i < ra->len; i++)
        if (match_expr(ra->data[i], expr)) n++;
    printf("%d\n", n);
    return 0;
}

/* =========================================================================
 * cmd_dump — dump all records
 * ======================================================================= */

static int cmd_dump(int argc, char **argv)
{
    Flags f = default_flags(FMT_COMPACT);
    argc = scan_flags(argc, argv, &f);
    if (argc < 1) { fputs("usage: mq dump [flags] file ...\n", stderr); return 1; }

    RecordArray *ra = ra_new();
    load_ra(ra, argc, argv);
    if (!f.no_sort) ra_sort_by_rel(ra);

    int seen = 0;
    for (int i = 0; i < ra->len; i++) {
        if (f.limit && seen >= f.limit) break;
        const Record *r = ra->data[i];
        if (f.fmt != FMT_TITLE && f.fmt != FMT_JSONL && f.fmt != FMT_TSV && !f.tmpl)
            printf("# %s\n", r->file ? r->file : "");
        print_record(r, &f);
        seen++;
    }
    return 0;
}

/* =========================================================================
 * cmd_show — display a specific record by file:id
 * ======================================================================= */

static int cmd_show(int argc, char **argv)
{
    Flags f = default_flags(FMT_FULL);
    argc = scan_flags(argc, argv, &f);
    if (argc < 1) {
        fputs("usage: mq show [flags] file:id ...\n"
              "  e.g.: mq show coverage-mined-data-analysts.miner:44\n", stderr);
        return 1;
    }

    int found_total = 0;
    for (int i = 0; i < argc; i++) {
        char *colon = strrchr(argv[i], ':');
        if (!colon || !*(colon + 1)) {
            fprintf(stderr, "mq show: expected file:id, got '%s'\n", argv[i]);
            continue;
        }
        char        *file = argv[i];
        const char  *id   = colon + 1;
        *colon = '\0';

        RecordArray  *ra    = ra_new();
        const char   *paths[1] = { file };
        load_files(paths, 1, ra);

        bool found = false;
        for (int j = 0; j < ra->len; j++) {
            if (ra->data[j]->id && !strcmp(ra->data[j]->id, id)) {
                print_record(ra->data[j], &f);
                found = true;
                found_total++;
                break;
            }
        }
        if (!found)
            fprintf(stderr, "mq show: record %s:%s not found\n", file, id);
    }
    return f.exit_status && found_total == 0 ? 1 : 0;
}

/* =========================================================================
 * cmd_sample — reservoir-sample N random records
 * ======================================================================= */

static int cmd_sample(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);
    if (argc < 2) {
        fputs("usage: mq sample N file ...\n", stderr);
        return 1;
    }

    int n = atoi(argv[0]);
    RecordArray *ra = ra_new();
    load_ra(ra, argc - 1, argv + 1);

    if (n <= 0 || ra->len == 0) return 0;
    if (n > ra->len) n = ra->len;

    /* Fisher-Yates partial shuffle on index array */
    int *idx = malloc(ra->len * sizeof(int));
    for (int i = 0; i < ra->len; i++) idx[i] = i;
    srand((unsigned)time(NULL));
    for (int i = 0; i < n; i++) {
        int j = i + rand() % (ra->len - i);
        int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }

    for (int i = 0; i < n; i++)
        print_record(ra->data[idx[i]], &f);
    free(idx);
    return 0;
}

/* =========================================================================
 * cmd_dedup — deduplicate records by field value
 * ======================================================================= */

static int cmd_dedup(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);
    if (argc < 2) {
        fputs("usage: mq dedup field file ...\n"
              "  e.g.: mq dedup url *.miner\n", stderr);
        return 1;
    }

    const char *field = argv[0];
    RecordArray *ra   = ra_new();
    load_ra(ra, argc - 1, argv + 1);
    if (!f.no_sort) ra_sort_by_rel(ra);

    /* O(n²) seen-set — fine for ~1500 records */
    const char **seen = malloc(ra->len * sizeof(char *));
    int n_seen = 0, out_count = 0;

    for (int i = 0; i < ra->len; i++) {
        if (f.limit && out_count >= f.limit) break;
        const char *val = get_field(ra->data[i], field);
        if (!val) continue;

        bool dup = false;
        for (int j = 0; j < n_seen; j++)
            if (!strcmp(seen[j], val)) { dup = true; break; }

        if (!dup) {
            seen[n_seen++] = val;
            print_record(ra->data[i], &f);
            out_count++;
        }
    }
    free(seen);
    return f.exit_status && out_count == 0 ? 1 : 0;
}

/* =========================================================================
 * cmd_freq — frequency table for a field's values
 *   default field: keywords (split on comma)
 * ======================================================================= */

typedef struct { char *key; int count; } FreqEntry;

static int cmp_freq_desc(const void *a, const void *b)
{
    return ((const FreqEntry *)b)->count - ((const FreqEntry *)a)->count;
}

static void freq_add(FreqEntry **entries, int *n, int *cap, const char *key)
{
    for (int i = 0; i < *n; i++) {
        if (!strcasecmp((*entries)[i].key, key)) { (*entries)[i].count++; return; }
    }
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *entries = realloc(*entries, (size_t)*cap * sizeof(FreqEntry));
    }
    (*entries)[*n].key   = strdup(key);
    (*entries)[*n].count = 1;
    (*n)++;
}

static void freq_add_csv(FreqEntry **e, int *n, int *cap, const char *val)
{
    if (!val) return;
    char *copy = strdup(val);
    char *tok  = strtok(copy, ",");
    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (*tok) freq_add(e, n, cap, tok);
        tok = strtok(NULL, ",");
    }
    free(copy);
}

static int cmd_freq(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);
    if (argc < 1) {
        fputs("usage: mq freq [field] file ...\n"
              "  default field: keywords (split on commas)\n", stderr);
        return 1;
    }

    /* if first arg doesn't exist as a path, treat it as a field name */
    const char *field = "keywords";
    int first_file = 0;
    {
        struct stat st;
        if (stat(argv[0], &st) != 0) { field = argv[0]; first_file = 1; }
    }
    if (argc - first_file < 1) {
        fputs("usage: mq freq [field] file ...\n", stderr);
        return 1;
    }

    RecordArray *ra = ra_new();
    load_ra(ra, argc - first_file, argv + first_file);

    FreqEntry *entries = NULL;
    int n = 0, cap = 0;
    bool csv_split = !strcmp(field, "keywords");

    for (int i = 0; i < ra->len; i++) {
        const char *val = get_field(ra->data[i], field);
        if (!val) continue;
        if (csv_split) freq_add_csv(&entries, &n, &cap, val);
        else           freq_add(&entries, &n, &cap, val);
    }

    qsort(entries, n, sizeof(FreqEntry), cmp_freq_desc);

    int limit = f.limit ? f.limit : n;
    for (int i = 0; i < n && i < limit; i++)
        printf("%5d  %s\n", entries[i].count, entries[i].key);

    return 0;
}

/* =========================================================================
 * cmd_urls — list all unique source URLs
 * ======================================================================= */

static int cmd_urls(int argc, char **argv)
{
    if (argc < 1) { fputs("usage: mq urls file ...\n", stderr); return 1; }

    RecordArray *ra = ra_new();
    load_ra(ra, argc, argv);

    const char **urls = malloc((size_t)ra->len * sizeof(char *));
    int n = 0;
    for (int i = 0; i < ra->len; i++)
        if (ra->data[i]->url) urls[n++] = ra->data[i]->url;

    qsort(urls, n, sizeof(char *), cmp_strp);

    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || strcmp(urls[i], urls[i - 1])) {
            puts(urls[i]);
            seen++;
        }
    }
    free(urls);
    return seen > 0 ? 0 : 1;
}

/* =========================================================================
 * cmd_group — show records grouped under field-value headers
 * ======================================================================= */

static int cmd_group(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    argc = scan_flags(argc, argv, &f);
    if (argc < 2) {
        fputs("usage: mq group field file ...\n"
              "  e.g.: mq group relevance *.miner\n", stderr);
        return 1;
    }

    const char *field = argv[0];
    RecordArray *ra   = ra_new();
    load_ra(ra, argc - 1, argv + 1);

    /* collect unique values in corpus order */
    const char **vals = malloc((size_t)ra->len * sizeof(char *));
    int n_vals = 0;
    for (int i = 0; i < ra->len; i++) {
        const char *v = get_field(ra->data[i], field);
        if (!v) v = "(none)";
        bool seen = false;
        for (int j = 0; j < n_vals; j++)
            if (!strcmp(vals[j], v)) { seen = true; break; }
        if (!seen) vals[n_vals++] = v;
    }

    /* sort values: relevance field gets rel_order, others alphabetical */
    if (!strcmp(field, "relevance")) {
        /* insertion sort by rel_order (typically ≤4 values) */
        for (int i = 1; i < n_vals; i++) {
            const char *key = vals[i];
            int j = i - 1;
            while (j >= 0 && rel_order(vals[j]) > rel_order(key)) {
                vals[j + 1] = vals[j]; j--;
            }
            vals[j + 1] = key;
        }
    } else {
        qsort(vals, n_vals, sizeof(char *), cmp_strp);
    }

    int total = 0;
    for (int g = 0; g < n_vals; g++) {
        printf("=== %s: %s ===\n", field, vals[g]);
        int grp_count = 0;
        for (int i = 0; i < ra->len; i++) {
            if (f.limit && total >= f.limit) goto done;
            const char *v = get_field(ra->data[i], field);
            if (!v) v = "(none)";
            if (!strcmp(v, vals[g])) {
                print_record(ra->data[i], &f);
                grp_count++;
                total++;
            }
        }
        printf("(%d records)\n\n", grp_count);
    }
done:
    free(vals);
    return 0;
}

/* =========================================================================
 * cmd_slice — select records by positional index range
 * ======================================================================= */

static int cmd_slice(int argc, char **argv)
{
    Flags f = default_flags(FMT_FACTS);
    f.no_sort = true;   /* positional by default */
    argc = scan_flags(argc, argv, &f);
    if (argc < 2) {
        fputs("usage: mq slice N[:M] file ...\n"
              "  N    — single record at index N (0-based)\n"
              "  N:M  — records from N up to (not including) M\n", stderr);
        return 1;
    }

    int start = 0, end = -1;
    char *range = argv[0];
    char *colon = strchr(range, ':');
    if (colon) {
        *colon = '\0';
        start = atoi(range);
        end   = atoi(colon + 1);
    } else {
        start = atoi(range);
        end   = start + 1;
    }

    RecordArray *ra = ra_new();
    load_ra(ra, argc - 1, argv + 1);

    if (end < 0 || end > ra->len) end = ra->len;

    int seen = 0;
    for (int i = start; i < end && i < ra->len; i++) {
        if (f.limit && seen >= f.limit) break;
        print_record(ra->data[i], &f);
        seen++;
    }
    return f.exit_status && seen == 0 ? 1 : 0;
}

/* =========================================================================
 * main + usage
 * ======================================================================= */

static void usage(void)
{
    fputs(
        "mq — miner query\n"
        "\n"
        "usage: mq <command> [flags] [args] file|dir ...\n"
        "\n"
        "commands:\n"
        "  meta   (m)  show file-level MINER_DUMP headers\n"
        "  top         top N records scored by query: mq top N 'query' files\n"
        "  filter (f)  filter by expression:          mq filter 'relevance==HIGH AND content~pat' files\n"
        "  grep   (g)  regex search (smart case, -e multi-pattern, -w word)\n"
        "  titles (t)  list titles grouped by file\n"
        "  select (s)  print chosen fields:            mq select title,url files\n"
        "  stats       per-file record counts by relevance\n"
        "  jsonl  (j)  export as JSONL\n"
        "  count  (c)  count matching records:         mq count 'expr' files\n"
        "  dump   (d)  dump all records\n"
        "  show        fetch one record:               mq show file.miner:42\n"
        "  sample      random N records:               mq sample 10 files\n"
        "  dedup       deduplicate by field:           mq dedup url files\n"
        "  freq        frequency table:                mq freq [field] files\n"
        "  urls        list all unique source URLs\n"
        "  group       records under field headers:    mq group relevance files\n"
        "  slice       records by index range:         mq slice 0:20 files\n"
        "\n"
        "flags (most commands):\n"
        "  -f, --format=<facts|compact|full|title|jsonl|tsv>  (default: facts)\n"
        "  -n, --limit=N          max records to output\n"
        "      --preview=N        chars for compact mode (default: 200)\n"
        "      --no-sort          skip relevance sort\n"
        "  -T, --template=FMT     custom output: mq top 5 q f -T '{{id}} {{title}}'\n"
        "      --exit-status      exit 1 if no records matched\n"
        "  -w, --word             whole-word match (grep)\n"
        "  -e  pattern            extra pattern, OR semantics (grep)\n",
        stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    argc -= 2;
    argv += 2;

    if (!strcmp(cmd, "meta")   || !strcmp(cmd, "m")) return cmd_meta(argc, argv);
    if (!strcmp(cmd, "top"))                          return cmd_top(argc, argv);
    if (!strcmp(cmd, "filter") || !strcmp(cmd, "f")) return cmd_filter(argc, argv);
    if (!strcmp(cmd, "grep")   || !strcmp(cmd, "g")) return cmd_grep(argc, argv);
    if (!strcmp(cmd, "titles") || !strcmp(cmd, "t")) return cmd_titles(argc, argv);
    if (!strcmp(cmd, "select") || !strcmp(cmd, "s")) return cmd_select(argc, argv);
    if (!strcmp(cmd, "stats"))                        return cmd_stats(argc, argv);
    if (!strcmp(cmd, "jsonl")  || !strcmp(cmd, "j")) return cmd_jsonl(argc, argv);
    if (!strcmp(cmd, "count")  || !strcmp(cmd, "c")) return cmd_count(argc, argv);
    if (!strcmp(cmd, "dump")   || !strcmp(cmd, "d")) return cmd_dump(argc, argv);
    if (!strcmp(cmd, "show"))                         return cmd_show(argc, argv);
    if (!strcmp(cmd, "sample"))                       return cmd_sample(argc, argv);
    if (!strcmp(cmd, "dedup"))                        return cmd_dedup(argc, argv);
    if (!strcmp(cmd, "freq"))                         return cmd_freq(argc, argv);
    if (!strcmp(cmd, "urls"))                         return cmd_urls(argc, argv);
    if (!strcmp(cmd, "group"))                        return cmd_group(argc, argv);
    if (!strcmp(cmd, "slice"))                        return cmd_slice(argc, argv);

    fprintf(stderr, "mq: unknown command '%s'\n", cmd);
    usage();
    return 1;
}
