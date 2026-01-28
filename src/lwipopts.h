#pragma once

// Minimal lwIP configuration for Pico W (polled mode).

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0

#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE (16 * 1024)

#define MEMP_NUM_PBUF 32
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_TCP_PCB 8
#define MEMP_NUM_TCP_PCB_LISTEN 4
#define MEMP_NUM_TCP_SEG 16
#define MEMP_NUM_SYS_TIMEOUT 10

#define PBUF_POOL_SIZE 24
#define PBUF_POOL_BUFSIZE 1524

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1

#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_NETIF_HOSTNAME 1

#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

#define TCP_MSS 1460
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_WND (4 * TCP_MSS)
