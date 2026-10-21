#ifndef REBELLION_TEST_H
#define REBELLION_TEST_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int g_tests_run;
extern int g_tests_failed;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_tests_run++;                                                                 \
        if (!(cond)) {                                                                 \
            g_tests_failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)

/* Assert that `haystack` contains the substring `needle`. */
#define CHECK_CONTAINS(haystack, needle)                                               \
    do {                                                                               \
        g_tests_run++;                                                                 \
        const char *_h = (haystack);                                                   \
        if (!_h || !strstr(_h, (needle))) {                                            \
            g_tests_failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: expected to find \"%s\"\n", __FILE__,         \
                    __LINE__, (needle));                                               \
            if (_h) fprintf(stderr, "   in: <<<%s>>>\n", _h);                          \
        }                                                                              \
    } while (0)

#define CHECK_STREQ(a, b)                                                              \
    do {                                                                               \
        g_tests_run++;                                                                 \
        const char *_a = (a);                                                          \
        const char *_b = (b);                                                          \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                       \
            g_tests_failed++;                                                          \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,      \
                    _a ? _a : "(null)", _b ? _b : "(null)");                           \
        }                                                                              \
    } while (0)

void test_markdown(void);
void test_charset(void);
void test_entities(void);
void test_url(void);
void test_robots(void);
void test_html(void);
void test_proxy(void);
void test_net(void);
void test_jsonval(void);
void test_integration(void);

#endif
