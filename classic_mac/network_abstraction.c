//====================================
// FILE: ./classic_mac/network_abstraction.c
//====================================

#include "network_abstraction.h"
#include "mactcp_impl.h"
#include "../shared/logging.h"
#include <Gestalt.h>
#include <string.h>
#include <Errors.h>     /* Add this for Mac error codes */
#include <MacTCP.h>     /* Add this for MacTCP-specific errors */

/* Global network state */
NetworkOperations *gNetworkOps = NULL;
NetworkImplementation gCurrentNetworkImpl = NETWORK_IMPL_NONE;

/* Check if OpenTransport is available (stub for now) */
static Boolean IsOpenTransportAvailable(void)
{
    /* TODO: Implement OpenTransport detection when adding OT support */
    /* For now, always return false to use MacTCP */
    return false;
}

/* Initialize the network abstraction layer */
OSErr InitNetworkAbstraction(void)
{
    OSErr err = noErr;
    
    log_debug("InitNetworkAbstraction: Starting network abstraction initialization");
    
    /* Check if already initialized */
    if (gNetworkOps != NULL) {
        log_debug("InitNetworkAbstraction: Already initialized with %s", 
                  GetNetworkImplementationName());
        return noErr;
    }
    
    /* Determine which implementation to use */
    if (IsOpenTransportAvailable()) {
        /* TODO: Initialize OpenTransport implementation */
        log_debug("InitNetworkAbstraction: OpenTransport detected but not yet implemented");
        gCurrentNetworkImpl = NETWORK_IMPL_MACTCP; /* Fall back to MacTCP for now */
    } else {
        log_debug("InitNetworkAbstraction: Using MacTCP implementation");
        gCurrentNetworkImpl = NETWORK_IMPL_MACTCP;
    }
    
    /* Initialize the appropriate implementation */
    switch (gCurrentNetworkImpl) {
        case NETWORK_IMPL_MACTCP:
            gNetworkOps = GetMacTCPOperations();
            if (gNetworkOps == NULL) {
                log_app_event("Fatal: Failed to get MacTCP operations table");
                return memFullErr;
            }
            break;
            
        case NETWORK_IMPL_OPENTRANSPORT:
            /* TODO: gNetworkOps = GetOpenTransportOperations(); */
            log_app_event("Fatal: OpenTransport not yet implemented");
            return unimpErr;
            
        default:
            log_app_event("Fatal: Unknown network implementation type: %d", 
                         gCurrentNetworkImpl);
            return paramErr;
    }
    
    /* Verify the implementation is available */
    if (!gNetworkOps->IsAvailable()) {
        log_app_event("Fatal: %s is not available on this system", 
                     gNetworkOps->GetImplementationName());
        gNetworkOps = NULL;
        gCurrentNetworkImpl = NETWORK_IMPL_NONE;
        return notOpenErr;
    }
    
    log_app_event("Network abstraction initialized with %s", 
                 gNetworkOps->GetImplementationName());
    return noErr;
}

/* Shutdown the network abstraction layer */
void ShutdownNetworkAbstraction(void)
{
    log_debug("ShutdownNetworkAbstraction: Shutting down network abstraction");
    
    /* The operations table is static, just clear the pointer */
    gNetworkOps = NULL;
    gCurrentNetworkImpl = NETWORK_IMPL_NONE;
    
    log_debug("ShutdownNetworkAbstraction: Complete");
}

/* Get current network implementation type */
NetworkImplementation GetCurrentNetworkImplementation(void)
{
    return gCurrentNetworkImpl;
}

/* Get network implementation name */
const char* GetNetworkImplementationName(void)
{
    if (gNetworkOps != NULL && gNetworkOps->GetImplementationName != NULL) {
        return gNetworkOps->GetImplementationName();
    }
    
    switch (gCurrentNetworkImpl) {
        case NETWORK_IMPL_MACTCP:
            return "MacTCP";
        case NETWORK_IMPL_OPENTRANSPORT:
            return "OpenTransport";
        default:
            return "None";
    }
}

/* Translate OS error codes to network error codes */
NetworkError TranslateOSErrToNetworkError(OSErr err)
{
    switch (err) {
        case noErr:
            return NETWORK_SUCCESS;
        case memFullErr:
            return NETWORK_ERROR_NO_MEMORY;
        case paramErr:
            return NETWORK_ERROR_INVALID_PARAM;
        case openFailed:
        case connectionDoesntExist:
            return NETWORK_ERROR_CONNECTION_FAILED;
        case connectionClosing:
        case connectionTerminated:
            return NETWORK_ERROR_CONNECTION_CLOSED;
        case commandTimeout:
            return NETWORK_ERROR_TIMEOUT;
        case duplicateSocket:
        case streamAlreadyOpen:
            return NETWORK_ERROR_BUSY;
        case notOpenErr:
            return NETWORK_ERROR_NOT_INITIALIZED;
        default:
            return NETWORK_ERROR_UNKNOWN;
    }
}

/* Get human-readable error string */
const char* GetNetworkErrorString(NetworkError err)
{
    switch (err) {
        case NETWORK_SUCCESS:
            return "Success";
        case NETWORK_ERROR_NOT_INITIALIZED:
            return "Network not initialized";
        case NETWORK_ERROR_INVALID_PARAM:
            return "Invalid parameter";
        case NETWORK_ERROR_NO_MEMORY:
            return "Out of memory";
        case NETWORK_ERROR_TIMEOUT:
            return "Operation timed out";
        case NETWORK_ERROR_CONNECTION_FAILED:
            return "Connection failed";
        case NETWORK_ERROR_CONNECTION_CLOSED:
            return "Connection closed";
        case NETWORK_ERROR_BUSY:
            return "Resource busy";
        case NETWORK_ERROR_NOT_SUPPORTED:
            return "Operation not supported";
        case NETWORK_ERROR_UNKNOWN:
        default:
            return "Unknown error";
    }
}