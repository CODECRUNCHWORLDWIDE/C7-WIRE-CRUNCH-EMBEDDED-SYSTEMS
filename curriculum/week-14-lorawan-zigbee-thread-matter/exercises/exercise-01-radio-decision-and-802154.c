/*
 * exercise-01-radio-decision-and-802154.c — Pick the right radio; parse a
 * 15.4 frame control field.
 *
 * This is a host-side exercise. It runs on your laptop, not the Pico:
 *
 *   cc -std=c11 -Wall -Wextra -I. exercise-01-radio-decision-and-802154.c -o ex1
 *   ./ex1
 *
 * Part A turns the radio-selection argument from Lecture 1 into a small
 * scoring routine and runs three product profiles through it. The point is
 * not that the weights are sacred — they are not — but that "which radio?"
 * is a decision with explicit inputs (range, duty cycle, payload, node
 * count, IP requirement, power budget, smart-home requirement) and you can
 * reason about it numerically instead of by vibes.
 *
 * Part B parses the 16-bit Frame Control Field that every 802.15.4 frame
 * starts with — the layer Zigbee and Thread share. You will see this field
 * in every Wireshark capture you take in the conceptual labs this week.
 *
 * Citations:
 *   - Lecture 1, "Choosing the radio" decision table.
 *   - IEEE 802.15.4-2020 §7.2.2.1 (Frame Control Field), Figure 7-2.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "lpwan_common.h"

/* =========================================================================
 * Part A — radio selection.
 *
 * The decision logic, in plain English, from Lecture 1:
 *
 *   - If the node must be reachable over kilometres on a battery and only
 *     talks occasionally (tens of bytes, minutes-to-hours apart), LoRaWAN
 *     is the only radio in this set that closes the link. Nothing else
 *     reaches that far at that power.
 *   - If the node must be internet-addressable (IPv6) and/or pair with a
 *     consumer smart-home ecosystem, Thread is the answer — it is the radio
 *     Matter standardised on for low-power mesh.
 *   - If you need a dense local mesh of mostly-mains-powered nodes
 *     (lighting, building automation) with no IP requirement and no
 *     long-range requirement, Zigbee remains a fine, mature choice.
 *   - If you need phone-proximity interaction (a wearable, a beacon, a
 *     commissioning flow) at short range, BLE Mesh wins — but that was
 *     Week 13's topic, so here we only return it when nothing else fits.
 * ===================================================================== */

radio_choice_t cc_choose_radio(const product_profile_t *profile) {
    if (profile == NULL) {
        return RADIO_NONE;
    }

    /*
     * Rule 1 — long range on battery, low duty cycle. This is LoRaWAN's
     * home turf and nothing else competes. The 802.15.4 mesh radios top out
     * around 100-300 m per hop; LoRaWAN reaches 2-15 km. If you need more
     * than ~1 km and you are on a battery, you are choosing LoRaWAN.
     */
    if (profile->range_m > 1000u && profile->needs_mains == 0u) {
        /* LoRaWAN airtime caps payloads hard. If the product wants to send
         * big messages frequently, LoRaWAN cannot serve it regardless of
         * range — flag it as a design conflict by returning NONE. */
        if (profile->payload_bytes > 51u || profile->uplink_period_s < 30u) {
            return RADIO_NONE;  /* duty-cycle / payload budget cannot close. */
        }
        return RADIO_LORAWAN;
    }

    /*
     * Rule 2 — must speak IP or join a smart-home ecosystem. Thread is the
     * low-power mesh that carries IPv6 and that Matter runs over. If either
     * the IP flag or the smart-home flag is set, pick Thread.
     */
    if (profile->needs_ip || profile->smart_home) {
        return RADIO_THREAD;
    }

    /*
     * Rule 3 — dense local mesh, no IP, mostly mains-powered. Zigbee is the
     * incumbent for lighting and building automation. We pick it for a large
     * node count at short range with no IP requirement.
     */
    if (profile->node_count >= 20u && profile->range_m <= 300u) {
        return RADIO_ZIGBEE;
    }

    /*
     * Rule 4 — short range, small mesh, phone-adjacent. Fall back to BLE
     * Mesh (covered in Week 13). This is the catch-all for the "near a
     * phone, few nodes" shape.
     */
    if (profile->range_m <= 100u) {
        return RADIO_BLE_MESH;
    }

    return RADIO_NONE;
}

static const char *radio_name(radio_choice_t r) {
    switch (r) {
    case RADIO_LORAWAN:  return "LoRaWAN";
    case RADIO_ZIGBEE:   return "Zigbee";
    case RADIO_THREAD:   return "Thread";
    case RADIO_BLE_MESH: return "BLE Mesh";
    default:             return "NONE (design conflict)";
    }
}

