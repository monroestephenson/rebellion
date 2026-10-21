/* libFuzzer entry point: robots.txt parsing and rule matching. The input is
 * split at the first NUL into robots.txt content and a request path. */
#include "robots.h"

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *buf = (char *)malloc(size + 2);
    if (!buf) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';
    buf[size + 1] = '\0';
    const char *content = buf;
    const char *path = buf + strlen(buf) + 1; /* "" when there is no NUL */
    if (!*path) path = "/";

    (void)rbl_robots_eval_allowed(content, path, "rebellion");
    (void)rbl_robots_eval_allowed(content, path, NULL);
    (void)rbl_robots_path_match(content, path);
    free(buf);
    return 0;
}
