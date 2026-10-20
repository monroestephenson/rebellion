#include "net.h"
#include "util.h"

#include <curl/curl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/socket.h>

static bool ipv4_is_private(uint32_t a /* host byte order */) {
    uint8_t b0 = (a >> 24) & 0xFF, b1 = (a >> 16) & 0xFF;
    if (b0 == 10) return true;                       /* 10.0.0.0/8      */
    if (b0 == 127) return true;                      /* 127.0.0.0/8     */
    if (b0 == 0) return true;                        /* 0.0.0.0/8       */
    if (b0 == 169 && b1 == 254) return true;         /* 169.254.0.0/16  */
    if (b0 == 172 && (b1 & 0xF0) == 16) return true; /* 172.16.0.0/12   */
    if (b0 == 192 && b1 == 168) return true;         /* 192.168.0.0/16  */
    if (b0 == 100 && (b1 & 0xC0) == 64) return true; /* 100.64.0.0/10   */
    if (b0 >= 224) return true;                      /* multicast/reserved */
    return false;
}

bool rbl_ip_is_private(const char *ip) {
    if (!ip || !*ip) return false;

    struct in_addr v4;
    if (inet_pton(AF_INET, ip, &v4) == 1) {
        return ipv4_is_private(ntohl(v4.s_addr));
    }

    struct in6_addr v6;
    if (inet_pton(AF_INET6, ip, &v6) == 1) {
        const unsigned char *b = v6.s6_addr;
        /* ::1 loopback, :: unspecified */
        bool all_zero = true;
        for (int i = 0; i < 15; i++)
            if (b[i]) {
                all_zero = false;
                break;
            }
        if (all_zero && (b[15] == 0 || b[15] == 1)) return true;
        /* fe80::/10 link-local */
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;
        /* fc00::/7 unique local */
        if ((b[0] & 0xFE) == 0xFC) return true;
        /* ff00::/8 multicast */
        if (b[0] == 0xFF) return true;
        /* ::ffff:a.b.c.d  IPv4-mapped */
        bool mapped = true;
        for (int i = 0; i < 10; i++)
            if (b[i]) {
                mapped = false;
                break;
            }
        if (mapped && b[10] == 0xFF && b[11] == 0xFF) {
            uint32_t a = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                         ((uint32_t)b[14] << 8) | b[15];
            return ipv4_is_private(a);
        }
        return false;
    }
    return false; /* not an IP literal */
}

bool rbl_host_is_private(const char *host) {
    if (!host || !*host) return true;

    /* Strip IPv6 brackets: [::1] -> ::1 */
    char tmp[256];
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']') {
        if (n - 2 >= sizeof(tmp)) return true;
        memcpy(tmp, host + 1, n - 2);
        tmp[n - 2] = '\0';
        host = tmp;
    }

    /* localhost and *.localhost are always local. */
    size_t hl = strlen(host);
    if (rbl_streq_ci(host, "localhost")) return true;
    if (hl > 10 && rbl_streq_ci(host + hl - 10, ".localhost")) return true;

    return rbl_ip_is_private(host);
}

bool rbl_url_host_is_private(const char *url) {
    if (!url) return false;
    CURLU *h = curl_url();
    if (!h) return false;
    bool blocked = false;
    if (curl_url_set(h, CURLUPART_URL, url, 0) == CURLUE_OK) {
        char *host = NULL;
        if (curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK && host) {
            blocked = rbl_host_is_private(host);
            curl_free(host);
        }
    }
    curl_url_cleanup(h);
    return blocked;
}

bool rbl_host_resolves_private(const char *host) {
    if (!host || !*host) return true;
    if (rbl_host_is_private(host)) return true;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    bool priv = false;
    for (struct addrinfo *a = res; a && !priv; a = a->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        const void *src = NULL;
        if (a->ai_family == AF_INET)
            src = &((struct sockaddr_in *)a->ai_addr)->sin_addr;
        else if (a->ai_family == AF_INET6)
            src = &((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;
        if (src && inet_ntop(a->ai_family, src, ip, sizeof(ip)))
            priv = rbl_ip_is_private(ip);
    }
    freeaddrinfo(res);
    return priv;
}
