/*
 * MacTCP UDP-Based Peer Discovery Implementation
 *
 * This module implements the peer discovery system using MacTCP's UDP
 * functionality. It enables automatic discovery of other P2P messenger
 * instances on the local network through broadcast messages and responses.
 *
 * DISCOVERY PROTOCOL OVERVIEW:
 *
 * 1. PERIODIC BROADCASTING:
 *    - Send discovery broadcasts at regular intervals
 *    - Use UDP broadcast to reach all network peers
 *    - Include local username and IP information
 *
 * 2. RESPONSE HANDLING:
 *    - Listen for discovery broadcasts from other peers
 *    - Send targeted responses with local peer information
 *    - Update local peer list with discovered peers
 *
 * 3. LIFECYCLE MANAGEMENT:
 *    - Send QUIT messages when application exits
 *    - Mark peers inactive when QUIT received
 *    - Timeout peers that haven't been seen recently
 *
 * MACTCP UDP ARCHITECTURE:
 *
 * MacTCP UDP has unique characteristics compared to modern UDP:
 *
 * 1. ASYNC OPERATIONS ONLY:
 *    - All operations are asynchronous (no synchronous UDP calls)
 *    - Must poll for completion of send/receive operations
 *    - Requires careful state management for overlapping operations
 *
 * 2. BUFFER OWNERSHIP MODEL:
 *    - MacTCP owns receive buffers during operations
 *    - Must explicitly return buffers after processing data
 *    - Buffer leaks will exhaust system resources
 *
 * 3. SINGLE OPERATION LIMITATION:
 *    - Only one send operation at a time per endpoint
 *    - Only one receive operation at a time per endpoint
 *    - Requires queueing for multiple concurrent operations
 *
 * 4. NO OPERATION CANCELLATION:
 *    - Per MacTCP Programmer's Guide: "No way to abort UDPWrite"
 *    - Must wait for operations to complete naturally
 *    - Queue system provides flow control when operations busy
 *
 * KEY DESIGN PATTERNS:
 *
 * 1. ASYNC STATE MACHINE:
 *    - Track state of all async operations (send, receive, buffer return)
 *    - Poll operations in main loop for completion
 *    - Handle operation completion and errors appropriately
 *
 * 2. MESSAGE QUEUEING:
 *    - Queue outgoing messages when send operation busy
 *    - Process queue when send operation becomes available
 *    - Prevents message loss during high traffic periods
 *
 * 3. BUFFER LIFECYCLE MANAGEMENT:
 *    - Careful tracking of buffer ownership between app and MacTCP
 *    - Immediate buffer return after processing received data
 *    - Error handling to prevent buffer leaks
 *
 * 4. DEFENSIVE PROGRAMMING:
 *    - Extensive error checking and logging
 *    - Graceful handling of network failures
 *    - Resource cleanup on all error paths
 *
 * PERFORMANCE CONSIDERATIONS:
 *
 * - Broadcast frequency balanced against network load
 * - Async operations prevent blocking main thread
 * - Message queueing provides flow control
 * - Buffer reuse minimizes memory allocation overhead
 *
 * References:
 * - MacTCP Programmer's Guide Chapter 3: "UDP Programming"
 * - Inside Macintosh Volume VI: "Networking"
 * - Technical Note TN1119: "MacTCP UDP Best Practices"
 */

#include "discovery.h"
#include "mactcp_impl.h"
#include "../shared/logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "../shared/discovery.h"
#include "../shared/peer_wrapper.h"
#include "network_init.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include <Errors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Memory.h>
#include <stddef.h>
#include <OSUtils.h>

/*
 * UDP SEND QUEUE IMPLEMENTATION
 *
 * MacTCP UDP has a critical limitation: only one send operation can be
 * active at a time per endpoint. Per MacTCP Programmer's Guide p.2798:
 * "There is no way to abort a UDPWrite operation once it has been started."
 *
 * QUEUEING RATIONALE:
 * When a send operation is in progress and another send is requested:
 * 1. Cannot cancel the current operation
 * 2. Cannot start a new operation until current completes
 * 3. Must queue the new message or drop it
 *
 * QUEUE DESIGN:
 * - Circular buffer with head/tail pointers
 * - Fixed size to prevent unbounded memory usage
 * - FIFO ordering ensures messages sent in request order
 * - In-use flags for debugging and validation
 *
 * FLOW CONTROL:
 * - Queue full: Drop new messages (with logging)
 * - Send complete: Process next queued message
 * - Empty queue: Idle state, ready for immediate sends
 *
 * SIZE SELECTION:
 * 8 entries chosen as reasonable balance:
 * - Large enough to handle burst traffic
 * - Small enough to prevent excessive memory usage
 * - Typical discovery traffic is low volume
 */
#define MAX_UDP_SEND_QUEUE 8