/* =========================================================================
 * Part B — 802.15.4 Frame Control Field.
 *
 * The FCF is the first two bytes of every 802.15.4 MAC frame (little-endian
 * on the wire). The low 3 bits are the frame type. IEEE 802.15.4-2020
 * §7.2.2.1, Figure 7-2.
 * ===================================================================== */

const char *cc_ieee802154_frame_type_name(uint16_t fcf) {
    switch (fcf & IEEE802154_FCF_FRAME_TYPE_MASK) {
    case IEEE802154_FRAME_TYPE_BEACON:  return "Beacon";
    case IEEE802154_FRAME_TYPE_DATA:    return "Data";
    case IEEE802154_FRAME_TYPE_ACK:     return "Acknowledgment";
    case IEEE802154_FRAME_TYPE_MAC_CMD: return "MAC Command";
    default:                            return "Reserved";
    }
}

static void print_fcf(const char *label, uint16_t fcf) {
    printf("  %-22s FCF=0x%04X type=%-14s [%s%s%s%s]\n",
           label, fcf,
           cc_ieee802154_frame_type_name(fcf),
           (fcf & IEEE802154_FCF_SECURITY_ENABLED)   ? "SEC "    : "",
           (fcf & IEEE802154_FCF_FRAME_PENDING)      ? "PEND "   : "",
           (fcf & IEEE802154_FCF_ACK_REQUEST)        ? "ACKREQ " : "",
           (fcf & IEEE802154_FCF_PAN_ID_COMPRESSION) ? "PANC"    : "");
}

/* =========================================================================
 * Driver.
 * ===================================================================== */

static void run_profile(const char *name, const product_profile_t *p) {
    radio_choice_t choice = cc_choose_radio(p);
    printf("  %-28s -> %s\n", name, radio_name(choice));
}

int main(void) {
    printf("=== Exercise 1: radio decision + 802.15.4 FCF ===\n\n");

    printf("Part A — radio selection\n");

    /* A soil-moisture sensor in a vineyard: 8 km to the gateway, on a
     * battery, sends 6 bytes every 15 minutes. Textbook LoRaWAN. */
    product_profile_t vineyard = {
        .range_m = 8000u, .uplink_period_s = 900u, .payload_bytes = 6u,
        .node_count = 200u, .needs_ip = 0u, .needs_mains = 0u, .smart_home = 0u,
    };
    run_profile("Vineyard soil sensor", &vineyard);

    /* A smart light bulb: 10 m to the hub, mains-powered, must pair with
     * Apple Home / Google Home. Matter-over-Thread. */
    product_profile_t bulb = {
        .range_m = 10u, .uplink_period_s = 1u, .payload_bytes = 12u,
        .node_count = 30u, .needs_ip = 1u, .needs_mains = 1u, .smart_home = 1u,
    };
    run_profile("Smart light bulb", &bulb);

    /* A warehouse lighting retrofit: 50 m max, mostly mains, 400 fixtures,
     * no IP, no consumer ecosystem. Zigbee. */
    product_profile_t warehouse = {
        .range_m = 50u, .uplink_period_s = 5u, .payload_bytes = 4u,
        .node_count = 400u, .needs_ip = 0u, .needs_mains = 1u, .smart_home = 0u,
    };
    run_profile("Warehouse lighting", &warehouse);

    /* A "long range AND big frequent payload on battery" request that no
     * radio in this set can satisfy — the function flags it. */
    product_profile_t impossible = {
        .range_m = 5000u, .uplink_period_s = 5u, .payload_bytes = 200u,
        .node_count = 10u, .needs_ip = 0u, .needs_mains = 0u, .smart_home = 0u,
    };
    run_profile("Long-range HD video (bad)", &impossible);

    printf("\nPart B — 802.15.4 frame control fields\n");

    /* A Thread MAC data frame requesting an ACK, security enabled. */
    print_fcf("Thread data frame",  0x0869u);
    /* A Zigbee beacon. */
    print_fcf("Zigbee beacon",      0x8000u);
    /* A bare acknowledgment frame. */
    print_fcf("802.15.4 ACK",       0x0002u);
    /* A MAC command (e.g., association request). */
    print_fcf("MAC command",        0x0023u);

    printf("\nAll checks complete. If the four Part A verdicts read\n");
    printf("LoRaWAN / Thread / Zigbee / NONE, your decision logic matches\n");
    printf("Lecture 1.\n");
    return 0;
}

/* End of exercise-01-radio-decision-and-802154.c. */
