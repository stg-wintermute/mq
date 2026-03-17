#ifndef MQ_H
#define MQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Record — one SOURCE or REPO block from a .miner file
 * ----------------------------------------------------------------------- */

#define MQ_MAX_EXTRA 16

typedef struct {
    char *file;        /* source filename (basename) */
    char *id;
    char *ts;
    char *relevance;   /* HIGH / MEDIUM / LOW */
    char *title;
    char *url;
    char *keywords;
    char *content;
    char *authors;
    char *year;
    char *venue;
    char *quality;
    /* catch-all for uncommon fields */
    char *extra_keys[MQ_MAX_EXTRA];
    char *extra_vals[MQ_MAX_EXTRA];
    int   n_extra;
} Record;

typedef struct {
    Record **data;
    int      len;
    int      cap;
} RecordArray;

/* MINER_DUMP header */
typedef struct {
    char *fname;
    char *topic;
    char *depth;
    char *status;
    int   n_records;
} FileMeta;

typedef struct {
    FileMeta **data;
    int        len;
    int        cap;
} MetaArray;

/* Output format */
typedef enum {
    FMT_FACTS = 0,
    FMT_COMPACT,
    FMT_FULL,
    FMT_TITLE,
    FMT_JSONL,
    FMT_TSV,
    FMT_HUMAN,   /* default human output: [ref] title / url / keywords */
    FMT_AGENT,   /* token-dense LLM output: title | url | keywords */
} Format;

/* -------------------------------------------------------------------------
 * parser.c
 * ----------------------------------------------------------------------- */
RecordArray *ra_new(void);
void         ra_push(RecordArray *ra, Record *r);
void         load_files(const char **paths, int n, RecordArray *out);
MetaArray   *load_meta(const char **paths, int n);

/* -------------------------------------------------------------------------
 * filter.c
 * ----------------------------------------------------------------------- */
const char *get_field(const Record *r, const char *key);
bool        match_expr(const Record *r, const char *expr);
int         score_record(const Record *r, const char **terms, int n_terms);
void        ra_sort_by_rel(RecordArray *ra);
int         rel_order(const char *rel);

/* -------------------------------------------------------------------------
 * format.c
 * ----------------------------------------------------------------------- */
Format parse_format(const char *s);
void   fmt_record(const Record *r, Format fmt, int preview);
void   fmt_template(const Record *r, const char *tmpl);

#endif /* MQ_H */
