/* A small, bounded recursive-descent JSON parser for API request bodies. */
#ifndef REBELLION_JSONVAL_H
#define REBELLION_JSONVAL_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
    RBL_JV_NULL,
    RBL_JV_BOOL,
    RBL_JV_NUM,
    RBL_JV_STR,
    RBL_JV_ARR,
    RBL_JV_OBJ
} rbl_jv_type;

typedef struct rbl_jv rbl_jv;
struct rbl_jv {
    rbl_jv_type type;
    bool b;      /* RBL_JV_BOOL */
    double num;  /* RBL_JV_NUM  */
    char *str;   /* RBL_JV_STR (owned) */
    /* RBL_JV_ARR / RBL_JV_OBJ */
    rbl_jv **items;
    char **keys; /* OBJ only, parallel to items */
    size_t count;
};

/* Parse NUL-terminated or counted JSON text. Returns NULL on malformed input
 * or if nesting exceeds the internal depth cap. Free with rbl_jv_free. */
rbl_jv *rbl_jv_parse(const char *text, size_t len);
void rbl_jv_free(rbl_jv *v);

/* Object member lookup (NULL if not an object or key absent). */
const rbl_jv *rbl_jv_get(const rbl_jv *o, const char *key);

/* Typed accessors with defaults. as_num accepts a JSON number; as_bool
 * accepts true/false (and treats a number as truthy if nonzero). */
const char *rbl_jv_str(const rbl_jv *v, const char *def);
bool rbl_jv_bool(const rbl_jv *v, bool def);
double rbl_jv_num(const rbl_jv *v, double def);

/* Convenience: look up `key` in object `o` then apply the accessor. */
const char *rbl_jv_get_str(const rbl_jv *o, const char *key, const char *def);
bool rbl_jv_get_bool(const rbl_jv *o, const char *key, bool def);
double rbl_jv_get_num(const rbl_jv *o, const char *key, double def);

#endif /* REBELLION_JSONVAL_H */