typedef struct {
    char message[BUFFER_SIZE];    /* Formatted message ready to send */
    ip_addr destIP;              /* Destination IP (network byte order) */
    udp_port destPort;           /* Destination port (network byte order) */
    Boolean inUse;               /* Debugging flag for queue validation */
} UDPQueuedMessage;

/* Forward declarations for internal functions */
static OSErr StartAsyncUDPRead(void);
static OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize);
static Boolean EnqueueUDPSend(const char *message, ip_addr destIP, udp_port destPort);
static Boolean DequeueUDPSend(UDPQueuedMessage *msg);
static void ProcessUDPSendQueue(void);

/*
 * UDP ENDPOINT STATE MANAGEMENT
 *
 * These variables track the complete state of the MacTCP UDP endpoint
 * and all associated async operations. Proper state tracking is critical
 * for correct MacTCP UDP operation.
 *
 * STATE VARIABLES:
 * - Endpoint handle for all UDP operations
 * - Receive buffer (owned by MacTCP during operations)
 * - Async operation handles for tracking completion
 * - Timing information for discovery broadcast scheduling
 *
 * HANDLE LIFECYCLE:
 * NULL handle = no operation in progress
 * Non-NULL handle = operation active, must poll for completion
 * Handles become NULL when operations complete (success or error)
 *
 * BUFFER OWNERSHIP:
 * - Application allocates receive buffer initially
 * - MacTCP owns buffer during receive operations
 * - Application must return buffer after processing data
 * - Buffer freed only during endpoint cleanup
 */
static UDPEndpointRef gUDPEndpoint = NULL;         /* MacTCP UDP endpoint handle */
static Ptr gUDPRecvBuffer = NULL;                  /* Non-relocatable receive buffer */
static MacTCPAsyncHandle gUDPReadHandle = NULL;     /* Active read operation handle */
static MacTCPAsyncHandle gUDPReturnHandle = NULL;   /* Active buffer return handle */
static MacTCPAsyncHandle gUDPSendHandle = NULL;     /* Active send operation handle */
static unsigned long gLastBroadcastTimeTicks = 0;  /* Last discovery broadcast time */

/*
 * MESSAGE FORMATTING BUFFERS
 *
 * Pre-allocated buffers for formatting discovery messages. Using static
 * buffers avoids memory allocation overhead during message processing
 * and ensures deterministic memory usage.
 *
 * BUFFER USAGE:
 * - gBroadcastBuffer: Format discovery broadcast messages
 * - gResponseBuffer: Format discovery response messages
 * - Buffers reused for each message (not persistent storage)
 * - Messages copied to queue if send operation busy
 *
 * THREAD SAFETY:
 * Classic Mac is single-threaded, so no synchronization needed.
 * Buffers are safe to reuse between operations.
 */
static char gBroadcastBuffer[BUFFER_SIZE];   /* Buffer for formatting broadcast messages */
static char gResponseBuffer[BUFFER_SIZE];    /* Buffer for formatting response messages */

/*
 * UDP SEND QUEUE STORAGE
 *
 * Circular buffer implementation for queueing UDP messages when
 * send operations are busy. Head points to next message to send,
 * tail points to next available slot.
 *
 * QUEUE STATE:
 * - Empty: head == tail
 * - Full: (tail + 1) % size == head
 * - Valid entries: head to tail (wrapping around)
 */
static UDPQueuedMessage gUDPSendQueue[MAX_UDP_SEND_QUEUE];  /* Circular send queue */
static int gUDPSendQueueHead = 0;  /* Next message to send */
static int gUDPSendQueueTail = 0;  /* Next slot for new message */

/*
 * PLATFORM INTEGRATION CALLBACKS
 *
 * These functions bridge the shared discovery logic with the Classic Mac
 * platform-specific implementation. They provide the interface between
 * the cross-platform discovery protocol and MacTCP-specific operations.
 *
 * CALLBACK DESIGN:
 * - Called from shared discovery logic when protocol events occur
 * - Handle platform-specific operations (networking, UI updates)
 * - Provide uniform interface across different platform implementations
 * - Enable code reuse between MacTCP and OpenTransport versions
 */

/*
 * Discovery Response Callback
 *
 * Called when a discovery request is received and a response should be sent.
 * Converts from shared code parameter format to MacTCP-specific format
 * and initiates the response transmission.
 *
 * PARAMETER CONVERSION:
 * - Shared code uses host byte order (cross-platform compatibility)
 * - MacTCP expects network byte order (big-endian)
 * - Simple cast is sufficient since both are 32-bit values
 */
