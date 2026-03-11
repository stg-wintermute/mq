#include "mq.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * String helpers
 * ----------------------------------------------------------------------- */

static char *xstrdup(const char *s)
{
    if (!s || !*s) return NULL;
    return strdup(s);
}

/* trim trailing whitespace in-place */
static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    s[n] = '\0';
}

/* return pointer past leading whitespace */
static char *ltrim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    return s;
}

/* append a line to a growing string buffer, '\n'-separated */
static void buf_append(char **buf, size_t *len, size_t *cap, const char *line)
{
    size_t ll   = strlen(line);
    size_t need = *len + ll + 2;   /* +1 for \n between lines, +1 for \0 */
    if (need > *cap) {
        *cap = need * 2 + 256;
        *buf = realloc(*buf, *cap);
    }
    if (*len > 0) (*buf)[(*len)++] = '\n';
    memcpy(*buf + *len, line, ll);
    *len += ll;
    (*buf)[*len] = '\0';
}

/* -------------------------------------------------------------------------
 * RecordArray
 * ----------------------------------------------------------------------- */

RecordArray *ra_new(void)
{
    return calloc(1, sizeof(RecordArray));
}

void ra_push(RecordArray *ra, Record *r)
{
    if (ra->len >= ra->cap) {
        ra->cap = ra->cap ? ra->cap * 2 : 64;
        ra->data = realloc(ra->data, ra->cap * sizeof(Record *));
    }
    ra->data[ra->len++] = r;
}

/* -------------------------------------------------------------------------
 * Block parsing
 * ----------------------------------------------------------------------- */

static void set_field(Record *r, const char *key, const char *val)
{
    if      (!strcmp(key, "id"))        r->id        = xstrdup(val);
    else if (!strcmp(key, "ts"))        r->ts        = xstrdup(val);
    else if (!strcmp(key, "relevance")) r->relevance = xstrdup(val);
    else if (!strcmp(key, "title"))     r->title     = xstrdup(val);
    else if (!strcmp(key, "url"))       r->url       = xstrdup(val);
    else if (!strcmp(key, "keywords"))  r->keywords  = xstrdup(val);
    else if (!strcmp(key, "authors"))   r->authors   = xstrdup(val);
    else if (!strcmp(key, "year"))      r->year      = xstrdup(val);
    else if (!strcmp(key, "venue"))     r->venue     = xstrdup(val);
    else if (!strcmp(key, "quality"))   r->quality   = xstrdup(val);
    else {
        if (r->n_extra < MQ_MAX_EXTRA) {
            r->extra_keys[r->n_extra] = strdup(key);
            r->extra_vals[r->n_extra] = xstrdup(val);
            r->n_extra++;
        }
    }
}

/*
 * Parse one SOURCE/REPO block.
 * `block` is a writable, nul-terminated C string (we modify it in-place).
 */
static Record *parse_block(char *block, const char *fname)
{
    block = ltrim(block);
    if (!*block) return NULL;

    /* first line = block type */
    char *nl   = strchr(block, '\n');
    char *rest = nl ? nl + 1 : block + strlen(block);
    if (nl) *nl = '\0';
    rtrim(block);

    if (strcmp(block, "SOURCE") != 0 && strcmp(block, "REPO") != 0)
        return NULL;

    Record *r = calloc(1, sizeof(Record));
    r->file = strdup(fname);

    bool   in_content  = false;
    char  *cbuf        = NULL;
    size_t clen = 0, ccap = 0;

    char *line = rest;
    while (*line) {
        char *le = strchr(line, '\n');
        if (le) *le = '\0';

        if (in_content) {
            buf_append(&cbuf, &clen, &ccap, line);
        } else {
            /* key must be [a-z_]+ followed by ':' */
            char *p = line;
            while (*p && (islower((unsigned char)*p) || *p == '_')) p++;
            if (*p == ':' && p > line) {
                *p = '\0';
                char *key = line;
                char *val = p + 1;
                while (*val == ' ') val++;    /* skip one space after ':' */
                rtrim(val);

                if (!strcmp(key, "content")) {
                    in_content = true;
                    if (*val) buf_append(&cbuf, &clen, &ccap, val);
                } else {
                    set_field(r, key, val);
                }
            }
            /* non-key lines outside content are silently ignored */
        }

        line = le ? le + 1 : line + strlen(line);
    }

    if (cbuf) {
        /* strip leading/trailing whitespace from the entire content block */
        char *start = ltrim(cbuf);
        rtrim(start);
        r->content = strdup(start);
        free(cbuf);
    }

    if (!r->id && !r->content) {
        free(r);
        return NULL;
    }
    return r;
}

/* -------------------------------------------------------------------------
 * MINER_DUMP header parsing
 * ----------------------------------------------------------------------- */

