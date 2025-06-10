//====================================
// FILE: ./classic_mac/network_abstraction.c
//====================================

#include "network_abstraction.h"
#include "mactcp_impl.h"
#include "opentransport_impl.h"
#include "../shared/logging.h"
#include <Gestalt.h>
#include <string.h>
#include <Errors.h>     /* Add this for Mac error codes */
#include <MacTCP.h>     /* Add this for MacTCP-specific errors */

/* Global network state */
NetworkOperations *gNetworkOps = NULL;
NetworkImplementation gCurrentNetworkImpl = NETWORK_IMPL_NONE;

/* Check if OpenTransport is available using proper detection */
static Boolean IsOpenTransportAvailable(void)
{
    /* Use the OpenTransport implementation's IsAvailable function */
    /* This will use InitOpenTransport() as per Apple's documentation */
    NetworkOperations *otOps = GetOpenTransportOperations();
    if (otOps != NULL && otOps->IsAvailable != NULL) {
        Boolean available = otOps->IsAvailable();
        log_debug_cat(LOG_CAT_NETWORKING, "IsOpenTransportAvailable: OpenTransport availability = %s",
                      available ? "true" : "false");
        return available;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "IsOpenTransportAvailable: Failed to get OpenTransport operations table");
    return false;
}

/* Initialize the network abstraction layer */
OSErr InitNetworkAbstraction(void)
{
    log_info_cat(LOG_CAT_NETWORKING, "InitNetworkAbstraction: Starting network abstraction initialization");

    /* Check if already initialized */
    if (gNetworkOps != NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "InitNetworkAbstraction: Already initialized with %s",
                      GetNetworkImplementationName());
        return noErr;
    }

    /* Determine which implementation to use - prefer OpenTransport if available */
    if (IsOpenTransportAvailable()) {
        log_info_cat(LOG_CAT_NETWORKING, "InitNetworkAbstraction: OpenTransport detected, using OpenTransport implementation");
        gCurrentNetworkImpl = NETWORK_IMPL_OPENTRANSPORT;
    } else {
        log_info_cat(LOG_CAT_NETWORKING, "InitNetworkAbstraction: OpenTransport not available, using MacTCP implementation");
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
        gNetworkOps = GetOpenTransportOperations();
        if (gNetworkOps == NULL) {
            log_app_event("Fatal: Failed to get OpenTransport operations table");
            return memFullErr;
        }
        break;

    case NETWORK_IMPL_NONE:
        log_app_event("Fatal: Network implementation not selected");
        return paramErr;

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
    log_debug_cat(LOG_CAT_NETWORKING, "ShutdownNetworkAbstraction: Shutting down network abstraction");

    /* The operations table is static, just clear the pointer */
    gNetworkOps = NULL;
    gCurrentNetworkImpl = NETWORK_IMPL_NONE;

    log_debug_cat(LOG_CAT_NETWORKING, "ShutdownNetworkAbstraction: Complete");
}

/* Get current network implementation type */
NetworkImplementation GetCurrentNetworkImplementation(void)
{
    return gCurrentNetworkImpl;
}

/* Get network implementation name */
const char *GetNetworkImplementationName(void)
{
    if (gNetworkOps != NULL && gNetworkOps->GetImplementationName != NULL) {
        return gNetworkOps->GetImplementationName();
    }

    switch (gCurrentNetworkImpl) {
    case NETWORK_IMPL_MACTCP:
        return "MacTCP";
    case NETWORK_IMPL_OPENTRANSPORT:
        return "OpenTransport";
    case NETWORK_IMPL_NONE:
        return "None";
    default:
        return "Unknown";
    }
}

/* Translate OS error codes to network error codes */
NetworkError TranslateOSErrToNetworkError(OSErr err)
{
    switch (err) {
    /* Success */
    case noErr:
        return NETWORK_SUCCESS;

    /* Memory errors */
    case memFullErr:
    case memWZErr:
    case nilHandleErr:
    case memSCErr:
    case memBCErr:
    case memPCErr:
    case memAZErr:
    case memPurErr:
    case memAdrErr:
    case memROZErr:
        return NETWORK_ERROR_NO_MEMORY;

    /* Parameter errors */
    case paramErr:
    case invalidStreamPtr:
    case invalidBufPtr:
    case invalidRDS:
        return NETWORK_ERROR_INVALID_PARAM;

    /* Connection errors */
    case openFailed:
    case connectionDoesntExist:
    case connectionExists:
    case duplicateSocket:
    case noResultProc:
    case noDataArea:
        return NETWORK_ERROR_CONNECTION_FAILED;

    /* Connection closed errors */
    case connectionClosing:
    case connectionTerminated:
    case TCPRemoteAbort:
        return NETWORK_ERROR_CONNECTION_CLOSED;

    /* Timeout errors */
    case commandTimeout:
        return NETWORK_ERROR_TIMEOUT;

    /* Resource busy errors */
    case streamAlreadyOpen:
    case insufficientResources:
        return NETWORK_ERROR_BUSY;

    /* Not initialized errors */
    case notOpenErr:
    case invalidLength:
        return NETWORK_ERROR_NOT_INITIALIZED;

    /* Not supported errors */
    case unimpErr:
    case badReqErr:
        return NETWORK_ERROR_NOT_SUPPORTED;

    default:
        return NETWORK_ERROR_UNKNOWN;
    }
}

/* Get human-readable error string */
const char *GetNetworkErrorString(NetworkError err)
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

/* Extended error information helper */
const char *GetMacTCPErrorString(OSErr err)
{
    switch (err) {
    /* MacTCP-specific errors */
    case ipBadLapErr:
        return "Bad network configuration";
    case ipBadCnfgErr:
        return "Bad IP configuration";
    case ipNoCnfgErr:
        return "No IP configuration";
    case ipLoadErr:
        return "Error loading MacTCP";
    case ipBadAddr:
        return "Bad IP address";
    case connectionClosing:
        return "Connection closing";
    case invalidLength:
        return "Invalid length";
    case connectionExists:
        return "Connection already exists";
    case duplicateSocket:
        return "Duplicate socket";
    case commandTimeout:
        return "Command timeout";
    case openFailed:
        return "Open failed";
    case connectionDoesntExist:
        return "Connection doesn't exist";
    case connectionTerminated:
        return "Connection terminated";
    case invalidBufPtr:
        return "Invalid buffer pointer";
    case invalidStreamPtr:
        return "Invalid stream pointer";
    case invalidRDS:
        return "Invalid RDS";
    case streamAlreadyOpen:
        return "Stream already open";
    case noResultProc:
        return "No result procedure";
    case noDataArea:
        return "No data area";
    case insufficientResources:
        return "Insufficient resources";
    case TCPRemoteAbort:
        return "Remote abort";
    default:
        return GetNetworkErrorString(TranslateOSErrToNetworkError(err));
    }
}

/* Log network error with context */
void LogNetworkError(const char *context, OSErr err)
{
    NetworkError netErr = TranslateOSErrToNetworkError(err);
    const char *errStr = GetMacTCPErrorString(err);

    if (netErr == NETWORK_ERROR_UNKNOWN) {
        log_app_event("%s: MacTCP error %d - %s", context, err, errStr);
    } else {
        log_app_event("%s: %s (MacTCP error %d)", context, errStr, err);
    }
}