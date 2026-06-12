/*
 * mqtt_client.c — MQTT 5 client implementation.
 *
 * The encoders build packets into a caller buffer using the Variable Byte
 * Integer and length-prefixed-UTF-8 helpers from wifi_common.h. The
 * client API (mqtt_connect / mqtt_publish / mqtt_subscribe / mqtt_ping /
 * mqtt_disconnect / mqtt_poll) drives those encoders over an abstract
 * transport and parses the broker's replies.
 *
 * Everything here is the natural growth of exercise-03's encode/decode
 * core into a full client. The VBI helpers are byte-for-byte the same.
 *
 * Wire references inline: MQTT 5.0 OASIS Standard, §3.1 (CONNECT, pp.
 * 38-53), §3.2 (CONNACK, pp. 53-60), §3.3 (PUBLISH, pp. 64-78), §3.8
 * (SUBSCRIBE, pp. 104-110), §3.9 (SUBACK, pp. 110-115), §3.12 (PINGREQ,
 * pp. 122-123), §3.14 (DISCONNECT, pp. 125-131).
 */

#include <string.h>

#include "pico/stdlib.h"

#include "mqtt_client.h"

/* ----- small helpers ---------------------------------------------------- */

/* Append a single byte; returns 1 on success, 0 if it would overflow. */
static int put_u8(uint8_t *buf, uint32_t *cur, uint32_t cap, uint8_t b) {
    if (*cur + 1U > cap) {
        return 0;
    }
    buf[*cur] = b;
    *cur += 1U;
    return 1;
}

/* Append a 2-byte big-endian integer. */
static int put_u16(uint8_t *buf, uint32_t *cur, uint32_t cap, uint16_t v) {
    if (*cur + 2U > cap) {
        return 0;
    }
    buf[*cur]      = (uint8_t) ((v >> 8U) & 0xFFU);
    buf[*cur + 1U] = (uint8_t) (v & 0xFFU);
    *cur += 2U;
    return 1;
}

/*
 * Prepend a fixed header (type byte + remaining-length VBI) ahead of a
 * already-built variable-header+payload region. We build the body first
 * into out[], reserving the maximum 5 fixed-header bytes at the front,
 * then shift. To keep it simple and allocation-free we instead build the
 * body at a known offset and write the fixed header in front of it.
 *
 * `body_len` is the number of bytes already written starting at
 * out[FIXED_MAX]. Returns the total packet length (fixed header + body)
 * relocated to the start of out[], or 0 on overflow.
 */
#define FIXED_MAX  5U  /* 1 type byte + up to 4 VBI bytes */

static uint32_t prepend_fixed_header(uint8_t *out, uint32_t outlen,
                                     uint8_t type_byte, uint32_t body_len) {
    mqtt_vbi_encoded_t rl = mqtt_vbi_encode(body_len);
    if (rl.count == 0U && body_len != 0U) {
        return 0U;  /* body too large for a 4-byte VBI */
    }
    if (body_len == 0U) {
        rl.bytes[0] = 0U;
        rl.count = 1U;
    }

    uint32_t header_len = 1U + rl.count;
    uint32_t total = header_len + body_len;
    if (total > outlen) {
        return 0U;
    }

    /* The body currently sits at out[FIXED_MAX..FIXED_MAX+body_len). We
     * want it at out[header_len..header_len+body_len). Shift it down to
     * sit immediately after the header. memmove handles overlap. */
    (void) memmove(&out[header_len], &out[FIXED_MAX], body_len);

    out[0] = type_byte;
    for (uint8_t i = 0U; i < rl.count; i++) {
        out[1U + i] = rl.bytes[i];
    }
    return total;
}

/* ----- encoders --------------------------------------------------------- */

uint32_t mqtt_encode_connect(uint8_t *out, uint32_t outlen,
                             const char *client_id, uint16_t keepalive_s) {
    uint32_t cur = FIXED_MAX;  /* leave room for the fixed header */

    /* Variable header. */
    if (mqtt_buf_put_utf8(out, &cur, outlen, "MQTT", 4U) < 0) { return 0U; }
    if (!put_u8(out, &cur, outlen, MQTT_PROTOCOL_LEVEL_5))    { return 0U; }
    if (!put_u8(out, &cur, outlen, MQTT_CONNECT_FLAG_CLEAN_START)) { return 0U; }
    if (!put_u16(out, &cur, outlen, keepalive_s))            { return 0U; }
    if (!put_u8(out, &cur, outlen, 0U))                      { return 0U; } /* props len 0 */

    /* Payload: client identifier. */
    uint32_t cid_len = (uint32_t) strlen(client_id);
    if (mqtt_buf_put_utf8(out, &cur, outlen, client_id, cid_len) < 0) { return 0U; }

    uint32_t body_len = cur - FIXED_MAX;
    return prepend_fixed_header(out, outlen, (uint8_t) MQTT_PT_CONNECT, body_len);
}

