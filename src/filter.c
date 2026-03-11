#include "mq.h"
#include <stdlib.h>
#include <string.h>
#include <regex.h>

/* -------------------------------------------------------------------------
 * Field access
 * ----------------------------------------------------------------------- */

const char *get_field(const Record *r, const char *key)
{
    if (!key) return NULL;
    if (!strcmp(key, "id"))        return r->id;
    if (!strcmp(key, "ts"))        return r->ts;
    if (!strcmp(key, "relevance")) return r->relevance;
    if (!strcmp(key, "title"))     return r->title;
    if (!strcmp(key, "url"))       return r->url;
    if (!strcmp(key, "keywords"))  return r->keywords;
    if (!strcmp(key, "content"))   return r->content;
    if (!strcmp(key, "authors"))   return r->authors;
    if (!strcmp(key, "year"))      return r->year;
    if (!strcmp(key, "venue"))     return r->venue;
    if (!strcmp(key, "quality"))   return r->quality;
    if (!strcmp(key, "file"))      return r->file;
    for (int i = 0; i < r->n_extra; i++)
        if (!strcmp(r->extra_keys[i], key)) return r->extra_vals[i];
    return NULL;
}

/* -------------------------------------------------------------------------
 * Filter expression evaluation
 * Supports: key == val, key != val, key ~ pattern, expr AND expr, expr OR expr
 * ----------------------------------------------------------------------- */

static const char *field_or_empty(const Record *r, const char *key)
{
    const char *v = get_field(r, key);
    return v ? v : "";
}

bool match_expr(const Record *r, const char *expr)
{
    /* AND — evaluated left-to-right, short-circuits */
    const char *and = strstr(expr, " AND ");
    if (and) {
        size_t llen = (size_t)(and - expr);
        char  *left = strndup(expr, llen);
        bool   ok   = match_expr(r, left) && match_expr(r, and + 5);
        free(left);
        return ok;
    }

    /* OR */
    const char *or = strstr(expr, " OR ");
    if (or) {
        size_t llen = (size_t)(or - expr);
        char  *left = strndup(expr, llen);
        bool   ok   = match_expr(r, left) || match_expr(r, or + 4);
        free(left);
        return ok;
    }

    /* single predicate — find operator */
    const char *eq  = strstr(expr, "==");
    const char *neq = strstr(expr, "!=");
    const char *til = strchr(expr, '~');

    /* pick the first operator that appears */
    const char *op     = NULL;
    int         op_len = 0;
    int         op_type = 0;   /* 1=eq, 2=neq, 3=regex */

    if (eq  && (!op || eq  < op)) { op = eq;  op_len = 2; op_type = 1; }
    if (neq && (!op || neq < op)) { op = neq; op_len = 2; op_type = 2; }
    if (til && (!op || til < op)) { op = til; op_len = 1; op_type = 3; }

    if (!op_type) return true;   /* no operator — trivially true */

    /* extract key */
    size_t klen = (size_t)(op - expr);
    while (klen > 0 && expr[klen - 1] == ' ') klen--;
    char *key = strndup(expr, klen);

    /* extract value, strip surrounding quotes */
    const char *vstart = op + op_len;
    while (*vstart == ' ') vstart++;
    if (*vstart == '\'' || *vstart == '"') vstart++;
    char *val = strdup(vstart);
    size_t vlen = strlen(val);
    if (vlen > 0 && (val[vlen - 1] == '\'' || val[vlen - 1] == '"'))
        val[vlen - 1] = '\0';

    const char *fval = field_or_empty(r, key);
    bool result = false;

    switch (op_type) {
    case 1:  /* == */
        result = strcasecmp(fval, val) == 0;
        break;
    case 2:  /* != */
        result = strcasecmp(fval, val) != 0;
        break;
    case 3: {  /* ~ regex */
        regex_t re;
        if (regcomp(&re, val, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0) {
            result = regexec(&re, fval, 0, NULL, 0) == 0;
            regfree(&re);
        }
        break;
    }
    }

    free(key);
    free(val);
    return result;
}

/* -------------------------------------------------------------------------
 * Relevance sort
 * ----------------------------------------------------------------------- */

int rel_order(const char *rel)
{
    if (!rel) return 3;
    if (!strcasecmp(rel, "HIGH"))   return 0;
    if (!strcasecmp(rel, "MEDIUM")) return 1;
    if (!strcasecmp(rel, "LOW"))    return 2;
    return 3;
}

static int cmp_rel(const void *a, const void *b)
{
    const Record *ra = *(const Record **)a;
    const Record *rb = *(const Record **)b;
    int oa = rel_order(ra->relevance);
    int ob = rel_order(rb->relevance);
    return oa - ob;
}

void ra_sort_by_rel(RecordArray *ra)
{
    qsort(ra->data, ra->len, sizeof(Record *), cmp_rel);
}

/* -------------------------------------------------------------------------
 * Relevance-weighted term scoring (for `top` command)
 * ----------------------------------------------------------------------- */

static int count_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return 0;
    int   count = 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strcasestr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

int score_record(const Record *r, const char **terms, int n_terms)
{
    static const int rel_bonus[] = { 4, 2, 0, 1 };   /* HIGH MEDIUM LOW UNKNOWN */
    int score = rel_bonus[rel_order(r->relevance)];

    for (int i = 0; i < n_terms; i++) {
        const char *t = terms[i];
        /* title weighted 3×, keywords 2×, content 1× */
        if (r->title)    score += 3 * count_ci(r->title,    t);
        if (r->keywords) score += 2 * count_ci(r->keywords, t);
        if (r->content)  score +=     count_ci(r->content,  t);
    }
    return score;
}
