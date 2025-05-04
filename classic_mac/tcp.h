#ifndef TCP_H
#define TCP_H 
#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"
#define kTCPDefaultTimeout 0
OSErr InitTCPListener(short macTCPRefNum);
void CleanupTCPListener(short macTCPRefNum);
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP);
extern ip_addr gCurrentConnectionIP;
extern tcp_port gCurrentConnectionPort;
extern Boolean gIsConnectionActive;
#endif
