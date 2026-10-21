#include "test.h"
#include "rebellion/rebellion.h"
#include "internal.h"

#include <stdlib.h>

void test_proxy(void) {
    rbl_client *c = rbl_client_new(NULL);
    CHECK(c != NULL);
    if (!c) return;

    const char *pool[] = {"http://p1:1", "http://p2:2", "http://p3:3"};

    /* round-robin cycles in order and wraps. */
    CHECK(rbl_client_set_proxies(c, pool, 3, RBL_PROXY_ROUND_ROBIN) == RBL_OK);
    char *a = rbl_client_pick_proxy(c, NULL);
    char *b = rbl_client_pick_proxy(c, NULL);
    char *d = rbl_client_pick_proxy(c, NULL);
    char *e = rbl_client_pick_proxy(c, NULL);
    CHECK_STREQ(a, "http://p1:1");
    CHECK_STREQ(b, "http://p2:2");
    CHECK_STREQ(d, "http://p3:3");
    CHECK_STREQ(e, "http://p1:1");
    free(a);
    free(b);
    free(d);
    free(e);

    /* sticky-per-host is deterministic per host. */
    CHECK(rbl_client_set_proxies(c, pool, 3, RBL_PROXY_STICKY_HOST) == RBL_OK);
    char *h1 = rbl_client_pick_proxy(c, "example.de");
    char *h1b = rbl_client_pick_proxy(c, "example.de");
    CHECK(h1 && h1b);
    CHECK_STREQ(h1, h1b);
    free(h1);
    free(h1b);

    /* malformed entries are rejected and leave the pool unchanged. */
    const char *bad1[] = {"http://ok:1", ""};
    const char *bad2[] = {"http://evil:1\r\nX-Injected: 1"};
    const char *bad3[] = {"http://has space:1"};
    const char *bad4[] = {NULL};
    CHECK(rbl_client_set_proxies(c, bad1, 2, RBL_PROXY_ROUND_ROBIN) ==
          RBL_ERR_INVALID_ARG);
    CHECK(rbl_client_set_proxies(c, bad2, 1, RBL_PROXY_ROUND_ROBIN) ==
          RBL_ERR_INVALID_ARG);
    CHECK(rbl_client_set_proxies(c, bad3, 1, RBL_PROXY_ROUND_ROBIN) ==
          RBL_ERR_INVALID_ARG);
    CHECK(rbl_client_set_proxies(c, bad4, 1, RBL_PROXY_ROUND_ROBIN) ==
          RBL_ERR_INVALID_ARG);
    CHECK(rbl_client_set_proxies(c, NULL, 2, RBL_PROXY_ROUND_ROBIN) ==
          RBL_ERR_INVALID_ARG);
    CHECK(rbl_client_set_proxies(c, pool, 3, (rbl_proxy_strategy)99) ==
          RBL_ERR_INVALID_ARG);
    const char *forms[] = {"proxy.example:3128", "socks5h://user:pw@10.0.0.1:1080",
                           "http://[2001:db8::1]:8080"};
    CHECK(rbl_client_set_proxies(c, forms, 3, RBL_PROXY_ROUND_ROBIN) == RBL_OK);
    char *f0 = rbl_client_pick_proxy(c, "example.de");
    CHECK_STREQ(f0, "proxy.example:3128");
    free(f0);

    /* clearing the pool yields NULL. */
    CHECK(rbl_client_set_proxies(c, NULL, 0, RBL_PROXY_ROUND_ROBIN) == RBL_OK);
    char *none = rbl_client_pick_proxy(c, NULL);
    CHECK(none == NULL);
    free(none);

    rbl_client_free(c);
}