static void parse_miner_dump(char *block, FileMeta *m)
{
    block = ltrim(block);
    if (strncmp(block, "MINER_DUMP", 10) != 0) return;

    char *line = strchr(block, '\n');
    if (!line) return;
    line++;

    while (*line) {
        char *le = strchr(line, '\n');
        if (le) *le = '\0';

        char *p = line;
        while (*p && (islower((unsigned char)*p) || *p == '_')) p++;
        if (*p == ':' && p > line) {
            *p = '\0';
            char *key = line;
            char *val = ltrim(p + 1);
            rtrim(val);
            if      (!strcmp(key, "topic"))  m->topic  = xstrdup(val);
            else if (!strcmp(key, "depth"))  m->depth  = xstrdup(val);
            else if (!strcmp(key, "status")) m->status = xstrdup(val);
        }

        line = le ? le + 1 : line + strlen(line);
    }
}

/* -------------------------------------------------------------------------
 * File reading and dispatch
 * ----------------------------------------------------------------------- */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static const char   DELIM[] = "<<<END>>>";
static const size_t DLEN    = 9;

/* split text on DELIM, parse each block, push records into `out` */
static void parse_text(char *text, RecordArray *out, const char *fname)
{
    char *pos = text;
    char *end;
    while ((end = strstr(pos, DELIM)) != NULL) {
        *end = '\0';
        Record *r = parse_block(pos, fname);
        if (r) ra_push(out, r);
        pos = end + DLEN;
    }
    Record *r = parse_block(pos, fname);
    if (r) ra_push(out, r);
}

/* like parse_text but also fills in FileMeta from the first MINER_DUMP block */
static void parse_text_meta(char *text, RecordArray *out,
                             const char *fname, FileMeta *m)
{
    int   before = out->len;
    char *pos    = text;
    char *end;
    bool  first  = true;

    while ((end = strstr(pos, DELIM)) != NULL) {
        *end = '\0';
        if (first) { parse_miner_dump(pos, m); first = false; }
        Record *r = parse_block(pos, fname);
        if (r) ra_push(out, r);
        pos = end + DLEN;
    }
    if (first) parse_miner_dump(pos, m);
    Record *r = parse_block(pos, fname);
    if (r) ra_push(out, r);

    m->n_records = out->len - before;
}

/* -------------------------------------------------------------------------
 * Directory enumeration
 * ----------------------------------------------------------------------- */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

typedef void (*file_cb)(const char *path, const char *fname, void *ud);

static void foreach_miner_file(const char *path, file_cb cb, void *ud)
{
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "mq: %s: ", path); perror(""); return; }

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) { fprintf(stderr, "mq: cannot open dir: %s\n", path); return; }

        char **names = NULL;
        int n = 0, cap = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t nl = strlen(ent->d_name);
            if (nl < 7 || strcmp(ent->d_name + nl - 6, ".miner") != 0) continue;
            if (n >= cap) {
                cap = cap ? cap * 2 : 16;
                names = realloc(names, cap * sizeof(char *));
            }
            names[n++] = strdup(ent->d_name);
        }
        closedir(d);
        qsort(names, n, sizeof(char *), cmp_str);

        for (int i = 0; i < n; i++) {
            char fpath[4096];
            snprintf(fpath, sizeof(fpath), "%s/%s", path, names[i]);
            cb(fpath, names[i], ud);
            free(names[i]);
        }
        free(names);
    } else {
        const char *slash = strrchr(path, '/');
        const char *fname = slash ? slash + 1 : path;
        cb(path, fname, ud);
    }
}

/* -------------------------------------------------------------------------
 * Public: load_files
 * ----------------------------------------------------------------------- */

static void load_files_cb(const char *path, const char *fname, void *ud)
{
    RecordArray *out = ud;
    char *text = read_file(path);
    if (!text) return;
    parse_text(text, out, fname);
    free(text);
}

void load_files(const char **paths, int n, RecordArray *out)
{
    for (int i = 0; i < n; i++)
        foreach_miner_file(paths[i], load_files_cb, out);
}

/* -------------------------------------------------------------------------
 * Public: load_meta
 * ----------------------------------------------------------------------- */

typedef struct { MetaArray *ma; RecordArray *tmp; } MetaCtx;

static void load_meta_cb(const char *path, const char *fname, void *ud)
{
    MetaCtx *ctx = ud;
    FileMeta *m  = calloc(1, sizeof(FileMeta));
    m->fname     = strdup(fname);

    char *text = read_file(path);
    if (!text) { free(m); return; }
    parse_text_meta(text, ctx->tmp, fname, m);
    free(text);

    MetaArray *ma = ctx->ma;
    if (ma->len >= ma->cap) {
        ma->cap  = ma->cap ? ma->cap * 2 : 16;
        ma->data = realloc(ma->data, ma->cap * sizeof(FileMeta *));
    }
    ma->data[ma->len++] = m;
}

MetaArray *load_meta(const char **paths, int n)
{
    MetaArray *ma  = calloc(1, sizeof(MetaArray));
    RecordArray *tmp = ra_new();    /* records discarded — only counting */
    MetaCtx ctx    = { ma, tmp };
    for (int i = 0; i < n; i++)
        foreach_miner_file(paths[i], load_meta_cb, &ctx);
    /* tmp records are intentionally leaked — CLI exits right after */
    free(tmp);
    return ma;
}
