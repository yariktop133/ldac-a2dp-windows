#include "test.h"

int g_failures;
int g_checks;

int main(void)
{
    test_msg();
    test_codec();
    test_loopback();
    test_replay();
    test_avrcp();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
