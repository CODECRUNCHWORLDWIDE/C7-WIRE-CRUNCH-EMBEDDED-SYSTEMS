# CMakeLists.txt — Week 13 mini-project: GATT occupancy peripheral + 3 mesh roles.
#
# Build:
#   mkdir build && cd build
#   cmake -G "Unix Makefiles" .. -DPICO_BOARD=pico_w
#   make -j8
#
# Produces:
#   gatt_sensor_peripheral.uf2  — the Step-2 standalone GATT peripheral
#   mesh_publisher.uf2          — mesh_node.c with -DNODE_ROLE=PUBLISHER (1)
#   mesh_subscriber.uf2         — mesh_node.c with -DNODE_ROLE=SUBSCRIBER (2)
#   mesh_relay.uf2              — mesh_node.c with -DNODE_ROLE=RELAY (3)

cmake_minimum_required(VERSION 3.13)

# The Pico W is required (the radio is on the W variant).
set(PICO_BOARD pico_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)   # standard SDK bootstrap (copy from pico-examples)

project(cc_week13_mesh C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- Step-2 standalone GATT peripheral ---------------------------------------
add_executable(gatt_sensor_peripheral
    gatt_sensor_peripheral.c
)
# Generate cc_sensor.h from cc_sensor.gatt with compile_gatt.py.
pico_btstack_make_gatt_header(gatt_sensor_peripheral
    ${CMAKE_CURRENT_LIST_DIR}/cc_sensor.gatt)
target_include_directories(gatt_sensor_peripheral PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gatt_sensor_peripheral
    pico_stdlib
    pico_btstack_ble
    pico_btstack_cyw43
    pico_cyw43_arch_none
    hardware_gpio
)
pico_enable_stdio_usb(gatt_sensor_peripheral 1)
pico_add_extra_outputs(gatt_sensor_peripheral)

# --- Mesh node: one source, three roles --------------------------------------
function(add_mesh_role target role_value enable_relay)
    add_executable(${target} mesh_node.c)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_definitions(${target} PRIVATE NODE_ROLE=${role_value})
    if(enable_relay)
        # The relay build advertises the relay feature in its Composition Data
        # so the provisioner can turn relaying on. (btstack_config.h gates the
        # mesh feature flags; ENABLE_MESH_RELAY must be defined for this build.)
        target_compile_definitions(${target} PRIVATE CC_MESH_RELAY_CAPABLE=1)
    endif()
    target_link_libraries(${target}
        pico_stdlib
        pico_btstack_ble
        pico_btstack_mesh
        pico_btstack_cyw43
        pico_cyw43_arch_none
        hardware_gpio
    )
    pico_enable_stdio_usb(${target} 1)
    pico_add_extra_outputs(${target})
endfunction()

add_mesh_role(mesh_publisher  1 FALSE)   # ROLE_PUBLISHER, PIR on GP16
add_mesh_role(mesh_subscriber 2 FALSE)   # ROLE_SUBSCRIBER
add_mesh_role(mesh_relay      3 TRUE)    # ROLE_RELAY, relay feature capable
