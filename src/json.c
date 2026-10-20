#include "json.h"
#include "entities.h"   /* rbl_utf8_append */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void rbl_json_escape(rbl_buf *b, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"':  rbl_buf_append_str(b, "\\\""); break;
            case '\\': rbl_buf_append_str(b, "\\\\"); break;
            case '\n': rbl_buf_append_str(b, "\\n"); break;
            case '\r': rbl_buf_append_str(b, "\\r"); break;
            case '\t': rbl_buf_append_str(b, "\\t"); break;
            case '\b': rbl_buf_append_str(b, "\\b"); break;
            case '\f': rbl_buf_append_str(b, "\\f"); break;
            default:
                if (c < 0x20) {
                    char u[8];
                    snprintf(u, sizeof(u), "\\u%04x", c);
                    rbl_buf_append_str(b, u);
                } else {
                    rbl_buf_append_ch(b, (char)c);
                }
        }
    }
}

void rbl_json_quote(rbl_buf *b, const char *s) {
    rbl_buf_append_ch(b, '"');
    rbl_json_escape(b, s);
    rbl_buf_append_ch(b, '"');
}

char *rbl_json_decode_string(const char *p, const char **end) {
    if (!p || *p != '"') return NULL;
    p++;
    rbl_buf b; rbl_buf_init(&b);
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            if (end) *end = p + 1;
            return rbl_buf_steal(&b);
        }
        if (c == '\\') {
            p++;
            char e = *p;
            switch (e) {
                case '"':  rbl_buf_append_ch(&b, '"'); break;
                case '\\': rbl_buf_append_ch(&b, '\\'); break;
                case '/':  rbl_buf_append_ch(&b, '/'); break;
                case 'n':  rbl_buf_append_ch(&b, '\n'); break;
                case 'r':  rbl_buf_append_ch(&b, '\r'); break;
                case 't':  rbl_buf_append_ch(&b, '\t'); break;
                case 'b':  rbl_buf_append_ch(&b, '\b'); break;
                case 'f':  rbl_buf_append_ch(&b, '\f'); break;
                case 'u': {
                    if (!p[1] || !p[2] || !p[3] || !p[4]) { p++; break; }
                    char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                    unsigned long cp = strtoul(hex, NULL, 16);
                    /* Handle UTF-16 surrogate pairs. */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        p[5] == '\\' && p[6] == 'u') {
                        char hex2[5] = { p[7], p[8], p[9], p[10], 0 };
                        unsigned long lo = strtoul(hex2, NULL, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    rbl_utf8_append(&b, cp);
                    p += 4;
                    break;
                }
                default: rbl_buf_append_ch(&b, e); break;
            }
            if (!*p) break;
            p++;
            continue;
        }
        rbl_buf_append_ch(&b, (char)c);
        p++;
    }
    rbl_buf_free(&b);
    return NULL; /* unterminated */
}

const char *rbl_json_find_value(const char *json, const char *name) {
    if (!json || !name) return NULL;
    size_t nl = strlen(name);
    char *needle = (char *)malloc(nl + 3);
    if (!needle) return NULL;
    needle[0] = '"';
    memcpy(needle + 1, name, nl);
    needle[nl + 1] = '"';
    needle[nl + 2] = '\0';

    const char *p = json;
    const char *found = NULL;
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = p + nl + 2;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            found = q;
            break;
        }
        p = p + nl + 2;
    }
    free(needle);
    return found;
}
