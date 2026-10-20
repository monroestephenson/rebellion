/* libFuzzer entry point for the JSON request parser used by the HTTP API —
 * it parses bodies sent by untrusted clients. */
#include "jsonval.h"

#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    rbl_jv *v = rbl_jv_parse((const char *)data, size);
    if (v) {
        /* Touch the accessors the server uses. */
        (void)rbl_jv_get_str(v, "url", NULL);
        (void)rbl_jv_get_num(v, "limit", 0);
        (void)rbl_jv_get_bool(v, "onlyMainContent", true);
        const rbl_jv *a = rbl_jv_get(v, "actions");
        if (a && a->type == RBL_JV_ARR)
            for (size_t i = 0; i < a->count; i++)
                (void)rbl_jv_get_str(a->items[i], "type", "");
        rbl_jv_free(v);
    }
    return 0;
}
