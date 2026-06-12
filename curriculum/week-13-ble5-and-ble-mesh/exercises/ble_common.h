/*
 * ble_common.h — Shared declarations for Week 13 exercises and mini-project.
 *
 * Constants here mirror the on-air formats described in the lectures. They are
 * deliberately stack-independent: the same AD-type and ATT-opcode numbers are
 * used by BTstack on the Pico W, by Zephyr on an nRF52840, and by the Nordic
 * SoftDevice. The values come from the specifications, not from any one stack.
 *
 * Citations:
 *   - Bluetooth Core Spec 5.4 Vol 3 Part C (GAP / AD types).
 *   - Bluetooth Core Spec 5.4 Vol 3 Part F (ATT opcodes).
 *   - Bluetooth Core Spec 5.4 Vol 3 Part G (GATT UUIDs).
 *   - Bluetooth Core Spec 5.4 Vol 6 Part B (Link Layer / advertising PDUs).
 *   - Bluetooth Mesh Protocol 1.1 §3 (network PDU, addressing).
 *   - Bluetooth SIG Assigned Numbers.
 *
 * Exercises 1 and 3 build standalone on a host (cc -std=c11 ...). Exercise 2 and
 * the mini-project build against the Pico SDK + BTstack.
 */

#ifndef CC_BLE_COMMON_H_
#define CC_BLE_COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Radio / PHY constants (Core Spec Vol 6 Part A).
 * ----------------------------------------------------------------------- */

#define BLE_NUM_RF_CHANNELS            40u   /* 2402..2480 MHz, 2 MHz spacing. */
#define BLE_NUM_DATA_CHANNELS          37u   /* Channel indices 0..36.         */
#define BLE_ADV_CHANNEL_37             37u   /* 2402 MHz.                       */
#define BLE_ADV_CHANNEL_38             38u   /* 2426 MHz.                       */
#define BLE_ADV_CHANNEL_39             39u   /* 2480 MHz.                       */

typedef enum {
    BLE_PHY_1M       = 0x01u,  /* LE 1M  — 1 Msym/s, the original.            */
    BLE_PHY_2M       = 0x02u,  /* LE 2M  — 2 Msym/s, half the air-time.        */
    BLE_PHY_CODED    = 0x03u,  /* LE Coded — S=2 (500 kbit/s) or S=8 (125k).   */
} ble_phy_t;

/* The access address of all legacy advertising channel PDUs is fixed. */
#define BLE_ADV_ACCESS_ADDRESS         0x8E89BED6u  /* Core Spec Vol 6 B §2.1.2 */

/* -------------------------------------------------------------------------
 * Advertising: PDU types and the AD-structure (Vol 6 Part B §2.3,
 * Vol 3 Part C §11 "Advertising and Scan Response Data Format").
 *
 * An advertising payload is a sequence of AD structures, each:
 *   [length: 1 byte] [AD type: 1 byte] [data: (length - 1) bytes]
 * The 'length' byte counts the AD-type byte plus the data, not itself.
 * The legacy advertising payload is at most 31 bytes.
 * ----------------------------------------------------------------------- */

#define BLE_ADV_LEGACY_MAX_PAYLOAD     31u

/* Advertising PDU header types (the 4-bit PDU Type field). */
typedef enum {
    BLE_ADV_IND          = 0x0u,  /* connectable scannable undirected.        */
    BLE_ADV_DIRECT_IND   = 0x1u,  /* connectable directed.                    */
    BLE_ADV_NONCONN_IND  = 0x2u,  /* non-connectable (beacons, mesh).         */
    BLE_SCAN_REQ         = 0x3u,
    BLE_SCAN_RSP         = 0x4u,
    BLE_CONNECT_IND      = 0x5u,
    BLE_ADV_SCAN_IND     = 0x6u,  /* scannable non-connectable.               */
    BLE_ADV_EXT_IND      = 0x7u,  /* extended advertising (BLE 5).            */
} ble_adv_pdu_type_t;

/* AD types (Bluetooth SIG Assigned Numbers, "Generic Access Profile"). */
#define BLE_AD_FLAGS                       0x01u
#define BLE_AD_INCOMPLETE_16BIT_UUIDS      0x02u
#define BLE_AD_COMPLETE_16BIT_UUIDS        0x03u
#define BLE_AD_INCOMPLETE_128BIT_UUIDS     0x06u
#define BLE_AD_COMPLETE_128BIT_UUIDS       0x07u
#define BLE_AD_SHORTENED_LOCAL_NAME        0x08u
#define BLE_AD_COMPLETE_LOCAL_NAME         0x09u
#define BLE_AD_TX_POWER_LEVEL              0x0Au
#define BLE_AD_SERVICE_DATA_16BIT          0x16u
#define BLE_AD_APPEARANCE                  0x19u
#define BLE_AD_MESH_PB_ADV                 0x29u  /* PB-ADV provisioning bearer. */
#define BLE_AD_MESH_MESSAGE                0x2Au  /* Mesh network/relay PDUs.     */
#define BLE_AD_MESH_BEACON                 0x2Bu  /* Unprovisioned-device beacon. */
#define BLE_AD_MANUFACTURER_SPECIFIC       0xFFu

