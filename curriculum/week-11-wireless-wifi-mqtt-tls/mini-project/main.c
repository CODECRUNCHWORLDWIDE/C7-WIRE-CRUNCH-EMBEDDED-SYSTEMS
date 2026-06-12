/*
 * main.c — Wi-Fi / MQTT 5 / TLS 1.2 sensor node for the Pico W.
 *
 * The whole week comes together here. On boot the node:
 *   1. loads the CYW43 firmware and joins WPA2/WPA3 Wi-Fi,
 *   2. resolves test.mosquitto.org via DNS,
 *   3. opens a raw-API lwIP TCP connection to port 8883,
 *   4. drives an mbedTLS 1.2 handshake over that TCP socket, verifying the
 *      broker's certificate against the pinned ISRG Root X1,
 *   5. sends an MQTT 5 CONNECT with a unique-id-derived client id,
 *   6. subscribes to cc7/devices/<id>/cmd for inbound commands,
 *   7. publishes a JSON telemetry reading on cc7/devices/<id>/telemetry
 *      every 10 s at QoS 0,
 *   8. sends a PINGREQ before the keep-alive window closes when idle, and
 *   9. on ANY failure, drops to an exponential-backoff-with-jitter state
 *      and reconnects from the appropriate point.
 *
 * The retry-backoff state machine (wifi_common.h's net_state_t) is the
 * load-bearing piece: every failure path lands in NET_STATE_BACKOFF, and
 * the backoff interval doubles to a 60 s cap with +-25% jitter, resetting
 * to 1 s on each successful MQTT CONNECT.
 *
 * lwIP variant: pico_cyw43_arch_lwip_poll. We never see callbacks from
 * interrupt context; cyw43_arch_poll() drives the stack from the main
 * loop, which composes cleanly with mbedTLS's want-read/want-write BIO.
 *
 * Build: see CMakeLists.txt. Links pico_stdlib, pico_cyw43_arch_lwip_poll,
 * pico_mbedtls, pico_unique_id, and the local mqtt_client object.
 *
 * Compiles clean under -Wall -Wextra at -Os, arm-none-eabi-gcc 13.x,
 * pico-sdk 1.5.1, PICO_BOARD=pico_w.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "pico/rand.h"

#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/debug.h"
#include "mbedtls/error.h"

#include "wifi_common.h"
#include "mqtt_client.h"
#include "ca_bundle.h"

/* ====================================================================== */
/* Device identity                                                        */
/* ====================================================================== */

/* The 3-hex-byte suffix derived from the RP2040 unique board id. */
static char  g_dev_id[7];                 /* e.g. "a1b2c3" + NUL */
static char  g_client_id[24];             /* "cc7-pico-a1b2c3" */
static char  g_topic_telemetry[40];       /* "cc7/devices/a1b2c3/telemetry" */
static char  g_topic_cmd[40];             /* "cc7/devices/a1b2c3/cmd" */

static void build_identity(void) {
    pico_unique_board_id_t board;
    pico_get_unique_board_id(&board);
    /* Use the last 3 bytes of the 8-byte id for the suffix. */
    (void) snprintf(g_dev_id, sizeof g_dev_id, "%02x%02x%02x",
                    board.id[5], board.id[6], board.id[7]);
    (void) snprintf(g_client_id, sizeof g_client_id, "cc7-pico-%s", g_dev_id);
    (void) snprintf(g_topic_telemetry, sizeof g_topic_telemetry,
                    "%s%s%s", MQTT_TOPIC_TELEMETRY_PREFIX, g_dev_id,
                    MQTT_TOPIC_TELEMETRY_SUFFIX);
    (void) snprintf(g_topic_cmd, sizeof g_topic_cmd,
                    "%s%s%s", MQTT_TOPIC_TELEMETRY_PREFIX, g_dev_id,
                    MQTT_TOPIC_CMD_SUFFIX);
}

/* ====================================================================== */
/* Timestamp helper                                                       */
/* ====================================================================== */

static uint32_t millis(void) {
    return (uint32_t) (time_us_64() / 1000ULL);
}

/* ====================================================================== */
/* TCP transport (lwIP raw API)                                           */
/* ====================================================================== */