uint32_t mqtt_encode_publish(uint8_t *out, uint32_t outlen,
                             const char *topic, const uint8_t *payload,
                             uint32_t payload_len) {
    uint32_t cur = FIXED_MAX;

    /* Variable header: topic name, no packet id (QoS 0), props len 0. */
    uint32_t topic_len = (uint32_t) strlen(topic);
    if (mqtt_buf_put_utf8(out, &cur, outlen, topic, topic_len) < 0) { return 0U; }
    if (!put_u8(out, &cur, outlen, 0U)) { return 0U; } /* props len 0 */

    /* Payload (raw, no length prefix in PUBLISH). */
    if (cur + payload_len > outlen) { return 0U; }
    (void) memcpy(&out[cur], payload, payload_len);
    cur += payload_len;

    uint32_t body_len = cur - FIXED_MAX;
    /* QoS 0 PUBLISH fixed-header flags are all zero -> type byte 0x30. */
    return prepend_fixed_header(out, outlen, (uint8_t) MQTT_PT_PUBLISH, body_len);
}

uint32_t mqtt_encode_subscribe(uint8_t *out, uint32_t outlen,
                               uint16_t packet_id, const char *topic_filter) {
    uint32_t cur = FIXED_MAX;

    /* Variable header: packet identifier, props len 0. */
    if (!put_u16(out, &cur, outlen, packet_id)) { return 0U; }
    if (!put_u8(out, &cur, outlen, 0U))         { return 0U; } /* props len 0 */

    /* Payload: one topic filter + subscription options byte (QoS 0). */
    uint32_t tf_len = (uint32_t) strlen(topic_filter);
    if (mqtt_buf_put_utf8(out, &cur, outlen, topic_filter, tf_len) < 0) { return 0U; }
    if (!put_u8(out, &cur, outlen, 0U)) { return 0U; } /* sub options: QoS 0 */

    uint32_t body_len = cur - FIXED_MAX;
    /* SUBSCRIBE requires fixed-header flags 0b0010 -> type byte 0x82. */
    return prepend_fixed_header(out, outlen, (uint8_t) MQTT_PT_SUBSCRIBE, body_len);
}

/* ----- transport plumbing ----------------------------------------------- */

/*
 * Send the full buffer, looping over partial/would-block sends. Returns
 * MQTT_OK or MQTT_ERR_TRANSPORT. A would-block (send returns 0) is retried
 * after a short yield; a fatal transport error (negative) aborts.
 */
static mqtt_result_t send_all(mqtt_client_t *c, const uint8_t *buf, uint32_t len) {
    uint32_t off = 0U;
    absolute_time_t deadline = make_timeout_time_ms(5000U);
    while (off < len) {
        int32_t n = c->transport.send(c->transport.ctx, &buf[off], len - off);
        if (n < 0) {
            return MQTT_ERR_TRANSPORT;
        }
        if (n == 0) {
            if (time_reached(deadline)) {
                return MQTT_ERR_TIMEOUT;
            }
            sleep_ms(2U);
            continue;
        }
        off += (uint32_t) n;
    }
    return MQTT_OK;
}

/*
 * Read exactly one MQTT packet into c->rx, returning its total length via
 * *out_len. Blocks (cooperatively) up to `timeout_ms`. Returns MQTT_OK,
 * MQTT_ERR_TIMEOUT, MQTT_ERR_PROTOCOL (malformed VBI / oversized), or
 * MQTT_ERR_TRANSPORT.
 */