/* The Flags AD-type bits (Vol 3 Part C §11.1.3). */
#define BLE_FLAG_LE_LIMITED_DISCOVERABLE   0x01u
#define BLE_FLAG_LE_GENERAL_DISCOVERABLE   0x02u
#define BLE_FLAG_BR_EDR_NOT_SUPPORTED      0x04u

/* -------------------------------------------------------------------------
 * GATT / ATT (Vol 3 Part F §3, Part G).
 * ----------------------------------------------------------------------- */

/* ATT protocol opcodes (Vol 3 Part F §3.4, Table 3.37). */
#define ATT_OP_ERROR_RSP                   0x01u
#define ATT_OP_EXCHANGE_MTU_REQ            0x02u
#define ATT_OP_EXCHANGE_MTU_RSP            0x03u
#define ATT_OP_FIND_INFO_REQ               0x04u
#define ATT_OP_FIND_INFO_RSP               0x05u
#define ATT_OP_READ_BY_TYPE_REQ            0x08u  /* characteristic discovery. */
#define ATT_OP_READ_BY_TYPE_RSP            0x09u
#define ATT_OP_READ_REQ                    0x0Au
#define ATT_OP_READ_RSP                    0x0Bu
#define ATT_OP_READ_BY_GROUP_TYPE_REQ      0x10u  /* service discovery.         */
#define ATT_OP_READ_BY_GROUP_TYPE_RSP      0x11u
#define ATT_OP_WRITE_REQ                   0x12u
#define ATT_OP_WRITE_RSP                   0x13u
#define ATT_OP_WRITE_CMD                   0x52u  /* write without response.    */
#define ATT_OP_HANDLE_VALUE_NTF            0x1Bu  /* notification.              */
#define ATT_OP_HANDLE_VALUE_IND            0x1Du  /* indication.                */
#define ATT_OP_HANDLE_VALUE_CFM            0x1Eu  /* confirmation.              */

#define ATT_MTU_DEFAULT                    23u    /* 20 bytes of value payload. */
#define ATT_MTU_MAX                        517u

/* Declaration UUIDs (Vol 3 Part G §3, Assigned Numbers). */
#define GATT_PRIMARY_SERVICE_UUID          0x2800u
#define GATT_SECONDARY_SERVICE_UUID        0x2801u
#define GATT_INCLUDE_UUID                  0x2802u
#define GATT_CHARACTERISTIC_UUID           0x2803u
#define GATT_CCCD_UUID                     0x2902u  /* Client Char Config Desc.  */

/* Characteristic properties byte (Vol 3 Part G §3.3.1.1, Table 3.5). */
#define GATT_PROP_BROADCAST                0x01u
#define GATT_PROP_READ                     0x02u
#define GATT_PROP_WRITE_NO_RSP             0x04u
#define GATT_PROP_WRITE                    0x08u
#define GATT_PROP_NOTIFY                   0x10u
#define GATT_PROP_INDICATE                 0x20u
#define GATT_PROP_AUTH_SIGNED_WRITE        0x40u
#define GATT_PROP_EXTENDED                 0x80u

/* CCCD value bits (Vol 3 Part G §3.3.3.3). */
#define GATT_CCCD_NOTIFY                   0x0001u
#define GATT_CCCD_INDICATE                 0x0002u

/* Common assigned 16-bit UUIDs we use this week (Assigned Numbers). */
#define UUID16_BATTERY_SERVICE             0x180Fu
#define UUID16_BATTERY_LEVEL               0x2A19u
#define UUID16_DEVICE_INFO_SERVICE         0x180Au
#define UUID16_ENV_SENSING_SERVICE         0x181Au
#define UUID16_TEMPERATURE                 0x2A6Eu

/* -------------------------------------------------------------------------
 * BLE Mesh (Mesh Protocol 1.1 §3).
 * ----------------------------------------------------------------------- */

/* Address ranges (Mesh Protocol 1.1 §3.4.2). */
#define MESH_ADDR_UNASSIGNED               0x0000u
#define MESH_ADDR_UNICAST_MIN              0x0001u
#define MESH_ADDR_UNICAST_MAX              0x7FFFu
#define MESH_ADDR_VIRTUAL_MIN              0x8000u
#define MESH_ADDR_VIRTUAL_MAX              0xBFFFu
#define MESH_ADDR_GROUP_MIN                0xC000u
#define MESH_ADDR_GROUP_MAX                0xFEFFu
#define MESH_ADDR_ALL_PROXIES              0xFFFCu
#define MESH_ADDR_ALL_FRIENDS              0xFFFDu
#define MESH_ADDR_ALL_RELAYS               0xFFFEu
#define MESH_ADDR_ALL_NODES                0xFFFFu

#define MESH_TTL_MAX                       0x7Fu  /* 127.                       */
#define MESH_TTL_DEFAULT                   0x07u