/*
 * A single TCP connection's state. recv_buf is a ring of received bytes
 * that the TLS BIO read callback drains; tcp_recv_cb fills it from the
 * lwIP tick context (which, under _lwip_poll, only runs inside
 * cyw43_arch_poll()), so no locking is needed in the cooperative model.
 */
#define TCP_RX_RING  ((uint32_t) 4096U)

typedef struct {
    struct tcp_pcb *pcb;
    volatile int8_t connected;   /* 1 once SYN-ACK received */
    volatile int8_t closed;      /* 1 once peer FIN or fatal err */
    uint8_t         ring[TCP_RX_RING];
    volatile uint32_t head;      /* producer (recv_cb) */
    volatile uint32_t tail;      /* consumer (BIO recv) */
} tcp_conn_t;

static tcp_conn_t g_tcp;

static uint32_t ring_used(const tcp_conn_t *t) {
    return t->head - t->tail;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb,
                         struct pbuf *p, err_t err) {
    tcp_conn_t *t = (tcp_conn_t *) arg;
    if (p == NULL) {
        t->closed = (int8_t) 1;       /* peer FIN */
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        t->closed = (int8_t) 1;
        return err;
    }

    /* Copy the pbuf chain into the ring; drop bytes that overflow (TLS
     * records arrive faster than we drain only if we stall, which we do
     * not in the steady state). */
    uint16_t off = 0U;
    while (off < p->tot_len) {
        uint32_t free_space = TCP_RX_RING - ring_used(t);
        if (free_space == 0U) {
            break;  /* ring full; backpressure (peer will retransmit) */
        }
        uint32_t idx = t->head % TCP_RX_RING;
        uint32_t run = TCP_RX_RING - idx;
        if (run > free_space) { run = free_space; }
        uint16_t want = (uint16_t) (p->tot_len - off);
        if ((uint32_t) want > run) { want = (uint16_t) run; }
        uint16_t got = pbuf_copy_partial(p, &t->ring[idx], want, off);
        if (got == 0U) { break; }
        t->head += got;
        off = (uint16_t) (off + got);
    }

    tcp_recved(pcb, off);  /* advance the window by what we actually took */
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err) {
    tcp_conn_t *t = (tcp_conn_t *) arg;
    (void) err;
    t->pcb    = NULL;     /* lwIP already freed it; never touch it again */
    t->closed = (int8_t) 1;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    tcp_conn_t *t = (tcp_conn_t *) arg;
    (void) pcb;
    if (err != ERR_OK) {
        t->closed = (int8_t) 1;
        return err;
    }
    t->connected = (int8_t) 1;
    return ERR_OK;
}

/* Open a TCP connection to ip:port. Returns 0 on success (connected). */
static int tcp_open(tcp_conn_t *t, const ip_addr_t *ip, uint16_t port) {
    (void) memset(t, 0, sizeof *t);
    t->pcb = tcp_new();
    if (t->pcb == NULL) { return -1; }

    tcp_arg(t->pcb, t);
    tcp_recv(t->pcb, tcp_recv_cb);
    tcp_err(t->pcb, tcp_err_cb);

    if (tcp_connect(t->pcb, ip, port, tcp_connected_cb) != ERR_OK) {
        return -1;
    }

    absolute_time_t deadline = make_timeout_time_ms(15000U);
    while (t->connected == (int8_t) 0 && t->closed == (int8_t) 0) {
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(50U));
        if (time_reached(deadline)) { return -1; }
    }
    return (t->connected == (int8_t) 1) ? 0 : -1;
}

static void tcp_shutdown(tcp_conn_t *t) {
    if (t->pcb != NULL) {
        tcp_arg(t->pcb, NULL);
        tcp_recv(t->pcb, NULL);
        tcp_err(t->pcb, NULL);
        (void) tcp_close(t->pcb);
        t->pcb = NULL;
    }
    t->connected = (int8_t) 0;
    t->closed    = (int8_t) 1;
}

