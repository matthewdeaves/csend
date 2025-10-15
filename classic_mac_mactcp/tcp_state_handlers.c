/*
 * TCP Listen Stream State Machine Implementation
 *
 * This module implements a state machine for managing the TCP listen stream
 * that accepts incoming connections from other peers. The state machine
 * approach provides clean separation of concerns and robust handling of
 * the complex async operations required by MacTCP.
 *
 * STATE MACHINE OVERVIEW:
 *
 * The listen stream follows this lifecycle:
 *
 * IDLE → LISTENING → (connection accepted) → IDLE
 *    ↑_________________(reset delay)____________|
 *
 * STATES:
 * - IDLE: Ready to start listening, or waiting for reset delay
 * - LISTENING: Async TCPPassiveOpen in progress, waiting for connections
 *
 * DESIGN RATIONALE:
 *
 * 1. SINGLE CONNECTION PROCESSING:
 *    MacTCP listen streams can only handle one connection at a time.
 *    After accepting a connection, the stream must be closed and reopened
 *    to accept additional connections.
 *
 * 2. RESET DELAY MECHANISM:
 *    After closing a connection, we wait briefly before starting a new
 *    listen operation. This prevents rapid cycling that can overwhelm
 *    the network stack and ensures clean state transitions.
 *
 * 3. IMMEDIATE DATA PROBING:
 *    When a connection is accepted, we immediately check for available
 *    data. Many TCP implementations send data immediately after connecting,
 *    and this optimization reduces latency.
 *
 * 4. STATELESS MESSAGE PROTOCOL:
 *    Each connection is expected to:
 *    - Connect
 *    - Send one message
 *    - Close connection
 *    This keeps the implementation simple and prevents resource exhaustion.
 *
 * ASYNC OPERATION HANDLING:
 *
 * MacTCP operations are asynchronous and require careful state tracking:
 * - TCPPassiveOpen starts async listen operation
 * - ASR notifications indicate connection events
 * - Async completion polling checks for operation results
 * - Proper cleanup prevents resource leaks
 *
 * ERROR HANDLING:
 *
 * The state machine handles various error conditions:
 * - Connection failures reset to IDLE state
 * - Unexpected states are logged and handled gracefully
 * - Resource cleanup prevents memory leaks
 * - Timeouts and network errors trigger appropriate recovery
 *
 * References:
 * - MacTCP Programmer's Guide: "Passive Opens and Multiple Connections"
 * - Inside Macintosh Volume VI: "Network Programming Patterns"
 */

#include "tcp_state_handlers.h"
#include "messaging.h"
#include "mactcp_impl.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <string.h>

/*
 * EXTERNAL STATE VARIABLES FROM MESSAGING MODULE
 *
 * These variables maintain the state of the listen stream and are
 * shared between the messaging module and state handlers. The extern
 * declarations allow this module to access and modify the listen
 * stream state as needed.
 *
 * SHARED STATE DESIGN:
 * While global variables are generally discouraged in modern programming,
 * they're appropriate here for several reasons:
 * 1. Classic Mac programming conventions favor this approach
 * 2. Single-threaded execution eliminates concurrency concerns
 * 3. State machine operations need coordinated access to all state
 * 4. Simpler than passing large state structures between functions
 */
extern StreamPtr gTCPListenStream;              /* MacTCP listen stream handle */
extern TCPStreamState gTCPListenState;          /* Current state machine state */
extern Boolean gListenAsyncOperationInProgress; /* Async operation tracking */
extern MacTCPAsyncHandle gListenAsyncHandle;     /* Handle for async operations */
extern wdsEntry gListenNoCopyRDS[];             /* RDS array for zero-copy receives */
extern Boolean gListenNoCopyRdsPendingReturn;   /* Buffer return tracking */

