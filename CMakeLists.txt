cmake_minimum_required(VERSION 3.16.0)
if(DEFINED NRL_EXTRA_COMPONENT_DIRS)
    foreach(extra_component_dir IN LISTS NRL_EXTRA_COMPONENT_DIRS)
        if(IS_ABSOLUTE "${extra_component_dir}")
            list(APPEND EXTRA_COMPONENT_DIRS "${extra_component_dir}")
        else()
            list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/${extra_component_dir}")
        endif()
    endforeach()
endif()
# The vendored esp-nimble-cpp overrides the registry copy the component manager
# pulls. This matters on S31, where the registry copy rejects the esp32s31 target;
# the vendored copy supports it and registers as an empty component there (see
# vendor/h2zero__esp-nimble-cpp/CMakeLists.txt), since S31 uses the Bluedroid host.
# The vendored gmf_core overrides the registry copy: it carries the NRL patch
# for the zero-copy payload leak in esp_gmf_io_acquire_read (every decode input
# cycle orphaned a freshly realloc'd payload buffer, ~100 KB/s of PSRAM).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/vendor/espressif__gmf_core/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/vendor/espressif__gmf_core")
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/vendor/h2zero__esp-nimble-cpp/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/vendor/h2zero__esp-nimble-cpp")
endif()
# Support for NATIVE idf.py builds (the GitHub CI, all boards); pass
# -DNRL_NATIVE_BUILD=1. PlatformIO is NOT involved in these, so we must
# reproduce what platformio.ini normally provides:
#   * src_dir = src -> register src/ as a component. There is no main/ dir, so
#     without this the app (incl. app_main) is never compiled/linked -> the link
#     fails with "undefined reference to app_main". WHOLE_ARCHIVE on the src
#     component (src/CMakeLists.txt) then keeps app_main from being dropped.
#   * [env:*] build_flags -> the board identity + feature macros. NRL_BOARD MUST
#     be set (via -DNRL_BOARD_ID=NRL_BOARD_GEZIPAI|_BI4UMD|_BH4TDV|_S31_KORVO) or
#     board_pins.h defaults to gezipai. Firmware version via -DNRL_FW_VERSION=...
#     (optional; falls back to the default in nrl_version.h).
# Gated on NRL_NATIVE_BUILD (not IDF_TARGET) so any PlatformIO build, which
# already registers src as its main component, does not double-register it.
if(NRL_NATIVE_BUILD)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")
    # Source files carry UTF-8 Chinese string literals (UI text). PlatformIO's
    # [env] build_flags forced UTF-8 in/out charset; reproduce that so the text
    # isn't mangled on a non-UTF-8 host (e.g. a Windows console). No-op on Linux.
    add_compile_options(-finput-charset=UTF-8 -fexec-charset=UTF-8 -fextended-identifiers)
    add_compile_definitions(
        NRL_BOARD=${NRL_BOARD_ID}
        NRL_ENABLE_AUDIO_AFE=1
        NRL_ENABLE_AEC=1
    )
    if(NRL_FW_VERSION)
        add_compile_definitions(NRL_FIRMWARE_VERSION="${NRL_FW_VERSION}")
    endif()
endif()

# ESP-IDF 6.2 currently reports an internal NimBLE bool default written as
# `default 0` (treated as `n`). Keep normal builds quiet without patching the
# user's ESP-IDF install; set KCONFIG_REPORT_VERBOSITY=default/verbose outside
# CMake when a full Kconfig report is needed.
if(NOT DEFINED ENV{KCONFIG_REPORT_VERBOSITY})
    set(ENV{KCONFIG_REPORT_VERBOSITY} "quiet")
endif()
if(NOT DEFINED CMAKE_MESSAGE_LOG_LEVEL)
    set(CMAKE_MESSAGE_LOG_LEVEL ERROR CACHE STRING
        "Suppress noisy ESP-IDF 6.2 component-validation warnings by default")
endif()

