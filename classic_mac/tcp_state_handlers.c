#include "tcp_state_handlers.h"
#include "messaging.h"
#include "network_abstraction.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <string.h>

/* gNetworkOps is declared in network_abstraction.h */

/* External variables from messaging.c */
extern NetworkStreamRef gTCPListenStream;
extern TCPStreamState gTCPListenState;
/* REMOVED: Reset tracking variables - listen endpoint remains persistent */
/* extern Boolean gListenStreamNeedsReset; */
/* extern unsigned long gListenStreamResetTime; */
extern Boolean gListenAsyncOperationInProgress;
extern NetworkAsyncHandle gListenAsyncHandle;
extern wdsEntry gListenNoCopyRDS[];
extern Boolean gListenNoCopyRdsPendingReturn;

/* Constants */
/* REMOVED: Reset delay - per Apple OpenTransport docs, listen endpoints should be persistent */
/* #define TCP_STREAM_RESET_DELAY_TICKS 60  -- 1 second at 60Hz -- REMOVED */
#define DATA_CHECK_INTERVAL_TICKS 30     /* 0.5 seconds */

/* Forward declarations */
void StartPassiveListen(void);
void ProcessIncomingTCPData(wdsEntry *rds, ip_addr remoteIP, tcp_port remotePort);

/* State handler table for listen stream */
static const tcp_state_handler_t listen_state_handlers[] = {
    {TCP_STATE_IDLE,         handle_listen_idle_state,         "Idle - waiting to listen"},
    {TCP_STATE_LISTENING,    handle_listen_listening_state,    "Listening for connections"},
    {TCP_STATE_CONNECTED_IN, handle_listen_connected_in_state, "Connected - receiving data"},
    /* Sentinel */
    {-1, NULL, NULL}
};

/* Dispatch to appropriate state handler */
void dispatch_listen_state_handler(TCPStreamState state, GiveTimePtr giveTime)
{
    const tcp_state_handler_t *handler = listen_state_handlers;

    while (handler->handler != NULL) {
        if (handler->state == state) {
            handler->handler(giveTime);
            return;
        }
        handler++;
    }

    /* Handle unexpected states */
    handle_listen_unexpected_state(giveTime);
}

/* REMOVED: Reset wait logic - per Apple OpenTransport documentation,
 * listen endpoints should remain persistent and immediately ready for next connection.
 * The TIME_WAIT state only applies to data endpoints, not listening endpoints. */

/* Boolean should_wait_for_stream_reset(void) -- FUNCTION REMOVED */
/*
 * REMOVED FUNCTION: This function implemented incorrect reset logic that caused
 * intermittent "Connection refused" errors. Per Apple's OpenTransport documentation,
 * listening endpoints should remain in T_IDLE state and be immediately ready
 * for the next connection without any delay.
 */

/* Handle IDLE state - start listening immediately (no reset delay) */
void handle_listen_idle_state(GiveTimePtr giveTime)
{
    (void)giveTime;  /* Unused parameter */

    /* FIXED: Per Apple OpenTransport docs, listen endpoints should remain persistent.
     * No reset delay needed - start listening immediately when in IDLE state. */
    StartPassiveListen();
}

/* Process async listen completion */
void process_listen_async_completion(GiveTimePtr giveTime)
{
    OSErr err, operationResult;
    void *resultData;

    err = gNetworkOps->TCPCheckAsyncStatus(gListenAsyncHandle, &operationResult, &resultData);

    if (err == 1) {
        return;  /* Still pending */
    }

    /* Operation completed */
    gListenAsyncOperationInProgress = false;
    gListenAsyncHandle = NULL;

    if (err == noErr && operationResult == noErr) {
        /* Get connection info */
        NetworkTCPInfo tcpInfo;
        if (gNetworkOps->TCPStatus(gTCPListenStream, &tcpInfo) == noErr) {
            /* Note: For OpenTransport, OTAccept is called automatically in the T_LISTEN notifier */
            /* The connection is already accepted when we reach this point */
            handle_connection_accepted(tcpInfo.remoteHost, tcpInfo.remotePort, giveTime);
        } else {
            log_app_event("TCPStatus failed after listen accept");
            gTCPListenState = TCP_STATE_IDLE;
        }
    } else {
        log_app_event("TCPListenAsync failed: %d.", operationResult);
        gTCPListenState = TCP_STATE_IDLE;
        /* REMOVED: Reset logic - listen endpoint immediately ready for retry */
    }
}