static void mac_send_discovery_response(uint32_t dest_ip_addr_host_order, uint16_t dest_port_host_order, void *platform_context)
{
    (void)platform_context;  /* Not used in Classic Mac implementation */

    /* Convert from shared code format to MacTCP format */
    ip_addr dest_ip_net = (ip_addr)dest_ip_addr_host_order;    /* Host to network order */
    udp_port dest_port_mac = dest_port_host_order;             /* Port conversion */

    /*
     * DISCOVERY RESPONSE TRANSMISSION
     *
     * Send discovery response directly without queueing. Discovery responses
     * are typically small, infrequent, and time-sensitive, so they get
     * priority over queued messages.
     *
     * ERROR HANDLING:
     * - noErr: Response sent successfully
     * - 1: Send operation already pending, response queued internally
     * - Other: MacTCP error, log and continue (non-fatal)
     */
    OSErr sendErr = SendDiscoveryResponseSync(gMacTCPRefNum, gMyUsername, gMyLocalIPStr, dest_ip_net, dest_port_mac);

    if (sendErr != noErr && sendErr != 1) {
        /* MacTCP error occurred */
        log_error_cat(LOG_CAT_DISCOVERY, "Error sending discovery response: %d to IP 0x%lX:%u", sendErr, (unsigned long)dest_ip_net, dest_port_mac);
    } else if (sendErr == 1) {
        /* Send operation already pending - response will be queued */
        log_debug_cat(LOG_CAT_DISCOVERY, "Discovery response skipped - send already pending");
    } else {
        /* Success - log the successful response */
        char tempIPStr[INET_ADDRSTRLEN];
        MacTCPImpl_AddressToString(dest_ip_net, tempIPStr);
        log_debug_cat(LOG_CAT_DISCOVERY, "Sent DISCOVERY_RESPONSE to %s:%u", tempIPStr, dest_port_mac);
    }
}

/*
 * Peer Management Callback
 *
 * Called when discovery protocol identifies a new peer or receives
 * updated information for an existing peer. Integrates with the
 * Classic Mac peer management system.
 *
 * RETURN VALUES:
 * - Positive: New peer added to list
 * - Zero: Existing peer updated
 * - Negative: Error (peer list full, invalid data, etc.)
 */
static int mac_add_or_update_peer(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context;  /* Not used in Classic Mac implementation */
    return AddOrUpdatePeer(ip, username);
}

/*
 * Peer List Update Notification Callback
 *
 * Called when the peer list has been modified and the UI should be
 * refreshed to show current peer status. Integrates with Classic Mac
 * Dialog Manager for UI updates.
 *
 * UI UPDATE CONDITIONS:
 * - Main window must be open and valid
 * - Peer list UI components must be initialized
 * - Forces immediate refresh of peer display
 */
static void mac_notify_peer_list_updated(void *platform_context)
{
    (void)platform_context;  /* Not used in Classic Mac implementation */

    /* Update peer list display if UI is active */
    if (gMainWindow != NULL && gPeerListHandle != NULL) {
        UpdatePeerDisplayList(true);  /* Force immediate update */
    }
}

/*
 * Peer Inactive Notification Callback
 *
 * Called when a peer sends a QUIT message or times out, indicating
 * that the peer is no longer active on the network. Updates peer
 * status in the local peer management system.
 *
 * INACTIVE vs REMOVAL:
 * - Peers are marked inactive rather than immediately removed
 * - Allows for temporary network issues without losing peer information
 * - Inactive peers may be pruned later by timeout logic
 */
static void mac_mark_peer_inactive(const char *ip, void *platform_context)
{
    (void)platform_context;  /* Not used in Classic Mac implementation */
    MarkPeerInactive(ip);
}

