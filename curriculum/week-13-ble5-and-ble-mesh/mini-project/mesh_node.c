/*
 * mesh_node.c — A BLE Mesh occupancy node for the Pico W.
 *
 * One source file, three roles, selected at build time with -DNODE_ROLE:
 *
 *   PUBLISHER  : hosts a PIR on PIR_GPIO and a Generic OnOff SERVER. On a
 *                motion edge it publishes a Generic OnOff Status to its
 *                configured publish address (the kitchen-lights group 0xC000).
 *                Relay feature OFF.
 *   SUBSCRIBER : hosts a Generic OnOff CLIENT subscribed to 0xC000; drives the
 *                onboard LED from the received state. Placed OUT of direct
 *                range of the publisher. Relay feature OFF.
 *   RELAY      : same as SUBSCRIBER (it also lights its LED), but with the
 *                relay feature ON. Placed between the publisher and the far
 *                subscriber so the message reaches the far node in two hops.
 *
 * The mesh stack is BTstack's pico_btstack_mesh. This node ships UNPROVISIONED:
 * it broadcasts an Unprovisioned Device beacon and waits for cc-mesh-provision.py
 * to provision it (unicast address, NetKey) and then configure it (AppKey bind,
 * publish/subscribe addresses, relay flag). None of those values are hardcoded
 * here — provisioning delivers them — which is the whole point of the mesh's
 * onboarding model (Lecture 3 §7).
 *
 * Build: Pico SDK + BTstack mesh, -DPICO_BOARD=pico_w -DNODE_ROLE=PUBLISHER
 * (or SUBSCRIBER / RELAY). btstack_config.h must enable ENABLE_MESH,
 * ENABLE_MESH_ADV_BEARER, and ENABLE_MESH_PROVISIONER is NOT needed (we are the
 * provisionee). The CMakeLists in the README builds all three.
 *
 * Citations:
 *   - Bluetooth Mesh Protocol 1.1 §3 (network, relay, TTL), §6 (provisioning).
 *   - Bluetooth Mesh Model 1.1 §3.2, §7.1 (Generic OnOff).
 *   - BTstack manual, "Mesh".
 */

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "ble_common.h"

/* -------------------------------------------------------------------------
 * Role selection. Exactly one of these must be defined at build time.
 * ----------------------------------------------------------------------- */
#define ROLE_PUBLISHER   1
#define ROLE_SUBSCRIBER  2
#define ROLE_RELAY       3

#ifndef NODE_ROLE
#error "Define NODE_ROLE to PUBLISHER, SUBSCRIBER, or RELAY at build time."
#endif

#if   NODE_ROLE == ROLE_PUBLISHER
#define ROLE_NAME "PUBLISHER"
#elif NODE_ROLE == ROLE_SUBSCRIBER
#define ROLE_NAME "SUBSCRIBER"
#elif NODE_ROLE == ROLE_RELAY
#define ROLE_NAME "RELAY"
#else
#error "NODE_ROLE must be PUBLISHER, SUBSCRIBER, or RELAY."
#endif

#define PIR_GPIO         16
#define STATUS_LED       CYW43_WL_GPIO_LED_PIN

/* A per-device-unique Device UUID. In production this is derived from the
 * chip's unique ID; here we tag the low bytes with the role so the provisioner
 * can tell the three nodes apart in its scan. cc-mesh-provision.py matches on
 * the last byte. */
static uint8_t device_uuid[16] = {
    0xCC, 0x7E, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)NODE_ROLE,
};

static btstack_packet_callback_registration_t hci_event_callback_registration;

/* Mesh model instances. The Generic OnOff server/client live on element 0. */
static mesh_model_t                 mesh_configuration_server_model;
static mesh_model_t                 mesh_health_server_model;
static mesh_model_t                 mesh_generic_on_off_model;
static mesh_generic_on_off_client_t generic_on_off_client_context;
static mesh_generic_on_off_state_t  generic_on_off_state;

/* -------------------------------------------------------------------------
 * Provisioning lifecycle: print progress, light the LED while unprovisioned so
 * a human can see which boards still need provisioning.
 * ----------------------------------------------------------------------- */

static void mesh_provisioning_message_handler(uint8_t packet_type,
                                              uint16_t channel,
                                              uint8_t *packet, uint16_t size) {
    (void) channel;
    (void) size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case MESH_SUBEVENT_PB_PROV_ATTENTION_TIMER:
            /* The provisioner asked us to "identify". Blink fast so the human
               can confirm which physical board is being provisioned. */
            printf("[mesh] identify: provisioner is talking to ME (%s)\n",
                   ROLE_NAME);
            break;
        case MESH_SUBEVENT_PB_PROV_COMPLETE:
            printf("[mesh] PROVISIONED. We now have a unicast address and the "
                   "NetKey. Awaiting configuration (AppKey bind, pub/sub).\n");
            cyw43_arch_gpio_put(STATUS_LED, 0);  /* stop the unprovisioned blink. */
            break;
        default:
            break;
    }
}

#if NODE_ROLE == ROLE_PUBLISHER
/* -------------------------------------------------------------------------
 * PUBLISHER: poll the PIR, and on an edge, publish the new OnOff state to the
 * model's configured publish address (set by the provisioner to 0xC000).
 * ----------------------------------------------------------------------- */
static btstack_timer_source_t pir_timer;
static uint8_t last_pir = 0xFF;

static void publish_on_off(uint8_t value) {
    /* Update our model's state. BTstack's generic-on-off server publishes the
       Status to the model's publication address when the state changes and a
       publication is configured. */
    generic_on_off_state.current_on_off_value = value ? 1 : 0;
    mesh_access_state_changed(&mesh_generic_on_off_model);
    printf("[pub] PIR edge -> OnOff %u; publishing OnOff Status to pub addr\n",
           value);
}