static mqtt_result_t read_packet(mqtt_client_t *c, uint32_t *out_len,
                                 uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    /* Step 1: get the fixed header. We need at least 2 bytes (type + first
     * VBI byte), then potentially up to 4 more VBI bytes. */
    uint32_t have = 0U;
    while (have < 2U) {
        int32_t n = c->transport.recv(c->transport.ctx, &c->rx[have], 1U);
        if (n < 0) { return MQTT_ERR_TRANSPORT; }
        if (n == 0) {
            if (time_reached(deadline)) { return MQTT_ERR_TIMEOUT; }
            sleep_ms(2U);
            continue;
        }
        have += (uint32_t) n;
    }

    /* Decode the remaining-length VBI, pulling more bytes as needed. */
    uint32_t vbi_off = 1U;
    while (1) {
        mqtt_vbi_decoded_t d = mqtt_vbi_decode(&c->rx[1], have - 1U);
        if (d.error == 0) {
            uint32_t remaining = d.value;
            uint32_t header_len = 1U + d.count;
            uint32_t total = header_len + remaining;
            if (total > MQTT_RX_BUF_SIZE) {
                return MQTT_ERR_PROTOCOL;  /* packet larger than our scratch */
            }
            /* Pull the rest of the packet body. */
            while (have < total) {
                int32_t n = c->transport.recv(c->transport.ctx,
                                              &c->rx[have], total - have);
                if (n < 0) { return MQTT_ERR_TRANSPORT; }
                if (n == 0) {
                    if (time_reached(deadline)) { return MQTT_ERR_TIMEOUT; }
                    sleep_ms(2U);
                    continue;
                }
                have += (uint32_t) n;
            }
            *out_len = total;
            return MQTT_OK;
        }
        /* VBI not complete yet; need another byte (max 4 VBI bytes). */
        if (vbi_off >= MQTT_VBI_MAX_BYTES) {
            return MQTT_ERR_PROTOCOL;
        }
        int32_t n = c->transport.recv(c->transport.ctx, &c->rx[have], 1U);
        if (n < 0) { return MQTT_ERR_TRANSPORT; }
        if (n == 0) {
            if (time_reached(deadline)) { return MQTT_ERR_TIMEOUT; }
            sleep_ms(2U);
            continue;
        }
        have += (uint32_t) n;
        vbi_off++;
    }
}

/*
 * Dispatch one already-buffered packet (length `len`, sitting in c->rx).
 * Handles inbound PUBLISH (-> on_publish) and silently consumes PINGRESP,
 * SUBACK, and broker DISCONNECT. Returns MQTT_OK or a protocol error.
 */
static mqtt_result_t dispatch_packet(mqtt_client_t *c, uint32_t len) {
    uint8_t type = (uint8_t) (c->rx[0] & 0xF0U);

    /* Skip the fixed header (type byte + remaining-length VBI). */
    mqtt_vbi_decoded_t rl = mqtt_vbi_decode(&c->rx[1], len - 1U);
    if (rl.error != 0) { return MQTT_ERR_PROTOCOL; }
    uint32_t off = 1U + rl.count;

    switch (type) {
        case MQTT_PT_PUBLISH: {
            /* Variable header: topic name (length-prefixed). QoS in the
             * fixed-header flags; for inbound commands we only accept QoS
             * 0, so there is no packet identifier to skip. */
            if (off + 2U > len) { return MQTT_ERR_PROTOCOL; }
            uint32_t tlen = ((uint32_t) c->rx[off] << 8U) | c->rx[off + 1U];
            off += 2U;
            if (off + tlen > len) { return MQTT_ERR_PROTOCOL; }
            const char *topic = (const char *) &c->rx[off];
            off += tlen;

            /* MQTT 5 properties length (VBI), then skip the properties. */
            mqtt_vbi_decoded_t props = mqtt_vbi_decode(&c->rx[off], len - off);
            if (props.error != 0) { return MQTT_ERR_PROTOCOL; }
            off += props.count + props.value;
            if (off > len) { return MQTT_ERR_PROTOCOL; }

            const uint8_t *payload = &c->rx[off];
            uint32_t payload_len = len - off;
            if (c->on_publish != NULL) {
                c->on_publish(topic, tlen, payload, payload_len);
            }
            return MQTT_OK;
        }
        case MQTT_PT_PINGRESP:
        case MQTT_PT_SUBACK:
        case MQTT_PT_PUBACK:
            /* Expected asynchronous acks; nothing to do at QoS 0. */
            return MQTT_OK;
        case MQTT_PT_DISCONNECT:
            /* Broker-initiated disconnect. Surface as a transport error so
             * the caller's state machine drops to backoff and reconnects. */
            return MQTT_ERR_TRANSPORT;
        default:
            /* Unexpected packet type; ignore rather than abort. */
            return MQTT_OK;
    }
}

/* ----- public API ------------------------------------------------------- */

void mqtt_client_init(mqtt_client_t *c, const mqtt_transport_t *t,
                      uint16_t keepalive_s, mqtt_on_publish_fn on_publish) {
    (void) memset(c, 0, sizeof *c);
    c->transport   = *t;
    c->keepalive_s = keepalive_s;
    c->on_publish  = on_publish;
    c->rx_fill     = 0U;
}

