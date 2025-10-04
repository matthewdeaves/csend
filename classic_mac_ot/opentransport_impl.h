//====================================
// FILE: ./classic_mac_ot_ppc/opentransport_impl.h
//====================================

#ifndef OPENTRANSPORT_IMPL_H
#define OPENTRANSPORT_IMPL_H

#include <MacTypes.h>
#include <OpenTransport.h>
#include <stddef.h>
#include "../shared/common_defs.h"

/* Port types */
typedef UInt16 tcp_port;
typedef UInt16 udp_port;

/* IP address type - 32-bit network order */
typedef UInt32 ip_addr;

/* Buffer size constants */
#define TCP_RCV_BUFFER_SIZE 8192
#define UDP_BUFFER_SIZE 2048
#define INET_ADDRSTRLEN 16

/* Network ports from common_defs */
#define TCP_PORT PORT_TCP
#define UDP_PORT PORT_UDP

/* Initialize/shutdown Open Transport */
OSErr InitOTForApp(void);
void ShutdownOTForApp(void);

/* Create endpoints */
OSErr CreateListenEndpoint(tcp_port localPort);
OSErr CreateDiscoveryEndpoint(udp_port localPort);

/* Event polling - call from main event loop */
void PollOTEvents(void);

/* Event handlers */
void HandleTCPEvent(EndpointRef endpoint, OTResult event);
void HandleUDPEvent(EndpointRef endpoint, OTResult event);
void HandleIncomingConnection(EndpointRef endpoint);
void HandleIncomingTCPData(EndpointRef endpoint);
void HandleIncomingUDPData(EndpointRef endpoint);
void HandleConnectionClosed(EndpointRef endpoint);

/* Message sending */
OSErr SendUDPMessage(const char* message, const char* targetIP, udp_port targetPort);
OSErr SendTCPMessage(const char* message, const char* targetIP, tcp_port targetPort);

/* Utility functions */
OSErr GetLocalIPAddress(char* ipStr, size_t ipStrSize);
void SetUsername(const char* username);
const char* GetUsername(void);

#endif /* OPENTRANSPORT_IMPL_H */