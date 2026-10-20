/* Headless-browser rendering via the Chrome DevTools Protocol.
 *
 * Launches the configured Chromium, drives it over a raw-socket WebSocket to
 * the DevTools endpoint (localhost, plaintext), navigates, waits for the page
 * to settle, and returns the live, JavaScript-rendered DOM. */
#ifndef REBELLION_BROWSER_H
#define REBELLION_BROWSER_H

#include "rebellion/rebellion.h"
#include <stddef.h>

typedef struct {
    long timeout_ms; /* overall navigation budget */
    long settle_ms;  /* extra wait after readyState==complete */
    const char *user_agent;
    const char *accept_language;
    bool screenshot;
    bool full_page;
} rbl_render_opts;

/* Render `url`. On success returns RBL_OK and fills the out-params (each an
 * owned heap allocation; out_png may be NULL if no screenshot was requested
 * or taken). Caller frees. */
rbl_status rbl_browser_render(rbl_client *c, const char *url,
                              const rbl_render_opts *opts, char **out_html,
                              char **out_final_url, unsigned char **out_png,
                              size_t *out_png_len);

/* True if a usable Chromium was found for this client. */
bool rbl_browser_available(rbl_client *c);

#endif /* REBELLION_BROWSER_H */
