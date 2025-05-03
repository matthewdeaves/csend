#ifndef DISCOVERY_H
#define DISCOVERY_H 
#include <MacTypes.h>
#include <Devices.h>
#include <MacTCP.h>
#define udpCreate UDPCreate
#define udpRead UDPRead
#define udpBfrReturn UDPBfrReturn
#define udpWrite UDPWrite
#define udpRelease UDPRelease
#include "common_defs.h"
#include "peer_mac.h"
#define BROADCAST_IP 0xFFFFFFFFUL
#define kMinUDPBufSize 2048
extern StreamPtr gUDPStream;
extern Ptr gUDPRecvBuffer;
extern unsigned long gLastBroadcastTimeTicks;
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum);
OSErr SendDiscoveryBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);
void CheckUDPReceive(short macTCPRefNum, ip_addr myLocalIP);
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum);
#endif
