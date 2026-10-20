/* Network safety helpers: classify addresses/hosts for the SSRF guard. */
#ifndef REBELLION_NET_H
#define REBELLION_NET_H

#include <stdbool.h>

/* True if the textual IP (IPv4 or IPv6) is loopback, private (RFC1918 /
 * ULA), link-local, CGNAT, unspecified, or otherwise not a public,
 * routable address. */
bool rbl_ip_is_private(const char *ip);

/* True if `host` should be blocked by default: "localhost"/"*.localhost",
 * or a literal IP that rbl_ip_is_private() rejects. Non-IP hostnames (other
 * than localhost) are not resolved here — DNS targets are caught at connect
 * time by the libcurl pre-request hook. */
bool rbl_host_is_private(const char *host);

/* Parse the host out of `url` (http/https) and apply rbl_host_is_private.
 * Returns true if the URL's host is blocked. Malformed URLs return false
 * (they will simply fail to fetch). */
bool rbl_url_host_is_private(const char *url);

#endif /* REBELLION_NET_H */