/* Handle new connection accepted */
void handle_connection_accepted(ip_addr remote_ip, tcp_port remote_port, GiveTimePtr giveTime)
{
    char ipStr[INET_ADDRSTRLEN];

    /* Convert IP to string */
    if (gNetworkOps->AddressToString) {
        gNetworkOps->AddressToString(remote_ip, ipStr);
    } else {
        sprintf(ipStr, "%lu.%lu.%lu.%lu",
                (remote_ip >> 24) & 0xFF, (remote_ip >> 16) & 0xFF,
                (remote_ip >> 8) & 0xFF, remote_ip & 0xFF);
    }

    log_app_event("Incoming TCP connection established from %s:%u.", ipStr, remote_port);
    gTCPListenState = TCP_STATE_CONNECTED_IN;

    /* Check for immediate data availability */
    Boolean urgentFlag, markFlag;
    memset(gListenNoCopyRDS, 0, sizeof(wdsEntry) * MAX_RDS_ENTRIES);

    OSErr rcvErr = gNetworkOps->TCPReceiveNoCopy(gTCPListenStream,
                   (Ptr)gListenNoCopyRDS,
                   MAX_RDS_ENTRIES,
                   0, /* non-blocking */
                   &urgentFlag,
                   &markFlag,
                   giveTime);

    log_debug_cat(LOG_CAT_MESSAGING, "Initial receive probe after accept: err=%d", rcvErr);

    if (rcvErr == noErr && (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL)) {
        log_debug_cat(LOG_CAT_MESSAGING, "Data already available on connection accept!");
        ProcessIncomingTCPData(gListenNoCopyRDS, remote_ip, remote_port);
        gListenNoCopyRdsPendingReturn = true;

        /* Return the buffers */
        OSErr bfrReturnErr = gNetworkOps->TCPReturnBuffer(gTCPListenStream,
                             (Ptr)gListenNoCopyRDS,
                             giveTime);
        if (bfrReturnErr == noErr) {
            gListenNoCopyRdsPendingReturn = false;
        }
    }
}

/* Handle LISTENING state - check for incoming connections */
void handle_listen_listening_state(GiveTimePtr giveTime)
{
    if (!gListenAsyncOperationInProgress || gListenAsyncHandle == NULL) {
        return;
    }

    process_listen_async_completion(giveTime);
}

/* Check for incoming data on established connection */
void check_for_incoming_data(GiveTimePtr giveTime)
{
    static unsigned long lastCheckTime = 0;
    unsigned long currentTime = TickCount();

    /* Check every DATA_CHECK_INTERVAL_TICKS */
    if (currentTime - lastCheckTime <= DATA_CHECK_INTERVAL_TICKS) {
        return;
    }

    lastCheckTime = currentTime;

    Boolean urgentFlag, markFlag;
    memset(gListenNoCopyRDS, 0, sizeof(wdsEntry) * MAX_RDS_ENTRIES);

    OSErr rcvErr = gNetworkOps->TCPReceiveNoCopy(gTCPListenStream,
                   (Ptr)gListenNoCopyRDS,
                   MAX_RDS_ENTRIES,
                   0, /* non-blocking */
                   &urgentFlag,
                   &markFlag,
                   giveTime);

    if (rcvErr == noErr && (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL)) {
        NetworkTCPInfo tcpInfo;
        if (gNetworkOps->TCPStatus(gTCPListenStream, &tcpInfo) == noErr) {
            log_debug_cat(LOG_CAT_MESSAGING, "Periodic check found data available");
            ProcessIncomingTCPData(gListenNoCopyRDS, tcpInfo.remoteHost, tcpInfo.remotePort);
            gListenNoCopyRdsPendingReturn = true;

            OSErr bfrReturnErr = gNetworkOps->TCPReturnBuffer(gTCPListenStream,
                                 (Ptr)gListenNoCopyRDS,
                                 giveTime);
            if (bfrReturnErr == noErr) {
                gListenNoCopyRdsPendingReturn = false;
            }
        }
    } else if (rcvErr == connectionClosing) {
        log_app_event("Listen connection closed by peer (periodic check).");
        gNetworkOps->TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;
        /* REMOVED: Reset logic - listen endpoint immediately ready for next connection */
    }
}

/* Handle CONNECTED_IN state - receive data from peer */
void handle_listen_connected_in_state(GiveTimePtr giveTime)
{
    if (gListenNoCopyRdsPendingReturn || gListenAsyncOperationInProgress) {
        return;  /* Busy with other operations */
    }

    check_for_incoming_data(giveTime);
}

/* Handle unexpected states */
void handle_listen_unexpected_state(GiveTimePtr giveTime)
{
    (void)giveTime;  /* Unused */

    switch (gTCPListenState) {
    /* These are the expected states, handled by their own functions */
    case TCP_STATE_IDLE:
    case TCP_STATE_LISTENING:
    case TCP_STATE_CONNECTED_IN:
        /* Should not reach here - these have their own handlers */
        log_warning_cat(LOG_CAT_MESSAGING, "Listen stream handler dispatch error for state: %d", gTCPListenState);
        break;

    /* These states are not expected for listen stream */
    case TCP_STATE_UNINITIALIZED:
    case TCP_STATE_CONNECTING_OUT:
    case TCP_STATE_CONNECTED_OUT:
    case TCP_STATE_SENDING:
    case TCP_STATE_CLOSING_GRACEFUL:
    case TCP_STATE_ABORTING:
    case TCP_STATE_RELEASING:
    case TCP_STATE_ERROR:
        log_warning_cat(LOG_CAT_MESSAGING, "Listen stream in unexpected state: %d", gTCPListenState);
        break;

    default:
        log_warning_cat(LOG_CAT_MESSAGING, "Listen stream in unknown state: %d", gTCPListenState);
        break;
    }
}