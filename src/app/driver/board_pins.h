#ifndef DRIVER_BOARD_PINS_H
#define DRIVER_BOARD_PINS_H

// Board variant IDs. The native build passes NRL_BOARD through
// -DNRL_BOARD_ID in scripts/build.py.
#define NRL_BOARD_GEZIPAI   0
#define NRL_BOARD_BH4TDV    1
#define NRL_BOARD_S31_KORVO 2
#define NRL_BOARD_S31_FUNCTION_COREBOARD 3
#define NRL_BOARD_GEZIPAI_4G 4
// USER CUSTOM BEGIN: BI4UMD board identity.
#define NRL_BOARD_BI4UMD    5
#define NRL_BOARD_BH4TDV_RF 6
// USER CUSTOM END: BI4UMD board identity.

#ifndef NRL_BOARD
#define NRL_BOARD NRL_BOARD_GEZIPAI
#endif

#define NRL_BOARD_IS_GEZIPAI_FAMILY \
    (NRL_BOARD == NRL_BOARD_GEZIPAI || NRL_BOARD == NRL_BOARD_GEZIPAI_4G || \
     NRL_BOARD == NRL_BOARD_BI4UMD || NRL_BOARD == NRL_BOARD_BH4TDV_RF)

#define NRL_BOARD_IS_BI4UMD_FAMILY \
    (NRL_BOARD == NRL_BOARD_BI4UMD || NRL_BOARD == NRL_BOARD_BH4TDV_RF)

#if NRL_BOARD == NRL_BOARD_GEZIPAI
#include "board_pins_gezipai.h"
#elif NRL_BOARD == NRL_BOARD_GEZIPAI_4G
#include "board_pins_gezipai_4g.h"
// USER CUSTOM BEGIN: BI4UMD board pin map.
#elif NRL_BOARD == NRL_BOARD_BI4UMD
#include "board_pins_bi4umd.h"
#elif NRL_BOARD == NRL_BOARD_BH4TDV_RF
#include "board_pins_bh4tdv_rf.h"
// USER CUSTOM END: BI4UMD board pin map.
#elif NRL_BOARD == NRL_BOARD_BH4TDV
#include "board_pins_bh4tdv.h"
#elif NRL_BOARD == NRL_BOARD_S31_KORVO
#include "board_pins_s31_korvo.h"
#elif NRL_BOARD == NRL_BOARD_S31_FUNCTION_COREBOARD
#include "board_pins_s31_function_coreboard.h"
#else
#error "Unknown NRL_BOARD: select a supported NRL_BOARD_* value"
#endif

#endif // DRIVER_BOARD_PINS_H