/* mbedTLS BIO send: hand bytes to lwIP. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    tcp_conn_t *t = (tcp_conn_t *) ctx;
    if (t->pcb == NULL || t->closed == (int8_t) 1) {
        /* Surface a fatal transport error to mbedTLS. We use the SSL-layer
         * internal-error code rather than MBEDTLS_ERR_NET_* so we do not
         * depend on mbedtls/net_sockets.h (we supply our own BIO). */
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    /* Respect the available send buffer; partial writes are fine, mbedTLS
     * will call again for the remainder. */
    uint16_t snd = tcp_sndbuf(t->pcb);
    if (snd == 0U) {
        cyw43_arch_poll();
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    uint16_t n = (len > snd) ? snd : (uint16_t) len;
    err_t e = tcp_write(t->pcb, buf, n, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) {
        return (e == ERR_MEM) ? MBEDTLS_ERR_SSL_WANT_WRITE
                              : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    (void) tcp_output(t->pcb);
    return (int) n;
}

/* mbedTLS BIO recv: drain the ring lwIP's recv_cb filled. */
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    tcp_conn_t *t = (tcp_conn_t *) ctx;
    cyw43_arch_poll();  /* let recv_cb run and fill the ring */
    uint32_t avail = ring_used(t);
    if (avail == 0U) {
        if (t->closed == (int8_t) 1) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    uint32_t n = (len < avail) ? (uint32_t) len : avail;
    for (uint32_t i = 0U; i < n; i++) {
        buf[i] = t->ring[t->tail % TCP_RX_RING];
        t->tail++;
    }
    return (int) n;
}

/* ====================================================================== */
/* TLS session                                                            */
/* ====================================================================== */

typedef struct {
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         cacert;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    int                      ready;
} tls_session_t;

static tls_session_t g_tls;

/* Hardware RNG seed source; the RP2040 ring-oscillator-backed get_rand. */
static int rng_entropy(void *ctx, unsigned char *out, size_t len) {
    (void) ctx;
    for (size_t i = 0U; i < len; i++) {
        out[i] = (unsigned char) (get_rand_32() & 0xFFU);
    }
    return 0;
}

#if defined(MBEDTLS_DEBUG_C)
static void tls_debug_cb(void *ctx, int level, const char *file,
                         int line, const char *str) {
    (void) ctx;
    (void) level;
    (void) printf("[mbedtls] %s:%04d: %s", file, line, str);
}
#endif

/* One-time TLS context init (entropy, RNG, CA parse, config). */
static int tls_global_init(tls_session_t *s) {
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->cacert);
    mbedtls_ctr_drbg_init(&s->drbg);
    mbedtls_entropy_init(&s->entropy);

    /* Add the RP2040 hardware RNG as an entropy source. */
    (void) mbedtls_entropy_add_source(&s->entropy, rng_entropy, NULL,
                                      32U, MBEDTLS_ENTROPY_SOURCE_STRONG);

    if (mbedtls_ctr_drbg_seed(&s->drbg, mbedtls_entropy_func, &s->entropy,
                              (const unsigned char *) g_client_id,
                              strlen(g_client_id)) != 0) {
        return -1;
    }

    /* Parse the pinned root. The PEM length must include the NUL. */
    if (mbedtls_x509_crt_parse(&s->cacert, CC_CA_ROOT_PEM,
                               CC_CA_ROOT_PEM_LEN) != 0) {
        return -1;
    }

    if (mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return -1;
    }
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s->conf, &s->cacert, NULL);
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->drbg);
    mbedtls_ssl_conf_max_version(&s->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3); /* TLS 1.2 */
    mbedtls_ssl_conf_min_version(&s->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
#if defined(MBEDTLS_DEBUG_C)
    mbedtls_ssl_conf_dbg(&s->conf, tls_debug_cb, NULL);
    mbedtls_debug_set_threshold(0);  /* raise to 3 for Challenge 2 */
#endif
    s->ready = 1;
    return 0;
}

