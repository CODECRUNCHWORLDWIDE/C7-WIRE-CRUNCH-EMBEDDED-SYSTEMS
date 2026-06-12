/*
 * mqtt_client.h — MQTT 5 client built on top of a generic byte-stream
 * transport. The transport is abstract on purpose: in the mini-project
 * we wire it to mbedTLS (TLS 1.2), but the same client encodes/decodes
 * the wire format regardless of whether the bytes go out plaintext or
 * encrypted.
 *
 * The client implements exactly the packet types a publish/subscribe
 * sensor node needs: CONNECT, CONNACK, PUBLISH (QoS 0, both directions),
 * SUBSCRIBE, SUBACK, PINGREQ, PINGRESP, DISCONNECT. QoS > 0, retained
 * messages, and Will payloads are out of scope and deliberately omitted.
 *
 * Wire format: MQTT 5.0 OASIS Standard, §2 (structure) and §3 (packets).
 * The Variable Byte Integer and length-prefixed-UTF-8 helpers live in
 * exercises/wifi_common.h and are reused verbatim here.
 *
 * Style: explicit-width integers, no dynamic allocation, every buffer
 * caller-provided. Compiles clean under -Wall -Wextra -Werror at -Os on
 * arm-none-eabi-gcc 13.x against pico-sdk 1.5.1.
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stddef.h>

#include "wifi_common.h"

/*
 * Transport abstraction. send() and recv() return the number of bytes
 * transferred, 0 for "would block / no data right now", or a negative
 * value for a fatal transport error. The mini-project backs these with
 * mbedtls_ssl_write / mbedtls_ssl_read (see tls_send / tls_recv in
 * main.c), mapping MBEDTLS_ERR_SSL_WANT_{READ,WRITE} to 0.
 */
typedef struct {
    int32_t (*send)(void *ctx, const uint8_t *buf, uint32_t len);
    int32_t (*recv)(void *ctx, uint8_t *buf, uint32_t len);
    void    *ctx;
} mqtt_transport_t;

/* Result codes returned by the client API. */
typedef enum {
    MQTT_OK              =  0,
    MQTT_ERR_TRANSPORT   = -1,  /* underlying send/recv failed */
    MQTT_ERR_PROTOCOL    = -2,  /* malformed packet from broker */
    MQTT_ERR_REJECTED    = -3,  /* CONNACK / SUBACK reason code != success */
    MQTT_ERR_BUFFER      = -4,  /* a caller buffer was too small */
    MQTT_ERR_TIMEOUT     = -5   /* expected reply did not arrive in time */
} mqtt_result_t;

/*
 * Client handle. The receive scratch buffer holds at most one inbound
 * packet at a time; 512 bytes is ample for CONNACK, SUBACK, PINGRESP,
 * and the small command PUBLISHes we accept on cc7/devices/<id>/cmd.
 */
#define MQTT_RX_BUF_SIZE  ((uint32_t) 512U)
#define MQTT_TX_BUF_SIZE  ((uint32_t) 512U)

typedef void (*mqtt_on_publish_fn)(const char *topic, uint32_t topic_len,
                                   const uint8_t *payload, uint32_t payload_len);

typedef struct {
    mqtt_transport_t   transport;
    mqtt_on_publish_fn on_publish;   /* called for inbound PUBLISH; may be NULL */
    uint16_t           keepalive_s;
    uint8_t            rx[MQTT_RX_BUF_SIZE];
    uint8_t            tx[MQTT_TX_BUF_SIZE];
    uint32_t           rx_fill;      /* bytes currently buffered in rx */
} mqtt_client_t;

/*
 * Initialize a client over the given transport. keepalive_s is sent in
 * CONNECT and governs how often you must call mqtt_ping() to keep the
 * session alive.
 */
void mqtt_client_init(mqtt_client_t *c, const mqtt_transport_t *t,
                      uint16_t keepalive_s, mqtt_on_publish_fn on_publish);

/*
 * Encode a minimal MQTT 5 CONNECT (clean start, no will, no auth) with
 * the given client id into `out`. Returns the encoded length, or 0 on
 * buffer overflow. Exposed separately so the encode logic is unit-testable
 * without a live transport (see the SOLUTIONS round-trip in exercise 3).
 */
uint32_t mqtt_encode_connect(uint8_t *out, uint32_t outlen,
                             const char *client_id, uint16_t keepalive_s);

/*
 * Encode an MQTT 5 PUBLISH at QoS 0 (no packet identifier, no QoS bits in
 * the fixed header). Returns encoded length or 0 on overflow.
 */
uint32_t mqtt_encode_publish(uint8_t *out, uint32_t outlen,
                             const char *topic, const uint8_t *payload,
                             uint32_t payload_len);

/*
 * Encode an MQTT 5 SUBSCRIBE for a single topic filter at QoS 0.
 * `packet_id` must be non-zero (SUBSCRIBE requires a packet identifier
 * per §3.8.2, p. 105). Returns encoded length or 0 on overflow.
 */
uint32_t mqtt_encode_subscribe(uint8_t *out, uint32_t outlen,
                               uint16_t packet_id, const char *topic_filter);

/*
 * Connect: encode + send CONNECT, then read CONNACK and check its reason
 * code. Returns MQTT_OK on Success (rc 0), MQTT_ERR_REJECTED with the
 * reason code stored in *out_reason on a non-success CONNACK, or a
 * transport/protocol error.
 */
mqtt_result_t mqtt_connect(mqtt_client_t *c, const char *client_id,
                           mqtt_connack_reason_t *out_reason);

/* Publish one payload to one topic at QoS 0. Fire-and-forget. */
mqtt_result_t mqtt_publish(mqtt_client_t *c, const char *topic,
                           const uint8_t *payload, uint32_t payload_len);

/* Subscribe to one topic filter at QoS 0 and wait for the SUBACK. */
mqtt_result_t mqtt_subscribe(mqtt_client_t *c, uint16_t packet_id,
                             const char *topic_filter);

/* Send a PINGREQ. Call this before keepalive_s elapses with no other TX. */
mqtt_result_t mqtt_ping(mqtt_client_t *c);

/* Send a graceful DISCONNECT (reason code Normal Disconnection, 0x00). */
mqtt_result_t mqtt_disconnect(mqtt_client_t *c);

/*
 * Service inbound traffic: drain whatever bytes the transport has, parse
 * any complete packets, dispatch PUBLISH to on_publish and silently
 * consume PINGRESP/SUBACK that arrive asynchronously. Non-blocking;
 * returns MQTT_OK if it made progress or had nothing to do, a negative
 * code on a transport/protocol error. Call this from the main loop.
 */
mqtt_result_t mqtt_poll(mqtt_client_t *c);

#endif /* MQTT_CLIENT_H */
