# CMakeLists.txt — Week 15 mini-project: tilt-stabilized platform.
#
# Builds fusion_node.uf2 from fusion_node.c + madgwick.c + actuators.c.
# Requires PICO_SDK_PATH in the environment.
#
#   mkdir build && cd build
#   cmake -G "Unix Makefiles" ..
#   make -j8
#   # drag fusion_node.uf2 onto the BOOTSEL drive

cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(cc_week15_fusion C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(fusion_node
    fusion_node.c
    madgwick.c
    actuators.c
)

# The shared imu_common.h lives in ../exercises.
target_include_directories(fusion_node PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../exercises
)

target_link_libraries(fusion_node
    pico_stdlib
    hardware_i2c
    hardware_pwm
    hardware_clocks
    hardware_watchdog
)

# Telemetry over USB CDC, not UART.
pico_enable_stdio_usb(fusion_node 1)
pico_enable_stdio_uart(fusion_node 0)

pico_add_extra_outputs(fusion_node)

# Optimize for size+speed; the soft-float math benefits from -O2.
target_compile_options(fusion_node PRIVATE -O2 -Wall -Wextra)
