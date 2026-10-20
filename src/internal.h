/* Definitions shared across translation units but not part of the public API. */
#ifndef REBELLION_INTERNAL_H
#define REBELLION_INTERNAL_H

#include "rebellion/rebellion.h"
#include <curl/curl.h>
#include <pthread.h>

/* Per-host bookkeeping for polite rate limiting and robots caching. */
typedef struct rbl_host_state {
    char *host;
    long long last_hit_ms; /* when we last fetched from this host */
    char *robots_body;     /* cached robots.txt (UTF-8) or NULL   */
    bool robots_fetched;
    long long robots_fetched_ms; /* for TTL-based refresh             */
    struct rbl_host_state *next;
} rbl_host_state;

struct rbl_client {
    rbl_client_config cfg; /* string fields point at the owned copies below */
    char *ua;              /* resolved default user agent  */
    char *accept_language; /* resolved default            */
    char *chromium_path;   /* resolved, or NULL           */
    char *cookie_file;     /* owned copy, or NULL          */
    char *profile_dir;     /* owned copy, or NULL          */
    bool proxied;          /* an HTTPS/ALL proxy is configured in env */

    CURLSH *share; /* shares cookies + DNS across easy handles */
    /* One mutex per curl_lock_data class, indexed by the enum value. */
    pthread_mutex_t share_mtx[CURL_LOCK_DATA_LAST];

    rbl_host_state *hosts; /* linked list, guarded by host_lock */
    pthread_mutex_t host_lock;

    /* Browser gate (guarded by browser_lock): caps live Chromium instances
     * and serializes sessions that share the persistent profile. Uses a
     * condition variable, so a slot may be released from another thread. */
    pthread_mutex_t browser_lock;
    pthread_cond_t browser_cond;
    int browsers_active;
    bool profile_busy;

    /* Proxy pool (guarded by proxy_lock). */
    char **proxies;
    size_t proxy_count;
    rbl_proxy_strategy proxy_strategy;
    size_t proxy_cursor; /* round-robin position */
    pthread_mutex_t proxy_lock;
};

/* Reserve a browser slot (and the persistent profile, if used), waiting up
 * to timeout_ms. Returns RBL_OK or RBL_ERR_BUSY. Pair with release. */
rbl_status rbl_client_browser_acquire(rbl_client *c, bool persistent_profile,
                                      long timeout_ms);
void rbl_client_browser_release(rbl_client *c, bool persistent_profile);

/* Pick a proxy for a request to `host` (may be NULL), per the pool strategy,
 * or NULL if no pool is configured. Returns an owned copy the caller frees. */
char *rbl_client_pick_proxy(rbl_client *c, const char *host);

/* Look up (creating if needed) the per-host state. Caller must hold nothing;
 * this takes host_lock internally and returns a stable pointer (entries are
 * never freed until the client is destroyed). */
rbl_host_state *rbl_client_host(rbl_client *c, const char *host);

/* Block until it is polite to hit `host` again, then record the hit. The
 * spacing is max(min_delay_ms, delay_ms); pass 0 for the client default. */
void rbl_client_rate_limit(rbl_client *c, const char *host, long delay_ms);

#endif /* REBELLION_INTERNAL_H */
