//====================================
// FILE: ./classic_mac/opentransport_impl.h
//====================================

#ifndef OPENTRANSPORT_IMPL_H
#define OPENTRANSPORT_IMPL_H

#include "network_abstraction.h"

/* Include OpenTransport headers - Retro68 has full OpenTransport support */
#include <OpenTransport.h>
#include <OpenTptInternet.h>

/* OpenTransport is available in Retro68 */
#define HAVE_OPENTRANSPORT 1

/* OpenTransport implementation functions */
NetworkOperations *GetOpenTransportOperations(void);

/* OpenTransport-specific error translation */
NetworkError TranslateOTErrToNetworkError(OSStatus err);
const char *GetOpenTransportErrorString(OSStatus err);

/* OpenTransport initialization utilities */
OSStatus InitializeOpenTransport(void);
void ShutdownOpenTransport(void);

#endif /* OPENTRANSPORT_IMPL_H */