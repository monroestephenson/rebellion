/* SSRF guard: address and host classification. */
#include "test.h"
#include "net.h"

#include <stdbool.h>

static void ip_cases(void) {
    static const char *const priv[] = {"127.0.0.1",
                                       "127.255.255.254",
                                       "10.0.0.1",
                                       "172.16.0.1",
                                       "172.31.255.255",
                                       "192.168.1.1",
                                       "169.254.169.254",
                                       "100.64.0.1",
                                       "100.127.255.255",
                                       "0.0.0.0",
                                       "224.0.0.1",
                                       "255.255.255.255",
                                       "::1",
                                       "::",
                                       "fe80::1",
                                       "fc00::1",
                                       "fd12:3456::1",
                                       "::ffff:127.0.0.1",
                                       "::ffff:10.1.2.3",
                                       NULL};
    static const char *const pub[] = {
        "8.8.8.8",     "1.1.1.1",     "172.15.255.255", "172.32.0.1",  "100.63.255.255",
        "100.128.0.1", "192.169.0.1", "2606:4700::1",   "2001:db8::1", "::ffff:8.8.8.8",
        NULL};
    for (int i = 0; priv[i]; i++) {
        g_tests_run++;
        if (!rbl_ip_is_private(priv[i])) {
            g_tests_failed++;
            fprintf(stderr, "FAIL %s:%d: %s should be private\n", __FILE__, __LINE__,
                    priv[i]);
        }
    }
    for (int i = 0; pub[i]; i++) {
        g_tests_run++;
        if (rbl_ip_is_private(pub[i])) {
            g_tests_failed++;
            fprintf(stderr, "FAIL %s:%d: %s should be public\n", __FILE__, __LINE__,
                    pub[i]);
        }
    }
    CHECK(!rbl_ip_is_private("example.com")); /* not an IP literal */
    CHECK(!rbl_ip_is_private(""));
}

static void host_cases(void) {
    CHECK(rbl_host_is_private("localhost"));
    CHECK(rbl_host_is_private("LOCALHOST"));
    CHECK(rbl_host_is_private("api.localhost"));
    CHECK(rbl_host_is_private("[::1]"));
    CHECK(rbl_host_is_private(""));
    CHECK(rbl_host_is_private(NULL));
    /* trailing dots name the same host */
    CHECK(rbl_host_is_private("localhost."));
    CHECK(rbl_host_is_private("127.0.0.1."));
    CHECK(rbl_host_is_private("."));
    /* legacy IPv4 spellings */
    CHECK(rbl_host_is_private("127.1"));
    CHECK(rbl_host_is_private("0x7f.1"));
    CHECK(rbl_host_is_private("2130706433"));
    CHECK(rbl_host_is_private("0177.0.0.1"));
    CHECK(rbl_host_is_private("10.1"));

    CHECK(!rbl_host_is_private("example.com"));
    CHECK(!rbl_host_is_private("example.com."));
    CHECK(!rbl_host_is_private("localhost.example.com"));
    CHECK(!rbl_host_is_private("notlocalhost"));
    CHECK(!rbl_host_is_private("8.8.8.8"));
    CHECK(!rbl_host_is_private("www.volksbank.de"));
}

static void url_cases(void) {
    CHECK(rbl_url_host_is_private("http://127.0.0.1:8080/x"));
    CHECK(rbl_url_host_is_private("http://127.1/"));
    CHECK(rbl_url_host_is_private("http://2130706433/"));
    CHECK(rbl_url_host_is_private("http://[::ffff:7f00:1]/"));
    CHECK(rbl_url_host_is_private("http://[0:0:0:0:0:0:0:1]/"));
    CHECK(rbl_url_host_is_private("http://localhost./"));
    CHECK(rbl_url_host_is_private("https://169.254.169.254/latest/meta-data/"));
    CHECK(rbl_url_host_is_private("http://user:pw@10.0.0.5/"));
    CHECK(!rbl_url_host_is_private("https://www.berliner-volksbank.de/"));
    CHECK(!rbl_url_host_is_private("not a url")); /* fails to fetch instead */
}

void test_net(void) {
    ip_cases();
    host_cases();
    url_cases();
}