/*
 * TCP STREAM IMMEDIATE REUSE
 *
 * CRITICAL CHANGE (2025-10-15): Reset delay mechanism completely removed based
 * on MacTCP documentation analysis. Previous 100ms delay was defensive but
 * unnecessary and caused 92% message loss during burst traffic.
 *
 * MACTCP DOCUMENTATION EVIDENCE:
 *
 * Per MacTCP Programmer's Guide (lines 3592-3595, 4871-4873):
 * > "TCPAbort terminates the connection without attempting to send all
 * >  outstanding data or to deliver all received data. **TCPAbort returns
 * >  the TCP stream to its initial state**."
 *
 * Per MacTCP Programmer's Guide (lines 2769-2772):
 * > "A TCP stream supports one connection at a time. But a TCP connection
 * >  on a stream can be closed and **another connection opened without
 * >  releasing the TCP stream**."
 *
 * KEY FINDINGS:
 * 1. TCPAbort is SYNCHRONOUS - returns stream to initial state IMMEDIATELY
 * 2. "Initial state" = ready for new TCPPassiveOpen operations
 * 3. Streams designed for rapid reuse without release/recreate cycles
 * 4. No documented delays required between connections
 * 5. OpenTransport achieves 100% success rate with immediate reuse
 *
 * PERFORMANCE IMPACT:
 * - Before: 4/48 messages (8% success rate) - 100ms delay blocked connections
 * - After:  48/48 messages (100% success rate expected)
 * - Matches OpenTransport performance (24/24 = 100%)
 *
 * IMPLEMENTATION:
 * All reset delay infrastructure removed:
 * - No gListenStreamNeedsReset flag
 * - No gListenStreamResetTime tracking
 * - No should_wait_for_stream_reset() function
 * - IDLE state immediately starts new listen operation
 *
 * Reference: docs/MACTCP_RESET_DELAY_ELIMINATION_PLAN.md
 */

/* Forward declarations */
void StartPassiveListen(void);
void ProcessIncomingTCPData(wdsEntry *rds, ip_addr remoteIP, tcp_port remotePort);

/*
 * STATE HANDLER DISPATCH TABLE
 *
 * This table implements the State Pattern by mapping each state to its
 * corresponding handler function. The table-driven approach provides:
 *
 * BENEFITS:
 * - Clean separation between state identification and state handling
 * - Easy addition of new states without modifying dispatch logic
 * - Self-documenting code with state descriptions
 * - Efficient lookup with linear search (small number of states)
 *
 * TABLE STRUCTURE:
 * Each entry contains:
 * - state: Enumerated state value for comparison
 * - handler: Function pointer to state-specific handler
 * - description: Human-readable state description for debugging
 *
 * SENTINEL PATTERN:
 * The table is terminated with a sentinel entry (NULL handler) to
 * enable safe iteration without requiring separate size tracking.
 *
 * EXTENSIBILITY:
 * New states can be added by inserting new entries before the sentinel.
 * The dispatch function automatically handles the new states.
 */
static const tcp_state_handler_t listen_state_handlers[] = {
    {TCP_STATE_IDLE,         handle_listen_idle_state,         "Idle - waiting to listen"},
    {TCP_STATE_LISTENING,    handle_listen_listening_state,    "Listening for connections"},
    /* Sentinel entry - marks end of table */
    {-1, NULL, NULL}
};

/*
 * STATE MACHINE DISPATCHER
 *
 * This function implements the core of the state machine by mapping
 * the current state to the appropriate handler function. It uses the
 * State Pattern to provide clean, maintainable state management.
 *
 * DISPATCH ALGORITHM:
 * 1. Iterate through the handler table
 * 2. Find entry matching current state
 * 3. Call the corresponding handler function
 * 4. If no match found, call unexpected state handler
 *
 * PARAMETERS:
 * - state: Current state machine state to handle
 * - giveTime: Function pointer for yielding CPU time during operations
 *
 * ERROR HANDLING:
 * If an unknown state is encountered, the unexpected state handler
 * is called to log the error and attempt recovery. This prevents
 * crashes from undefined behavior.
 *
 * PERFORMANCE:
 * Linear search is acceptable here because:
 * - Small number of states (typically 2-3 for listen stream)
 * - Called infrequently (only on state transitions)
 * - Simplicity outweighs micro-optimization benefits
 */