/* Generic OnOff model (Mesh Model 1.1 §3.2). */
#define MESH_MODEL_GENERIC_ONOFF_SERVER    0x1000u
#define MESH_MODEL_GENERIC_ONOFF_CLIENT    0x1001u
#define MESH_MODEL_SENSOR_SERVER           0x1100u
#define MESH_MODEL_CONFIG_SERVER           0x0000u
#define MESH_MODEL_HEALTH_SERVER           0x0002u

/* Generic OnOff message opcodes (Mesh Model 1.1 §7.1). */
#define MESH_GENERIC_ONOFF_GET             0x8201u
#define MESH_GENERIC_ONOFF_SET             0x8202u
#define MESH_GENERIC_ONOFF_SET_UNACK       0x8203u
#define MESH_GENERIC_ONOFF_STATUS          0x8204u

/*
 * A Mesh Network PDU on the air (Mesh Protocol 1.1 §3.4.4). After the
 * network-layer obfuscation is removed and the NetMIC verified, the cleartext
 * fields are:
 *
 *   IVI (1 bit) | NID (7 bits)      — IV-index LSB and NetKey identifier.
 *   CTL (1 bit) | TTL (7 bits)      — control-vs-access flag and time-to-live.
 *   SEQ (24 bits)                   — per-source monotonically increasing.
 *   SRC (16 bits)                   — source unicast address.
 *   DST (16 bits)                   — destination (unicast/group/virtual).
 *   TransportPDU (1..16 bytes)      — the lower-transport PDU.
 *   NetMIC (4 bytes if CTL=0, 8 if CTL=1).
 *
 * We model the cleartext (decrypted) header here for Exercise 3; the on-air PDU
 * has the header obfuscated and the destination + transport encrypted.
 */
typedef struct {
    uint8_t  ivi;        /* 1 bit.  */
    uint8_t  nid;        /* 7 bits. */
    uint8_t  ctl;        /* 1 bit.  */
    uint8_t  ttl;        /* 7 bits. */
    uint32_t seq;        /* 24 bits. */
    uint16_t src;        /* 16 bits. */
    uint16_t dst;        /* 16 bits. */
    const uint8_t *transport_pdu;
    size_t   transport_len;
} mesh_network_pdu_t;

typedef enum {
    MESH_OK                       = 0,
    MESH_ERR_TTL_OUT_OF_RANGE     = 1,
    MESH_ERR_SRC_NOT_UNICAST      = 2,
    MESH_ERR_DST_UNASSIGNED       = 3,
    MESH_ERR_SEQ_REPLAY           = 4,
    MESH_ERR_NETMIC_MISMATCH      = 5,
    MESH_ERR_NOT_RELAYABLE        = 6,
} mesh_result_t;

/* -------------------------------------------------------------------------
 * Helpers: explicit-endianness loads/stores. BLE is little-endian on the air
 * for most fields, but the Mesh network header packs several big-endian
 * multi-byte fields (SEQ, SRC, DST), so we provide both.
 * ----------------------------------------------------------------------- */

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

static inline uint16_t cc_read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t cc_read_be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2]);
}

static inline void cc_write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static inline void cc_write_be24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 16) & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)(v & 0xFFu);
}

/* Mesh address classification (Mesh Protocol 1.1 §3.4.2). */
static inline bool mesh_addr_is_unicast(uint16_t a) {
    return a >= MESH_ADDR_UNICAST_MIN && a <= MESH_ADDR_UNICAST_MAX;
}

static inline bool mesh_addr_is_group(uint16_t a) {
    return a >= MESH_ADDR_GROUP_MIN && a <= MESH_ADDR_ALL_NODES;
}

static inline bool mesh_addr_is_virtual(uint16_t a) {
    return a >= MESH_ADDR_VIRTUAL_MIN && a <= MESH_ADDR_VIRTUAL_MAX;
}

/* -------------------------------------------------------------------------
 * Advertising-payload builder API (implemented in exercise-01).
 * ----------------------------------------------------------------------- */

/* Append one AD structure. Returns the new total length, or 0 on overflow. */
size_t cc_ad_append(uint8_t *buf, size_t buf_cap, size_t used,
                    uint8_t ad_type, const uint8_t *data, size_t data_len);

/* Parse the next AD structure starting at buf[*offset]; advances *offset. */
bool cc_ad_next(const uint8_t *buf, size_t buf_len, size_t *offset,
                uint8_t *out_type, const uint8_t **out_data, size_t *out_len);

/* -------------------------------------------------------------------------
 * Mesh network-PDU API (implemented in exercise-03).
 * ----------------------------------------------------------------------- */

/* Validate header fields and (conceptually) the NetMIC. */
mesh_result_t cc_mesh_validate(const mesh_network_pdu_t *pdu);

/* Decrement TTL for a relay; returns MESH_ERR_NOT_RELAYABLE if TTL < 2. */
mesh_result_t cc_mesh_relay_ttl(mesh_network_pdu_t *pdu);

#ifdef __cplusplus
}
#endif

#endif /* CC_BLE_COMMON_H_ */
