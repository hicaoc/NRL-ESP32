#ifndef SRC_LIB_NRL_VERSION_H
#define SRC_LIB_NRL_VERSION_H

// NRL_FIRMWARE_VERSION can be overridden at build time via -D so CI can
// stamp the tag (e.g. v0.0.2 -> "0.0.2") into the binary.
#ifndef NRL_FIRMWARE_VERSION
#define NRL_FIRMWARE_VERSION "0.8.81"
#endif

#ifndef NRL_FIRMWARE_NAME
#define NRL_FIRMWARE_NAME "NRL-ESP32"
#endif

#define NRL_FIRMWARE_BANNER NRL_FIRMWARE_NAME " v" NRL_FIRMWARE_VERSION

#endif // SRC_LIB_NRL_VERSION_H
