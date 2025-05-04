#ifndef TCP_H
#define TCP_H 
#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"
#define kMinTCPBufSize 2048
extern StreamPtr gTCPListenStream;
extern Ptr gTCPListenRecvBuffer;
extern TCPiopb gTCPListenPB;
extern Boolean gTCPListenPending;
extern StreamPtr gTCPConnectionStream;
extern Ptr gTCPRecvBuffer;
extern TCPiopb gTCPRecvPB;
extern TCPiopb gTCPReleasePB;
extern Boolean gTCPRecvPending;
extern Boolean gTCPReleasePending;
extern ip_addr gCurrentConnectionIP;
extern tcp_port gCurrentConnectionPort;
OSErr InitTCPListener(short macTCPRefNum);
void CleanupTCPListener(short macTCPRefNum);
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP);
#endif
