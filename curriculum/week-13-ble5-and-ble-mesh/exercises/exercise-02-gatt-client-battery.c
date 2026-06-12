/*
 * exercise-02-gatt-client-battery.c — A BLE GATT client on the Pico W.
 *
 * This firmware turns the Pico W into a CENTRAL that:
 *   1. Scans for advertising peripherals.
 *   2. Connects to the first one that advertises the Battery Service (0x180F).
 *   3. Discovers the Battery Service, then the Battery Level characteristic
 *      (0x2A19).
 *   4. Reads the Battery Level once.
 *   5. Subscribes to notifications (writes 0x0001 to the CCCD) and prints each
 *      update.
 *
 * It is the mirror image of a GATT server: where a server publishes an
 * attribute table and waits, a client connects and walks that table. Everything
 * in BTstack's gatt_client API is ASYNCHRONOUS — you kick off an operation and
 * a later event tells you it finished. The single most common bug this week is
 * starting the next operation before the previous one's *_QUERY_COMPLETE event
 * has arrived. We drive a small explicit state machine to make the ordering
 * impossible to get wrong.
 *
 * Build: against the Pico SDK with BTstack. CMakeLists snippet:
 *
 *   add_executable(gatt_client_battery exercise-02-gatt-client-battery.c)
 *   target_link_libraries(gatt_client_battery
 *       pico_stdlib pico_btstack_ble pico_btstack_cyw43 pico_cyw43_arch_none)
 *   target_include_directories(gatt_client_battery PRIVATE ${CMAKE_CURRENT_LIST_DIR})
 *   pico_add_extra_outputs(gatt_client_battery)
 *
 * with -DPICO_BOARD=pico_w and a btstack_config.h on the include path that
 * enables ENABLE_BLE, ENABLE_LE_CENTRAL, and ENABLE_GATT_CLIENT.
 *
 * Citations:
 *   - Core Spec 5.4 Vol 3 Part G §4.4 (Primary Service Discovery),
 *     §4.6 (Characteristic Discovery), §4.8 (Read), §4.10 (Notifications).
 *   - BTstack manual, "GATT Client".
 *   - Assigned Numbers: Battery Service 0x180F, Battery Level 0x2A19.
 */

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "ble_common.h"

/* -------------------------------------------------------------------------
 * Client state machine. The ordering constraint that bites everyone:
 * you cannot start characteristic discovery until service discovery has
 * emitted GATT_EVENT_QUERY_COMPLETE.
 * ----------------------------------------------------------------------- */

typedef enum {
    TC_OFF,
    TC_IDLE,
    TC_W4_SCAN_RESULT,
    TC_W4_CONNECT,
    TC_W4_SERVICE_RESULT,
    TC_W4_CHARACTERISTIC_RESULT,
    TC_W4_READ_RESULT,
    TC_W4_ENABLE_NOTIFICATIONS_COMPLETE,
    TC_W4_NOTIFICATION,
} gc_state_t;

static gc_state_t                       state = TC_OFF;
static bd_addr_t                        server_addr;
static bd_addr_type_t                   server_addr_type;
static hci_con_handle_t                 connection_handle;

static gatt_client_service_t            battery_service;
static gatt_client_characteristic_t     battery_level_char;
static gatt_client_notification_t       notification_listener;

static btstack_packet_callback_registration_t hci_event_callback_registration;

/* -------------------------------------------------------------------------
 * Does this advertisement carry the Battery Service UUID? We re-use the AD
 * parser logic from Exercise 1 (here inlined against the iterator BTstack
 * provides) to scan the Complete/Incomplete 16-bit UUID lists.
 * ----------------------------------------------------------------------- */

static bool advertisement_has_battery_service(const uint8_t *adv_data,
                                              uint8_t adv_len) {
    ad_context_t context;
    for (ad_iterator_init(&context, adv_len, adv_data);
         ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        uint8_t type = ad_iterator_get_data_type(&context);
        uint8_t len  = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);

        if (type == BLE_AD_COMPLETE_16BIT_UUIDS ||
            type == BLE_AD_INCOMPLETE_16BIT_UUIDS) {
            for (uint8_t i = 0u; (i + 1u) < len; i += 2u) {
                if (cc_read_le16(&data[i]) == UUID16_BATTERY_SERVICE) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Forward declaration. */
static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel,
                                     uint8_t *packet, uint16_t size);

/* -------------------------------------------------------------------------
 * Top-level HCI/GAP event handler: scanning, connect, disconnect.
 * ----------------------------------------------------------------------- */

static void hci_event_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void) channel;
    (void) size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) {
                break;
            }
            /* Controller is up. Start active scanning on the 1M PHY. */
            printf("[gc] HCI up. Scanning for a Battery Service peripheral...\n");
            state = TC_W4_SCAN_RESULT;
            gap_set_scan_parameters(1 /* active */, 0x0030, 0x0030);
            gap_start_scan();
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (state != TC_W4_SCAN_RESULT) {
                break;
            }
            const uint8_t *adv_data = gap_event_advertising_report_get_data(packet);
            uint8_t adv_len = gap_event_advertising_report_get_data_length(packet);
            if (!advertisement_has_battery_service(adv_data, adv_len)) {
                break;
            }
            /* Found one. Stop scanning and connect. */
            gap_event_advertising_report_get_address(packet, server_addr);
            server_addr_type =
                (bd_addr_type_t) gap_event_advertising_report_get_address_type(packet);
            printf("[gc] found %s, connecting...\n", bd_addr_to_str(server_addr));
            state = TC_W4_CONNECT;
            gap_stop_scan();
            gap_connect(server_addr, server_addr_type);
            break;
        }

        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) !=
                HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                break;
            }
            connection_handle =
                hci_subevent_le_connection_complete_get_connection_handle(packet);
            printf("[gc] connected (handle 0x%04x). Discovering services...\n",
                   connection_handle);
            state = TC_W4_SERVICE_RESULT;
            /* Discover only the Battery Service, not every service. */
            gatt_client_discover_primary_services_by_uuid16(
                handle_gatt_client_event, connection_handle,
                UUID16_BATTERY_SERVICE);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            printf("[gc] disconnected. Re-scanning.\n");
            state = TC_W4_SCAN_RESULT;
            gap_start_scan();
            break;

        default:
            break;
    }
}

