#include "tcp_state_handlers.h"
#include "messaging.h"
#include "mactcp_impl.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <string.h>

/* External variables from messaging.c */
extern StreamPtr gTCPListenStream;
extern TCPStreamState gTCPListenState;
extern Boolean gListenStreamNeedsReset;
extern unsigned long gListenStreamResetTime;
extern Boolean gListenAsyncOperationInProgress;
extern MacTCPAsyncHandle gListenAsyncHandle;
extern wdsEntry gListenNoCopyRDS[];
extern Boolean gListenNoCopyRdsPendingReturn;

/* Constants */
#define TCP_STREAM_RESET_DELAY_TICKS 6  /* 100ms at 60Hz - reduced from 1s for faster connection acceptance */

/* Forward declarations */
void StartPassiveListen(void);
void ProcessIncomingTCPData(wdsEntry *rds, ip_addr remoteIP, tcp_port remotePort);

/* State handler table for listen stream */
static const tcp_state_handler_t listen_state_handlers[] = {
    {TCP_STATE_IDLE,         handle_listen_idle_state,         "Idle - waiting to listen"},
    {TCP_STATE_LISTENING,    handle_listen_listening_state,    "Listening for connections"},
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

/* Check if we should wait for stream reset */
Boolean should_wait_for_stream_reset(void)
{
    if (!gListenStreamNeedsReset) {
        return false;
    }

    unsigned long currentTime = TickCount();
    if ((currentTime - gListenStreamResetTime) < TCP_STREAM_RESET_DELAY_TICKS) {
        return true;  /* Still waiting */
    }

    /* Enough time has passed */
    gListenStreamNeedsReset = false;
    return false;
}

/* Handle IDLE state - start listening if ready */
void handle_listen_idle_state(GiveTimePtr giveTime)
{
    (void)giveTime;  /* Unused parameter */

    if (should_wait_for_stream_reset()) {
        return;  /* Still waiting for reset */
    }

    StartPassiveListen();
}

/* Process async listen completion */
void process_listen_async_completion(GiveTimePtr giveTime)
{
    OSErr err, operationResult;
    void *resultData;

    err = MacTCPImpl_TCPCheckAsyncStatus(gListenAsyncHandle, &operationResult, &resultData);

    if (err == 1) {
        return;  /* Still pending */
    }

    /* Operation completed */
    gListenAsyncOperationInProgress = false;
    gListenAsyncHandle = NULL;

    if (err == noErr && operationResult == noErr) {
        /* Get connection info from async result data (TCPPassiveOpen params) */
        if (resultData != NULL) {
            /* resultData points to csParam.open (TCPOpenPB structure) */
            /* Layout: ulpTimeoutValue, ulpTimeoutAction, validityFlags, commandTimeoutValue (4 bytes),
             * then remoteHost (4 bytes), remotePort (2 bytes) */
            struct {
                unsigned char ulpTimeoutValue;
                unsigned char ulpTimeoutAction;
                unsigned char validityFlags;
                unsigned char commandTimeoutValue;
                unsigned long remoteHost;
                unsigned short remotePort;
            } *openParams = resultData;

            handle_connection_accepted(openParams->remoteHost, openParams->remotePort, giveTime);
        } else {
            log_app_event("No connection info after listen accept");
            gTCPListenState = TCP_STATE_IDLE;
        }
    } else {
        log_app_event("TCPListenAsync failed: %d.", operationResult);
        gTCPListenState = TCP_STATE_IDLE;
        gListenStreamNeedsReset = true;
        gListenStreamResetTime = TickCount();
    }
}

/* Handle new connection accepted */
void handle_connection_accepted(ip_addr remote_ip, tcp_port remote_port, GiveTimePtr giveTime)
{
    char ipStr[INET_ADDRSTRLEN];

    /* Convert IP to string */
    MacTCPImpl_AddressToString(remote_ip, ipStr);

    log_app_event("Incoming TCP connection established from %s:%u.", ipStr, remote_port);

    /* Check for immediate data availability */
    Boolean urgentFlag, markFlag;
    memset(gListenNoCopyRDS, 0, sizeof(wdsEntry) * MAX_RDS_ENTRIES);

    OSErr rcvErr = MacTCPImpl_TCPReceiveNoCopy(gTCPListenStream,
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
        OSErr bfrReturnErr = MacTCPImpl_TCPReturnBuffer(gTCPListenStream,
                             (Ptr)gListenNoCopyRDS,
                             giveTime);
        if (bfrReturnErr == noErr) {
            gListenNoCopyRdsPendingReturn = false;
        }

        /* Close connection immediately after reading message
         * MacTCP streams can only handle one connection at a time
         * Close + new TCPPassiveOpen is required to accept another connection */
        log_debug_cat(LOG_CAT_MESSAGING, "Closing listen connection to allow new connections");
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;
        gListenStreamNeedsReset = true;
        gListenStreamResetTime = TickCount();
    } else {
        /* No immediate data - also close and restart listen
         * Keeping stream in CONNECTED state blocks new connections */
        log_debug_cat(LOG_CAT_MESSAGING, "No immediate data on accept, closing to allow new connections");
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;
        gListenStreamNeedsReset = true;
        gListenStreamResetTime = TickCount();
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

/* Handle unexpected states */
void handle_listen_unexpected_state(GiveTimePtr giveTime)
{
    (void)giveTime;  /* Unused */

    switch (gTCPListenState) {
    /* These are the expected states, handled by their own functions */
    case TCP_STATE_IDLE:
    case TCP_STATE_LISTENING:
        /* Should not reach here - these have their own handlers */
        log_warning_cat(LOG_CAT_MESSAGING, "Listen stream handler dispatch error for state: %d", gTCPListenState);
        break;

    /* These states are not expected for listen stream */
    case TCP_STATE_UNINITIALIZED:
    case TCP_STATE_CONNECTING_OUT:
    case TCP_STATE_CONNECTED_IN:
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