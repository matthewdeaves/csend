//====================================
// FILE: ./classic_mac_ot/messaging.h
//====================================

#ifndef MESSAGING_H
#define MESSAGING_H

#include <MacTypes.h>
#include "../shared/common_defs.h"

/* Message buffer size */
#define BUFFER_SIZE 1024

/* Network ports */
#define TCP_PORT PORT_TCP
#define UDP_PORT PORT_UDP

/* Initialize and shutdown messaging */
OSErr InitMessaging(void);
void ShutdownMessaging(void);

/* Message processing - handled by OpenTransport events */
void ProcessIncomingMessage(const char* rawMessage, const char* senderIP);
void AddMessageToList(const char* username, const char* message, int isSent);
OSErr BroadcastQuitMessage(void);

/* Send messages */
OSErr SendMessageToPeer(const char* peerIP, const char* message, const char* msg_type);
OSErr BroadcastMessage(const char* message);

#endif /* MESSAGING_H */