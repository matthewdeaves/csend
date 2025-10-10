//====================================
// FILE: ./classic_mac/network_init.h
//====================================

#ifndef NETWORK_INIT_H
#define NETWORK_INIT_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"

/* System 6 / Mac SE optimizations
 * Per MacTCP Programmer's Guide p.1045-1046 and p.2743-2744:
 * "The buffer memory can be allocated off the application heap instead of
 * the system heap, which is very limited."
 *
 * Mac SE (System 6.0.8, 4MB RAM) has limited system heap (~256-512 KB).
 * Standard build uses ~82 KB system heap, SE build uses ~20 KB app heap.
 */
#ifdef __MAC_SE__
/* Mac SE / System 6 configuration - optimized for limited memory */
#define PREFERRED_TCP_STREAM_RCV_BUFFER_SIZE (unsigned long)(8 * 1024)   /* 8 KB for SE */
#define TCP_SEND_STREAM_POOL_SIZE 2                                      /* 2 streams for SE */
#define USE_APPLICATION_HEAP 1                                           /* Use app heap on SE */
#else
/* Standard configuration - System 7+ with ample memory */
#define PREFERRED_TCP_STREAM_RCV_BUFFER_SIZE (unsigned long)(16 * 1024)  /* 16 KB standard */
#define TCP_SEND_STREAM_POOL_SIZE 4                                      /* 4 streams standard */
#define USE_APPLICATION_HEAP 0                                           /* System heap standard */
#endif

#define MINIMUM_TCP_STREAM_RCV_BUFFER_SIZE (unsigned long)(4 * 1024)
#define GLOBAL_USERNAME_BUFFER_SIZE 32

/* MacTCP driver name - declare as extern */
extern const unsigned char kTCPDriverName[];

extern short gMacTCPRefNum;
extern ip_addr gMyLocalIP;
extern char gMyLocalIPStr[INET_ADDRSTRLEN];
extern char gMyUsername[GLOBAL_USERNAME_BUFFER_SIZE];

OSErr InitializeNetworking(void);
void CleanupNetworking(void);
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr);
void YieldTimeToSystem(void);

#endif