#include "internal.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

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
    cfg->max_response_bytes = 32u * 1024u * 1024u;
    cfg->max_concurrent_browsers = 4;
    cfg->chromium_path = NULL;
    cfg->cookie_file = NULL;
    cfg->verbose = false;
}

/* Locate a usable Chromium/Chrome binary. Honors $REBELLION_CHROMIUM and the
 * Playwright layout used in the cloud image, then common system paths. */
static char *detect_chromium(const char *explicit_path) {
    /* An explicitly configured binary (config, then environment) is
     * authoritative: if it is unusable the browser engine is unavailable,
     * rather than silently running some other browser. */
    const char *configured[] = {explicit_path, getenv("REBELLION_CHROMIUM"),
                                getenv("CHROME_PATH")};
    for (size_t i = 0; i < sizeof(configured) / sizeof(configured[0]); i++) {
        if (!configured[i] || !configured[i][0]) continue;
        if (access(configured[i], X_OK) == 0) return rbl_strdup(configured[i]);
        rbl_logf("browser: %s is not an executable Chromium", configured[i]);
        return NULL;
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
    if (c->cfg.max_retries < 0) c->cfg.max_retries = 0;
    if (c->cfg.max_response_bytes == 0) c->cfg.max_response_bytes = 32u * 1024u * 1024u;
    if (c->cfg.max_concurrent_browsers <= 0) c->cfg.max_concurrent_browsers = 4;

    rbl_log_set_verbose(cfg_in->verbose);

    c->chromium_path = detect_chromium(cfg_in->chromium_path);
    /* Own every string we keep, so callers may pass temporaries. */
    c->cookie_file = cfg_in->cookie_file ? rbl_strdup(cfg_in->cookie_file) : NULL;
    c->profile_dir =
        cfg_in->browser_profile_dir ? rbl_strdup(cfg_in->browser_profile_dir) : NULL;
    c->cfg.user_agent = c->ua;
    c->cfg.accept_language = c->accept_language;
    c->cfg.chromium_path = c->chromium_path;
    c->cfg.cookie_file = c->cookie_file;
    c->cfg.browser_profile_dir = c->profile_dir;
    /* libcurl reads these (http_proxy only in lowercase). */
    c->proxied = env_set("http_proxy") || env_set("HTTPS_PROXY") ||
                 env_set("https_proxy") || env_set("ALL_PROXY") || env_set("all_proxy");

    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
        pthread_mutex_init(&c->share_mtx[i], NULL);
    pthread_mutex_init(&c->host_lock, NULL);
    pthread_mutex_init(&c->proxy_lock, NULL);
    pthread_mutex_init(&c->browser_lock, NULL);
    pthread_cond_init(&c->browser_cond, NULL);

    c->share = curl_share_init();
    if (c->share) {
        curl_share_setopt(c->share, CURLSHOPT_USERDATA, c);
        curl_share_setopt(c->share, CURLSHOPT_LOCKFUNC, share_lock_cb);
        curl_share_setopt(c->share, CURLSHOPT_UNLOCKFUNC, share_unlock_cb);
        curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(c->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }

    if (!c->ua || !c->accept_language || (cfg_in->cookie_file && !c->cookie_file) ||
        (cfg_in->browser_profile_dir && !c->profile_dir)) {
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
    pthread_mutex_destroy(&c->proxy_lock);
    pthread_mutex_destroy(&c->browser_lock);
    pthread_cond_destroy(&c->browser_cond);
    rbl_host_state *h = c->hosts;
    while (h) {
        rbl_host_state *next = h->next;
        free(h->host);
        free(h->robots_body);
        free(h);
        h = next;
    }
    for (size_t i = 0; i < c->proxy_count; i++) free(c->proxies[i]);
    free(c->proxies);
    free(c->ua);
    free(c->accept_language);
    free(c->chromium_path);
    free(c->cookie_file);
    free(c->profile_dir);
    free(c);
}

rbl_status rbl_client_browser_acquire(rbl_client *c, bool persistent_profile,
                                      long timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (timeout_ms < 0) timeout_ms = 0;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&c->browser_lock);
    for (;;) {
        bool slot = c->browsers_active < c->cfg.max_concurrent_browsers;
        bool prof = !persistent_profile || !c->profile_busy;
        if (slot && prof) break;
        if (pthread_cond_timedwait(&c->browser_cond, &c->browser_lock, &deadline) ==
            ETIMEDOUT) {
            pthread_mutex_unlock(&c->browser_lock);
            return RBL_ERR_BUSY;
        }
    }
    c->browsers_active++;
    if (persistent_profile) c->profile_busy = true;
    pthread_mutex_unlock(&c->browser_lock);
    return RBL_OK;
}

void rbl_client_browser_release(rbl_client *c, bool persistent_profile) {
    pthread_mutex_lock(&c->browser_lock);
    if (c->browsers_active > 0) c->browsers_active--;
    if (persistent_profile) c->profile_busy = false;
    pthread_cond_broadcast(&c->browser_cond);
    pthread_mutex_unlock(&c->browser_lock);
}

/* A proxy entry must be a URL with a host ("host:port" alone is accepted as
 * http) and nothing that could smuggle extra arguments or headers. */
static bool proxy_valid(const char *p) {
    if (!p || !*p) return false;
    for (const char *q = p; *q; q++)
        if ((unsigned char)*q <= 0x20 || *q == 0x7F) return false;
    CURLU *h = curl_url();
    if (!h) return false;
    char *host = NULL;
    bool ok =
        curl_url_set(h, CURLUPART_URL, p,
                     CURLU_NON_SUPPORT_SCHEME | CURLU_GUESS_SCHEME) == CURLUE_OK &&
        curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK && host && *host;
    curl_free(host);
    curl_url_cleanup(h);
    return ok;
}

rbl_status rbl_client_set_proxies(rbl_client *c, const char *const *proxies, size_t n,
                                  rbl_proxy_strategy strategy) {
    if (!c || (n > 0 && !proxies)) return RBL_ERR_INVALID_ARG;
    if (strategy != RBL_PROXY_ROUND_ROBIN && strategy != RBL_PROXY_RANDOM &&
        strategy != RBL_PROXY_STICKY_HOST)
        return RBL_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; i++)
        if (!proxy_valid(proxies[i])) return RBL_ERR_INVALID_ARG;
    char **copy = NULL;
    if (n > 0) {
        copy = (char **)calloc(n, sizeof(char *));
        if (!copy) return RBL_ERR_OOM;
        for (size_t i = 0; i < n; i++) {
            copy[i] = rbl_strdup(proxies[i]);
            if (!copy[i]) {
                for (size_t j = 0; j < i; j++) free(copy[j]);
                free(copy);
                return RBL_ERR_OOM;
            }
        }
    }
    pthread_mutex_lock(&c->proxy_lock);
    for (size_t i = 0; i < c->proxy_count; i++) free(c->proxies[i]);
    free(c->proxies);
    c->proxies = copy;
    c->proxy_count = n;
    c->proxy_strategy = strategy;
    c->proxy_cursor = 0;
    pthread_mutex_unlock(&c->proxy_lock);
    return RBL_OK;
}

static unsigned long djb2(const char *s) {
    unsigned long h = 5381;
    for (; s && *s; s++) h = ((h << 5) + h) + (unsigned char)*s;
    return h;
}

char *rbl_client_pick_proxy(rbl_client *c, const char *host) {
    if (!c) return NULL;
    char *chosen = NULL;
    pthread_mutex_lock(&c->proxy_lock);
    if (c->proxy_count > 0) {
        size_t idx = 0;
        switch (c->proxy_strategy) {
        case RBL_PROXY_RANDOM:
            idx = (size_t)rand() % c->proxy_count;
            break;
        case RBL_PROXY_STICKY_HOST:
            idx = (size_t)(djb2(host) % c->proxy_count);
            break;
        case RBL_PROXY_ROUND_ROBIN:
        default:
            idx = c->proxy_cursor % c->proxy_count;
            c->proxy_cursor++;
            break;
        }
        chosen = rbl_strdup(c->proxies[idx]);
    }
    pthread_mutex_unlock(&c->proxy_lock);
    return chosen;
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

void rbl_client_rate_limit(rbl_client *c, const char *host, long delay_ms) {
    long min_delay = c->cfg.min_delay_ms;
    if (delay_ms > min_delay) min_delay = delay_ms;
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