/* Per-connection TLS setup + handshake. Returns 0 on success. */
static int tls_handshake(tls_session_t *s, tcp_conn_t *t,
                         const char *hostname) {
    if (mbedtls_ssl_setup(&s->ssl, &s->conf) != 0) { return -1; }
    if (mbedtls_ssl_set_hostname(&s->ssl, hostname) != 0) { return -1; }
    mbedtls_ssl_set_bio(&s->ssl, t, bio_send, bio_recv, NULL);

    absolute_time_t deadline = make_timeout_time_ms(15000U);
    int ret;
    while ((ret = mbedtls_ssl_handshake(&s->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf[96];
            mbedtls_strerror(ret, errbuf, sizeof errbuf);
            (void) printf("[%lu ms] TLS handshake FAILED rc=-0x%04x (%s)\n",
                          (unsigned long) millis(), (unsigned) -ret, errbuf);
            return -1;
        }
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(20U));
        if (time_reached(deadline)) {
            (void) printf("[%lu ms] TLS handshake timed out\n",
                          (unsigned long) millis());
            return -1;
        }
    }

    uint32_t flags = mbedtls_ssl_get_verify_result(&s->ssl);
    if (flags != 0U) {
        char vbuf[256];
        (void) mbedtls_x509_crt_verify_info(vbuf, sizeof vbuf, "  ! ", flags);
        (void) printf("[%lu ms] TLS cert verify flags 0x%08lx:\n%s",
                      (unsigned long) millis(), (unsigned long) flags, vbuf);
        return -1;
    }
    return 0;
}

/* Tear down one TLS session (between reconnects). */
static void tls_reset(tls_session_t *s) {
    mbedtls_ssl_session_reset(&s->ssl);
}

/* MQTT transport callbacks over the TLS session. */
static int32_t tls_send(void *ctx, const uint8_t *buf, uint32_t len) {
    tls_session_t *s = (tls_session_t *) ctx;
    int r = mbedtls_ssl_write(&s->ssl, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    return (r < 0) ? (int32_t) -1 : (int32_t) r;
}

static int32_t tls_recv(void *ctx, uint8_t *buf, uint32_t len) {
    tls_session_t *s = (tls_session_t *) ctx;
    int r = mbedtls_ssl_read(&s->ssl, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return -1;
    }
    return (r < 0) ? (int32_t) -1 : (int32_t) r;
}

/* ====================================================================== */
/* JSON encoder (heap-free, no float printf)                              */
/* ====================================================================== */

/*
 * Emit a compact telemetry object into `out`:
 *   {"seq":1234,"temp_c":24.7,"vbat_v":3.28,"uptime_s":140}
 * Fixed-point decimals are formatted by hand (temp in milli-degrees,
 * vbat in milli-volts) because pulling in float printf costs ~12 KB of
 * flash for one decimal place. Returns the byte count written.
 */
static uint32_t json_emit_telemetry(uint8_t *out, uint32_t outlen,
                                    uint32_t seq, int32_t temp_mC,
                                    uint32_t vbat_mV, uint32_t uptime_s) {
    int n = snprintf((char *) out, outlen,
                     "{\"seq\":%lu,"
                     "\"temp_c\":%ld.%01ld,"
                     "\"vbat_v\":%lu.%02lu,"
                     "\"uptime_s\":%lu}",
                     (unsigned long) seq,
                     (long) (temp_mC / 1000), (long) (labs(temp_mC % 1000) / 100),
                     (unsigned long) (vbat_mV / 1000),
                     (unsigned long) ((vbat_mV % 1000) / 10),
                     (unsigned long) uptime_s);
    return (n < 0 || (uint32_t) n >= outlen) ? 0U : (uint32_t) n;
}

/* ====================================================================== */
/* Sensor mock                                                            */
/* ====================================================================== */

/*
 * The mini-project ships a deterministic mock so the soak run is
 * reproducible without a physical sensor. temp drifts a slow sine-ish
 * wobble around 24.0 C; vbat decays slowly from 3.30 V. Swap these two
 * functions for real BME280 / ADC reads when you have the hardware.
 */
static int32_t sensor_temp_mC(uint32_t uptime_s) {
    /* +-1.5 C wobble with a ~5 min period, integer-only. */
    int32_t phase = (int32_t) (uptime_s % 300U);          /* 0..299 */
    int32_t tri   = (phase < 150) ? phase : (300 - phase); /* 0..150..0 */
    return 24000 + (tri - 75) * 20;                        /* ~22.5..25.5 C */
}

static uint32_t sensor_vbat_mV(uint32_t uptime_s) {
    uint32_t decay = uptime_s / 3600U;          /* 1 mV per hour, mocked */
    return (3300U > decay) ? (3300U - decay) : 3000U;
}

/* ====================================================================== */
/* Retry-backoff                                                          */
/* ====================================================================== */

static uint32_t backoff_next_ms(uint32_t current_ms) {
    uint32_t doubled = current_ms * 2U;
    if (doubled > NET_BACKOFF_MAX_MS) { doubled = NET_BACKOFF_MAX_MS; }
    /* +-25% jitter. */
    uint32_t jitter_span = (doubled * NET_BACKOFF_JITTER_PCT) / 100U;
    if (jitter_span == 0U) { jitter_span = 1U; }
    int32_t  delta = (int32_t) (get_rand_32() % (2U * jitter_span)) -
                     (int32_t) jitter_span;
    int32_t  result = (int32_t) doubled + delta;
    if (result < 0) { result = 0; }
    return (uint32_t) result;
}

/* ====================================================================== */
/* Inbound command handler                                                */
/* ====================================================================== */

static volatile uint32_t g_publish_interval_ms = PUBLISH_INTERVAL_MS;
static volatile int8_t   g_reboot_requested;

static void on_command(const char *topic, uint32_t topic_len,
                       const uint8_t *payload, uint32_t payload_len) {
    (void) topic;
    (void) topic_len;
    char cmd[64];
    uint32_t n = (payload_len < sizeof cmd - 1U) ? payload_len
                                                 : (sizeof cmd - 1U);
    (void) memcpy(cmd, payload, n);
    cmd[n] = '\0';

    (void) printf("[%lu ms] CMD recv: \"%s\"\n", (unsigned long) millis(), cmd);

    if (strcmp(cmd, "reboot") == 0) {
        g_reboot_requested = (int8_t) 1;
    } else if (strncmp(cmd, "set-interval ", 13) == 0) {
        long secs = strtol(cmd + 13, NULL, 10);
        if (secs >= 1 && secs <= 3600) {
            g_publish_interval_ms = (uint32_t) (secs * 1000);
            (void) printf("[%lu ms] interval set to %ld s\n",
                          (unsigned long) millis(), secs);
        }
    }
}

/* ====================================================================== */
/* DNS                                                                    */
/* ====================================================================== */

static volatile int8_t  g_dns_done;
static volatile int8_t  g_dns_ok;
static ip_addr_t        g_broker_ip;

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void) name;
    (void) arg;
    if (ipaddr != NULL) {
        g_broker_ip = *ipaddr;
        g_dns_ok = (int8_t) 1;
    }
    g_dns_done = (int8_t) 1;
}