/*
 * INITIALIZE UDP DISCOVERY ENDPOINT
 *
 * Sets up the complete UDP discovery system including endpoint creation,
 * buffer allocation, and initial async read operation. This function
 * establishes the foundation for all peer discovery operations.
 *
 * INITIALIZATION SEQUENCE:
 * 1. Validate parameters and clean up any previous state
 * 2. Initialize send queue for flow control
 * 3. Allocate non-relocatable receive buffer
 * 4. Create MacTCP UDP endpoint
 * 5. Start initial async read operation
 *
 * MEMORY MANAGEMENT:
 * Uses non-relocatable memory for receive buffer as required by MacTCP.
 * Buffer allocation strategy varies by build target:
 * - Mac SE: Application heap (system heap very limited)
 * - Standard: System heap (better isolation)
 *
 * ERROR HANDLING:
 * If any step fails, all previously allocated resources are cleaned up
 * to prevent memory leaks and resource exhaustion.
 *
 * PARAMETERS:
 * - macTCPRefNum: MacTCP driver reference number from initialization
 *
 * RETURNS:
 * - noErr: UDP discovery system ready for operation
 * - Various error codes: Specific initialization step failed
 */
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum)
{
    OSErr err;

    log_info_cat(LOG_CAT_DISCOVERY, "Initializing UDP Discovery Endpoint using MacTCPImpl...");

    /* Parameter validation */
    if (macTCPRefNum == 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (InitUDP): macTCPRefNum is 0.");
        return paramErr;
    }

    /*
     * STATE INITIALIZATION
     *
     * Clear all global state variables to ensure clean initialization.
     * This handles cases where previous initialization failed partway
     * through or cleanup was incomplete.
     */
    gUDPEndpoint = NULL;              /* No endpoint allocated yet */
    gUDPRecvBuffer = NULL;            /* No receive buffer allocated yet */
    gUDPReadHandle = NULL;            /* No async operations active */
    gUDPReturnHandle = NULL;
    gUDPSendHandle = NULL;
    gLastBroadcastTimeTicks = 0;      /* Reset broadcast timing */

    /*
     * SEND QUEUE INITIALIZATION
     *
     * Clear the entire send queue structure and reset head/tail pointers.
     * This ensures no stale message data from previous sessions.
     */
    memset(gUDPSendQueue, 0, sizeof(gUDPSendQueue));
    gUDPSendQueueHead = 0;  /* Queue is empty */
    gUDPSendQueueTail = 0;

    /*
     * CRITICAL BUFFER ALLOCATION FOR MACTCP UDP
     *
     * MacTCP has strict requirements for UDP receive buffers that differ
     * significantly from modern networking APIs:
     *
     * 1. NON-RELOCATABLE MEMORY:
     *    Buffer must not move in memory during endpoint lifetime.
     *    MacTCP stores internal pointers to this buffer.
     *
     * 2. FIXED ADDRESS REQUIREMENT:
     *    Buffer address must remain constant throughout all UDP operations.
     *    Moving the buffer will cause crashes or data corruption.
     *
     * 3. MACTCP OWNERSHIP MODEL:
     *    Per MacTCP Programmer's Guide p.2789: "The receive buffer area
     *    belongs to UDP while the stream is open and cannot be modified
     *    or relocated until UDPRelease is called."
     *
     * HEAP SELECTION STRATEGY:
     * The choice between application and system heap affects:
     * - Memory fragmentation patterns
     * - Available memory pool size
     * - Isolation between application and system
     *
     * Mac SE Build: Uses application heap because system heap is severely
     * limited (often < 1MB) and cannot accommodate network buffers.
     *
     * Standard Build: Uses system heap for better isolation and stability.
     */
#if USE_APPLICATION_HEAP
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);     /* Application heap for Mac SE */
#else
    gUDPRecvBuffer = NewPtrSysClear(kMinUDPBufSize);  /* System heap for standard build */
#endif
    if (gUDPRecvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate UDP receive buffer (%ld bytes).", (long)kMinUDPBufSize);
        return memFullErr;
    }
    log_debug_cat(LOG_CAT_DISCOVERY, "Allocated %ld bytes for UDP receive buffer (non-relocatable) at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    /* Create UDP endpoint using MacTCPImpl */
    err = MacTCPImpl_UDPCreate(macTCPRefNum, &gUDPEndpoint, PORT_UDP, gUDPRecvBuffer, kMinUDPBufSize);
    if (err != noErr || gUDPEndpoint == NULL) {
        log_app_event("Error (InitUDP): UDPCreate failed (Error: %d).", err);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
        return err;
    }

    log_info_cat(LOG_CAT_DISCOVERY, "UDP Endpoint created successfully using MacTCPImpl on port %u.", PORT_UDP);

    /* Start initial async read */
    err = StartAsyncUDPRead();
    if (err != noErr && err != 1) {
        log_app_event("Error (InitUDP): Failed to start initial async UDP read. Error: %d", err);
        CleanupUDPDiscoveryEndpoint(macTCPRefNum);
        return err;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "Initial asynchronous UDP read started.");
    return noErr;
}

/*
 * CLEANUP UDP DISCOVERY ENDPOINT
 *
 * Performs complete shutdown of the UDP discovery system in proper order
 * to prevent resource leaks and ensure clean termination. This function
 * must handle both normal shutdown and emergency cleanup scenarios.
 *
 * CLEANUP SEQUENCE:
 * 1. Cancel all pending async operations (read, send, buffer return)
 * 2. Release UDP endpoint (closes network connection)
 * 3. Dispose allocated memory buffers
 * 4. Reset global state variables
 *
 * ASYNC OPERATION CANCELLATION:
 * MacTCP async operations must be explicitly cancelled to prevent:
 * - Callbacks to invalid memory after cleanup
 * - Resource leaks from incomplete operations
 * - System instability from orphaned operations
 *
 * DEFENSIVE CLEANUP:
 * Each cleanup step includes NULL checks to handle scenarios where
 * initialization failed partway through or cleanup is called multiple times.
 *
 * PARAMETERS:
 * - macTCPRefNum: MacTCP driver reference number for endpoint operations
 */
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum)
{
    OSErr err;

    log_debug_cat(LOG_CAT_DISCOVERY, "Cleaning up UDP Discovery Endpoint...");

    /*
     * ASYNC OPERATION CANCELLATION
     *
     * Cancel all pending async operations before releasing resources.
     * This prevents MacTCP from calling into freed memory or accessing
     * invalid handles after cleanup.
     *
     * Order is important: Cancel operations before releasing the endpoint
     * they depend on.
     */
    if (gUDPReadHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Cancelling pending UDP read operation...");
        MacTCPImpl_UDPCancelAsync(gUDPReadHandle);
        gUDPReadHandle = NULL;
    }

    if (gUDPReturnHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Cancelling pending UDP buffer return operation...");
        MacTCPImpl_UDPCancelAsync(gUDPReturnHandle);
        gUDPReturnHandle = NULL;
    }

    if (gUDPSendHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Cancelling pending UDP send operation...");
        MacTCPImpl_UDPCancelAsync(gUDPSendHandle);
        gUDPSendHandle = NULL;
    }

    /* Release UDP endpoint */
    if (gUDPEndpoint != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Releasing UDP endpoint...");
        err = MacTCPImpl_UDPRelease(macTCPRefNum, gUDPEndpoint);
        if (err != noErr) {
            log_warning_cat(LOG_CAT_DISCOVERY, "UDPRelease failed during cleanup (Error: %d).", err);
        }
        gUDPEndpoint = NULL;
    }

    /* Dispose receive buffer */
    if (gUDPRecvBuffer != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
    }

    gLastBroadcastTimeTicks = 0;
    log_debug_cat(LOG_CAT_DISCOVERY, "UDP Discovery Endpoint cleanup finished.");
}

