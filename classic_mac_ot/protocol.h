//====================================
// FILE: ./classic_mac_ot/protocol.h
//====================================

#ifndef PROTOCOL_H
#define PROTOCOL_H

/* Include the shared protocol header */
#include "../shared/protocol.h"

/* Message types - use the values directly since they're enums */
#define MSG_TYPE_TEXT 0
#define MSG_TYPE_DISCOVERY 1
#define MSG_TYPE_DISCOVERY_RESPONSE 2
#define MSG_TYPE_QUIT 3

/* Backward compatibility */
#define MSG_TEXT 0
#define MSG_DISCOVERY 1
#define MSG_DISCOVERY_RESPONSE 2
#define MSG_QUIT 3

#endif /* PROTOCOL_H */