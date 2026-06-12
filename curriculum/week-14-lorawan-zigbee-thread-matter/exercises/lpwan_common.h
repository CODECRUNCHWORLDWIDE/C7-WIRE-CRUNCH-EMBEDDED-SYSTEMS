/*
 * lpwan_common.h — Shared declarations for Week 14 exercises and mini-project.
 *
 * This header captures the constants you need across the LoRa/LoRaWAN
 * exercises and the mini-project. The SX126x register and command opcodes
 * come straight from the Semtech datasheet; the LoRaWAN field layouts come
 * from the LoRa Alliance specification; the 802.15.4 / Thread / Matter
 * constants come from the respective specs cited inline.
 *
 * Everything compiles on the host (so Exercise 1 and Exercise 3 run on your
 * laptop) and on the RP2040 + Pico SDK (so Exercise 2 and the mini-project
 * run on the bench). There is no platform-specific code in this header; the
 * SPI access lives in the .c files.
 *
 * Citations:
 *   - Semtech SX1261/2 datasheet rev. 2.1 (DS.SX1261-2.W.APP), §13 "Commands",
 *     §6.1 "Modem Configuration".
 *   - LoRaWAN L2 1.0.4 Specification (LoRa Alliance TS001-1.0.4), §4 "MAC
 *     Message Formats", §6 "End-Device Activation".
 *   - LoRaWAN Regional Parameters RP002-1.0.4, "EU868" and "US915" channel plans.
 *   - IEEE 802.15.4-2020, §7 "MAC frame formats".
 *   - Thread 1.3.0 Specification, §3 "Network Layer", §5 "Mesh Link
 *     Establishment".
 *   - Matter 1.3 Core Specification, §4 "Secure Channel", §13 "Data Model".
 */

#ifndef CC_LPWAN_COMMON_H_
#define CC_LPWAN_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * SX126x (SX1261 / SX1262) — the LoRa transceiver we drive over SPI.
 *
 * The Pico has no radio of its own. We hang a Semtech SX1262 module (the
 * Waveshare "SX1262 LoRa Node (HF)" or the RAKwireless RAK3172 in transparent
 * mode) off SPI0. The SX126x speaks a command/response protocol over SPI:
 * the host asserts NSS, sends an opcode byte, sends or reads parameters, and
 * deasserts NSS. The chip signals "I have data / I am done" on the BUSY line
 * and on the DIO1 interrupt pin. Datasheet §13 lists every opcode.
 * ===================================================================== */

/* SPI command opcodes (Semtech SX1261/2 datasheet §13, Table 13-1). */
#define SX126X_CMD_SET_SLEEP                  0x84u
#define SX126X_CMD_SET_STANDBY                0x80u
#define SX126X_CMD_SET_FS                     0xC1u
#define SX126X_CMD_SET_TX                     0x83u
#define SX126X_CMD_SET_RX                     0x82u
#define SX126X_CMD_SET_RX_DUTY_CYCLE          0x94u
#define SX126X_CMD_SET_CAD                    0xC5u
#define SX126X_CMD_SET_PACKET_TYPE            0x8Au
#define SX126X_CMD_GET_PACKET_TYPE            0x11u
#define SX126X_CMD_SET_RF_FREQUENCY           0x86u
#define SX126X_CMD_SET_PA_CONFIG              0x95u
#define SX126X_CMD_SET_TX_PARAMS              0x8Eu
#define SX126X_CMD_SET_MODULATION_PARAMS      0x8Bu
#define SX126X_CMD_SET_PACKET_PARAMS          0x8Cu
#define SX126X_CMD_SET_BUFFER_BASE_ADDRESS    0x8Fu
#define SX126X_CMD_WRITE_BUFFER               0x0Eu
#define SX126X_CMD_READ_BUFFER                0x1Eu
#define SX126X_CMD_WRITE_REGISTER             0x0Du
#define SX126X_CMD_READ_REGISTER              0x1Du
#define SX126X_CMD_SET_DIO_IRQ_PARAMS         0x08u
#define SX126X_CMD_GET_IRQ_STATUS             0x12u
#define SX126X_CMD_CLEAR_IRQ_STATUS           0x02u
#define SX126X_CMD_GET_RX_BUFFER_STATUS       0x13u
#define SX126X_CMD_GET_PACKET_STATUS          0x14u
#define SX126X_CMD_GET_STATUS                 0xC0u
#define SX126X_CMD_GET_DEVICE_ERRORS          0x17u
#define SX126X_CMD_CALIBRATE                  0x89u
#define SX126X_CMD_SET_DIO3_AS_TCXO_CTRL      0x97u
#define SX126X_CMD_SET_DIO2_AS_RF_SWITCH_CTRL 0x9Du
#define SX126X_CMD_SET_REGULATOR_MODE         0x96u

