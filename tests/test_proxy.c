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

    /* clearing the pool yields NULL. */
    CHECK(rbl_client_set_proxies(c, NULL, 0, RBL_PROXY_ROUND_ROBIN) == RBL_OK);
    char *none = rbl_client_pick_proxy(c, NULL);
    CHECK(none == NULL);
    free(none);

    rbl_client_free(c);
}
