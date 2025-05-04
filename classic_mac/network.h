#ifndef NETWORK_H
#define NETWORK_H 
#include <MacTypes.h>
#include "discovery.h"
#define __MACTCPCOMMONTYPES__ 
#include <AddressXlation.h>
#include "common_defs.h"
#include "tcp.h"
#define kTCPDriverName "\p.IPP"
#define ipctlGetAddr 15
extern short gMacTCPRefNum;
extern ip_addr gMyLocalIP;
extern char gMyLocalIPStr[INET_ADDRSTRLEN];
extern char gMyUsername[32];
OSErr InitializeNetworking(void);
void CleanupNetworking(void);
#endif
