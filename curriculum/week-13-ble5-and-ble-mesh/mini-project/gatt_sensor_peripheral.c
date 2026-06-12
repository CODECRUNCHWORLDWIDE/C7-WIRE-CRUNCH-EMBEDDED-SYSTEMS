/*
 * gatt_sensor_peripheral.c — A standalone GATT sensor peripheral for the Pico W.
 *
 * This is the Step-2 sanity board: before any mesh is wired up, you flash this,
 * connect with nRF Connect (or the Exercise 2 client), and confirm the whole
 * GATT-server path works end to end — advertising, connection, service/
 * characteristic discovery, a readable+notifiable occupancy state, and a
 * writable LED-control characteristic. Once this works, the mesh node reuses the
 * same occupancy-sensing and LED-driving code with a mesh model instead of GATT.
 *
 * The GATT database comes from cc_sensor.gatt, compiled by BTstack's
 * compile_gatt.py via the SDK's pico_btstack_make_gatt_header(). That generates
 * cc_sensor.h with the profile_data[] table and the *_VALUE_HANDLE constants
 * referenced below.
 *
 * Custom service (128-bit UUID generated once with uuidgen):
 *   Service     A1B2C3D4-0000-1000-8000-00805F9B34FB
 *   Occupancy   A1B2C3D4-0001-1000-8000-00805F9B34FB  READ | NOTIFY  (1 byte)
 *   LED control A1B2C3D4-0002-1000-8000-00805F9B34FB  WRITE          (1 byte)
 *
 * Build: Pico SDK + BTstack, -DPICO_BOARD=pico_w. CMakeLists snippet in the
 * mini-project README. btstack_config.h must enable ENABLE_BLE and
 * ENABLE_LE_PERIPHERAL.
 *
 * Citations:
 *   - Core Spec 5.4 Vol 3 Part G (GATT server, characteristics, CCCD).
 *   - BTstack manual, "GATT Server".
 *   - Pico W connectivity guide Ch. 4 (BLE on the Pico W).
 */

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "cc_sensor.h"   /* generated from cc_sensor.gatt by compile_gatt.py */
#include "ble_common.h"

/* The PIR occupancy sensor's digital output. Active-high on motion. */
#define PIR_GPIO            16
#define STATUS_LED_GPIO     CYW43_WL_GPIO_LED_PIN  /* the onboard "LED". */

/* -------------------------------------------------------------------------
 * Server state.
 * ----------------------------------------------------------------------- */

static uint8_t  g_occupancy_state   = 0;   /* 0 = clear, 1 = occupied.       */
static int      g_notifications_on  = 0;   /* set when a client writes CCCD.  */
static hci_con_handle_t g_con_handle = HCI_CON_HANDLE_INVALID;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t                 poll_timer;

/* The advertising payload: Flags + name + the custom 128-bit service UUID. */
static const uint8_t adv_data[] = {
    /* Flags: general discoverable, BR/EDR not supported. */
    0x02, BLE_AD_FLAGS, BLE_FLAG_LE_GENERAL_DISCOVERABLE | BLE_FLAG_BR_EDR_NOT_SUPPORTED,
    /* Complete local name "CC Occupancy". */
    0x0D, BLE_AD_COMPLETE_LOCAL_NAME,
        'C','C',' ','O','c','c','u','p','a','n','c','y',
    /* Incomplete 128-bit UUID list (our custom service), little-endian. */
    0x11, BLE_AD_COMPLETE_128BIT_UUIDS,
        0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,
        0x00,0x10,0x00,0x00,0xD4,0xC3,0xB2,0xA1,
};
static const uint8_t adv_data_len = sizeof(adv_data);

/* -------------------------------------------------------------------------
 * ATT read callback: serve the occupancy value.
 * ----------------------------------------------------------------------- */