static void pir_poll(btstack_timer_source_t *ts) {
    uint8_t pir = gpio_get(PIR_GPIO) ? 1u : 0u;
    if (pir != last_pir) {
        last_pir = pir;
        publish_on_off(pir);
    }
    btstack_run_loop_set_timer(ts, 200);
    btstack_run_loop_add_timer(ts);
}
#endif /* PUBLISHER */

#if NODE_ROLE == ROLE_SUBSCRIBER || NODE_ROLE == ROLE_RELAY
/* -------------------------------------------------------------------------
 * SUBSCRIBER / RELAY: receive Generic OnOff Status messages sent to the group
 * address we subscribed to (0xC000), and drive the LED. The RELAY additionally
 * re-broadcasts every network message it hears (the relay feature, enabled by
 * the provisioner) — that re-broadcast is handled inside BTstack's network
 * layer, not here; here we only consume the application state.
 * ----------------------------------------------------------------------- */
static void on_off_status_handler(mesh_model_t *model, mesh_pdu_t *pdu) {
    (void) model;
    uint8_t present = mesh_access_parser_get_uint8(
        mesh_access_parser_init(pdu));
    cyw43_arch_gpio_put(STATUS_LED, present != 0u);
    printf("[%s] received OnOff Status = %u (src=0x%04x, ttl=%u) -> LED %s\n",
           ROLE_NAME, present, mesh_pdu_src(pdu), mesh_pdu_ttl(pdu),
           present ? "ON" : "off");
    mesh_access_message_processed(pdu);
}
#endif /* SUBSCRIBER || RELAY */

/* -------------------------------------------------------------------------
 * Bring up the mesh stack and register the models for this role.
 * ----------------------------------------------------------------------- */

static void mesh_setup(void) {
    /* Core mesh init: network layer, transport, provisioning bearer. */
    mesh_init();

    /* Provisioning: we are the provisionee (device), not the provisioner. */
    mesh_register_provisioning_device_packet_handler(
        &mesh_provisioning_message_handler);
    mesh_node_set_device_uuid(device_uuid);

    /* Element 0 is created by mesh_init(). Add the mandatory foundation
       models, then this role's application model. */
    mesh_element_t *primary = mesh_node_get_primary_element();

    mesh_configuration_server_model.model_identifier =
        mesh_model_get_model_identifier_bluetooth_sig(MESH_MODEL_CONFIG_SERVER);
    mesh_configuration_server_register_model(&mesh_configuration_server_model);
    mesh_element_add_model(primary, &mesh_configuration_server_model);

    mesh_health_server_model.model_identifier =
        mesh_model_get_model_identifier_bluetooth_sig(MESH_MODEL_HEALTH_SERVER);
    mesh_health_server_register_model(&mesh_health_server_model);
    mesh_element_add_model(primary, &mesh_health_server_model);

#if NODE_ROLE == ROLE_PUBLISHER
    /* Generic OnOff SERVER: owns the occupancy state, publishes on change. */
    mesh_generic_on_off_model.model_identifier =
        mesh_model_get_model_identifier_bluetooth_sig(MESH_MODEL_GENERIC_ONOFF_SERVER);
    mesh_generic_on_off_server_register_model(&mesh_generic_on_off_model,
                                              &generic_on_off_state);
    mesh_element_add_model(primary, &mesh_generic_on_off_model);
    printf("[setup] role=PUBLISHER, Generic OnOff Server on element 0\n");
#else
    /* Generic OnOff CLIENT: subscribes to the group, receives Status. */
    mesh_generic_on_off_model.model_identifier =
        mesh_model_get_model_identifier_bluetooth_sig(MESH_MODEL_GENERIC_ONOFF_CLIENT);
    mesh_generic_on_off_client_register_model(&mesh_generic_on_off_model,
                                              &generic_on_off_client_context);
    mesh_generic_on_off_client_set_status_handler(&mesh_generic_on_off_model,
                                                  &on_off_status_handler);
    mesh_element_add_model(primary, &mesh_generic_on_off_model);
    printf("[setup] role=%s, Generic OnOff Client on element 0\n", ROLE_NAME);
#endif

    /* The relay/proxy/friend feature flags are NOT set here — the provisioner
       sets them via the Configuration Server during configuration. We only
       declare the node's *capability* to relay so the provisioner may enable
       it (Mesh Protocol 1.1 §4.4.2 Composition Data, "Features"). BTstack
       reports the compiled-in features; ENABLE_MESH_RELAY in btstack_config.h
       must be set on the RELAY build so the provisioner can turn it on. */
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init() != 0) {
        printf("[mesh] cyw43_arch_init failed\n");
        return -1;
    }

    printf("\n=== CC Mesh Node: role=%s ===\n", ROLE_NAME);

#if NODE_ROLE == ROLE_PUBLISHER
    gpio_init(PIR_GPIO);
    gpio_set_dir(PIR_GPIO, GPIO_IN);
    gpio_pull_down(PIR_GPIO);
#endif

    /* Light the LED to indicate "unprovisioned, waiting" until provisioning
       completes (the provisioning handler clears it). */
    cyw43_arch_gpio_put(STATUS_LED, 1);

    l2cap_init();
    sm_init();
    mesh_setup();

    hci_event_callback_registration.callback = &mesh_provisioning_message_handler;
    hci_add_event_handler(&hci_event_callback_registration);

#if NODE_ROLE == ROLE_PUBLISHER
    btstack_run_loop_set_timer_handler(&pir_timer, &pir_poll);
    btstack_run_loop_set_timer(&pir_timer, 200);
    btstack_run_loop_add_timer(&pir_timer);
#endif

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}

/* End of mesh_node.c. */
