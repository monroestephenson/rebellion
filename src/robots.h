/* robots.txt fetching (cached per host) and rule evaluation. */
#ifndef REBELLION_ROBOTS_H
#define REBELLION_ROBOTS_H

#include "rebellion/rebellion.h"
#include "url.h"

/* Return true if `u` may be fetched according to the host's robots.txt for
 * the given user-agent token (e.g. "rebellion"). Fetches and caches
 * robots.txt on first use for the host. A missing/unreachable robots.txt is
 * treated as "allowed" (standard behavior). */
bool rbl_robots_allowed(rbl_client *c, const rbl_url *u, const char *ua_token);

/* Parse a crawl-delay (seconds) for the host, or -1 if none. */
long rbl_robots_crawl_delay(rbl_client *c, const rbl_url *u, const char *ua_token);

/* Exposed for testing: does `path` match a robots.txt rule `pattern`
 * (supporting '*' wildcards and a trailing '$' anchor)? */
bool rbl_robots_path_match(const char *pattern, const char *path);

/* Exposed for testing: evaluate robots.txt `content` for `path` and
 * `ua_token`; returns true if allowed. */
bool rbl_robots_eval_allowed(const char *content, const char *path,
                             const char *ua_token);

#endif /* REBELLION_ROBOTS_H */
