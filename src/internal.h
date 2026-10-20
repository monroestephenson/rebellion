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
    struct rbl_host_state *next;
} rbl_host_state;

struct rbl_client {
    rbl_client_config cfg; /* owns its string fields */
    char *ua;              /* resolved default user agent  */
    char *accept_language; /* resolved default            */
    char *chromium_path;   /* resolved, or NULL           */
    bool proxied;          /* an HTTPS/ALL proxy is configured in env */

    CURLSH *share; /* shares cookies + DNS across easy handles */
    /* One mutex per curl_lock_data class, indexed by the enum value. */
    pthread_mutex_t share_mtx[CURL_LOCK_DATA_LAST];

    rbl_host_state *hosts; /* linked list, guarded by host_lock */
    pthread_mutex_t host_lock;
};

/* Look up (creating if needed) the per-host state. Caller must hold nothing;
 * this takes host_lock internally and returns a stable pointer (entries are
 * never freed until the client is destroyed). */
rbl_host_state *rbl_client_host(rbl_client *c, const char *host);

/* Block until it is polite to hit `host` again, then record the hit. */
void rbl_client_rate_limit(rbl_client *c, const char *host);

#endif /* REBELLION_INTERNAL_H */
