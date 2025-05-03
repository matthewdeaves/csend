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
extern UDPiopb gUDPReadPB;
extern UDPiopb gUDPBfrReturnPB;
extern Boolean gUDPReadPending;
extern Boolean gUDPBfrReturnPending;
extern unsigned long gLastBroadcastTimeTicks;
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum);
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum);
OSErr StartAsyncUDPRead(void);
OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);
OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr, ip_addr destIP, udp_port destPort);
OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize);
void ProcessUDPReceive(short macTCPRefNum, ip_addr myLocalIP);
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);
#endif
