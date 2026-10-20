#include <rebellion/rebellion.h>
#include <stdio.h>

int main(void) {
    rbl_client *c = rbl_client_new(NULL);
    if (!c) return 1;
    printf("linked against rebellion %s\n", RBL_VERSION_STRING);
    rbl_client_free(c);
    return 0;
}
