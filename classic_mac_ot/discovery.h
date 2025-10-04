//====================================
// FILE: ./classic_mac_ot/discovery.h
//====================================

#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <MacTypes.h>
#include "../shared/common_defs.h"

/* Buffer size */
#define BUFFER_SIZE 1024

/* UDP port for discovery */
#define UDP_PORT PORT_UDP

/* Initialize and shutdown discovery */
OSErr InitDiscovery(void);
void ShutdownDiscovery(void);

/* Discovery operations */
OSErr SendDiscoveryBroadcast(void);
OSErr SendDiscoveryResponse(const char* peerIP);
void ProcessDiscovery(void);
void ProcessIncomingUDPMessage(const char* buffer, int len, const char* senderIPStr, UInt32 senderIPAddr, UInt16 senderPort);

#endif /* DISCOVERY_H */