/* Packet types (datasheet §13.4.2, SetPacketType). */
#define SX126X_PACKET_TYPE_GFSK               0x00u
#define SX126X_PACKET_TYPE_LORA               0x01u

/* Standby configs (datasheet §13.1.2, SetStandby). */
#define SX126X_STANDBY_RC                     0x00u  /* 13 MHz RC oscillator */
#define SX126X_STANDBY_XOSC                   0x01u  /* 32 MHz TCXO / XTAL    */

/* IRQ bit masks (datasheet §13.3.1, Table 13-29). */
#define SX126X_IRQ_TX_DONE                    (1u << 0)
#define SX126X_IRQ_RX_DONE                    (1u << 1)
#define SX126X_IRQ_PREAMBLE_DETECTED          (1u << 2)
#define SX126X_IRQ_SYNCWORD_VALID             (1u << 3)
#define SX126X_IRQ_HEADER_VALID               (1u << 4)
#define SX126X_IRQ_HEADER_ERR                 (1u << 5)
#define SX126X_IRQ_CRC_ERR                    (1u << 6)
#define SX126X_IRQ_CAD_DONE                   (1u << 7)
#define SX126X_IRQ_CAD_DETECTED               (1u << 8)
#define SX126X_IRQ_TIMEOUT                    (1u << 9)
#define SX126X_IRQ_ALL                        0x03FFu

/*
 * LoRa modulation parameters. Spreading factor trades data rate against
 * link budget: SF7 is fast and short-range, SF12 is slow and long-range.
 * Every step up in SF roughly doubles airtime and adds ~2.5 dB of link
 * budget. Datasheet §6.1.1, Table 6-1.
 */
typedef enum {
    SX126X_LORA_SF5  = 0x05u,
    SX126X_LORA_SF6  = 0x06u,
    SX126X_LORA_SF7  = 0x07u,
    SX126X_LORA_SF8  = 0x08u,
    SX126X_LORA_SF9  = 0x09u,
    SX126X_LORA_SF10 = 0x0Au,
    SX126X_LORA_SF11 = 0x0Bu,
    SX126X_LORA_SF12 = 0x0Cu,
} sx126x_sf_t;

/* LoRa bandwidth codes (datasheet §13.4.6, Table 13-47). */
typedef enum {
    SX126X_LORA_BW_125 = 0x04u,
    SX126X_LORA_BW_250 = 0x05u,
    SX126X_LORA_BW_500 = 0x06u,
} sx126x_bw_t;

/* LoRa coding rate codes (datasheet §13.4.6, Table 13-48). */
typedef enum {
    SX126X_LORA_CR_4_5 = 0x01u,
    SX126X_LORA_CR_4_6 = 0x02u,
    SX126X_LORA_CR_4_7 = 0x03u,
    SX126X_LORA_CR_4_8 = 0x04u,
} sx126x_cr_t;

typedef struct {
    sx126x_sf_t sf;
    sx126x_bw_t bw;
    sx126x_cr_t cr;
    uint8_t     low_data_rate_optimize;  /* 1 for SF11/SF12 at BW125 */
} sx126x_lora_modulation_t;