static int resolve_broker(const char *host, ip_addr_t *out) {
    g_dns_done = (int8_t) 0;
    g_dns_ok   = (int8_t) 0;

    err_t e = dns_gethostbyname(host, &g_broker_ip, dns_cb, NULL);
    if (e == ERR_OK) {
        *out = g_broker_ip;          /* cached hit */
        return 0;
    }
    if (e != ERR_INPROGRESS) {
        return -1;
    }
    absolute_time_t deadline = make_timeout_time_ms(10000U);
    while (g_dns_done == (int8_t) 0) {
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(50U));
        if (time_reached(deadline)) { return -1; }
    }
    if (g_dns_ok == (int8_t) 0) { return -1; }
    *out = g_broker_ip;
    return 0;
}

/* ====================================================================== */
/* Connection bring-up (Wi-Fi -> DNS -> TCP -> TLS -> MQTT)               */
/* ====================================================================== */

static mqtt_client_t g_mqtt;

static int wifi_up(void) {
    (void) printf("[%lu ms] WIFI_DOWN: connecting to \"%s\"...\n",
                  (unsigned long) millis(), WIFI_SSID);
    int rc = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID, WIFI_PASSWORD, (uint32_t) WIFI_AUTH_MODE,
        WIFI_CONNECT_TIMEOUT_MS);
    if (rc != 0) {
        (void) printf("[%lu ms] wifi connect failed: %d\n",
                      (unsigned long) millis(), rc);
        return -1;
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    (void) printf("[%lu ms] connected; ip=%s\n",
                  (unsigned long) millis(), ip4addr_ntoa(ip));
    return 0;
}