# Keep ESP-IDF's application descriptor in sync with the firmware version
# shown by the UI and used by OTA. Without this, esp_app_desc_t falls back to
# a stale project/git version even when NRL_FW_VERSION is supplied by CI.
if(NRL_FW_VERSION)
    set(PROJECT_VER "${NRL_FW_VERSION}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nrl-esp32)

# S31 Bluetooth (Bluedroid HFP AG): the default Bluedroid security-service-record
# table holds only 3 entries (SDP + AG + device). When the AG *initiates* a
# headset connection it also needs records for SDP discovery + the RFCOMM
# service, overflowing 3 ("BTM_SEC_REG: Out of Service Records (3)" ->
# "MX service not found" -> RFCOMM start fails). Raise the table for the bt
# component so the HFP service-level connection can complete.
if(IDF_TARGET STREQUAL "esp32s31")
    idf_component_get_property(bt_lib bt COMPONENT_LIB)
    if(bt_lib)
        target_compile_definitions(${bt_lib} PRIVATE BTM_SEC_MAX_SERVICE_RECORDS=16)
        # The S31 builds at -O2 (UI render speed); gcc's deeper analysis then
        # trips stringop-truncation inside Bluedroid's own BD_NAME strncpy
        # (bt_types.h). IDF code, not ours -- demote it for this component.
        target_compile_options(${bt_lib} PRIVATE -Wno-error=stringop-truncation)
    endif()

    # Component requirement discovery runs before custom project variables are
    # reliably available inside src/CMakeLists.txt. Connect the Korvo BSP after
    # project() instead, when the selected board is known and both component
    # targets exist. Function CoreBoard never registers or links this BSP.
    if(NRL_BOARD_ID STREQUAL "NRL_BOARD_S31_KORVO")
        idf_component_get_property(src_lib src COMPONENT_LIB)
        idf_component_get_property(korvo_bsp_lib esp32_s31_korvo COMPONENT_LIB)
        if(src_lib AND korvo_bsp_lib)
            target_link_libraries(${src_lib} PRIVATE ${korvo_bsp_lib})
        endif()
    endif()

    # LVGL is fetched only for display-equipped boards. The S31 Function
    # CoreBoard has no display, so do not query a component that is deliberately
    # absent from that build.
    if(NOT NRL_BOARD_ID STREQUAL "NRL_BOARD_S31_FUNCTION_COREBOARD" AND
       EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/lvgl/lvgl.h")
        # LVGL's vendored ESP cmake only REQUIRES esp_timer; its FreeType binding
        # (CONFIG_LV_USE_FREETYPE, S31 only) needs the freetype component's headers
        # and library, so wire them up here instead of patching the vendored tree.
        idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
        idf_component_get_property(freetype_lib espressif__freetype COMPONENT_LIB)
        idf_component_get_property(ppa_lib esp_driver_ppa COMPONENT_LIB)
        idf_component_get_property(esp_mm_lib esp_mm COMPONENT_LIB)

        # LVGL 9.5.0's Espressif PPA backend checks the P4-style L2 cache line
        # Kconfig, while S31 exposes only the L1 DCache line size. They are both
        # 64 bytes on this board, so provide the missing alias for LVGL only.
        target_compile_definitions(${lvgl_lib} PRIVATE
            CONFIG_CACHE_L2_CACHE_LINE_SIZE=CONFIG_CACHE_L1_DCACHE_LINE_SIZE
            # PNG inflate/conversion uses several large scratch allocations.
            # Keep those out of S31's small internal-DRAM heap; the final
            # lv_draw_buf pixel storage is separately checked by map_tiles.cpp.
            LODEPNG_NO_COMPILE_ALLOCATORS)
        target_compile_options(${lvgl_lib} PRIVATE -Wno-unused-variable)
        # The vendored LVGL ESP CMake helper declares these for PPA, but this
        # project consumes LVGL as a plain IDF component, so provide them here.
        if(ppa_lib)
            target_link_libraries(${lvgl_lib} PUBLIC ${ppa_lib})
        endif()
        if(esp_mm_lib)
            target_link_libraries(${lvgl_lib} PUBLIC ${esp_mm_lib})
        endif()
        if(freetype_lib)
            target_link_libraries(${lvgl_lib} PUBLIC ${freetype_lib})
        endif()
    endif()

endif()

# Both map-capable boards have PSRAM. Use the application-provided lodepng
# allocators so PNG inflate/conversion scratch never consumes internal RAM.
# The final lv_draw_buf allocation is forced through a PSRAM image handler in
# map_tiles.cpp as well.
if(NRL_BOARD_ID STREQUAL "NRL_BOARD_BI4UMD" AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/lvgl/lvgl.h")
    idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
    if(lvgl_lib)
        target_compile_definitions(${lvgl_lib} PRIVATE LODEPNG_NO_COMPILE_ALLOCATORS)
    endif()
endif()

# libsmb2 is used on every board in 0.7.8. Compile its legacy C sources with
# non-fatal compatibility warnings regardless of the selected target.
idf_component_get_property(smb2_lib sahlberg__libsmb2 COMPONENT_LIB)
if(smb2_lib)
    target_compile_options(${smb2_lib} PRIVATE -std=gnu17 -Wno-error
                           -include "${CMAKE_CURRENT_LIST_DIR}/cmake/libsmb2_preinclude.h"
                           -Wno-implicit-function-declaration
                           -Wno-array-parameter
                           -Wno-missing-field-initializers
                           -Wno-unused-variable
                           -Wno-implicit-fallthrough)
    # Keep the managed dependency pristine. Only libsmb2's readv references
    # are redirected to our ESP-lwIP compatibility shim; every other component
    # continues to use the normal system readv implementation.
    idf_component_get_property(smb2_dir sahlberg__libsmb2 COMPONENT_DIR)
    set_source_files_properties(
        "${smb2_dir}/lib/socket.c"
        TARGET_DIRECTORY ${smb2_lib}
        PROPERTIES COMPILE_DEFINITIONS "readv=NRL_SMB_Readv")
endif()

if(TARGET ${CMAKE_PROJECT_NAME}.elf)
    set(nrl_mbedtls_archive_dir "${CMAKE_BINARY_DIR}/esp-idf/mbedtls/mbedtls")
    target_link_libraries(${CMAKE_PROJECT_NAME}.elf PRIVATE
        "-Wl,--start-group"
        "${nrl_mbedtls_archive_dir}/library/libmbedtls.a"
        "${nrl_mbedtls_archive_dir}/library/libmbedx509.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/core/libtfpsacrypto.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/builtin/libmbed-builtin.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/platform/libplatform.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/utilities/libutilities.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/extras/libextras.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/everest/libeverest.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/p256-m/libp256-m.a"
        "${nrl_mbedtls_archive_dir}/tf-psa-crypto/drivers/pqcp/libpqcp.a"
        "-Wl,--end-group")
endif()