/* =========================================================================
 * LoRaWAN — the MAC layer that runs on top of LoRa modulation.
 *
 * LoRaWAN turns a point-to-point LoRa link into a star-of-stars network:
 * end-devices broadcast uplinks, any gateway in range forwards them to a
 * network server, and the network server deduplicates and routes. The end
 * device has three identity values (DevEUI, JoinEUI, DevAddr) and two root
 * keys (AppKey for OTAA; or NwkSKey + AppSKey for ABP). We implement an
 * uplink in the mini-project. Spec: LoRaWAN L2 1.0.4 §4 and §6.
 * ===================================================================== */

/* MType field, top 3 bits of MHDR (LoRaWAN L2 1.0.4 §4.2.1, Table 4). */
#define LORAWAN_MTYPE_JOIN_REQUEST            0x00u
#define LORAWAN_MTYPE_JOIN_ACCEPT             0x01u
#define LORAWAN_MTYPE_UNCONFIRMED_UP          0x02u
#define LORAWAN_MTYPE_UNCONFIRMED_DOWN        0x03u
#define LORAWAN_MTYPE_CONFIRMED_UP            0x04u
#define LORAWAN_MTYPE_CONFIRMED_DOWN          0x05u
#define LORAWAN_MTYPE_REJOIN_REQUEST          0x06u

/* MHDR Major version (LoRaWAN L2 1.0.4 §4.2.2): only 0 (LoRaWAN R1) is legal. */
#define LORAWAN_MAJOR_R1                      0x00u

/* FCtrl bits in the uplink frame header (LoRaWAN L2 1.0.4 §4.3.1, Table 8). */
#define LORAWAN_FCTRL_ADR                     (1u << 7)
#define LORAWAN_FCTRL_ADRACKREQ               (1u << 6)
#define LORAWAN_FCTRL_ACK                     (1u << 5)
#define LORAWAN_FCTRL_CLASSB                  (1u << 4)
/* low nibble is FOptsLen */

/* Field sizes (LoRaWAN L2 1.0.4 §4.3). */
#define LORAWAN_DEVADDR_SIZE                  4u
#define LORAWAN_MIC_SIZE                      4u
#define LORAWAN_FHDR_MIN_SIZE                 7u   /* DevAddr(4)+FCtrl(1)+FCnt(2) */
#define LORAWAN_KEY_SIZE                      16u  /* AES-128 keys are 16 bytes  */
#define LORAWAN_EUI_SIZE                      8u   /* DevEUI / JoinEUI            */

/* The "direction" byte in the B0 block used for MIC and encryption. */
#define LORAWAN_DIR_UPLINK                    0x00u
#define LORAWAN_DIR_DOWNLINK                  0x01u

/*
 * A fully-built uplink, ready to hand to the radio. The PHYPayload is:
 *   MHDR(1) | FHDR(7..) | FPort(0..1) | FRMPayload(0..N) | MIC(4)
 * We cap it at 64 bytes which covers any EU868/US915 data rate's max payload.
 */
#define LORAWAN_MAX_PHY_PAYLOAD               64u

typedef struct {
    uint8_t  devaddr[LORAWAN_DEVADDR_SIZE];   /* little-endian on the wire */
    uint8_t  nwk_skey[LORAWAN_KEY_SIZE];
    uint8_t  app_skey[LORAWAN_KEY_SIZE];
    uint32_t fcnt_up;
} lorawan_session_t;

typedef struct {
    uint8_t  phy[LORAWAN_MAX_PHY_PAYLOAD];
    size_t   phy_len;
} lorawan_uplink_t;

/* Result codes shared by the LoRaWAN builder/parsers. */
typedef enum {
    LORAWAN_OK                  = 0,
    LORAWAN_ERR_TOO_SHORT       = 1,
    LORAWAN_ERR_TOO_LONG        = 2,
    LORAWAN_ERR_BAD_MTYPE       = 3,
    LORAWAN_ERR_BAD_MAJOR       = 4,
    LORAWAN_ERR_MIC_MISMATCH    = 5,
    LORAWAN_ERR_NULL_ARG        = 6,
} lorawan_result_t;

