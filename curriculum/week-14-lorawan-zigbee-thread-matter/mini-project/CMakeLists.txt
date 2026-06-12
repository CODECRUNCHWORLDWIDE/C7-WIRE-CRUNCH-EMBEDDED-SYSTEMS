# CMakeLists.txt — Week 14 mini-project: LoRaWAN Class-A node.
#
# Produces lorawan-node.uf2. Requires the Pico SDK (PICO_SDK_PATH set) and the
# arm-none-eabi toolchain. Copy secrets.h.example to secrets.h and fill in your
# TTN keys before building.

cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(lorawan_node C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Fail early with a clear message if the user forgot to create secrets.h.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR
        "secrets.h not found. Copy secrets.h.example to secrets.h and fill in "
        "your TTN DevAddr / NwkSKey / AppSKey (see Lecture 2 on byte order).")
endif()

add_executable(lorawan-node
    main.c
    lorawan.c
    sx1262.c
)

target_include_directories(lorawan-node PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../exercises   # for lpwan_common.h
)

target_link_libraries(lorawan-node
    pico_stdlib
    hardware_spi
    hardware_gpio
    hardware_adc
)

# Optimise for size; the AES/CMAC code is small but -Os keeps the image tidy.
target_compile_options(lorawan-node PRIVATE -Os -Wall -Wextra)

# Log over USB CDC (so `screen /dev/cu.usbmodem*` shows the uplink trace).
pico_enable_stdio_usb(lorawan-node 1)
pico_enable_stdio_uart(lorawan-node 0)

pico_add_extra_outputs(lorawan-node)
