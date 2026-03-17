#include "mq.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>
#include <unistd.h>

#define BOLD  "\033[1m"
#define DIM   "\033[2m"
#define RESET "\033[0m"

static int use_color(void) { return isatty(STDOUT_FILENO); }

/* -------------------------------------------------------------------------
 * Format name → enum
 * ----------------------------------------------------------------------- */

Format parse_format(const char *s)
{
    if (!s)                       return FMT_FACTS;
    if (!strcmp(s, "facts"))      return FMT_FACTS;
    if (!strcmp(s, "compact"))    return FMT_COMPACT;
    if (!strcmp(s, "full"))       return FMT_FULL;
    if (!strcmp(s, "title"))      return FMT_TITLE;
    if (!strcmp(s, "jsonl"))      return FMT_JSONL;
    if (!strcmp(s, "tsv"))        return FMT_TSV;
    if (!strcmp(s, "human"))      return FMT_HUMAN;
    if (!strcmp(s, "agent"))      return FMT_AGENT;
    fprintf(stderr, "mq: unknown format '%s' (human/facts/compact/full/title/jsonl/tsv/agent)\n", s);
    return FMT_FACTS;
}

/* -------------------------------------------------------------------------
 * JSON string output (for jsonl format)
 * ----------------------------------------------------------------------- */

static void json_str(const char *s)
{
    putchar('"');
    if (!s) { putchar('"'); return; }
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n",  stdout); break;
        case '\r': fputs("\\r",  stdout); break;
        case '\t': fputs("\\t",  stdout); break;
        default:
            if ((unsigned char)*s < 0x20)
                printf("\\u%04x", (unsigned char)*s);
            else
                putchar(*s);
        }
    }
    putchar('"');
}

/* -------------------------------------------------------------------------
 * Filename shortener for facts format
 * Strip ".miner", "coverage-mined-", "research-raw-" prefixes
 * ----------------------------------------------------------------------- */

static void print_short_fname(const char *fname)
{
    char buf[256];
    size_t n = strlen(fname);

    /* strip .miner suffix */
    if (n > 6 && !strcmp(fname + n - 6, ".miner")) n -= 6;
    strncpy(buf, fname, n);
    buf[n] = '\0';

    /* strip known prefixes */
    char *p = buf;
    if (!strncmp(p, "coverage-mined-", 15)) p += 15;
    else if (!strncmp(p, "research-raw-",  13)) { fputs("raw-", stdout); p += 13; }

    fputs(p, stdout);
}

/* -------------------------------------------------------------------------
 * facts format: first meaningful content line
 *
 * Skips:
 *   - empty lines
 *   - lines starting with doi:, doi , pages:, url:, http, fetched:
 *   - bare ALL-CAPS section headers ending with ':' and nothing else
 * ----------------------------------------------------------------------- */

static const char *SKIP_PFXS[] = {
    "doi:", "doi ", "pages:", "url:", "http", "fetched:", "- url:", "- quality:", NULL
};

/* returns true if `line` looks like "ALL CAPS HEADER:" with no body */
static bool is_section_header(const char *line)
{
    const char *p = line;
    /* must contain only uppercase, space, '/', '(', ')', '-', '_', '&' */
    while (*p && *p != ':') {
        if (!isupper((unsigned char)*p) &&
            *p != ' ' && *p != '/' && *p != '(' && *p != ')' &&
            *p != '-' && *p != '_' && *p != '&' && *p != ',')
            return false;
        p++;
    }
    if (*p != ':') return false;
    /* nothing meaningful after the colon */
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return !*p;
}