static uint16_t att_read_callback(hci_con_handle_t con_handle,
                                  uint16_t att_handle, uint16_t offset,
                                  uint8_t *buffer, uint16_t buffer_size) {
    (void) con_handle;
    if (att_handle == ATT_CHARACTERISTIC_A1B2C3D4_0001_1000_8000_00805F9B34FB_01_VALUE_HANDLE) {
        return att_read_callback_handle_blob(&g_occupancy_state, 1u,
                                             offset, buffer, buffer_size);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * ATT write callback: handle the LED-control write and the CCCD subscribe.
 * ----------------------------------------------------------------------- */

static int att_write_callback(hci_con_handle_t con_handle, uint16_t att_handle,
                              uint16_t transaction_mode, uint16_t offset,
                              uint8_t *buffer, uint16_t buffer_size) {
    (void) con_handle;
    (void) transaction_mode;
    (void) offset;

    if (att_handle == ATT_CHARACTERISTIC_A1B2C3D4_0002_1000_8000_00805F9B34FB_02_VALUE_HANDLE) {
        if (buffer_size >= 1u) {
            cyw43_arch_gpio_put(STATUS_LED_GPIO, buffer[0] != 0u);
            printf("[peri] LED control write: %u\n", buffer[0]);
        }
        return 0;
    }
    if (att_handle == ATT_CHARACTERISTIC_A1B2C3D4_0001_1000_8000_00805F9B34FB_01_CLIENT_CONFIGURATION_HANDLE) {
        g_notifications_on = little_endian_read_16(buffer, 0) != 0;
        printf("[peri] occupancy notifications %s\n",
               g_notifications_on ? "enabled" : "disabled");
        return 0;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Poll the PIR; on a state change, update the value and notify subscribers.
 * Runs every 200 ms off a BTstack timer (no FreeRTOS needed for this board).
 * ----------------------------------------------------------------------- */

static void poll_handler(btstack_timer_source_t *ts) {
    uint8_t pir = gpio_get(PIR_GPIO) ? 1u : 0u;
    if (pir != g_occupancy_state) {
        g_occupancy_state = pir;
        printf("[peri] occupancy -> %s\n", pir ? "OCCUPIED" : "clear");
        if (g_notifications_on && g_con_handle != HCI_CON_HANDLE_INVALID) {
            att_server_notify(g_con_handle,
                ATT_CHARACTERISTIC_A1B2C3D4_0001_1000_8000_00805F9B34FB_01_VALUE_HANDLE,
                &g_occupancy_state, 1u);
        }
    }
    btstack_run_loop_set_timer(ts, 200);
    btstack_run_loop_add_timer(ts);
}

/* -------------------------------------------------------------------------
 * HCI event handler: connection lifecycle and (re)advertising.
 * ----------------------------------------------------------------------- */

static void packet_handler(uint8_t packet_type, uint16_t channel,
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
            /* Configure and start advertising. */
            {
                uint16_t adv_int_min = 0x0030;  /* 30 ms. */
                uint16_t adv_int_max = 0x0030;
                bd_addr_t null_addr = {0};
                gap_advertisements_set_params(adv_int_min, adv_int_max, 0, 0,
                                              null_addr, 0x07, 0x00);
                gap_advertisements_set_data(adv_data_len, (uint8_t *)adv_data);
                gap_advertisements_enable(1);
                printf("[peri] advertising as 'CC Occupancy'\n");
            }
            break;

        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) ==
                HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                g_con_handle =
                    hci_subevent_le_connection_complete_get_connection_handle(packet);
                printf("[peri] connected (handle 0x%04x)\n", g_con_handle);
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            printf("[peri] disconnected; re-advertising\n");
            g_con_handle = HCI_CON_HANDLE_INVALID;
            g_notifications_on = 0;
            gap_advertisements_enable(1);
            break;

        default:
            break;
    }
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init() != 0) {
        printf("[peri] cyw43_arch_init failed\n");
        return -1;
    }

    gpio_init(PIR_GPIO);
    gpio_set_dir(PIR_GPIO, GPIO_IN);
    gpio_pull_down(PIR_GPIO);

    /* Host-stack bring-up. */
    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(
        SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    att_server_init(profile_data, att_read_callback, att_write_callback);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    att_server_register_packet_handler(packet_handler);

    /* Start the PIR-poll timer. */
    btstack_run_loop_set_timer_handler(&poll_timer, &poll_handler);
    btstack_run_loop_set_timer(&poll_timer, 200);
    btstack_run_loop_add_timer(&poll_timer);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}

/* End of gatt_sensor_peripheral.c. */
