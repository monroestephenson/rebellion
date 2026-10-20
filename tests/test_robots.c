#include "test.h"
#include "robots.h"

void test_robots(void) {
    /* pattern matching */
    CHECK(rbl_robots_path_match("/private", "/private/x"));
    CHECK(rbl_robots_path_match("/private", "/private"));
    CHECK(!rbl_robots_path_match("/private", "/public"));
    CHECK(rbl_robots_path_match("/*.pdf", "/docs/report.pdf"));
    CHECK(rbl_robots_path_match("/a/*/b", "/a/x/b"));
    CHECK(rbl_robots_path_match("/x$", "/x"));
    CHECK(!rbl_robots_path_match("/x$", "/x/y"));

    /* full evaluation: generic group */
    const char *r1 = "User-agent: *\n"
                     "Disallow: /admin\n"
                     "Allow: /admin/public\n";
    CHECK(!rbl_robots_eval_allowed(r1, "/admin/secret", "rebellion"));
    CHECK(rbl_robots_eval_allowed(r1, "/admin/public/page", "rebellion"));
    CHECK(rbl_robots_eval_allowed(r1, "/", "rebellion"));

    /* empty Disallow means allow all */
    const char *r2 = "User-agent: *\nDisallow:\n";
    CHECK(rbl_robots_eval_allowed(r2, "/anything", "rebellion"));

    /* a specific group overrides the wildcard group */
    const char *r3 = "User-agent: *\n"
                     "Disallow: /\n"
                     "\n"
                     "User-agent: rebellion\n"
                     "Disallow: /nope\n";
    CHECK(rbl_robots_eval_allowed(r3, "/ok", "rebellion"));
    CHECK(!rbl_robots_eval_allowed(r3, "/nope", "rebellion"));
    /* a different bot still hits the wildcard disallow-all */
    CHECK(!rbl_robots_eval_allowed(r3, "/ok", "somebot"));

    /* no robots content => allowed */
    CHECK(rbl_robots_eval_allowed(NULL, "/x", "rebellion"));
}
