#ifndef TCP_H
#define TCP_H 
#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"
typedef void (*GiveTimePtr)(void);
#define kTCPDefaultTimeout 0
OSErr InitTCPListener(short macTCPRefNum);
void CleanupTCPListener(short macTCPRefNum);
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP);
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime);
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime);
extern ip_addr gCurrentConnectionIP;
extern tcp_port gCurrentConnectionPort;
extern Boolean gIsConnectionActive;
#endif