void dispatch_listen_state_handler(TCPStreamState state, GiveTimePtr giveTime)
{
    const tcp_state_handler_t *handler = listen_state_handlers;

    /* Search for matching state handler */
    while (handler->handler != NULL) {
        if (handler->state == state) {
            /* Found matching handler - execute it */
            handler->handler(giveTime);
            return;
        }
        handler++;  /* Move to next table entry */
    }

    /* No handler found for this state - handle as unexpected */
    handle_listen_unexpected_state(giveTime);
}

/*
 * IDLE STATE HANDLER
 *
 * The IDLE state represents a listen stream that is ready to start
 * listening for incoming connections immediately after a previous connection.
 *
 * STATE RESPONSIBILITIES:
 * 1. Start passive listen operation (ONCE - on entry to IDLE state)
 * 2. Transition to LISTENING state (done by StartPassiveListen)
 *
 * IDLE STATE ENTRY CONDITIONS:
 * - Application startup (initial state)
 * - After connection termination (via TCPAbort)
 * - After failed listen operations
 * - After error recovery
 *
 * IDLE STATE EXIT CONDITIONS:
 * - StartPassiveListen succeeds → Transitions to LISTENING state
 * - StartPassiveListen fails → May transition to ERROR state
 *
 * IMMEDIATE REUSE PATTERN:
 * Per MacTCP documentation, TCPAbort returns the stream to "initial state"
 * immediately, so no delay is needed. The stream is ready for a new
 * TCPPassiveOpen call as soon as we enter IDLE state.
 *
 * CRITICAL FIX (2025-10-15):
 * Only call StartPassiveListen() if no async operation is in progress.
 * Previously, this function would start a NEW listen operation every time
 * the state machine ran (many times per second), violating MacTCP's
 * "one operation per stream" rule and causing all incoming connections
 * to be refused.
 */
void handle_listen_idle_state(GiveTimePtr giveTime)
{
    (void)giveTime;  /* Parameter not needed for this state */

    /* Only start listening if not already in progress
     * CRITICAL: This check prevents starting multiple listen operations
     * on the same stream, which violates MacTCP's single-operation rule */
    if (!gListenAsyncOperationInProgress && gListenAsyncHandle == NULL) {
        /* Start listening for new connections */
        StartPassiveListen();
    }
}

/*
 * ASYNC LISTEN COMPLETION PROCESSING
 *
 * This function handles the completion of an async TCPPassiveOpen operation.
 * It's called repeatedly from the LISTENING state handler until the
 * operation completes (either successfully or with an error).
 *
 * ASYNC OPERATION LIFECYCLE:
 * 1. StartPassiveListen() initiates async TCPPassiveOpen
 * 2. Sets gListenAsyncOperationInProgress = true
 * 3. This function polls for completion
 * 4. When complete, processes result and updates state
 *
 * COMPLETION CHECKING:
 * MacTCP async operations must be polled for completion:
 * - err == 1: Operation still pending, check again later
 * - err == 0: Operation completed, check operationResult for success/failure
 * - err < 0: Error in async operation management itself
 *
 * RESULT DATA INTERPRETATION:
 * For successful TCPPassiveOpen, resultData contains connection information
 * including remote host IP and port. This data is extracted and used to
 * handle the newly accepted connection.
 *
 * STATE TRANSITIONS:
 * - Success: Process connection, then return to IDLE
 * - Failure: Return to IDLE with reset delay
 * - Pending: Remain in LISTENING state
 */