/*
 * SEND DISCOVERY BROADCAST MESSAGE
 *
 * Transmits a discovery broadcast to the local network to announce this
 * peer's presence and discover other peers. Uses UDP broadcast to reach
 * all peers on the local network segment.
 *
 * BROADCAST PROTOCOL:
 * - Message type: MSG_DISCOVERY
 * - Destination: Broadcast IP (255.255.255.255)
 * - Port: Standard discovery port (PORT_UDP)
 * - Content: Username and IP address of this peer
 *
 * ASYNC OPERATION HANDLING:
 * - If send operation already pending: Queue the broadcast
 * - If send available: Start async send immediately
 * - Uses message queue for flow control when sends are busy
 *
 * FLOW CONTROL:
 * When a send operation is already in progress, new messages are queued
 * rather than dropped. This ensures broadcasts aren't lost during busy
 * periods while maintaining proper MacTCP operation constraints.
 *
 * PARAMETERS:
 * - macTCPRefNum: MacTCP driver reference (not used directly here)
 * - myUsername: Username to include in broadcast
 * - myLocalIPStr: IP address string to include in broadcast
 *
 * RETURNS:
 * - noErr: Broadcast sent or queued successfully
 * - notOpenErr: UDP endpoint not initialized
 * - paramErr: Invalid parameters or message formatting failed
 * - memFullErr: Send queue full, broadcast dropped
 */
OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    OSErr err;
    int formatted_len;

    (void)macTCPRefNum; /* Not needed for this operation */

    /* Parameter and state validation */
    if (gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    log_debug_cat(LOG_CAT_DISCOVERY, "Sending Discovery Broadcast...");

    /* Format the message */
    formatted_len = format_message(gBroadcastBuffer, BUFFER_SIZE, MSG_DISCOVERY,
                                   generate_message_id(), myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: format_message failed for DISCOVERY");
        return paramErr;
    }

    /*
     * FLOW CONTROL FOR CONCURRENT SENDS
     *
     * MacTCP limitation: Only one UDP send operation can be active at a time
     * per endpoint. If a send is already in progress, we must queue the new
     * message rather than attempting to start a conflicting operation.
     *
     * QUEUING STRATEGY:
     * - Check if send handle is active (non-NULL)
     * - If busy: Add message to queue for later processing
     * - If available: Proceed with immediate send
     *
     * This approach ensures no messages are lost while respecting MacTCP's
     * single-operation limitation.
     */
    if (gUDPSendHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "SendDiscoveryBroadcastSync: Send pending, queueing broadcast");
        if (EnqueueUDPSend(gBroadcastBuffer, BROADCAST_IP, PORT_UDP)) {
            return noErr;  /* Successfully queued for later transmission */
        } else {
            return memFullErr;  /* Queue full - message dropped */
        }
    }

    /* Send using MacTCPImpl async UDP send */
    err = MacTCPImpl_UDPSendAsync(gUDPEndpoint, BROADCAST_IP, PORT_UDP,
                                  (Ptr)gBroadcastBuffer, formatted_len - 1,
                                  &gUDPSendHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error starting async broadcast: %d", err);
        gUDPSendHandle = NULL;
    } else {
        log_debug_cat(LOG_CAT_DISCOVERY, "Broadcast send initiated asynchronously");
    }

    return err;
}

OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr,
                                ip_addr destIP, udp_port destPort)
{
    OSErr err;
    int formatted_len;

    (void)macTCPRefNum; /* Not needed here */

    if (gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    log_debug_cat(LOG_CAT_DISCOVERY, "Sending Discovery Response to IP 0x%lX:%u...", (unsigned long)destIP, destPort);

    /* Format the message */
    formatted_len = format_message(gResponseBuffer, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE,
                                   generate_message_id(), myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: format_message failed for DISCOVERY_RESPONSE");
        return paramErr;
    }

    /* Check if a send is already pending - queue if busy */
    if (gUDPSendHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "SendDiscoveryResponseSync: Send pending, queueing response");
        if (EnqueueUDPSend(gResponseBuffer, destIP, destPort)) {
            return noErr;  /* Successfully queued */
        } else {
            return memFullErr;  /* Queue full */
        }
    }

    /* Send using MacTCPImpl async UDP send */
    err = MacTCPImpl_UDPSendAsync(gUDPEndpoint, destIP, destPort,
                                  (Ptr)gResponseBuffer, formatted_len - 1,
                                  &gUDPSendHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error starting async response: %d", err);
        gUDPSendHandle = NULL;
    } else {
        log_debug_cat(LOG_CAT_DISCOVERY, "Response send initiated asynchronously");
    }

    return err;
}

OSErr BroadcastQuitMessage(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    OSErr err;
    int formatted_len;
    static char quitBuffer[BUFFER_SIZE];

    (void)macTCPRefNum; /* Not needed here */

    if (gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    /* Check if a send is already pending - wait briefly if needed */
    if (gUDPSendHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "BroadcastQuitMessage: Send pending, waiting briefly...");
        unsigned long startTime = TickCount();
        while (gUDPSendHandle != NULL && (TickCount() - startTime) < 60) {  /* Wait up to 1 second */
            OSErr status = MacTCPImpl_UDPCheckSendStatus(gUDPSendHandle);
            if (status != 1) {  /* Not pending anymore */
                gUDPSendHandle = NULL;
                break;
            }
            YieldTimeToSystem();
        }
        if (gUDPSendHandle != NULL) {
            log_warning_cat(LOG_CAT_DISCOVERY, "BroadcastQuitMessage: Previous send still pending, sending anyway");
            gUDPSendHandle = NULL;  /* Force clear to allow quit message */
        }
    }

    log_info_cat(LOG_CAT_DISCOVERY, "Broadcasting quit message");

    /* Format the quit message */
    formatted_len = format_message(quitBuffer, BUFFER_SIZE, MSG_QUIT,
                                   generate_message_id(), myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: format_message failed for MSG_QUIT");
        return paramErr;
    }

    /* Send using MacTCPImpl async UDP send */
    err = MacTCPImpl_UDPSendAsync(gUDPEndpoint, BROADCAST_IP, PORT_UDP,
                                  (Ptr)quitBuffer, formatted_len - 1,
                                  &gUDPSendHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error broadcasting quit message: %d", err);
        gUDPSendHandle = NULL;
    } else {
        log_debug_cat(LOG_CAT_DISCOVERY, "Quit broadcast initiated asynchronously");
    }

    return err;
}

/*
 * START ASYNCHRONOUS UDP READ OPERATION
 *
 * Initiates an async UDP receive operation to listen for discovery messages
 * from other peers. This function must be called to maintain continuous
 * listening for peer discovery traffic.
 *
 * ASYNC READ PATTERN:
 * MacTCP UDP requires explicit async read operations - there's no automatic
 * "always listening" mode. The application must:
 * 1. Start async read
 * 2. Poll for completion
 * 3. Process received data
 * 4. Return buffer to MacTCP
 * 5. Start new async read (repeat cycle)
 *
 * OPERATION EXCLUSIVITY:
 * Only one read operation can be active at a time per endpoint.
 * Attempting to start a read while one is pending will fail.
 *
 * STATE VALIDATION:
 * Multiple conditions must be checked before starting a read:
 * - Endpoint must be valid and open
 * - No read operation currently pending
 * - No buffer return operation pending (blocks new reads)
 * - Receive buffer must be valid
 *
 * RETURNS:
 * - noErr: Async read operation started successfully
 * - 1: Operation already pending or conflicting operation active
 * - invalidStreamPtr: Endpoint not initialized
 * - invalidBufPtr: Receive buffer not allocated
 * - Other MacTCP error codes
 */
static OSErr StartAsyncUDPRead(void)
{
    OSErr err;

    /* Validate endpoint state */
    if (gUDPEndpoint == NULL) return invalidStreamPtr;

    /* Check for conflicting read operation */
    if (gUDPReadHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "StartAsyncUDPRead: UDP read already pending. Ignoring request.");
        return 1;  /* Operation already active */
    }

    /*
     * BUFFER RETURN CONFLICT CHECK
     *
     * MacTCP requires that buffer return operations complete before new
     * read operations can be started. This prevents conflicts over buffer
     * ownership between the application and MacTCP.
     */
    if (gUDPReturnHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "StartAsyncUDPRead: Cannot start new read, buffer return is pending. Try later.");
        return 1;  /* Wait for buffer return to complete */
    }

    /* Validate receive buffer */
    if (gUDPRecvBuffer == NULL) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (StartAsyncUDPRead): gUDPRecvBuffer is NULL.");
        return invalidBufPtr;
    }

    /* Start async receive using MacTCPImpl */
    err = MacTCPImpl_UDPReceiveAsync(gUDPEndpoint, &gUDPReadHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (StartAsyncUDPRead): UDPReceiveAsync failed. Error: %d", err);
        gUDPReadHandle = NULL;
        return err;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "StartAsyncUDPRead: Async UDP read initiated.");
    return noErr;
}

static OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize)
{
    OSErr err;

    if (gUDPEndpoint == NULL) return invalidStreamPtr;

    if (gUDPReturnHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "ReturnUDPBufferAsync: Buffer return already pending. Ignoring request.");
        return 1;
    }

    if (dataPtr == NULL) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (ReturnUDPBufferAsync): dataPtr is NULL. Cannot return.");
        return invalidBufPtr;
    }

    /* Return buffer using MacTCPImpl */
    err = MacTCPImpl_UDPReturnBufferAsync(gUDPEndpoint, dataPtr, bufferSize, &gUDPReturnHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "CRITICAL Error (ReturnUDPBufferAsync): UDPReturnBufferAsync failed. Error: %d.", err);
        gUDPReturnHandle = NULL;
        return err;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "ReturnUDPBufferAsync: Async buffer return initiated for buffer 0x%lX.", (unsigned long)dataPtr);
    return noErr;
}

void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    unsigned long currentTimeTicks = TickCount();
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60UL;

    if (gUDPEndpoint == NULL) return;

    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks;
    }

    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        log_debug_cat(LOG_CAT_DISCOVERY, "CheckSendBroadcast: Interval elapsed. Sending broadcast.");
        OSErr sendErr = SendDiscoveryBroadcastSync(macTCPRefNum, myUsername, myLocalIPStr);
        if (sendErr == noErr) {
            gLastBroadcastTimeTicks = currentTimeTicks;
        } else {
            log_error_cat(LOG_CAT_DISCOVERY, "Sync broadcast initiation FAILED (Error: %d). Will retry next interval.", sendErr);
        }
    }
}

void PollUDPListener(short macTCPRefNum, ip_addr myLocalIP)
{
    OSErr status;
    ip_addr remoteHost;
    udp_port remotePort;
    Ptr dataPtr;
    unsigned short dataLength;

    static discovery_platform_callbacks_t mac_callbacks = {
        .send_response_callback = mac_send_discovery_response,
        .add_or_update_peer_callback = mac_add_or_update_peer,
        .notify_peer_list_updated_callback = mac_notify_peer_list_updated,
        .mark_peer_inactive_callback = mac_mark_peer_inactive
    };

    (void)macTCPRefNum; /* Not needed here */

    if (gUDPEndpoint == NULL) return;

    /* Check async read status */
    if (gUDPReadHandle != NULL) {
        status = MacTCPImpl_UDPCheckAsyncStatus(gUDPReadHandle, &remoteHost, &remotePort,
                 &dataPtr, &dataLength);

        if (status == 1) {
            /* Still pending, nothing to do */
        } else if (status == noErr) {
            /* Read completed successfully */
            gUDPReadHandle = NULL;

            if (dataLength > 0) {
                if (remoteHost != myLocalIP) {
                    char senderIPStr[INET_ADDRSTRLEN];
                    MacTCPImpl_AddressToString(remoteHost, senderIPStr);

                    uint32_t sender_ip_for_shared = (uint32_t)remoteHost;
                    discovery_logic_process_packet((const char *)dataPtr, dataLength,
                                                   senderIPStr, sender_ip_for_shared, remotePort,
                                                   &mac_callbacks, NULL);
                } else {
                    char selfIPStr[INET_ADDRSTRLEN];
                    MacTCPImpl_AddressToString(remoteHost, selfIPStr);
                    log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Ignored UDP packet from self (%s).", selfIPStr);
                }

                /* Return the buffer asynchronously
                 * Per MacTCP Programmer's Guide p.1247: Must return buffer after successful UDPRead */
                OSErr returnErr = ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                if (returnErr == 1) {
                    /* Buffer return already pending - defer to next state machine cycle
                     * The buffer return operation will complete and then StartAsyncUDPRead()
                     * will be called automatically in the next iteration (see line 966-968).
                     * This maintains async operation benefits without blocking. */
                    log_debug_cat(LOG_CAT_DISCOVERY, "Buffer return pending, will retry next cycle");
                    return;  /* Exit early - retry in next PollUDPListener() call */
                } else if (returnErr != noErr) {
                    log_error_cat(LOG_CAT_DISCOVERY, "CRITICAL Error: Failed to initiate async buffer return. Error: %d", returnErr);
                } else {
                    log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Initiated return for buffer 0x%lX.", (unsigned long)dataPtr);
                }
            } else {
                log_debug_cat(LOG_CAT_DISCOVERY, "DEBUG: Async UDP read returned noErr but 0 bytes. Returning buffer.");
                (void)ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
            }
        } else {
            /* Error occurred */
            gUDPReadHandle = NULL;
            log_error_cat(LOG_CAT_DISCOVERY, "Error (PollUDPListener): Async UDP read completed with error: %d", status);

            /* Try to return buffer if possible */
            if (dataPtr != NULL) {
                (void)ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
            }
        }
    }

    /* Check buffer return completion */
    if (gUDPReturnHandle != NULL) {
        status = MacTCPImpl_UDPCheckReturnStatus(gUDPReturnHandle);

        if (status == 1) {
            /* Still pending */
        } else if (status == noErr) {
            /* Return completed successfully */
            gUDPReturnHandle = NULL;
            log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Async buffer return completed successfully.");

            /* Start a new read if none pending */
            if (gUDPReadHandle == NULL && gUDPEndpoint != NULL) {
                StartAsyncUDPRead();
            }
        } else {
            /* Error occurred */
            gUDPReturnHandle = NULL;
            log_error_cat(LOG_CAT_DISCOVERY, "CRITICAL Error: Async buffer return completed with error: %d.", status);
        }
    }

    /* Check UDP send completion */
    if (gUDPSendHandle != NULL) {
        status = MacTCPImpl_UDPCheckSendStatus(gUDPSendHandle);

        if (status == 1) {
            /* Still pending */
        } else if (status == noErr) {
            /* Send completed successfully */
            gUDPSendHandle = NULL;
            log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: UDP send completed successfully");
        } else {
            /* Error occurred */
            gUDPSendHandle = NULL;
            log_error_cat(LOG_CAT_DISCOVERY, "PollUDPListener: UDP send completed with error: %d", status);
        }
    }

    /* Ensure we always have a read pending */
    if (gUDPReadHandle == NULL && gUDPReturnHandle == NULL && gUDPEndpoint != NULL) {
        OSErr startErr = StartAsyncUDPRead();
        if (startErr != noErr && startErr != 1) {
            log_error_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Failed to start new UDP read in idle fallback. Error: %d", startErr);
        }
    }

    /* Process queued sends if send is idle */
    ProcessUDPSendQueue();
}

