#include "jsonval.h"
#include "json.h" /* rbl_json_decode_string */
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define RBL_JV_MAX_DEPTH 64

typedef struct {
    const char *p;
    const char *end;
    int depth;
} jp;

static rbl_jv *parse_value(jp *s);

static void skip_ws(jp *s) {
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            s->p++;
        else
            break;
    }
}

static rbl_jv *jv_new(rbl_jv_type t) {
    rbl_jv *v = (rbl_jv *)calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

void rbl_jv_free(rbl_jv *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->count; i++) {
        rbl_jv_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

static bool arr_push(rbl_jv *a, rbl_jv *item, char *key) {
    rbl_jv **ni = realloc(a->items, (a->count + 1) * sizeof(rbl_jv *));
    if (!ni) return false;
    a->items = ni;
    if (key || a->keys) {
        char **nk = realloc(a->keys, (a->count + 1) * sizeof(char *));
        if (!nk) return false;
        a->keys = nk;
        a->keys[a->count] = key;
    }
    a->items[a->count] = item;
    a->count++;
    return true;
}

static rbl_jv *parse_string(jp *s) {
    const char *endp = NULL;
    char *str = rbl_json_decode_string(s->p, &endp); /* expects opening quote */
    if (!str) return NULL;
    s->p = endp;
    rbl_jv *v = jv_new(RBL_JV_STR);
    if (!v) {
        free(str);
        return NULL;
    }
    v->str = str;
    return v;
}

static rbl_jv *parse_number(jp *s) {
    char *endp = NULL;
    double d = strtod(s->p, &endp);
    if (endp == s->p) return NULL;
    s->p = endp;
    rbl_jv *v = jv_new(RBL_JV_NUM);
    if (v) v->num = d;
    return v;
}

static rbl_jv *parse_literal(jp *s, const char *lit, rbl_jv_type t, bool bval) {
    size_t n = strlen(lit);
    if ((size_t)(s->end - s->p) < n || strncmp(s->p, lit, n) != 0) return NULL;
    s->p += n;
    rbl_jv *v = jv_new(t);
    if (v) v->b = bval;
    return v;
}

static rbl_jv *parse_array(jp *s) {
    s->p++; /* [ */
    rbl_jv *a = jv_new(RBL_JV_ARR);
    if (!a) return NULL;
    skip_ws(s);
    if (s->p < s->end && *s->p == ']') {
        s->p++;
        return a;
    }
    for (;;) {
        rbl_jv *item = parse_value(s);
        if (!item || !arr_push(a, item, NULL)) {
            rbl_jv_free(item);
            rbl_jv_free(a);
            return NULL;
        }
        skip_ws(s);
        if (s->p >= s->end) {
            rbl_jv_free(a);
            return NULL;
        }
        if (*s->p == ',') {
            s->p++;
            skip_ws(s);
            continue;
        }
        if (*s->p == ']') {
            s->p++;
            return a;
        }
        rbl_jv_free(a);
        return NULL;
    }
}

static rbl_jv *parse_object(jp *s) {
    s->p++; /* { */
    rbl_jv *o = jv_new(RBL_JV_OBJ);
    if (!o) return NULL;
    skip_ws(s);
    if (s->p < s->end && *s->p == '}') {
        s->p++;
        return o;
    }
    for (;;) {
        skip_ws(s);
        if (s->p >= s->end || *s->p != '"') {
            rbl_jv_free(o);
            return NULL;
        }
        const char *endp = NULL;
        char *key = rbl_json_decode_string(s->p, &endp);
        if (!key) {
            rbl_jv_free(o);
            return NULL;
        }
        s->p = endp;
        skip_ws(s);
        if (s->p >= s->end || *s->p != ':') {
            free(key);
            rbl_jv_free(o);
            return NULL;
        }
        s->p++;
        rbl_jv *val = parse_value(s);
        if (!val || !arr_push(o, val, key)) {
            free(key);
            rbl_jv_free(val);
            rbl_jv_free(o);
            return NULL;
        }
        skip_ws(s);
        if (s->p >= s->end) {
            rbl_jv_free(o);
            return NULL;
        }
        if (*s->p == ',') {
            s->p++;
            continue;
        }
        if (*s->p == '}') {
            s->p++;
            return o;
        }
        rbl_jv_free(o);
        return NULL;
    }
}

static rbl_jv *parse_value(jp *s) {
    if (++s->depth > RBL_JV_MAX_DEPTH) {
        s->depth--;
        return NULL;
    }
    skip_ws(s);
    rbl_jv *v = NULL;
    if (s->p < s->end) {
        char c = *s->p;
        switch (c) {
        case '"': v = parse_string(s); break;
        case '[': v = parse_array(s); break;
        case '{': v = parse_object(s); break;
        case 't': v = parse_literal(s, "true", RBL_JV_BOOL, true); break;
        case 'f': v = parse_literal(s, "false", RBL_JV_BOOL, false); break;
        case 'n': v = parse_literal(s, "null", RBL_JV_NULL, false); break;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) v = parse_number(s);
            break;
        }
    }
    s->depth--;
    return v;
}

rbl_jv *rbl_jv_parse(const char *text, size_t len) {
    if (!text) return NULL;
    /* strtod() and the string decoder stop at NUL, not at `len`, so parse a
     * NUL-terminated copy: input is never read past its end. An embedded NUL
     * simply ends the document (and fails the trailing-data check). */
    char *copy = rbl_strndup(text, len);
    if (!copy) return NULL;
    jp s = {copy, copy + len, 0};
    rbl_jv *v = parse_value(&s);
    if (v) {
        skip_ws(&s);
        if (s.p != s.end) { /* trailing garbage */
            rbl_jv_free(v);
            v = NULL;
        }
    }
    free(copy);
    return v;
}

const rbl_jv *rbl_jv_get(const rbl_jv *o, const char *key) {
    if (!o || o->type != RBL_JV_OBJ || !o->keys || !key) return NULL;
    for (size_t i = 0; i < o->count; i++)
        if (o->keys[i] && strcmp(o->keys[i], key) == 0) return o->items[i];
    return NULL;
}

const char *rbl_jv_str(const rbl_jv *v, const char *def) {
    return (v && v->type == RBL_JV_STR) ? v->str : def;
}
bool rbl_jv_bool(const rbl_jv *v, bool def) {
    if (!v) return def;
    if (v->type == RBL_JV_BOOL) return v->b;
    if (v->type == RBL_JV_NUM) return v->num != 0.0;
    return def;
}
double rbl_jv_num(const rbl_jv *v, double def) {
    return (v && v->type == RBL_JV_NUM) ? v->num : def;
}

const char *rbl_jv_get_str(const rbl_jv *o, const char *key, const char *def) {
    return rbl_jv_str(rbl_jv_get(o, key), def);
}
bool rbl_jv_get_bool(const rbl_jv *o, const char *key, bool def) {
    return rbl_jv_bool(rbl_jv_get(o, key), def);
}
double rbl_jv_get_num(const rbl_jv *o, const char *key, double def) {
    return rbl_jv_num(rbl_jv_get(o, key), def);
}
