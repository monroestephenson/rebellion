#include "charset.h"
#include "util.h"

#include <iconv.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* Pull the value after "charset" (case-insensitive), skipping '=' and
 * optional quotes, stopping at a delimiter. */
static char *extract_charset(const char *s) {
    if (!s) return NULL;
    const char *p = s;
    while ((p = strcasestr(p, "charset")) != NULL) {
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') { p += 7; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        char quote = 0;
        if (*q == '"' || *q == '\'') { quote = *q; q++; }
        const char *start = q;
        while (*q) {
            if (quote) { if (*q == quote) break; }
            else if (*q == ';' || *q == ' ' || *q == '\t' ||
                     *q == '\r' || *q == '\n' || *q == '/' ||
                     *q == '>' || *q == '"' || *q == '\'') break;
            q++;
        }
        if (q > start) {
            char *out = rbl_strndup(start, (size_t)(q - start));
            rbl_str_tolower_inplace(out);
            return out;
        }
        p += 7;
    }
    return NULL;
}

char *rbl_charset_from_content_type(const char *content_type) {
    return extract_charset(content_type);
}

char *rbl_charset_from_html(const char *data, size_t len) {
    if (!data || len == 0) return NULL;

    /* BOMs. */
    if (len >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        return rbl_strdup("utf-8");
    if (len >= 2 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE)
        return rbl_strdup("utf-16le");
    if (len >= 2 && (unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF)
        return rbl_strdup("utf-16be");

    /* Scan only the head of the document (meta charset must appear early). */
    size_t scan = len < 4096 ? len : 4096;
    char *head = rbl_strndup(data, scan);
    if (!head) return NULL;

    char *result = NULL;
    /* Look inside <meta ...> tags for a charset. */
    const char *p = head;
    while ((p = strchr(p, '<')) != NULL) {
        if (strncasecmp(p, "<meta", 5) == 0) {
            const char *end = strchr(p, '>');
            size_t tlen = end ? (size_t)(end - p) : strlen(p);
            char *tag = rbl_strndup(p, tlen);
            char *cs = extract_charset(tag);
            free(tag);
            if (cs) { result = cs; free(head); return result; }
        }
        p++;
    }
    free(head);
    return NULL;
}

/* Normalize a few common aliases to names iconv reliably understands. */
static const char *canon_charset(const char *cs) {
    if (!cs) return NULL;
    if (!strcmp(cs, "utf8") || !strcmp(cs, "utf-8")) return "UTF-8";
    if (!strcmp(cs, "latin1") || !strcmp(cs, "iso-8859-1") ||
        !strcmp(cs, "iso8859-1") || !strcmp(cs, "l1")) return "ISO-8859-1";
    if (!strcmp(cs, "iso-8859-15") || !strcmp(cs, "iso8859-15") ||
        !strcmp(cs, "latin9")) return "ISO-8859-15";
    if (!strcmp(cs, "windows-1252") || !strcmp(cs, "cp1252") ||
        !strcmp(cs, "win-1252")) return "WINDOWS-1252";
    if (!strcmp(cs, "us-ascii") || !strcmp(cs, "ascii")) return "ISO-8859-1";
    if (!strcmp(cs, "utf-16")) return "UTF-16";
    if (!strcmp(cs, "utf-16le")) return "UTF-16LE";
    if (!strcmp(cs, "utf-16be")) return "UTF-16BE";
    return NULL; /* unknown: let iconv try the raw name, else fall back */
}

/* Is the byte stream already valid UTF-8? */
static bool is_valid_utf8(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        int extra;
        unsigned char lo = 0x80, hi = 0xBF;
        if ((c & 0xE0) == 0xC0) { extra = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else return false;
        if (i + (size_t)extra >= n) return false;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = p[i + k];
            unsigned char lob = (k == 1) ? lo : 0x80;
            unsigned char hib = (k == 1) ? hi : 0xBF;
            if (cc < lob || cc > hib) return false;
        }
        i += (size_t)extra + 1;
    }
    return true;
}

/* Map a single ISO-8859-1 byte to UTF-8 (1 or 2 bytes). Used as a last
 * resort so we never produce garbage for unknown encodings. */
static char *latin1_to_utf8(const char *in, size_t n, size_t *out_len) {
    rbl_buf b; rbl_buf_init(&b);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) rbl_buf_append_ch(&b, (char)c);
        else {
            rbl_buf_append_ch(&b, (char)(0xC0 | (c >> 6)));
            rbl_buf_append_ch(&b, (char)(0x80 | (c & 0x3F)));
        }
    }
    char *s = rbl_buf_steal(&b);
    if (out_len) *out_len = s ? strlen(s) : 0;
    return s;
}

static char *iconv_convert(const char *from, const char *in, size_t in_len,
                           size_t *out_len) {
    iconv_t cd = iconv_open("UTF-8//TRANSLIT", from);
    if (cd == (iconv_t)-1) {
        cd = iconv_open("UTF-8", from);
        if (cd == (iconv_t)-1) return NULL;
    }

    rbl_buf b; rbl_buf_init(&b);
    char *inbuf = (char *)in;
    size_t inleft = in_len;
    char tmp[8192];

    while (inleft > 0) {
        char *outbuf = tmp;
        size_t outleft = sizeof(tmp);
        size_t r = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
        rbl_buf_append(&b, tmp, sizeof(tmp) - outleft);
        if (r == (size_t)-1) {
            if (errno == E2BIG) continue;        /* flush and keep going */
            if (errno == EILSEQ || errno == EINVAL) {
                /* Skip the offending byte and continue, so one bad byte does
                 * not nuke the whole document. */
                if (inleft == 0) break;
                inbuf++; inleft--;
                continue;
            }
            break;
        }
    }
    iconv_close(cd);

    char *s = rbl_buf_steal(&b);
    if (out_len) *out_len = s ? strlen(s) : 0;
    return s;
}

char *rbl_to_utf8(const char *in, size_t in_len, const char *from_charset,
                  size_t *out_len) {
    if (!in) { if (out_len) *out_len = 0; return rbl_strdup(""); }

    const char *canon = canon_charset(from_charset);

    /* Already UTF-8 (declared or detected): validate and copy, dropping a
     * leading BOM. */
    bool declared_utf8 = canon && strcmp(canon, "UTF-8") == 0;
    if ((declared_utf8 || !from_charset) && is_valid_utf8(in, in_len)) {
        size_t off = 0;
        if (in_len >= 3 && (unsigned char)in[0] == 0xEF &&
            (unsigned char)in[1] == 0xBB && (unsigned char)in[2] == 0xBF)
            off = 3;
        char *s = rbl_strndup(in + off, in_len - off);
        if (out_len) *out_len = in_len - off;
        return s;
    }

    /* Try iconv with the canonical name, then the raw declared name. */
    const char *names[3];
    int n = 0;
    if (canon) names[n++] = canon;
    if (from_charset && (!canon || strcmp(canon, from_charset) != 0))
        names[n++] = from_charset;
    for (int i = 0; i < n; i++) {
        size_t ol = 0;
        char *s = iconv_convert(names[i], in, in_len, &ol);
        if (s && ol > 0) { if (out_len) *out_len = ol; return s; }
        free(s);
    }

    /* Unknown/undeclared and not valid UTF-8: assume latin1 so German text
     * from legacy pages still renders rather than turning into mojibake. */
    return latin1_to_utf8(in, in_len, out_len);
}