/*
 * Bring the broker session up from a live Wi-Fi link: DNS, TCP, TLS,
 * MQTT CONNECT, SUBSCRIBE. Returns 0 on a fully-established session.
 */
static int broker_up(void) {
    ip_addr_t ip;
    if (resolve_broker(MQTT_BROKER_HOST, &ip) != 0) {
        (void) printf("[%lu ms] DNS failed for %s\n",
                      (unsigned long) millis(), MQTT_BROKER_HOST);
        return -1;
    }
    (void) printf("[%lu ms] DNS %s -> %s\n", (unsigned long) millis(),
                  MQTT_BROKER_HOST, ipaddr_ntoa(&ip));

    if (tcp_open(&g_tcp, &ip, MQTT_BROKER_PORT) != 0) {
        (void) printf("[%lu ms] TCP connect failed\n",
                      (unsigned long) millis());
        tcp_shutdown(&g_tcp);
        return -1;
    }

    tls_reset(&g_tls);
    uint32_t t0 = millis();
    if (tls_handshake(&g_tls, &g_tcp, MQTT_BROKER_HOST) != 0) {
        tcp_shutdown(&g_tcp);
        return -1;
    }
    (void) printf("[%lu ms] TLS handshake ok (%lu ms)\n",
                  (unsigned long) millis(), (unsigned long) (millis() - t0));

    mqtt_transport_t tr = { tls_send, tls_recv, &g_tls };
    mqtt_client_init(&g_mqtt, &tr, MQTT_KEEPALIVE_SECONDS, on_command);

    mqtt_connack_reason_t reason = MQTT_RC_SUCCESS;
    mqtt_result_t cr = mqtt_connect(&g_mqtt, g_client_id, &reason);
    if (cr != MQTT_OK) {
        (void) printf("[%lu ms] MQTT CONNECT rejected rc=%d reason=%u\n",
                      (unsigned long) millis(), cr, (unsigned) reason);
        tcp_shutdown(&g_tcp);
        return -1;
    }
    (void) printf("[%lu ms] MQTT CONNACK rc=0\n", (unsigned long) millis());

    if (mqtt_subscribe(&g_mqtt, 1U, g_topic_cmd) != MQTT_OK) {
        (void) printf("[%lu ms] SUBSCRIBE to %s failed\n",
                      (unsigned long) millis(), g_topic_cmd);
        /* Non-fatal: telemetry still works without the command channel. */
    } else {
        (void) printf("[%lu ms] subscribed to %s\n",
                      (unsigned long) millis(), g_topic_cmd);
    }
    return 0;
}

/* ====================================================================== */
/* Main                                                                   */
/* ====================================================================== */