/* -------------------------------------------------------------------------
 * GATT client event handler: the result of each async query lands here.
 * ----------------------------------------------------------------------- */

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel,
                                     uint8_t *packet, uint16_t size) {
    (void) packet_type;
    (void) channel;
    (void) size;

    switch (state) {
        case TC_W4_SERVICE_RESULT:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_SERVICE_QUERY_RESULT:
                    gatt_event_service_query_result_get_service(packet,
                                                                &battery_service);
                    printf("[gc] Battery Service: handles 0x%04x-0x%04x\n",
                           battery_service.start_group_handle,
                           battery_service.end_group_handle);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    if (gatt_event_query_complete_get_att_status(packet) !=
                        ATT_ERROR_SUCCESS) {
                        printf("[gc] service discovery failed; disconnecting.\n");
                        gap_disconnect(connection_handle);
                        break;
                    }
                    /* Now — and only now — discover the characteristic. */
                    state = TC_W4_CHARACTERISTIC_RESULT;
                    gatt_client_discover_characteristics_for_service_by_uuid16(
                        handle_gatt_client_event, connection_handle,
                        &battery_service, UUID16_BATTERY_LEVEL);
                    break;
                default:
                    break;
            }
            break;

        case TC_W4_CHARACTERISTIC_RESULT:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    gatt_event_characteristic_query_result_get_characteristic(
                        packet, &battery_level_char);
                    printf("[gc] Battery Level char: value handle 0x%04x, "
                           "properties 0x%02x\n",
                           battery_level_char.value_handle,
                           battery_level_char.properties);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    if (battery_level_char.value_handle == 0u) {
                        printf("[gc] no Battery Level characteristic found.\n");
                        gap_disconnect(connection_handle);
                        break;
                    }
                    state = TC_W4_READ_RESULT;
                    gatt_client_read_value_of_characteristic(
                        handle_gatt_client_event, connection_handle,
                        &battery_level_char);
                    break;
                default:
                    break;
            }
            break;

        case TC_W4_READ_RESULT:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
                    const uint8_t *value =
                        gatt_event_characteristic_value_query_result_get_value(packet);
                    uint16_t len =
                        gatt_event_characteristic_value_query_result_get_value_length(packet);
                    if (len >= 1u) {
                        printf("[gc] Battery Level = %u%%\n", value[0]);
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE:
                    /* If the characteristic supports notify, subscribe. */
                    if (battery_level_char.properties & GATT_PROP_NOTIFY) {
                        state = TC_W4_ENABLE_NOTIFICATIONS_COMPLETE;
                        gatt_client_listen_for_characteristic_value_updates(
                            &notification_listener, handle_gatt_client_event,
                            connection_handle, &battery_level_char);
                        gatt_client_write_client_characteristic_configuration(
                            handle_gatt_client_event, connection_handle,
                            &battery_level_char,
                            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                    } else {
                        printf("[gc] characteristic is read-only; done.\n");
                        gap_disconnect(connection_handle);
                    }
                    break;
                default:
                    break;
            }
            break;

        case TC_W4_ENABLE_NOTIFICATIONS_COMPLETE:
            if (hci_event_packet_get_type(packet) == GATT_EVENT_QUERY_COMPLETE) {
                printf("[gc] notifications enabled. Listening for updates...\n");
                state = TC_W4_NOTIFICATION;
            }
            break;

        case TC_W4_NOTIFICATION:
            if (hci_event_packet_get_type(packet) == GATT_EVENT_NOTIFICATION) {
                const uint8_t *value =
                    gatt_event_notification_get_value(packet);
                uint16_t len = gatt_event_notification_get_value_length(packet);
                if (len >= 1u) {
                    printf("[gc] notification: Battery Level = %u%%\n", value[0]);
                }
            }
            break;

        default:
            break;
    }
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init() != 0) {
        printf("[gc] cyw43_arch_init failed\n");
        return -1;
    }

    /* Bring up L2CAP, the Security Manager, and the GATT client. */
    l2cap_init();
    sm_init();
    gatt_client_init();

    /* Register for HCI/GAP events. */
    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    state = TC_IDLE;
    hci_power_control(HCI_POWER_ON);

    /* The cyw43_arch_none run loop: BTstack drives everything from callbacks. */
    btstack_run_loop_execute();
    return 0;
}

/* End of exercise-02-gatt-client-battery.c. */
