#include "test.h"

int g_tests_run = 0;
int g_tests_failed = 0;

int main(void) {
    test_entities();
    test_charset();
    test_url();
    test_html();
    test_markdown();
    test_robots();
    test_proxy();
    test_net();
    test_extract();
    test_jsonval();
    test_integration();

    printf("\n%d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