/* =========================================================================
 * 802.15.4 — the PHY/MAC shared by Zigbee and Thread.
 *
 * Both Zigbee and Thread run on IEEE 802.15.4 (2.4 GHz, O-QPSK, 250 kbit/s,
 * 16 channels 11-26). The difference is the network layer above the MAC:
 * Zigbee uses the Zigbee PRO stack with its own application profiles;
 * Thread uses 6LoWPAN + IPv6 + a mesh routing layer. We do not build an
 * 802.15.4 stack from scratch (that is a Week 14 conceptual exercise, not a
 * bring-up) but we parse a frame control field in Exercise 1.
 * IEEE 802.15.4-2020 §7.2.2.1, Figure 7-2.
 * ===================================================================== */

/* Frame Control Field bit positions (IEEE 802.15.4-2020 §7.2.2.1). */
#define IEEE802154_FCF_FRAME_TYPE_MASK        0x0007u
#define IEEE802154_FRAME_TYPE_BEACON          0x0u
#define IEEE802154_FRAME_TYPE_DATA            0x1u
#define IEEE802154_FRAME_TYPE_ACK             0x2u
#define IEEE802154_FRAME_TYPE_MAC_CMD         0x3u
#define IEEE802154_FCF_SECURITY_ENABLED       (1u << 3)
#define IEEE802154_FCF_FRAME_PENDING          (1u << 4)
#define IEEE802154_FCF_ACK_REQUEST            (1u << 5)
#define IEEE802154_FCF_PAN_ID_COMPRESSION     (1u << 6)

#define IEEE802154_CHANNEL_MIN                11u
#define IEEE802154_CHANNEL_MAX                26u

/* =========================================================================
 * Radio-selection scoring — the decision logic from Lecture 1.
 *
 * Exercise 1 reduces "which radio do I pick?" to a small scoring routine.
 * These weights are the ones argued for in Lecture 1; the test in the
 * exercise feeds three product profiles through them and checks the verdict.
 * ===================================================================== */

typedef enum {
    RADIO_LORAWAN = 0,
    RADIO_ZIGBEE  = 1,
    RADIO_THREAD  = 2,
    RADIO_BLE_MESH = 3,
    RADIO_NONE    = 4,
} radio_choice_t;

typedef struct {
    uint32_t range_m;          /* required link distance in metres        */
    uint32_t uplink_period_s;  /* how often the node talks                */
    uint32_t payload_bytes;    /* typical message size                    */
    uint32_t node_count;       /* devices in the deployment               */
    uint8_t  needs_ip;         /* must speak IPv6 / be internet-addressable */
    uint8_t  needs_mains;      /* 0 = battery, 1 = mains-powered ok        */
    uint8_t  smart_home;       /* must pair with Apple/Google/Amazon hubs  */
} product_profile_t;

/* =========================================================================
 * Helpers — little/big-endian loads, shared by host and target builds.
 * ===================================================================== */

static inline uint16_t cc_read_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t cc_read_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void cc_write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void cc_write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* -------------------------------------------------------------------------
 * Public APIs implemented in the exercise .c files.
 * ----------------------------------------------------------------------- */

/* Exercise 1: returns the recommended radio for a product profile. */
radio_choice_t cc_choose_radio(const product_profile_t *profile);

/* Exercise 1: parse an 802.15.4 frame control field into a human label. */
const char *cc_ieee802154_frame_type_name(uint16_t fcf);

/* Exercise 3: build an unconfirmed uplink PHYPayload (no encryption of an
 * empty FRMPayload here — that path is in the mini-project). Returns
 * LORAWAN_OK and fills `out` on success. */
lorawan_result_t cc_lorawan_build_uplink(const lorawan_session_t *session,
                                         uint8_t fport,
                                         const uint8_t *frm_payload,
                                         size_t frm_len,
                                         lorawan_uplink_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CC_LPWAN_COMMON_H_ */
