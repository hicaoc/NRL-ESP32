#include "app/driver/ptt_output.h"

#include <assert.h>

int main()
{
    assert(NrlPttOutputLevel(false, 1u) == 0);
    assert(NrlPttOutputLevel(true, 1u) == 1);
    assert(NrlPttOutputLevel(false, 0u) == 1);
    assert(NrlPttOutputLevel(true, 0u) == 0);
    return 0;
}
