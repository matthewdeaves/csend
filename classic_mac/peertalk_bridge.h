#ifndef PEERTALK_BRIDGE_MAC_H
#define PEERTALK_BRIDGE_MAC_H

#include <MacTypes.h>
#include "peertalk.h"

/* Flag set by callbacks when peer list changes */
extern Boolean gPeerListNeedsRefresh;

/* Initialize peertalk callbacks */
void bridge_mac_init(PT_Context *ctx);

#endif
