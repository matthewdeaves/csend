//====================================
// FILE: ./classic_mac/discovery.h
//====================================

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <MacTypes.h>
#include <MacTCP.h>  /* Need this for ip_addr and udp_port types */
#include "common_defs.h"
#include "peer.h"

#define BROADCAST_IP 0xFFFFFFFFUL
#define kMinUDPBufSize 2048

/* All MacTCP-specific types removed - now internal to implementation */

/* Public interface functions */
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum);
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum);
OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);
OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr,
                                ip_addr destIP, udp_port destPort);
OSErr BroadcastQuitMessage(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);
void PollUDPListener(short macTCPRefNum, ip_addr myLocalIP);

#endif