/* Enqueue a UDP send when send is busy */
static Boolean EnqueueUDPSend(const char *message, ip_addr destIP, udp_port destPort)
{
    int nextTail = (gUDPSendQueueTail + 1) % MAX_UDP_SEND_QUEUE;

    if (nextTail == gUDPSendQueueHead) {
        log_error_cat(LOG_CAT_DISCOVERY, "EnqueueUDPSend: Queue full, dropping message");
        return false;
    }

    strncpy(gUDPSendQueue[gUDPSendQueueTail].message, message, BUFFER_SIZE - 1);
    gUDPSendQueue[gUDPSendQueueTail].message[BUFFER_SIZE - 1] = '\0';
    gUDPSendQueue[gUDPSendQueueTail].destIP = destIP;
    gUDPSendQueue[gUDPSendQueueTail].destPort = destPort;
    gUDPSendQueue[gUDPSendQueueTail].inUse = true;
    gUDPSendQueueTail = nextTail;

    log_debug_cat(LOG_CAT_DISCOVERY, "EnqueueUDPSend: Queued message to 0x%lX:%u", (unsigned long)destIP, destPort);
    return true;
}

/* Dequeue a UDP send */
static Boolean DequeueUDPSend(UDPQueuedMessage *msg)
{
    if (gUDPSendQueueHead == gUDPSendQueueTail) {
        return false;  /* Queue empty */
    }

    *msg = gUDPSendQueue[gUDPSendQueueHead];
    gUDPSendQueue[gUDPSendQueueHead].inUse = false;
    gUDPSendQueueHead = (gUDPSendQueueHead + 1) % MAX_UDP_SEND_QUEUE;
    return true;
}

/* Process queued UDP sends when send is idle */
static void ProcessUDPSendQueue(void)
{
    UDPQueuedMessage msg;
    OSErr err;

    /* Only process queue if send is idle */
    if (gUDPSendHandle != NULL) {
        return;
    }

    if (!DequeueUDPSend(&msg)) {
        return;  /* Queue empty */
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "ProcessUDPSendQueue: Sending queued message to 0x%lX:%u",
                  (unsigned long)msg.destIP, msg.destPort);

    /* Send using MacTCPImpl async UDP send */
    err = MacTCPImpl_UDPSendAsync(gUDPEndpoint, msg.destIP, msg.destPort,
                                  (Ptr)msg.message, strlen(msg.message),
                                  &gUDPSendHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "ProcessUDPSendQueue: Failed to send queued message: %d", err);
        gUDPSendHandle = NULL;
    }
}