static void print_first_content_line(const char *content)
{
    if (!content || !*content) return;

    const char *line = content;
    while (*line) {
        const char *le = strchr(line, '\n');

        /* strip leading whitespace and list markers */
        const char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '-' ||
               *s == '*' || *s == '\xe2' /* UTF-8 bullet start */) s++;
        /* skip UTF-8 bullet • (e2 80 a2) and → (e2 86 92) */
        if ((unsigned char)*s == 0xe2) s += 3;

        /* skip empty */
        if (!*s || s == le) { line = le ? le + 1 : line + strlen(line); continue; }

        /* skip unwanted prefixes */
        bool skip = false;
        for (int i = 0; SKIP_PFXS[i]; i++) {
            if (!strncasecmp(s, SKIP_PFXS[i], strlen(SKIP_PFXS[i]))) { skip = true; break; }
        }
        if (!skip && is_section_header(s)) skip = true;

        if (!skip) {
            /* print up to 160 chars, stopping at newline */
            fputs("  ", stdout);
            int n = 0;
            for (const char *p = s; *p && *p != '\n' && n < 160; p++, n++)
                putchar(*p);
            if (le && (le - s) > 160) fputs("...", stdout);
            putchar('\n');
            return;
        }

        line = le ? le + 1 : line + strlen(line);
    }
}

/* -------------------------------------------------------------------------
 * Public: fmt_record
 * ----------------------------------------------------------------------- */

