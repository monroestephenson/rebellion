#include "test.h"
#include "url.h"

void test_url(void) {
    rbl_url u;

    CHECK(rbl_url_parse("https://www.Example.de/Pfad?q=1#frag", &u));
    CHECK_STREQ(u.scheme, "https");
    CHECK_STREQ(u.host, "www.example.de");
    CHECK_STREQ(u.path, "/Pfad");
    rbl_url_free(&u);

    CHECK(!rbl_url_parse("ftp://example.com/x", &u));
    CHECK(!rbl_url_parse("not a url", &u));

    char *r = rbl_url_resolve("https://a.de/dir/page.html", "../other.html");
    CHECK_STREQ(r, "https://a.de/other.html");
    free(r);

    r = rbl_url_resolve("https://a.de/dir/", "sub/x");
    CHECK_STREQ(r, "https://a.de/dir/sub/x");
    free(r);

    r = rbl_url_resolve("https://a.de/dir/", "https://b.de/y");
    CHECK_STREQ(r, "https://b.de/y");
    free(r);

    r = rbl_url_resolve("https://a.de/dir/", "//cdn.de/z");
    CHECK_STREQ(r, "https://cdn.de/z");
    free(r);

    r = rbl_url_strip_fragment("https://a.de/x#y");
    CHECK_STREQ(r, "https://a.de/x");
    free(r);

    CHECK(rbl_host_matches("www.example.de", "shop.example.de", true));
    CHECK(!rbl_host_matches("www.example.de", "example.com", true));
    CHECK(!rbl_host_matches("www.example.de", "shop.example.de", false));
    CHECK(rbl_host_matches("example.de", "example.de", false));
}
