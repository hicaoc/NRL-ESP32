#include "lib/nrl_hostname.h"

#include <assert.h>
#include <string.h>

int main()
{
    char hostname[40] = {};

    NrlMakeHostname(nullptr, 0u, hostname, sizeof(hostname));
    assert(strcmp(hostname, "nocall-0") == 0);

    NrlMakeHostname("", 0u, hostname, sizeof(hostname));
    assert(strcmp(hostname, "nocall-0") == 0);

    NrlMakeHostname("BH4TDV", 1u, hostname, sizeof(hostname));
    assert(strcmp(hostname, "bh4tdv-1") == 0);

    NrlMakeHostname("BI4 UMD", 15u, hostname, sizeof(hostname));
    assert(strcmp(hostname, "bi4-umd-15") == 0);

    return 0;
}
