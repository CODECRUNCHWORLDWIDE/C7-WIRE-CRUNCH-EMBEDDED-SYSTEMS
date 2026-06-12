# CMakeLists.txt — Week 12 mini-project: the crash-dump server + pentathlon.
#
# Builds debug_app.uf2 from debug_app.c + fault_handler.c + crash_store.c.
# Uses a custom linker script (memmap_noinit.ld) that adds a .noinit (NOLOAD)
# section so the crash dump survives a warm reset.
#
# A bug can be pinned at configure time:
#   cmake .. -DCC_BUG=BUG_NULL_DEREF
# Valid values: BUG_NULL_DEREF, BUG_STACK_OVERFLOW, BUG_BUFFER_OVERRUN,
#               BUG_ISR_JITTER, BUG_RACE. Unset => interactive menu over CDC.

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(cc_debug_pentathlon C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(debug_app
    debug_app.c
    fault_handler.c
    crash_store.c
)

target_link_libraries(debug_app
    pico_stdlib
    hardware_timer
    hardware_sync
    hardware_watchdog
    hardware_exception
)

# The no-init crash region needs the custom linker script.
pico_set_linker_script(debug_app ${CMAKE_CURRENT_SOURCE_DIR}/memmap_noinit.ld)

# stdio over USB CDC (the bench console) and UART (the debugprobe bridge).
pico_enable_stdio_usb(debug_app 1)
pico_enable_stdio_uart(debug_app 1)

# Optional: pin a specific injected bug at configure time.
if(DEFINED CC_BUG)
    target_compile_definitions(debug_app PRIVATE CC_BUG=${CC_BUG})
endif()

# Embed a description picotool can read.
pico_add_extra_outputs(debug_app)

# Add binary info so `picotool info -a` shows what this is.
target_compile_definitions(debug_app PRIVATE
    PICO_DEFAULT_BINARY_TYPE=default
)
