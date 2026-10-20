#include "entities.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void rbl_utf8_append(rbl_buf *b, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD; /* replacement character for invalid scalar values */
    if (cp < 0x80) {
        rbl_buf_append_ch(b, (char)cp);
    } else if (cp < 0x800) {
        rbl_buf_append_ch(b, (char)(0xC0 | (cp >> 6)));
        rbl_buf_append_ch(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        rbl_buf_append_ch(b, (char)(0xE0 | (cp >> 12)));
        rbl_buf_append_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        rbl_buf_append_ch(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        rbl_buf_append_ch(b, (char)(0xF0 | (cp >> 18)));
        rbl_buf_append_ch(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        rbl_buf_append_ch(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        rbl_buf_append_ch(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Named entities: the Latin-1 set (vital for German), plus common markup and
 * typographic names. Not the full HTML5 list, but covers real-world pages. */
typedef struct { const char *name; unsigned long cp; } ent;
static const ent ENTS[] = {
    {"quot",34},{"amp",38},{"apos",39},{"lt",60},{"gt",62},
    {"nbsp",160},{"iexcl",161},{"cent",162},{"pound",163},{"curren",164},
    {"yen",165},{"brvbar",166},{"sect",167},{"uml",168},{"copy",169},
    {"ordf",170},{"laquo",171},{"not",172},{"shy",173},{"reg",174},
    {"macr",175},{"deg",176},{"plusmn",177},{"sup2",178},{"sup3",179},
    {"acute",180},{"micro",181},{"para",182},{"middot",183},{"cedil",184},
    {"sup1",185},{"ordm",186},{"raquo",187},{"frac14",188},{"frac12",189},
    {"frac34",190},{"iquest",191},
    {"Agrave",192},{"Aacute",193},{"Acirc",194},{"Atilde",195},{"Auml",196},
    {"Aring",197},{"AElig",198},{"Ccedil",199},{"Egrave",200},{"Eacute",201},
    {"Ecirc",202},{"Euml",203},{"Igrave",204},{"Iacute",205},{"Icirc",206},
    {"Iuml",207},{"ETH",208},{"Ntilde",209},{"Ograve",210},{"Oacute",211},
    {"Ocirc",212},{"Otilde",213},{"Ouml",214},{"times",215},{"Oslash",216},
    {"Ugrave",217},{"Uacute",218},{"Ucirc",219},{"Uuml",220},{"Yacute",221},
    {"THORN",222},{"szlig",223},
    {"agrave",224},{"aacute",225},{"acirc",226},{"atilde",227},{"auml",228},
    {"aring",229},{"aelig",230},{"ccedil",231},{"egrave",232},{"eacute",233},
    {"ecirc",234},{"euml",235},{"igrave",236},{"iacute",237},{"icirc",238},
    {"iuml",239},{"eth",240},{"ntilde",241},{"ograve",242},{"oacute",243},
    {"ocirc",244},{"otilde",245},{"ouml",246},{"divide",247},{"oslash",248},
    {"ugrave",249},{"uacute",250},{"ucirc",251},{"uuml",252},{"yacute",253},
    {"thorn",254},{"yuml",255},
    /* typography / symbols */
    {"euro",8364},{"OElig",338},{"oelig",339},{"Scaron",352},{"scaron",353},
    {"Yuml",376},{"fnof",402},{"circ",710},{"tilde",732},
    {"ensp",8194},{"emsp",8195},{"thinsp",8201},{"zwnj",8204},{"zwj",8205},
    {"lrm",8206},{"rlm",8207},{"ndash",8211},{"mdash",8212},{"lsquo",8216},
    {"rsquo",8217},{"sbquo",8218},{"ldquo",8220},{"rdquo",8221},{"bdquo",8222},
    {"dagger",8224},{"Dagger",8225},{"bull",8226},{"hellip",8230},
    {"permil",8240},{"prime",8242},{"Prime",8243},{"lsaquo",8249},
    {"rsaquo",8250},{"oline",8254},{"frasl",8260},{"trade",8482},
    {"larr",8592},{"uarr",8593},{"rarr",8594},{"darr",8595},{"harr",8596},
    {"minus",8722},{"lowast",8727},{"ne",8800},{"le",8804},{"ge",8805},
    {"times",215},{"middot",183},{"deg",176},
    {"alpha",945},{"beta",946},{"gamma",947},{"delta",948},{"pi",960},
    {"sigma",963},{"omega",969},{"mu",956},{"infin",8734},
    {"spades",9824},{"clubs",9827},{"hearts",9829},{"diams",9830},
};
static const size_t ENTS_N = sizeof(ENTS) / sizeof(ENTS[0]);

static unsigned long lookup_named(const char *name, size_t len) {
    for (size_t i = 0; i < ENTS_N; i++) {
        if (strlen(ENTS[i].name) == len && memcmp(ENTS[i].name, name, len) == 0)
            return ENTS[i].cp;
    }
    return 0;
}

/* Windows-1252 overrides for numeric references in the 0x80..0x9F range,
 * which legacy pages (mis)use for curly quotes, em dashes, the euro sign,
 * etc. Browsers remap these; we do too. */
static unsigned long cp1252_remap(unsigned long cp) {
    switch (cp) {
        case 0x80: return 0x20AC; case 0x82: return 0x201A;
        case 0x83: return 0x0192; case 0x84: return 0x201E;
        case 0x85: return 0x2026; case 0x86: return 0x2020;
        case 0x87: return 0x2021; case 0x88: return 0x02C6;
        case 0x89: return 0x2030; case 0x8A: return 0x0160;
        case 0x8B: return 0x2039; case 0x8C: return 0x0152;
        case 0x8E: return 0x017D; case 0x91: return 0x2018;
        case 0x92: return 0x2019; case 0x93: return 0x201C;
        case 0x94: return 0x201D; case 0x95: return 0x2022;
        case 0x96: return 0x2013; case 0x97: return 0x2014;
        case 0x98: return 0x02DC; case 0x99: return 0x2122;
        case 0x9A: return 0x0161; case 0x9B: return 0x203A;
        case 0x9C: return 0x0153; case 0x9E: return 0x017E;
        case 0x9F: return 0x0178; default: return cp;
    }
}

void rbl_decode_entities(rbl_buf *out, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        char c = s[i];
        if (c != '&') { rbl_buf_append_ch(out, c); i++; continue; }

        /* Numeric: &#123; or &#x1F; */
        if (i + 2 < len && s[i + 1] == '#') {
            size_t j = i + 2;
            unsigned long cp = 0;
            bool hex = false;
            if (j < len && (s[j] == 'x' || s[j] == 'X')) { hex = true; j++; }
            size_t digits = 0;
            while (j < len) {
                char d = s[j];
                if (hex) {
                    if (d >= '0' && d <= '9') cp = cp * 16 + (unsigned)(d - '0');
                    else if (d >= 'a' && d <= 'f') cp = cp * 16 + (unsigned)(d - 'a' + 10);
                    else if (d >= 'A' && d <= 'F') cp = cp * 16 + (unsigned)(d - 'A' + 10);
                    else break;
                } else {
                    if (d >= '0' && d <= '9') cp = cp * 10 + (unsigned)(d - '0');
                    else break;
                }
                digits++; j++;
                if (cp > 0x10FFFF) cp = 0xFFFD;
            }
            if (digits > 0) {
                if (j < len && s[j] == ';') j++;
                rbl_utf8_append(out, cp1252_remap(cp));
                i = j;
                continue;
            }
        }

        /* Named: &amp; etc. Accept up to 32 chars; allow missing ';'. */
        if (i + 1 < len && (isalpha((unsigned char)s[i + 1]))) {
            size_t j = i + 1;
            while (j < len && (isalnum((unsigned char)s[j])) && j - i <= 33) j++;
            size_t nlen = j - (i + 1);
            unsigned long cp = lookup_named(s + i + 1, nlen);
            if (cp) {
                if (j < len && s[j] == ';') j++;
                rbl_utf8_append(out, cp);
                i = j;
                continue;
            }
        }

        /* Not a recognized entity: emit the ampersand literally. */
        rbl_buf_append_ch(out, '&');
        i++;
    }
}

char *rbl_decode_entities_str(const char *s) {
    if (!s) return NULL;
    rbl_buf b; rbl_buf_init(&b);
    rbl_decode_entities(&b, s, strlen(s));
    return rbl_buf_steal(&b);
}

char *rbl_decode_entities_str_n(const char *s, size_t len) {
    if (!s) return NULL;
    rbl_buf b; rbl_buf_init(&b);
    rbl_decode_entities(&b, s, len);
    return rbl_buf_steal(&b);
}
