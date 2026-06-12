/*
 * lwipopts.h — lwIP 2.2.0 options for the sensor node.
 *
 * pico_cyw43_arch_lwip_poll requires NO_SYS=1 (no RTOS, single-threaded
 * cooperative) and the raw API only. The defaults the pico-examples repo
 * ships are a good starting point; we add DNS, raise a couple of buffer
 * counts so a TLS record plus an MQTT publish can be in flight at once,
 * and keep everything else lean for a 264 KB-SRAM target.
 *
 * Reference: lwIP 2.2.0 lwipopts and the pico-examples
 * pico_w/wifi/*/lwipopts.h baselines.
 */

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* No RTOS: lwIP runs from cyw43_arch_poll() in the main loop. */
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

/* Memory. mem_size covers the heap lwIP allocates pbufs and PCBs from. */
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

/* Core protocols. */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1

/* TCP tuning. A TLS record (up to 4 KB content) plus headers needs a send
 * buffer big enough that mbedtls_ssl_write does not stall every call. */
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define MEMP_NUM_TCP_PCB            8

/* DHCP needs a hostname to present; harmless and helps you find the node
 * in your router's lease table. */
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_DHCP_DOES_ACD_CHECK    0

/* Checksums: let the driver/hardware do nothing special; lwIP computes. */
#define LWIP_CHKSUM_ALGORITHM       3

/* Stats off in production builds (flip on while debugging memory). */
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

/* DNS: one server, small cache, matches the single-broker workload. */
#define DNS_TABLE_SIZE              2
#define DNS_MAX_NAME_LENGTH         128

#endif /* LWIPOPTS_H */