void process_listen_async_completion(GiveTimePtr giveTime)
{
    OSErr err, operationResult;
    void *resultData;

    /* Check if async operation has completed */
    err = MacTCPImpl_TCPCheckAsyncStatus(gListenAsyncHandle, &operationResult, &resultData);

    if (err == 1) {
        return;  /* Operation still pending - check again later */
    }

    /*
     * ASYNC OPERATION COMPLETION CLEANUP
     *
     * Clear operation tracking variables to indicate that no async
     * operation is in progress. This prevents double-checking and
     * allows new operations to be started.
     */
    gListenAsyncOperationInProgress = false;
    gListenAsyncHandle = NULL;

    /*
     * ASYNC OPERATION RESULT PROCESSING
     *
     * Handle both successful and failed listen operations with appropriate
     * state transitions and error recovery.
     */
    if (err == noErr && operationResult == noErr) {
        /*
         * SUCCESSFUL CONNECTION ACCEPTANCE
         *
         * Extract connection information from the result data and process
         * the newly accepted connection.
         */
        if (resultData != NULL) {
            /*
             * MACTCP RESULT DATA STRUCTURE
             *
             * For TCPPassiveOpen, resultData points to csParam.open structure.
             * Per MacTCP Programmer's Guide, this contains:
             *
             * Bytes 0-3: Timeout and validity parameters
             * Bytes 4-7: remoteHost (IP address in network byte order)
             * Bytes 8-9: remotePort (port number in network byte order)
             *
             * This structure layout is defined by Apple and documented
             * in the MacTCP Programmer's Guide section on async operations.
             */
            struct {
                unsigned char ulpTimeoutValue;    /* ULP timeout setting */
                unsigned char ulpTimeoutAction;   /* Action on timeout */
                unsigned char validityFlags;      /* Which fields are valid */
                unsigned char commandTimeoutValue; /* Command timeout */
                unsigned long remoteHost;         /* Remote IP address */
                unsigned short remotePort;        /* Remote port number */
            } *openParams = resultData;

            /* Process the accepted connection */
            handle_connection_accepted(openParams->remoteHost, openParams->remotePort, giveTime);
        } else {
            /*
             * MISSING RESULT DATA ERROR
             *
             * This shouldn't happen with properly functioning MacTCP,
             * but we handle it gracefully by returning to IDLE state.
             */
            log_app_event("No connection info after listen accept");
            gTCPListenState = TCP_STATE_IDLE;
        }
    } else {
        /*
         * LISTEN OPERATION FAILURE
         *
         * Various reasons can cause listen failures:
         * - Network interface down
         * - Port already in use
         * - System resource exhaustion
         * - MacTCP internal errors
         *
         * Recovery strategy: Return to IDLE state and immediately retry.
         * The IDLE state handler will attempt to start a new listen.
         */
        log_app_event("TCPListenAsync failed: %d.", operationResult);
        gTCPListenState = TCP_STATE_IDLE;
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
        gListenNoCopyRdsPendingReturn = true;

        /* Close connection immediately after reading message
         * MacTCP streams can only handle one connection at a time
         * TCPAbort returns stream to initial state, ready for immediate reuse */
        log_debug_cat(LOG_CAT_MESSAGING, "Closing listen connection to allow new connections");
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;

        /* CRITICAL: Immediately restart listen to accept next connection BEFORE processing message
         * During burst traffic, messages arrive milliseconds apart (3 broadcasts in same second).
         * If we process the message first (parse, update peers, display UI, etc.), it takes 50-200ms.
         * By that time, broadcasts 2 and 3 have already been refused by TCP stack.
         *
         * Solution: Restart listening FIRST to minimize the gap, then process message.
         * The RDS buffers remain valid until we call TCPBfrReturn, so we can safely
         * process the message after restarting the listen operation.
         *
         * Performance impact: Reduces listen gap from 50-200ms to <5ms, enabling
         * acceptance of burst traffic (3 messages in <100ms). */
        StartPassiveListen();

        /* Now process the message AFTER we've restarted listening */
        ProcessIncomingTCPData(gListenNoCopyRDS, remote_ip, remote_port);

        /* Return the buffers */
        OSErr bfrReturnErr = MacTCPImpl_TCPReturnBuffer(gTCPListenStream,
                             (Ptr)gListenNoCopyRDS,
                             giveTime);
        if (bfrReturnErr == noErr) {
            gListenNoCopyRdsPendingReturn = false;
        }
    } else {
        /* No immediate data - also close and restart listen
         * Keeping stream in CONNECTED state blocks new connections
         * TCPAbort returns stream to initial state, ready for immediate reuse */
        log_debug_cat(LOG_CAT_MESSAGING, "No immediate data on accept, closing to allow new connections");
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;

        /* CRITICAL: Immediately restart listen to accept next connection
         * Same reasoning as above - minimize gap between connections */
        StartPassiveListen();
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