void fmt_record(const Record *r, Format fmt, int preview)
{
    switch (fmt) {

    /* ------------------------------------------------------------------
     * facts: [shortfile:id/R] Title \n  first meaningful content line
     * ---------------------------------------------------------------- */
    case FMT_FACTS:
        putchar('[');
        print_short_fname(r->file ? r->file : "");
        putchar(':');
        fputs(r->id ? r->id : "?", stdout);
        putchar('/');
        putchar(r->relevance ? (char)toupper((unsigned char)r->relevance[0]) : '?');
        fputs("] ", stdout);
        fputs(r->title ? r->title : "(no title)", stdout);
        putchar('\n');
        print_first_content_line(r->content);
        break;

    /* ------------------------------------------------------------------
     * compact: [RELEVANCE] Title \n  url \n  preview
     * ---------------------------------------------------------------- */
    case FMT_COMPACT: {
        printf("[%s] %s\n",
               r->relevance ? r->relevance : "?",
               r->title     ? r->title     : "(no title)");
        if (r->url) printf("  %s\n", r->url);
        if (r->content) {
            int n = 0;
            for (const char *p = r->content; *p && n < preview; p++, n++)
                putchar(*p == '\n' ? ' ' : *p);
            if ((int)strlen(r->content) > preview) fputs("...", stdout);
            putchar('\n');
        }
        break;
    }

    /* ------------------------------------------------------------------
     * full: SOURCE block as-is
     * ---------------------------------------------------------------- */
    case FMT_FULL:
        fputs("SOURCE\n", stdout);
        if (r->id)        printf("id: %s\n",        r->id);
        if (r->ts)        printf("ts: %s\n",        r->ts);
        if (r->relevance) printf("relevance: %s\n", r->relevance);
        if (r->title)     printf("title: %s\n",     r->title);
        if (r->url)       printf("url: %s\n",       r->url);
        if (r->keywords)  printf("keywords: %s\n",  r->keywords);
        if (r->authors)   printf("authors: %s\n",   r->authors);
        if (r->year)      printf("year: %s\n",      r->year);
        if (r->venue)     printf("venue: %s\n",     r->venue);
        if (r->quality)   printf("quality: %s\n",   r->quality);
        for (int i = 0; i < r->n_extra; i++)
            printf("%s: %s\n", r->extra_keys[i], r->extra_vals[i] ? r->extra_vals[i] : "");
        if (r->content)   printf("content:\n%s\n",  r->content);
        break;

    /* ------------------------------------------------------------------
     * title: [RELEVANCE] Title
     * ---------------------------------------------------------------- */
    case FMT_TITLE:
        printf("[%s] %s\n",
               r->relevance ? r->relevance : "?",
               r->title     ? r->title     : "(no title)");
        break;

    /* ------------------------------------------------------------------
     * jsonl: one JSON object per line
     * ---------------------------------------------------------------- */
    case FMT_JSONL: {
        putchar('{');
        bool first = true;
#define JFIELD(k, v) do { \
    if (v) { \
        if (!first) putchar(','); \
        printf("\"%s\":", k); json_str(v); \
        first = false; \
    } \
} while (0)
        JFIELD("id",        r->id);
        JFIELD("ts",        r->ts);
        JFIELD("relevance", r->relevance);
        JFIELD("title",     r->title);
        JFIELD("url",       r->url);
        JFIELD("keywords",  r->keywords);
        JFIELD("authors",   r->authors);
        JFIELD("year",      r->year);
        JFIELD("venue",     r->venue);
        JFIELD("quality",   r->quality);
        JFIELD("content",   r->content);
        for (int i = 0; i < r->n_extra; i++) {
            if (r->extra_vals[i]) {
                if (!first) putchar(',');
                putchar('"'); fputs(r->extra_keys[i], stdout); fputs("\":", stdout);
                json_str(r->extra_vals[i]);
                first = false;
            }
        }
        JFIELD("file", r->file);
#undef JFIELD
        fputs("}\n", stdout);
        break;
    }

    /* ------------------------------------------------------------------
     * human: Title
     *          url
     *          keywords  [shortfile:id/R]
     * ---------------------------------------------------------------- */
    case FMT_HUMAN: {
        int col = use_color();
        if (col) fputs(BOLD, stdout);
        fputs(r->title ? r->title : "(no title)", stdout);
        if (col) fputs(RESET, stdout);
        putchar('\n');
        if (r->url) printf("  %s\n", r->url);
        fputs("  ", stdout);
        if (r->keywords) fputs(r->keywords, stdout);
        if (col) fputs(DIM, stdout);
        fputs(" [", stdout);
        print_short_fname(r->file ? r->file : "");
        putchar(':');
        fputs(r->id ? r->id : "?", stdout);
        putchar('/');
        putchar(r->relevance ? (char)toupper((unsigned char)r->relevance[0]) : '?');
        putchar(']');
        if (col) fputs(RESET, stdout);
        putchar('\n');
        break;
    }

    /* ------------------------------------------------------------------
     * agent: [shortfile:id/R] title | url | keywords  (single line, no blanks)
     * ---------------------------------------------------------------- */
    case FMT_AGENT:
        putchar('[');
        print_short_fname(r->file ? r->file : "");
        putchar(':');
        fputs(r->id ? r->id : "?", stdout);
        putchar('/');
        putchar(r->relevance ? (char)toupper((unsigned char)r->relevance[0]) : '?');
        fputs("] ", stdout);
        fputs(r->title ? r->title : "(no title)", stdout);
        if (r->url)      { fputs(" | ", stdout); fputs(r->url,      stdout); }
        if (r->keywords) { fputs(" | ", stdout); fputs(r->keywords, stdout); }
        putchar('\n');
        break;

    /* ------------------------------------------------------------------
     * tsv: id TAB relevance TAB title TAB url TAB keywords TAB file
     * ---------------------------------------------------------------- */
    case FMT_TSV: {
#define TFIELD(v) do { \
    if (v) { \
        for (const char *_p = (v); *_p; _p++) \
            putchar((*_p == '\t' || *_p == '\n' || *_p == '\r') ? ' ' : *_p); \
    } \
    putchar('\t'); \
} while (0)
        TFIELD(r->id);
        TFIELD(r->relevance);
        TFIELD(r->title);
        TFIELD(r->url);
        TFIELD(r->keywords);
        /* file: last field, end with newline not tab */
        if (r->file) fputs(r->file, stdout);
        putchar('\n');
#undef TFIELD
        break;
    }
    }
}

/* -------------------------------------------------------------------------
 * fmt_template — {{field}} substitution with \n \t escape support
 * ----------------------------------------------------------------------- */

void fmt_template(const Record *r, const char *tmpl)
{
    const char *p = tmpl;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (!end) { fputs(p, stdout); break; }
            size_t len = (size_t)(end - (p + 2));
            if (len < 64) {
                char fname[64];
                memcpy(fname, p + 2, len);
                fname[len] = '\0';
                const char *val = get_field(r, fname);
                if (val) fputs(val, stdout);
            }
            p = end + 2;
        } else if (p[0] == '\\' && p[1] == 'n') {
            putchar('\n');
            p += 2;
        } else if (p[0] == '\\' && p[1] == 't') {
            putchar('\t');
            p += 2;
        } else {
            putchar(*p++);
        }
    }
    putchar('\n');
}
