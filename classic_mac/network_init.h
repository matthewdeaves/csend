//====================================
// FILE: ./classic_mac/network_init.h
//====================================

#ifndef NETWORK_INIT_H
#define NETWORK_INIT_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"

#define PREFERRED_TCP_STREAM_RCV_BUFFER_SIZE (unsigned long)(16 * 1024)
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