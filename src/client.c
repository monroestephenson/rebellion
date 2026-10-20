#include "internal.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A current, realistic desktop Chrome UA. Many sites (German banks included)
 * serve a stripped or blocked page to obvious bot UAs, so we present as a
 * mainstream browser by default. */
#define RBL_DEFAULT_UA                                                                 \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "                    \
    "(KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36"
#define RBL_DEFAULT_ACCEPT_LANG "de-DE,de;q=0.9,en-US;q=0.7,en;q=0.6"

void rbl_client_config_init(rbl_client_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->user_agent = RBL_DEFAULT_UA;
    cfg->accept_language = RBL_DEFAULT_ACCEPT_LANG;
    cfg->default_timeout_ms = 30000;
    cfg->min_delay_ms = 250;
    cfg->max_redirects = 10;
    cfg->max_retries = 2;
    cfg->chromium_path = NULL;
    cfg->cookie_file = NULL;
    cfg->verbose = false;
}

/* Locate a usable Chromium/Chrome binary. Honors $REBELLION_CHROMIUM and the
 * Playwright layout used in the cloud image, then common system paths. */
static char *detect_chromium(const char *explicit_path) {
    const char *candidates_env[] = {explicit_path, getenv("REBELLION_CHROMIUM"),
                                    getenv("CHROME_PATH"), NULL};
    for (int i = 0; candidates_env[i]; i++) {
        if (candidates_env[i][0] && access(candidates_env[i], X_OK) == 0)
            return rbl_strdup(candidates_env[i]);
    }
    const char *paths[] = {
        "/opt/pw-browsers/chromium",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/snap/bin/chromium",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        NULL};
    for (int i = 0; paths[i]; i++)
        if (access(paths[i], X_OK) == 0) return rbl_strdup(paths[i]);
    return NULL;
}

/* curl_global_init is not thread-safe and must run exactly once. */
static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void curl_global_once(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* libcurl share callbacks: serialize access to shared cookie/DNS/SSL data. */
static void share_lock_cb(CURL *handle, curl_lock_data data, curl_lock_access access,
                          void *userptr) {
    (void)handle;
    (void)access;
    rbl_client *c = (rbl_client *)userptr;
    if ((int)data >= 0 && data < CURL_LOCK_DATA_LAST)
        pthread_mutex_lock(&c->share_mtx[data]);
}
static void share_unlock_cb(CURL *handle, curl_lock_data data, void *userptr) {
    (void)handle;
    rbl_client *c = (rbl_client *)userptr;
    if ((int)data >= 0 && data < CURL_LOCK_DATA_LAST)
        pthread_mutex_unlock(&c->share_mtx[data]);
}

static bool env_set(const char *name) {
    const char *v = getenv(name);
    return v && *v;
}

rbl_client *rbl_client_new(const rbl_client_config *cfg_in) {
    pthread_once(&g_curl_once, curl_global_once);

    rbl_client *c = (rbl_client *)calloc(1, sizeof(*c));
    if (!c) return NULL;

    rbl_client_config def;
    if (!cfg_in) {
        rbl_client_config_init(&def);
        cfg_in = &def;
    }
    c->cfg = *cfg_in;

    c->ua = rbl_strdup(cfg_in->user_agent ? cfg_in->user_agent : RBL_DEFAULT_UA);
    c->accept_language = rbl_strdup(cfg_in->accept_language ? cfg_in->accept_language
                                                            : RBL_DEFAULT_ACCEPT_LANG);
    if (c->cfg.default_timeout_ms <= 0) c->cfg.default_timeout_ms = 30000;
    if (c->cfg.max_redirects <= 0) c->cfg.max_redirects = 10;

    rbl_log_set_verbose(cfg_in->verbose);

    c->chromium_path = detect_chromium(cfg_in->chromium_path);
    c->proxied = env_set("HTTPS_PROXY") || env_set("https_proxy") ||
                 env_set("ALL_PROXY") || env_set("all_proxy");

    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
        pthread_mutex_init(&c->share_mtx[i], NULL);
    pthread_mutex_init(&c->host_lock, NULL);

    c->share = curl_share_init();
    if (c->share) {
        curl_share_setopt(c->share, CURLSHOPT_USERDATA, c);
        curl_share_setopt(c->share, CURLSHOPT_LOCKFUNC, share_lock_cb);
        curl_share_setopt(c->share, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
        curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }

    if (!c->ua || !c->accept_language) {
        rbl_client_free(c);
        return NULL;
    }
    return c;
}

void rbl_client_free(rbl_client *c) {
    if (!c) return;
    if (c->share) curl_share_cleanup(c->share); /* before destroying mutexes */
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
        pthread_mutex_destroy(&c->share_mtx[i]);
    pthread_mutex_destroy(&c->host_lock);
    rbl_host_state *h = c->hosts;
    while (h) {
        rbl_host_state *next = h->next;
        free(h->host);
        free(h->robots_body);
        free(h);
        h = next;
    }
    free(c->ua);
    free(c->accept_language);
    free(c->chromium_path);
    free(c);
}

rbl_host_state *rbl_client_host(rbl_client *c, const char *host) {
    if (!host) host = "";
    pthread_mutex_lock(&c->host_lock);
    rbl_host_state *h = c->hosts;
    while (h) {
        if (strcmp(h->host, host) == 0) {
            pthread_mutex_unlock(&c->host_lock);
            return h;
        }
        h = h->next;
    }
    h = (rbl_host_state *)calloc(1, sizeof(*h));
    if (h) {
        h->host = rbl_strdup(host);
        h->last_hit_ms = 0;
        h->next = c->hosts;
        c->hosts = h;
    }
    pthread_mutex_unlock(&c->host_lock);
    return h;
}

void rbl_client_rate_limit(rbl_client *c, const char *host) {
    long min_delay = c->cfg.min_delay_ms;
    if (min_delay <= 0) return;
    rbl_host_state *h = rbl_client_host(c, host);
    if (!h) return;

    pthread_mutex_lock(&c->host_lock);
    long long now = rbl_now_ms();
    long long wait = 0;
    if (h->last_hit_ms != 0) {
        long long elapsed = now - h->last_hit_ms;
        if (elapsed < min_delay) wait = min_delay - elapsed;
    }
    /* Reserve our slot now so concurrent callers queue behind us. */
    h->last_hit_ms = now + wait;
    pthread_mutex_unlock(&c->host_lock);

    if (wait > 0) {
        rbl_logf("rate-limit: waiting %lldms for %s", wait, host);
        rbl_sleep_ms((long)wait);
    }
}