int main(void) {
    (void) stdio_init_all();
    sleep_ms(2000U);

    build_identity();
    (void) printf("\n[%lu ms] cc7 sensor node boot; device-id=%s client-id=%s\n",
                  (unsigned long) millis(), g_dev_id, g_client_id);

    if (cyw43_arch_init() != 0) {
        (void) printf("[boot] cyw43_arch_init failed; halting\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    if (tls_global_init(&g_tls) != 0) {
        (void) printf("[boot] TLS global init failed; halting\n");
        return 1;
    }

    net_state_t state      = NET_STATE_WIFI_DOWN;
    uint32_t    backoff_ms = NET_BACKOFF_INITIAL_MS;
    uint32_t    seq        = 0U;
    uint32_t    boot_ms    = millis();
    absolute_time_t next_pub  = make_timeout_time_ms(0);
    absolute_time_t next_ping = make_timeout_time_ms(
        (MQTT_KEEPALIVE_SECONDS * 1000U) / 2U);

    for (;;) {
        switch (state) {
            case NET_STATE_WIFI_DOWN:
                if (wifi_up() == 0) {
                    state = NET_STATE_WIFI_UP_BROKER_DOWN;
                } else {
                    state = NET_STATE_BACKOFF;
                }
                break;

            case NET_STATE_WIFI_UP_BROKER_DOWN:
                if (broker_up() == 0) {
                    backoff_ms = NET_BACKOFF_INITIAL_MS;  /* reset on success */
                    next_pub   = make_timeout_time_ms(0);
                    next_ping  = make_timeout_time_ms(
                        (MQTT_KEEPALIVE_SECONDS * 1000U) / 2U);
                    state = NET_STATE_CONNECTED;
                } else {
                    /* Broker bring-up failed. Go to backoff; the backoff
                     * state itself decides whether to resume from Wi-Fi
                     * association or just retry the broker, based on the
                     * live link status after the sleep. */
                    state = NET_STATE_BACKOFF;
                }
                break;

            case NET_STATE_CONNECTED: {
                /* Service inbound traffic (commands, PINGRESP, broker
                 * DISCONNECT). A transport error drops us to backoff. */
                if (mqtt_poll(&g_mqtt) != MQTT_OK || g_tcp.closed) {
                    (void) printf("[%lu ms] link DOWN — entering backoff\n",
                                  (unsigned long) millis());
                    tcp_shutdown(&g_tcp);
                    state = NET_STATE_BACKOFF;
                    break;
                }

                if (g_reboot_requested) {
                    (void) printf("[%lu ms] reboot command honored\n",
                                  (unsigned long) millis());
                    (void) mqtt_disconnect(&g_mqtt);
                    sleep_ms(100U);
                    /* Trigger a watchdog reset (hands back to Week 10's
                     * bootloader, which re-validates and boots us again). */
                    watchdog_reboot(0U, 0U, 0U);
                    for (;;) { tight_loop_contents(); }
                }

                if (time_reached(next_pub)) {
                    uint32_t uptime_s = (millis() - boot_ms) / 1000U;
                    uint8_t  payload[96];
                    uint32_t plen = json_emit_telemetry(
                        payload, sizeof payload, seq,
                        sensor_temp_mC(uptime_s), sensor_vbat_mV(uptime_s),
                        uptime_s);
                    mqtt_result_t pr = mqtt_publish(&g_mqtt, g_topic_telemetry,
                                                    payload, plen);
                    if (pr == MQTT_OK) {
                        (void) printf("[%lu ms] PUB seq=%lu bytes=%lu rc=0\n",
                                      (unsigned long) millis(),
                                      (unsigned long) seq,
                                      (unsigned long) plen);
                        next_ping = make_timeout_time_ms(
                            (MQTT_KEEPALIVE_SECONDS * 1000U) / 2U);
                    } else {
                        (void) printf("[%lu ms] PUB seq=%lu FAILED rc=%d\n",
                                      (unsigned long) millis(),
                                      (unsigned long) seq, pr);
                        tcp_shutdown(&g_tcp);
                        state = NET_STATE_BACKOFF;
                    }
                    seq++;  /* increment per ATTEMPT, per the soak protocol */
                    next_pub = make_timeout_time_ms(g_publish_interval_ms);
                    break;
                }

                if (time_reached(next_ping)) {
                    if (mqtt_ping(&g_mqtt) != MQTT_OK) {
                        tcp_shutdown(&g_tcp);
                        state = NET_STATE_BACKOFF;
                        break;
                    }
                    next_ping = make_timeout_time_ms(
                        (MQTT_KEEPALIVE_SECONDS * 1000U) / 2U);
                }

                cyw43_arch_poll();
                cyw43_arch_wait_for_work_until(next_pub);
                break;
            }

            case NET_STATE_BACKOFF: {
                (void) printf("[%lu ms] backoff sleep=%lu ms\n",
                              (unsigned long) millis(),
                              (unsigned long) backoff_ms);
                absolute_time_t wake = make_timeout_time_ms(backoff_ms);
                while (!time_reached(wake)) {
                    cyw43_arch_poll();
                    cyw43_arch_wait_for_work_until(wake);
                }
                backoff_ms = backoff_next_ms(backoff_ms);

                /* Decide where to resume: if Wi-Fi is still up, retry the
                 * broker; otherwise restart from Wi-Fi association. */
                int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
                state = (link == CYW43_LINK_UP)
                        ? NET_STATE_WIFI_UP_BROKER_DOWN
                        : NET_STATE_WIFI_DOWN;
                break;
            }

            case NET_STATE_BOOT:
            default:
                state = NET_STATE_WIFI_DOWN;
                break;
        }
    }

    /* Unreachable. */
    cyw43_arch_deinit();
    return 0;
}
