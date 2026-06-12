# CMakeLists.txt — Week 11 mini-project: Wi-Fi / MQTT 5 / TLS sensor node.
#
# Builds sensor-node.uf2 for the Pico W. Links the pico_cyw43_arch_lwip_poll
# stack (CYW43 driver + lwIP 2.2.0) and pico_mbedtls (the SDK's mbedTLS
# target) against our trimmed mbedtls_config.h and lwipopts.h.
#
# Usage:
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   mkdir build && cd build
#   cmake -DPICO_BOARD=pico_w \
#         -DWIFI_SSID="\"my-ap\"" \
#         -DWIFI_PASSWORD="\"my-password\"" ..
#   make -j$(nproc)
#   cp sensor-node.uf2 /Volumes/RPI-RP2      # or deploy via Week 10 OTA

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(cc7_week11_sensor_node C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- Fetch and embed the pinned CA root --------------------------------------
# The pinned root is security-critical; we embed the authoritative PEM rather
# than transcribing Base64 by hand. Prefer a vendored, checksum-verified copy
# in certs/; fall back to downloading it at configure time.
set(CA_PEM "${CMAKE_CURRENT_BINARY_DIR}/isrgrootx1.pem")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certs/isrgrootx1.pem")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/certs/isrgrootx1.pem"
                   "${CA_PEM}" COPYONLY)
else()
    message(STATUS "Downloading ISRG Root X1 from letsencrypt.org")
    file(DOWNLOAD
        "https://letsencrypt.org/certs/isrgrootx1.pem"
        "${CA_PEM}"
        EXPECTED_HASH SHA256=22b557a27055b33606b6559f37703928d3e4ad79f110b407d04986e1843543d1
        TLS_VERIFY ON
        STATUS dl_status)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        message(FATAL_ERROR "Could not fetch ISRG Root X1. Place it in certs/isrgrootx1.pem manually.")
    endif()
endif()

# Append a NUL so mbedtls_x509_crt_parse sees a terminated PEM, then xxd -i.
set(CA_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ca_root_pem.h")
add_custom_command(
    OUTPUT "${CA_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CA_PEM}" "${CA_PEM}.nul"
    COMMAND ${CMAKE_COMMAND} -E echo_append "" >> "${CA_PEM}.nul"
    COMMAND printf '\\0' >> "${CA_PEM}.nul"
    COMMAND bash -c "xxd -i -n isrgrootx1_pem '${CA_PEM}.nul' | sed 's/unsigned int .*_len/const unsigned int isrgrootx1_pem_len/;s/unsigned char/const unsigned char/' > '${CA_HEADER}'"
    DEPENDS "${CA_PEM}"
    COMMENT "Generating ca_root_pem.h from the pinned root"
    VERBATIM)
add_custom_target(gen_ca_header DEPENDS "${CA_HEADER}")

add_executable(sensor-node
    main.c
    mqtt_client.c
)
add_dependencies(sensor-node gen_ca_header)

# Pull in the generated CA header and the shared exercise header.
target_include_directories(sensor-node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # for ca_root_pem.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../exercises     # for wifi_common.h
)

# Wi-Fi credentials are compiled in via -D from the cmake command line.
target_compile_definitions(sensor-node PRIVATE
    WIFI_SSID=${WIFI_SSID}
    WIFI_PASSWORD=${WIFI_PASSWORD}
    # Point mbedTLS at our trimmed config.
    MBEDTLS_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_config.h"
)

target_link_libraries(sensor-node
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_mbedtls
    pico_unique_id
    pico_rand
)

# Console over USB-CDC (so a single cable carries power + logs).
pico_enable_stdio_usb(sensor-node 1)
pico_enable_stdio_uart(sensor-node 0)

pico_add_extra_outputs(sensor-node)
