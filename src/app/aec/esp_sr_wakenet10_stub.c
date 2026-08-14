#include "esp_wn_iface.h"

// esp-sr 2.5.0's generic WakeNet factory table references the WakeNet10
// implementation even when no wake-word model is selected. NRL only uses the
// AFE for AEC/noise suppression and explicitly disables WakeNet at runtime,
// so satisfy that unused table entry without pulling WakeNet10 and esp-dl into
// every firmware image.
const esp_wn_iface_t esp_sr_wakenet10_quantized = {0};