mqtt_result_t mqtt_connect(mqtt_client_t *c, const char *client_id,
                           mqtt_connack_reason_t *out_reason) {
    uint32_t n = mqtt_encode_connect(c->tx, MQTT_TX_BUF_SIZE,
                                     client_id, c->keepalive_s);
    if (n == 0U) { return MQTT_ERR_BUFFER; }

    mqtt_result_t sr = send_all(c, c->tx, n);
    if (sr != MQTT_OK) { return sr; }

    uint32_t rlen = 0U;
    mqtt_result_t rr = read_packet(c, &rlen, 10000U);
    if (rr != MQTT_OK) { return rr; }

    if ((c->rx[0] & 0xF0U) != MQTT_PT_CONNACK) {
        return MQTT_ERR_PROTOCOL;
    }
    /* CONNACK variable header: ack flags (1) + reason code (1) [+ props]. */
    mqtt_vbi_decoded_t rl = mqtt_vbi_decode(&c->rx[1], rlen - 1U);
    if (rl.error != 0) { return MQTT_ERR_PROTOCOL; }
    uint32_t off = 1U + rl.count;
    if (off + 2U > rlen) { return MQTT_ERR_PROTOCOL; }

    uint8_t reason = c->rx[off + 1U];
    if (out_reason != NULL) {
        *out_reason = (mqtt_connack_reason_t) reason;
    }
    return (reason == (uint8_t) MQTT_RC_SUCCESS) ? MQTT_OK : MQTT_ERR_REJECTED;
}

mqtt_result_t mqtt_publish(mqtt_client_t *c, const char *topic,
                           const uint8_t *payload, uint32_t payload_len) {
    uint32_t n = mqtt_encode_publish(c->tx, MQTT_TX_BUF_SIZE,
                                     topic, payload, payload_len);
    if (n == 0U) { return MQTT_ERR_BUFFER; }
    return send_all(c, c->tx, n);
}

mqtt_result_t mqtt_subscribe(mqtt_client_t *c, uint16_t packet_id,
                             const char *topic_filter) {
    uint32_t n = mqtt_encode_subscribe(c->tx, MQTT_TX_BUF_SIZE,
                                       packet_id, topic_filter);
    if (n == 0U) { return MQTT_ERR_BUFFER; }

    mqtt_result_t sr = send_all(c, c->tx, n);
    if (sr != MQTT_OK) { return sr; }

    /* Wait for the SUBACK and check its single granted-QoS reason byte. */
    uint32_t rlen = 0U;
    mqtt_result_t rr = read_packet(c, &rlen, 10000U);
    if (rr != MQTT_OK) { return rr; }
    if ((c->rx[0] & 0xF0U) != MQTT_PT_SUBACK) {
        /* It might be an interleaved PUBLISH; dispatch and try once more. */
        (void) dispatch_packet(c, rlen);
        rr = read_packet(c, &rlen, 10000U);
        if (rr != MQTT_OK) { return rr; }
        if ((c->rx[0] & 0xF0U) != MQTT_PT_SUBACK) { return MQTT_ERR_PROTOCOL; }
    }
    /* SUBACK payload's reason byte: < 0x80 means granted, >= 0x80 failure. */
    mqtt_vbi_decoded_t rl = mqtt_vbi_decode(&c->rx[1], rlen - 1U);
    if (rl.error != 0) { return MQTT_ERR_PROTOCOL; }
    uint32_t off = 1U + rl.count;
    /* var header: packet id (2) + props len (VBI). */
    if (off + 2U > rlen) { return MQTT_ERR_PROTOCOL; }
    off += 2U;
    mqtt_vbi_decoded_t props = mqtt_vbi_decode(&c->rx[off], rlen - off);
    if (props.error != 0) { return MQTT_ERR_PROTOCOL; }
    off += props.count + props.value;
    if (off >= rlen) { return MQTT_ERR_PROTOCOL; }
    uint8_t granted = c->rx[off];
    return (granted < 0x80U) ? MQTT_OK : MQTT_ERR_REJECTED;
}

mqtt_result_t mqtt_ping(mqtt_client_t *c) {
    /* PINGREQ is a 2-byte packet: 0xC0 0x00. */
    uint8_t pkt[2] = { (uint8_t) MQTT_PT_PINGREQ, 0x00U };
    return send_all(c, pkt, sizeof pkt);
}

mqtt_result_t mqtt_disconnect(mqtt_client_t *c) {
    /* DISCONNECT with reason code 0x00 (Normal Disconnection), no props.
     * Encoded as 0xE0 0x01 0x00. */
    uint8_t pkt[3] = { (uint8_t) MQTT_PT_DISCONNECT, 0x01U, 0x00U };
    return send_all(c, pkt, sizeof pkt);
}

mqtt_result_t mqtt_poll(mqtt_client_t *c) {
    /* Peek for a packet with a very short timeout; if nothing is waiting,
     * read_packet returns MQTT_ERR_TIMEOUT which we treat as "no work". */
    uint32_t rlen = 0U;
    mqtt_result_t rr = read_packet(c, &rlen, 5U);
    if (rr == MQTT_ERR_TIMEOUT) {
        return MQTT_OK;  /* nothing pending */
    }
    if (rr != MQTT_OK) {
        return rr;       /* transport/protocol failure -> caller reconnects */
    }
    return dispatch_packet(c, rlen);
}
