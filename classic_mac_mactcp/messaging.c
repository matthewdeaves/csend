/*
 * MacTCP TCP Messaging Implementation for Classic Mac P2P Messenger
 *
 * This module implements the core TCP networking functionality using MacTCP.
 * It follows the asynchronous programming patterns from MacTCP Programmer's Guide
 * and handles the complex state management required for reliable networking
 * on the Classic Macintosh platform.
 *
 * KEY ARCHITECTURAL PATTERNS:
 *
 * 1. CONNECTION POOLING:
 *    Uses a pool of pre-allocated TCP streams to enable concurrent message
 *    sending without blocking. Each pool entry maintains its own state machine.
 *    Reference: MacTCP Programmer's Guide Chapter 4 - "Advanced Programming"
 *
 * 2. ASR (ASYNCHRONOUS SERVICE ROUTINE) HANDLING:
 *    MacTCP uses interrupt-level callbacks (ASRs) for network events.
 *    CRITICAL: ASRs have severe restrictions on what operations are safe.
 *    Reference: Inside Macintosh Volume IV - "Writing an ASR"
 *
 * 3. STATE MACHINE ARCHITECTURE:
 *    Each connection maintains explicit state to handle async operations.
 *    States: IDLE -> CONNECTING -> CONNECTED -> SENDING -> CLOSING -> IDLE
 *
 * 4. MESSAGE QUEUEING:
 *    When all pool entries are busy, messages are queued and processed
 *    as pool entries become available.
 *
 * 5. LISTEN/ACCEPT MODEL:
 *    Single listen stream accepts incoming connections, processes one
 *    message per connection, then closes (stateless messaging protocol).
 *
 * MEMORY MANAGEMENT:
 * - Uses non-relocatable memory (NewPtrSysClear) for network buffers
 * - MacTCP requires buffers to remain at fixed addresses during I/O
 * - Careful cleanup prevents memory leaks on error conditions
 *
 * PERFORMANCE CONSIDERATIONS:
 * - Connection pooling reduces setup/teardown overhead
 * - Async operations prevent blocking the main thread
 * - Message queueing provides flow control under high load
 *
 * ERROR HANDLING:
 * - Comprehensive error logging for debugging
 * - Graceful degradation when resources exhausted
 * - Automatic timeout handling for stuck connections
 */

#include "messaging.h"
#include "tcp_state_handlers.h"
#include "mactcp_impl.h"
#include "../shared/logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "../shared/peer_wrapper.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include "dialog_messages.h"
#include "network_init.h"
#include "../shared/messaging.h"
#include <Devices.h>
#include <Errors.h>
#include <MacTypes.h>
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Events.h>
#include <OSUtils.h>
#include <MixedMode.h>

/*
 * LISTEN STREAM MANAGEMENT
 *
 * The listen stream handles incoming TCP connections from other peers.
 * Classic Mac networking requires explicit management of connection state
 * and careful coordination between main thread and interrupt-level ASR handlers.
 *
 * Design Pattern: Single persistent listen stream that:
 * 1. Accepts incoming connections asynchronously
 * 2. Receives one message per connection
 * 3. Processes message through shared protocol handler
 * 4. Closes connection (stateless messaging)
 * 5. Returns to listening state
 *
 * Memory Management:
 * - Receive buffer allocated as non-relocatable (MacTCP requirement)
 * - Buffer size configurable but typically 8KB for message protocols
 * - Buffer must remain valid for entire stream lifetime
 */
StreamPtr gTCPListenStream = kInvalidStreamPtr;     /* MacTCP stream handle for incoming connections */
static Ptr gTCPListenRcvBuffer = NULL;              /* Non-relocatable receive buffer */
static unsigned long gTCPStreamRcvBufferSize = 0;   /* Size of receive buffer in bytes */
TCPStreamState gTCPListenState = TCP_STATE_UNINITIALIZED; /* Current listen stream state */

/*
 * ASR (ASYNCHRONOUS SERVICE ROUTINE) EVENT HANDLING
 *
 * MacTCP uses interrupt-level callbacks to notify applications of network events.
 * Since ASRs execute at interrupt level, they have severe restrictions:
 *
 * SAFE OPERATIONS IN ASR:
 * - Setting simple variables and flags
 * - Copying small amounts of data
 * - Basic arithmetic and comparisons
 *
 * UNSAFE OPERATIONS IN ASR:
 * - Memory Manager calls (NewPtr, DisposePtr, etc.)
 * - Moving or purging memory
 * - Toolbox calls
 * - Synchronous MacTCP operations
 * - Accessing unlocked handles
 *
 * Solution: ASR sets flags and copies event data, main thread processes events.
 */
static volatile ASR_Event_Info gListenAsrEvent;     /* ASR event storage for listen stream */

/*
 * TCP CONNECTION POOL FOR CONCURRENT MESSAGING
 *
 * Classic implementation challenge: Single TCP stream can't handle concurrent
 * message sends without complex synchronization. Solution: Pre-allocate a pool
 * of TCP streams that can operate independently.
 *
 * POOL ARCHITECTURE:
 * - Each entry is a complete TCP stream with its own state machine
 * - Pool entries cycle: IDLE -> CONNECTING -> SENDING -> CLOSING -> IDLE
 * - When all entries busy, messages queued until entry becomes available
 * - Pool size tuned for expected concurrent message load
 *
 * BENEFITS:
 * - Enables concurrent message sending to multiple peers
 * - Reduces connection establishment latency (streams pre-created)
 * - Provides natural flow control when system under load
 * - Simplifies error handling (each connection independent)
 *
 * RESOURCE MANAGEMENT:
 * - Each pool entry has dedicated receive buffer (MacTCP requirement)
 * - Careful state tracking prevents resource leaks
 * - Timeout handling prevents stuck connections from blocking pool
 *
 * Reference: MacTCP Programmer's Guide Chapter 4 - "Using Multiple Streams"
 */
static TCPSendStreamPoolEntry gSendStreamPool[TCP_SEND_STREAM_POOL_SIZE];
static int gPoolInitialized = 0;  /* Pool initialization state flag */

/*
 * ZERO-COPY RECEIVE DATA STRUCTURES
 *
 * MacTCP's TCPNoCopyRcv provides zero-copy networking by returning pointers
 * directly into MacTCP's internal buffers. This avoids expensive memory copies
 * but requires careful buffer lifecycle management.
 *
 * RDS (Receive Data Structure) Pattern:
 * 1. Call TCPNoCopyRcv with empty RDS array
 * 2. MacTCP fills RDS with buffer pointers and lengths
 * 3. Process data directly from MacTCP buffers
 * 4. Call TCPBfrReturn to release buffers back to MacTCP
 * 5. CRITICAL: Must return ALL buffers or MacTCP will leak memory
 *
 * Reference: MacTCP Programmer's Guide Chapter 3 - "Receiving Data"
 */
wdsEntry gListenNoCopyRDS[MAX_RDS_ENTRIES + 1];     /* RDS array for zero-copy receives */
Boolean gListenNoCopyRdsPendingReturn = false;      /* Track buffers needing return */

/*
 * ASYNCHRONOUS OPERATION TRACKING
 *
 * MacTCP async operations return handles that must be polled for completion.
 * Handles remain valid until operation completes or is cancelled.
 * Proper handle management prevents resource leaks and crashes.
 */
MacTCPAsyncHandle gListenAsyncHandle = NULL;        /* Handle for async listen operation */
Boolean gListenAsyncOperationInProgress = false;    /* Track async operation state */

/*
 * MESSAGE QUEUE FOR FLOW CONTROL
 *
 * When all connection pool entries are busy, incoming message requests
 * are queued and processed as pool entries become available. This provides
 * graceful flow control and prevents message loss under high load.
 *
 * QUEUE IMPLEMENTATION:
 * - Circular buffer using head/tail pointers
 * - Fixed size array (avoids dynamic allocation complexity)
 * - Thread-safe for single producer/single consumer (main thread only)
 * - FIFO ordering ensures messages sent in request order
 *
 * FLOW CONTROL BEHAVIOR:
 * - Queue full: Reject new messages with memFullErr
 * - Pool entries busy: Queue messages for later processing
 * - Pool entry available: Dequeue and process immediately
 *
 * This design balances memory usage with performance under load.
 */
static QueuedMessage gMessageQueue[MAX_QUEUED_MESSAGES];  /* Circular message buffer */
static int gQueueHead = 0;  /* Index of next message to process */
static int gQueueTail = 0;  /* Index where next message will be stored */

/* Timeout constants
 * Per MacTCP Programmer's Guide p.2849: Minimum timeout is 2 seconds.
 * Values less than 2 will be rounded up to 2 seconds by MacTCP.
 *
 * Note: TCP_CLOSE_ULP_TIMEOUT_S was removed because we now use async close
 * (MacTCPImpl_TCPCloseAsync) which doesn't require a timeout parameter.
 * See ProcessPoolEntryStateMachine() TCP_STATE_CLOSING_GRACEFUL handling.
 */
#define TCP_RECEIVE_CMD_TIMEOUT_S 2  /* Minimum is 2 seconds per MacTCP spec */

/* Forward declarations */
void StartPassiveListen(void);
static void HandleListenASREvents(GiveTimePtr giveTime);
static void HandlePoolEntryASREvents(int poolIndex, GiveTimePtr giveTime);
void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip_from_status, tcp_port remote_port_from_status);
static Boolean EnqueueMessage(const char *peerIP, const char *msgType, const char *content);
static Boolean DequeueMessage(QueuedMessage *msg);
static int AllocatePoolEntry(void);
static void ProcessMessageQueue(GiveTimePtr giveTime);
static OSErr StartAsyncSendOnPoolEntry(int poolIndex, const char *peerIPStr, const char *message_content,
                                       const char *msg_type);
static void ProcessPoolEntryStateMachine(int poolIndex, GiveTimePtr giveTime);
static void CheckPoolEntryTimeout(int poolIndex);

/*
 * PLATFORM INTEGRATION CALLBACKS
 *
 * These callbacks bridge the shared networking protocol handlers with
 * the Classic Mac platform-specific UI and peer management. They follow
 * the callback pattern established in the shared codebase for cross-platform
 * compatibility.
 *
 * CALLBACK RESPONSIBILITIES:
 * - Update peer list in platform-specific data structures
 * - Refresh UI displays when peer state changes
 * - Handle platform-specific message display formatting
 * - Manage platform-specific peer lifecycle events
 *
 * Thread Safety: All callbacks execute on main thread only (Classic Mac
 * is single-threaded), so no synchronization required.
 */

/*
 * Peer Management Callback
 *
 * Called when TCP message processing identifies a new peer or updates
 * an existing peer's information. Integrates with Classic Mac UI by
 * updating the peer list display when changes occur.
 *
 * Returns: >0 if peer added, 0 if updated, <0 if error
 */
static int mac_tcp_add_or_update_peer_callback(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context;  /* Unused - Classic Mac uses global state */
    int addResult = AddOrUpdatePeer(ip, username);
    if (addResult > 0) {
        log_debug_cat(LOG_CAT_PEER_MGMT, "Peer added/updated via TCP: %s@%s", username, ip);
        if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
    } else if (addResult == 0) {
        log_debug_cat(LOG_CAT_PEER_MGMT, "Peer updated via TCP: %s@%s", username, ip);
    } else {
        log_error_cat(LOG_CAT_PEER_MGMT, "Peer list full or error for %s@%s from TCP.", username, ip);
    }
    return addResult;
}

/*
 * Message Display Callback
 *
 * Called when a text message is received and needs to be displayed to the user.
 * Integrates with Classic Mac Dialog Manager by appending formatted text to
 * the messages TextEdit field.
 *
 * Classic Mac UI Considerations:
 * - TextEdit requires explicit refresh after content changes
 * - Carriage return (\r) used for line endings (Mac convention)
 * - Buffer size limits prevent TextEdit overflow
 * - NULL checks required for defensive programming
 */
static void mac_tcp_display_text_message_callback(const char *username, const char *ip, const char *message_content,
        void *platform_context)
{
    (void)platform_context;  /* Unused - Classic Mac uses global state */
    (void)ip;               /* IP not shown in message display */
    char displayMsg[BUFFER_SIZE + 100];  /* Buffer for formatted message */
    if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) {
        sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : "");
        AppendToMessagesTE(displayMsg);
        AppendToMessagesTE("\r");
    }
    log_debug_cat(LOG_CAT_MESSAGING, "Message from %s@%s displayed: %s", username, ip, message_content);
}

/*
 * Peer Inactive Callback
 *
 * Called when a QUIT message is received, indicating that a peer is
 * gracefully leaving the network. Updates peer status and refreshes
 * the UI to reflect the change.
 *
 * Protocol Behavior:
 * - QUIT messages indicate intentional departure (not network failure)
 * - Peer remains in list but marked as inactive
 * - UI display updated to show inactive status
 * - Inactive peers may be pruned by timeout logic later
 */
static void mac_tcp_mark_peer_inactive_callback(const char *ip, void *platform_context)
{
    (void)platform_context;  /* Unused - Classic Mac uses global state */

    /* Defensive programming: validate parameters */
    if (!ip) return;

    log_info_cat(LOG_CAT_PEER_MGMT, "Peer %s has sent QUIT via TCP. Marking inactive.", ip);
    MarkPeerInactive(ip);
    if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true);
}

static tcp_platform_callbacks_t g_mac_tcp_callbacks = {
    .add_or_update_peer = mac_tcp_add_or_update_peer_callback,
    .display_text_message = mac_tcp_display_text_message_callback,
    .mark_peer_inactive = mac_tcp_mark_peer_inactive_callback
};

/*
 * MESSAGE QUEUE MANAGEMENT FUNCTIONS
 *
 * Implements circular buffer for queueing messages when connection pool
 * is fully utilized. Provides flow control and message ordering guarantees.
 */

/*
 * Enqueue Message for Later Processing
 *
 * Adds a message to the queue when no connection pool entries are available.
 * Uses circular buffer logic to maximize queue utilization.
 *
 * CIRCULAR BUFFER IMPLEMENTATION:
 * - Head points to next message to dequeue
 * - Tail points to where next message will be stored
 * - Queue full when (tail + 1) % size == head
 * - Queue empty when tail == head
 *
 * Returns: true if message queued successfully, false if queue full
 */
static Boolean EnqueueMessage(const char *peerIP, const char *msgType, const char *content)
{
    int nextTail = (gQueueTail + 1) % MAX_QUEUED_MESSAGES;

    /* Check for queue overflow */
    if (nextTail == gQueueHead) {
        log_error_cat(LOG_CAT_MESSAGING, "EnqueueMessage: Queue full, cannot enqueue message to %s", peerIP);
        return false;
    }

    QueuedMessage *msg = &gMessageQueue[gQueueTail];
    strncpy(msg->peerIP, peerIP, INET_ADDRSTRLEN - 1);
    msg->peerIP[INET_ADDRSTRLEN - 1] = '\0';
    strncpy(msg->messageType, msgType, 31);
    msg->messageType[31] = '\0';
    strncpy(msg->content, content ? content : "", BUFFER_SIZE - 1);
    msg->content[BUFFER_SIZE - 1] = '\0';
    msg->inUse = true;

    gQueueTail = nextTail;
    log_debug_cat(LOG_CAT_MESSAGING, "EnqueueMessage: Queued message to %s (type: %s)", peerIP, msgType);
    return true;
}

/*
 * Dequeue Message for Processing
 *
 * Removes the oldest message from the queue for processing by an available
 * connection pool entry. Maintains FIFO ordering and proper queue state.
 *
 * Returns: true if message dequeued, false if queue empty
 */
static Boolean DequeueMessage(QueuedMessage *msg)
{
    /* Check for queue underflow */
    if (gQueueHead == gQueueTail) {
        return false;  /* Queue empty */
    }

    /* Copy message data and advance head pointer */
    *msg = gMessageQueue[gQueueHead];
    gMessageQueue[gQueueHead].inUse = false;  /* Mark slot as available */
    gQueueHead = (gQueueHead + 1) % MAX_QUEUED_MESSAGES;  /* Circular advance */
    return true;
}

/* Find an IDLE pool entry for sending a message
 * Returns pool index if found, -1 if no IDLE entries available
 */
static int AllocatePoolEntry(void)
{
    int i;

    if (!gPoolInitialized) return -1;

    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        if (gSendStreamPool[i].state == TCP_STATE_IDLE) {
            return i;
        }
    }

    return -1; /* No IDLE entries available */
}

static void ProcessMessageQueue(GiveTimePtr giveTime)
{
    QueuedMessage msg;
    int poolIndex;

    (void)giveTime; /* Unused parameter */

    if (!gPoolInitialized) return;

    /* Try to process one queued message per call */
    poolIndex = AllocatePoolEntry();
    if (poolIndex >= 0) {
        if (DequeueMessage(&msg)) {
            log_debug_cat(LOG_CAT_MESSAGING, "ProcessMessageQueue: Pool[%d] processing queued message to %s",
                          poolIndex, msg.peerIP);
            StartAsyncSendOnPoolEntry(poolIndex, msg.peerIP, msg.content, msg.messageType);
        }
    }
}

int GetQueuedMessageCount(void)
{
    int count = 0;
    int idx = gQueueHead;
    while (idx != gQueueTail) {
        if (gMessageQueue[idx].inUse) {
            count++;
        }
        idx = (idx + 1) % MAX_QUEUED_MESSAGES;
    }
    return count;
}

OSErr MacTCP_QueueMessage(const char *peerIPStr, const char *message_content, const char *msg_type)
{
    int poolIndex;
    int queuedCount;

    if (peerIPStr == NULL || msg_type == NULL) {
        return paramErr;
    }

    if (!gPoolInitialized) {
        log_error_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Pool not initialized");
        return notOpenErr;
    }

    /* Try to allocate a pool entry for immediate send */
    poolIndex = AllocatePoolEntry();
    if (poolIndex >= 0) {
        log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Pool[%d] available, attempting immediate send to %s",
                      poolIndex, peerIPStr);
        return StartAsyncSendOnPoolEntry(poolIndex, peerIPStr, message_content, msg_type);
    }

    /* No pool entry available - queue the message */
    queuedCount = GetQueuedMessageCount();
    log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: All pool entries busy (%d queued), queueing message to %s",
                  queuedCount, peerIPStr);

    if (EnqueueMessage(peerIPStr, msg_type, message_content)) {
        log_debug_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Message queued (queue: %d/%d)",
                      queuedCount + 1, MAX_QUEUED_MESSAGES);
        return noErr;
    } else {
        log_error_cat(LOG_CAT_MESSAGING, "MacTCP_QueueMessage: Failed to queue message - queue full");
        return memFullErr;
    }
}

/*
 * ASR (ASYNCHRONOUS SERVICE ROUTINE) FOR LISTEN STREAM
 *
 * This is one of the most critical and dangerous parts of Classic Mac networking.
 * ASRs execute at INTERRUPT LEVEL with severe restrictions on allowed operations.
 *
 * INTERRUPT LEVEL EXECUTION CONTEXT:
 * ===================================
 *
 * When MacTCP detects network events (data arrival, connection termination, etc.),
 * it calls this routine IMMEDIATELY at interrupt level, preempting normal program
 * execution. This provides ultra-low latency notification but imposes restrictions.
 *
 * REGISTER PRESERVATION REQUIREMENTS:
 * Per Inside Macintosh Volume IV, Chapter 6 "Writing an ASR":
 * - MUST preserve registers A3-A7, D3-D7 (caller's registers)
 * - MAY modify registers A0-A2, D0-D2 (scratch registers)
 * - A5 register may not point to application globals (use SetA5/GetA5)
 *
 * FORBIDDEN OPERATIONS (Will crash or corrupt system):
 * - Memory Manager calls: NewPtr, DisposePtr, NewHandle, HLock, etc.
 * - Moving or purging memory blocks
 * - Depending on validity of unlocked handles
 * - Synchronous MacTCP calls (will deadlock)
 * - Most Toolbox calls (Window Manager, Menu Manager, etc.)
 * - File system operations
 * - Calls that might trigger memory movement
 *
 * SAFE OPERATIONS:
 * - Setting simple variables and flags
 * - Copying small amounts of data between fixed memory locations
 * - Basic arithmetic and logical operations
 * - Calling asynchronous MacTCP functions (carefully)
 *
 * IMPLEMENTATION STRATEGY:
 * The ASR does minimal work - just copies event information into pre-allocated
 * storage and sets a flag. The main thread polls for these flags and does the
 * actual processing at normal execution level where all operations are safe.
 *
 * MEMORY ACCESS PATTERN:
 * All data accessed by ASR must be in non-relocatable memory or application
 * globals (if A5 world properly established). This is why we use 'volatile'
 * for ASR-accessed variables.
 *
 * References:
 * - Inside Macintosh Volume IV, Chapter 6: "Writing Device Drivers"
 * - MacTCP Programmer's Guide, Chapter 2: "Asynchronous Service Routines"
 * - Technical Note TN1104: "MacTCP and the 68000 Processor"
 */
/*
 * Listen Stream ASR Implementation
 *
 * This function executes at interrupt level when network events occur on
 * the listen stream. It must be extremely careful about what operations
 * it performs.
 *
 * PARAMETERS:
 * - tcpStream: MacTCP stream that generated the event
 * - eventCode: Type of event (data arrival, termination, etc.)
 * - userDataPtr: User-defined data (unused in our implementation)
 * - terminReason: Additional info for termination events
 * - icmpMsg: ICMP error information (if applicable)
 *
 * EXECUTION SAFETY:
 * - Validates stream identity using simple pointer comparison
 * - Checks for pending events to avoid overwriting unprocessed events
 * - Performs minimal data copying using safe operations
 * - Sets atomic flag to notify main thread
 */
pascal void TCP_Listen_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
                                   unsigned short terminReason, struct ICMPReport *icmpMsg)
{
    (void)userDataPtr; /* Unused parameter - avoid compiler warning */

    /* Validate stream identity - must be our listen stream */
    if (gTCPListenStream == kInvalidStreamPtr || tcpStream != (StreamPtr)gTCPListenStream) {
        return;  /* Not our stream - ignore event */
    }

    /* Prevent event overwriting - drop new events if one pending */
    if (gListenAsrEvent.eventPending) {
        return;  /* Event already pending - drop this one to avoid corruption */
    }

    /*
     * SAFE EVENT DATA STORAGE AT INTERRUPT LEVEL
     *
     * Store event information using only operations safe at interrupt level.
     * No Memory Manager calls, no handle dereferencing, no function calls
     * that might trigger memory movement.
     */

    /* Store basic event information (simple assignment - safe) */
    gListenAsrEvent.eventCode = (TCPEventCode)eventCode;
    gListenAsrEvent.termReason = terminReason;

    /* Handle ICMP error information if present */
    if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
        /*
         * CRITICAL SAFETY NOTE:
         * Direct struct assignment (*icmpMsg) is SAFE at interrupt level because:
         * 1. No Memory Manager calls involved
         * 2. Simple memory copy operation
         * 3. Both source and destination are fixed memory locations
         *
         * DO NOT use BlockMoveData() here - it's a Memory Manager call and
         * will crash or corrupt memory when called at interrupt level.
         */
        gListenAsrEvent.icmpReport = *icmpMsg;
    } else {
        /*
         * Zero ICMP structure manually without Memory Manager calls.
         * memset() might call Memory Manager internally, so use explicit loop.
         * This ensures safe operation at interrupt level.
         */
        char *dst = (char *)&gListenAsrEvent.icmpReport;
        int i;
        for (i = 0; i < sizeof(ICMPReport); i++) {
            dst[i] = 0;
        }
    }

    /* Set event pending flag - atomic operation, safe at interrupt level */
    gListenAsrEvent.eventPending = true;
}

/*
 * SEND POOL ASR HANDLER - MULTIPLEXED EVENT HANDLING
 *
 * This ASR handles events for ALL entries in the connection pool. Since each
 * pool entry is an independent TCP stream, we must identify which entry
 * generated the event and store the event information appropriately.
 *
 * MULTIPLEXING CHALLENGE:
 * MacTCP only provides the StreamPtr in the ASR callback, but we need to
 * identify which pool entry corresponds to that stream. Solution: Linear
 * search through pool to find matching stream.
 *
 * PERFORMANCE CONSIDERATION:
 * Linear search at interrupt level is normally discouraged, but with small
 * pool sizes (typically 4-8 entries) and simple pointer comparison, the
 * overhead is acceptable. Alternative would be complex stream-to-index
 * mapping that adds memory management complexity.
 *
 * EVENT STORAGE PATTERN:
 * Each pool entry has its own ASR_Event_Info structure to store pending
 * events. This allows multiple pool entries to have pending events
 * simultaneously without interference.
 *
 * ERROR HANDLING:
 * If stream not found in pool (shouldn't happen), log warning and ignore.
 * If event already pending for pool entry, drop new event to prevent
 * corruption (main thread should process events promptly).
 *
 * References:
 * - MacTCP Programmer's Guide Section 4-3: "Using Asynchronous Routines"
 * - Technical Note TN1083: "MacTCP and Multiple Streams"
 */
/*
 * Send Pool ASR Implementation
 *
 * Handles network events for any stream in the connection pool.
 * Must identify which pool entry owns the stream and store event data safely.
 */
pascal void TCP_Send_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
                                 unsigned short terminReason, struct ICMPReport *icmpMsg)
{
    int i;
    (void)userDataPtr; /* Unused parameter - avoid compiler warning */

    /* Basic validation - pool must be initialized and stream valid */
    if (!gPoolInitialized || tcpStream == NULL) {
        return;  /* Pool not ready or invalid stream */
    }

    /*
     * POOL ENTRY IDENTIFICATION
     *
     * Search through pool to find which entry owns this stream.
     * This is a linear search but acceptable given small pool size.
     * Alternative approaches (hash table, etc.) add complexity without
     * significant benefit for typical pool sizes.
     */
    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        if (gSendStreamPool[i].stream == (StreamPtr)tcpStream) {
            /*
             * Found matching pool entry. Check for pending events to prevent
             * corruption of unprocessed event data.
             */
            if (gSendStreamPool[i].asrEvent.eventPending) {
                /*
                 * Event already pending for this pool entry. Drop the new event
                 * to prevent overwriting unprocessed data. This indicates that
                 * the main thread is not processing events fast enough.
                 *
                 * Note: log_warning_cat() is generally unsafe at interrupt level
                 * because it might call Memory Manager functions internally.
                 * However, many logging implementations are designed to be safe.
                 * If crashes occur, remove this logging call.
                 */
                log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: ASR event dropped (event pending)", i);
                return;
            }

            /*
             * SAFE EVENT STORAGE FOR POOL ENTRY
             *
             * Store event data using only interrupt-safe operations.
             * Same safety principles as listen stream ASR.
             */

            /* Store basic event information (simple assignment - safe) */
            gSendStreamPool[i].asrEvent.eventCode = (TCPEventCode)eventCode;
            gSendStreamPool[i].asrEvent.termReason = terminReason;

            /* Handle ICMP error information if present */
            if (eventCode == TCPICMPReceived && icmpMsg != NULL) {
                /*
                 * Direct struct assignment - safe at interrupt level.
                 * See detailed safety explanation in listen ASR handler.
                 */
                gSendStreamPool[i].asrEvent.icmpReport = *icmpMsg;
            } else {
                /*
                 * Manual structure zeroing without Memory Manager calls.
                 * Safe byte-by-byte clearing for interrupt level execution.
                 */
                char *dst = (char *)&gSendStreamPool[i].asrEvent.icmpReport;
                int j;
                for (j = 0; j < sizeof(ICMPReport); j++) {
                    dst[j] = 0;
                }
            }

            /* Set pending flag and return (atomic operation - safe) */
            gSendStreamPool[i].asrEvent.eventPending = true;
            return;
        }
    }

    /*
     * STREAM NOT FOUND ERROR HANDLING
     *
     * This should never happen in normal operation - indicates either:
     * 1. Pool corruption (memory overwrite)
     * 2. Stream lifecycle bug (ASR called after stream released)
     * 3. ASR called with wrong stream pointer
     *
     * Log warning for debugging but continue safely.
     */
    log_warning_cat(LOG_CAT_MESSAGING, "TCP_Send_ASR_Handler: Unknown stream 0x%lX", (unsigned long)tcpStream);
}

/* Wrapper functions to bridge Pascal calling convention to C calling convention */
static void ListenNotifyWrapper(void *stream, unsigned short eventCode,
                                Ptr userDataPtr, unsigned short terminReason,
                                struct ICMPReport *icmpMsg)
{
    /* Call the actual Pascal UPP with proper casting */
    TCP_Listen_ASR_Handler((StreamPtr)stream, eventCode, userDataPtr, terminReason, icmpMsg);
}

static void SendNotifyWrapper(void *stream, unsigned short eventCode,
                              Ptr userDataPtr, unsigned short terminReason,
                              struct ICMPReport *icmpMsg)
{
    /* Call the actual Pascal UPP with proper casting */
    TCP_Send_ASR_Handler((StreamPtr)stream, eventCode, userDataPtr, terminReason, icmpMsg);
}

/*
 * INITIALIZE TCP MESSAGING SUBSYSTEM
 *
 * This function sets up the complete TCP networking infrastructure including:
 * 1. Listen stream for incoming connections
 * 2. Connection pool for outgoing messages
 * 3. Message queue for flow control
 * 4. ASR event handling
 * 5. Memory management for network buffers
 *
 * INITIALIZATION SEQUENCE:
 * The order of operations is critical for proper functionality:
 * 1. Validate parameters and check initialization state
 * 2. Allocate non-relocatable memory for network buffers
 * 3. Create listen stream with ASR handler
 * 4. Initialize connection pool with individual streams
 * 5. Set up message queue and state variables
 * 6. Start passive listening for incoming connections
 *
 * MEMORY MANAGEMENT STRATEGY:
 * MacTCP requires all network buffers to be non-relocatable and remain
 * at fixed memory addresses for the lifetime of the streams. We use
 * NewPtrSysClear() for system heap allocation or NewPtrClear() for
 * application heap (Mac SE build) depending on compilation flags.
 *
 * ERROR HANDLING:
 * If any step fails, we must carefully clean up all resources allocated
 * up to that point to prevent memory leaks. This includes disposing
 * buffers and releasing TCP streams.
 *
 * PARAMETERS:
 * - macTCPRefNum: MacTCP driver reference number from OpenDriver
 * - streamReceiveBufferSize: Size of receive buffer for each stream
 * - listenAsrUPP: Universal Procedure Pointer for listen stream ASR
 * - sendAsrUPP: Universal Procedure Pointer for send stream ASRs
 *
 * RETURNS: noErr on success, various MacTCP error codes on failure
 */
OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize, TCPNotifyUPP listenAsrUPP,
              TCPNotifyUPP sendAsrUPP)
{
    OSErr err;
    int i, j;  /* Loop counters for pool initialization */

    log_info_cat(LOG_CAT_MESSAGING, "Initializing TCP Messaging Subsystem with connection pool...");

    if (gTCPListenState != TCP_STATE_UNINITIALIZED || gPoolInitialized) {
        log_debug_cat(LOG_CAT_MESSAGING, "InitTCP: Already initialized");
        return streamAlreadyOpen;
    }

    if (macTCPRefNum == 0) return paramErr;
    if (listenAsrUPP == NULL || sendAsrUPP == NULL) {
        log_error_cat(LOG_CAT_MESSAGING, "InitTCP: ASR UPPs are NULL. Cannot proceed.");
        return paramErr;
    }

    gTCPStreamRcvBufferSize = streamReceiveBufferSize;

    /*
     * CRITICAL MEMORY ALLOCATION FOR MACTCP BUFFERS
     *
     * MacTCP has strict requirements for buffer memory:
     *
     * 1. NON-RELOCATABLE: Buffers must not move in memory during stream lifetime
     * 2. FIXED ADDRESS: MacTCP stores pointers to these buffers internally
     * 3. PROPER HEAP: System vs application heap choice affects performance
     *
     * HEAP SELECTION STRATEGY:
     * - System Heap (NewPtrSysClear): More stable, limited size, higher overhead
     * - Application Heap (NewPtrClear): Larger, faster, but can fragment
     *
     * Mac SE Build: Uses application heap due to very limited system heap
     * Standard Build: Uses system heap for stability and isolation
     *
     * BUFFER SIZE CONSIDERATIONS:
     * - Larger buffers reduce receive calls but use more memory
     * - Smaller buffers save memory but increase processing overhead
     * - Typical sizes: 2KB-8KB for message-based protocols
     *
     * References:
     * - MacTCP Programmer's Guide p.2-32: "Buffer Requirements"
     * - MacTCP Programmer's Guide p.2-43: "Memory Management"
     * - Technical Note TN1085: "MacTCP Buffer Management"
     */
#if USE_APPLICATION_HEAP
    gTCPListenRcvBuffer = NewPtrClear(gTCPStreamRcvBufferSize);     /* Application heap for Mac SE */
#else
    gTCPListenRcvBuffer = NewPtrSysClear(gTCPStreamRcvBufferSize);  /* System heap for standard build */
#endif
    if (gTCPListenRcvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate TCP listen stream receive buffer (%lu bytes).",
                      gTCPStreamRcvBufferSize);
        return memFullErr;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Allocated TCP listen stream receive buffer (non-relocatable): %lu bytes", gTCPStreamRcvBufferSize);

    /* Create listen stream */
    err = MacTCPImpl_TCPCreate(macTCPRefNum, &gTCPListenStream, gTCPStreamRcvBufferSize,
                                 gTCPListenRcvBuffer, ListenNotifyWrapper);
    if (err != noErr || gTCPListenStream == kInvalidStreamPtr) {
        log_app_event("Error: Failed to create TCP Listen Stream: %d", err);
        DisposePtr(gTCPListenRcvBuffer);
        gTCPListenRcvBuffer = NULL;
        return err;
    }

    /*
     * CONNECTION POOL INITIALIZATION
     *
     * The connection pool is the heart of our concurrent messaging system.
     * It allows multiple outgoing messages to be sent simultaneously without
     * blocking each other or the main application thread.
     *
     * POOL DESIGN PRINCIPLES:
     * 1. Pre-allocation: All streams created at startup to avoid allocation
     *    overhead during message sending
     * 2. Independent State: Each pool entry maintains its own state machine
     * 3. Resource Isolation: Each entry has dedicated receive buffer
     * 4. Error Isolation: Failure in one connection doesn't affect others
     *
     * CONCURRENCY MODEL:
     * - Multiple connections can be in different states simultaneously
     * - CONNECTING_OUT, SENDING, CLOSING states can overlap across entries
     * - Message queue provides flow control when all entries busy
     *
     * MEMORY LAYOUT:
     * Each pool entry contains:
     * - TCP stream handle (StreamPtr)
     * - Dedicated receive buffer (non-relocatable)
     * - State machine variables
     * - ASR event storage
     * - Message data and target information
     *
     * Based on MacTCP Programmer's Guide Chapter 4: "Advanced Techniques"
     */
    log_info_cat(LOG_CAT_MESSAGING, "Initializing TCP send stream pool (%d streams)...", TCP_SEND_STREAM_POOL_SIZE);

    /* Clear entire pool structure to ensure clean initial state */
    memset(gSendStreamPool, 0, sizeof(gSendStreamPool));

    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        /*
         * DEDICATED BUFFER ALLOCATION FOR EACH POOL ENTRY
         *
         * Each pool entry needs its own receive buffer because:
         * 1. MacTCP streams operate independently
         * 2. Buffers cannot be shared between streams
         * 3. Each stream may receive data at different times
         * 4. Buffer reuse would require complex synchronization
         *
         * Same heap selection strategy as listen stream buffer.
         */
#if USE_APPLICATION_HEAP
        gSendStreamPool[i].rcvBuffer = NewPtrClear(gTCPStreamRcvBufferSize);     /* Application heap for Mac SE */
#else
        gSendStreamPool[i].rcvBuffer = NewPtrSysClear(gTCPStreamRcvBufferSize);  /* System heap for standard build */
#endif
        if (gSendStreamPool[i].rcvBuffer == NULL) {
            log_app_event("Fatal Error: Could not allocate pool[%d] receive buffer (%lu bytes).", i, gTCPStreamRcvBufferSize);

            /* Clean up previously allocated buffers */
            for (j = 0; j < i; j++) {
                if (gSendStreamPool[j].stream) {
                    MacTCPImpl_TCPRelease(macTCPRefNum, gSendStreamPool[j].stream);
                }
                if (gSendStreamPool[j].rcvBuffer) {
                    DisposePtr(gSendStreamPool[j].rcvBuffer);
                }
            }
            MacTCPImpl_TCPRelease(macTCPRefNum, gTCPListenStream);
            DisposePtr(gTCPListenRcvBuffer);
            gTCPListenStream = kInvalidStreamPtr;
            gTCPListenRcvBuffer = NULL;
            return memFullErr;
        }

        /* Create TCP stream for this pool entry */
        err = MacTCPImpl_TCPCreate(macTCPRefNum, &gSendStreamPool[i].stream,
                                     gTCPStreamRcvBufferSize, gSendStreamPool[i].rcvBuffer,
                                     SendNotifyWrapper);
        if (err != noErr || gSendStreamPool[i].stream == kInvalidStreamPtr) {
            log_app_event("Error: Failed to create pool[%d] TCP stream: %d", i, err);

            /* Clean up this entry's buffer */
            DisposePtr(gSendStreamPool[i].rcvBuffer);

            /* Clean up previously created entries */
            for (j = 0; j < i; j++) {
                if (gSendStreamPool[j].stream) {
                    MacTCPImpl_TCPRelease(macTCPRefNum, gSendStreamPool[j].stream);
                }
                if (gSendStreamPool[j].rcvBuffer) {
                    DisposePtr(gSendStreamPool[j].rcvBuffer);
                }
            }
            MacTCPImpl_TCPRelease(macTCPRefNum, gTCPListenStream);
            DisposePtr(gTCPListenRcvBuffer);
            gTCPListenStream = kInvalidStreamPtr;
            gTCPListenRcvBuffer = NULL;
            return err;
        }

        /*
         * POOL ENTRY STATE INITIALIZATION
         *
         * Each pool entry maintains extensive state information for:
         * - Connection state machine (IDLE -> CONNECTING -> SENDING -> CLOSING)
         * - Target information (IP address, port)
         * - Message data and type
         * - Timing information for timeout detection
         * - Async operation handles for MacTCP operations
         * - ASR event storage for interrupt-level notifications
         *
         * All entries start in IDLE state and are ready for immediate use.
         */
        gSendStreamPool[i].state = TCP_STATE_IDLE;           /* Ready for new connection */
        gSendStreamPool[i].targetIP = 0;                     /* No target set */
        gSendStreamPool[i].targetPort = 0;                   /* No target port */
        gSendStreamPool[i].peerIPStr[0] = '\0';              /* Empty IP string */
        gSendStreamPool[i].message[0] = '\0';                /* No message data */
        gSendStreamPool[i].msgType[0] = '\0';                /* No message type */
        gSendStreamPool[i].connectStartTime = 0;             /* No timing info */
        gSendStreamPool[i].sendStartTime = 0;                /* No send timing */
        gSendStreamPool[i].connectHandle = NULL;             /* No async operation */
        gSendStreamPool[i].sendHandle = NULL;                /* No send operation */
        gSendStreamPool[i].closeHandle = NULL;               /* No close operation */
        gSendStreamPool[i].poolIndex = i;                    /* Self-reference for debugging */
        memset((void *)&gSendStreamPool[i].asrEvent, 0, sizeof(ASR_Event_Info)); /* Clear ASR events */

        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Stream created at 0x%lX", i, (unsigned long)gSendStreamPool[i].stream);
    }

    gPoolInitialized = 1;
    log_info_cat(LOG_CAT_MESSAGING, "TCP send stream pool initialized (%d streams)", TCP_SEND_STREAM_POOL_SIZE);

    /* Initialize message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;

    /* Initialize states */
    gTCPListenState = TCP_STATE_IDLE;
    gListenAsyncOperationInProgress = false;
    gListenNoCopyRdsPendingReturn = false;
    memset((Ptr)&gListenAsrEvent, 0, sizeof(ASR_Event_Info));

    /* Start listening */
    StartPassiveListen();

    log_info_cat(LOG_CAT_MESSAGING, "TCP Messaging Subsystem initialized with dual streams.");
    return noErr;
}

/*
 * CLEANUP TCP MESSAGING SUBSYSTEM
 *
 * Performs complete shutdown of TCP networking infrastructure in proper order
 * to prevent resource leaks, crashes, or data corruption. This function must
 * handle both normal shutdown and emergency cleanup scenarios.
 *
 * CLEANUP SEQUENCE:
 * 1. Clear message queue to prevent new operations
 * 2. Abort any active async operations (with delays for completion)
 * 3. Return any pending MacTCP buffers
 * 4. Release all TCP streams
 * 5. Dispose all allocated memory buffers
 * 6. Reset global state variables
 *
 * CRITICAL TIMING CONSIDERATIONS:
 * MacTCP async operations (TCPAbort, TCPRelease) may not complete immediately.
 * We provide brief delays to allow MacTCP to process these operations before
 * proceeding to resource cleanup. This prevents crashes from accessing
 * freed resources.
 *
 * ERROR RESILIENCE:
 * Cleanup continues even if individual operations fail. This ensures that
 * as many resources as possible are freed, even in error conditions.
 *
 * MEMORY SAFETY:
 * All pointers are validated before disposal and set to NULL after cleanup
 * to prevent accidental reuse of freed memory.
 *
 * PARAMETERS:
 * - macTCPRefNum: MacTCP driver reference number for stream operations
 */
void CleanupTCP(short macTCPRefNum)
{
    int i;

    log_debug_cat(LOG_CAT_MESSAGING, "Cleaning up TCP Messaging Subsystem...");

    /* Clear message queue */
    memset(gMessageQueue, 0, sizeof(gMessageQueue));
    gQueueHead = 0;
    gQueueTail = 0;

    /* Clean up listen stream */
    if (gListenAsyncOperationInProgress && gTCPListenStream != kInvalidStreamPtr) {
        log_debug_cat(LOG_CAT_MESSAGING, "Listen async operation was in progress. Aborting.");
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gListenAsyncOperationInProgress = false;
    }

    if (gListenNoCopyRdsPendingReturn && gTCPListenStream != kInvalidStreamPtr) {
        log_debug_cat(LOG_CAT_MESSAGING, "Listen RDS Buffers were pending return. Attempting return.");
        MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, YieldTimeToSystem);
        gListenNoCopyRdsPendingReturn = false;
    }

    if (gTCPListenStream != kInvalidStreamPtr) {
        log_debug_cat(LOG_CAT_MESSAGING, "Releasing TCP Listen Stream...");
        MacTCPImpl_TCPRelease(macTCPRefNum, gTCPListenStream);
        gTCPListenStream = kInvalidStreamPtr;
    }

    /*
     * CONNECTION POOL CLEANUP
     *
     * Each pool entry requires individual cleanup with proper sequencing:
     * 1. Abort active connections (allows in-progress operations to complete)
     * 2. Wait for abort to take effect (MacTCP abort is asynchronous)
     * 3. Release TCP stream (frees MacTCP internal resources)
     * 4. Dispose memory buffers (frees application memory)
     *
     * TIMING CRITICAL SECTION:
     * MacTCP operations are asynchronous and may not complete immediately.
     * The delay after TCPAbort allows MacTCP to process the abort and
     * clean up its internal state before we release the stream.
     */
    if (gPoolInitialized) {
        log_debug_cat(LOG_CAT_MESSAGING, "Cleaning up TCP send stream pool (%d streams)...", TCP_SEND_STREAM_POOL_SIZE);

        for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
            if (gSendStreamPool[i].stream != kInvalidStreamPtr) {
                /*
                 * ACTIVE CONNECTION ABORT
                 *
                 * If pool entry is not idle, it may have active async operations.
                 * TCPAbort cancels all pending operations and forces immediate
                 * connection termination.
                 */
                if (gSendStreamPool[i].state != TCP_STATE_IDLE &&
                        gSendStreamPool[i].state != TCP_STATE_UNINITIALIZED) {
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Aborting active connection", i);
                    MacTCPImpl_TCPAbort(gSendStreamPool[i].stream);

                    /*
                     * CRITICAL TIMING DELAY
                     *
                     * MacTCP abort is asynchronous - it starts the abort process
                     * but doesn't complete immediately. We wait briefly to allow
                     * MacTCP to:
                     * 1. Send RST packets to remote peers
                     * 2. Clean up internal connection state
                     * 3. Cancel pending async operations
                     * 4. Free internal buffers
                     *
                     * Without this delay, subsequent TCPRelease might crash or
                     * corrupt MacTCP's internal data structures.
                     *
                     * Delay: 100ms (6 ticks at 60Hz) - empirically determined
                     * to be sufficient for most abort operations.
                     */
                    unsigned long startTime = TickCount();
                    while ((TickCount() - startTime) < 6) {  /* 100ms delay */
                        YieldTimeToSystem();  /* Yield to system and other processes */
                    }
                }

                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Releasing TCP stream", i);
                MacTCPImpl_TCPRelease(macTCPRefNum, gSendStreamPool[i].stream);
                gSendStreamPool[i].stream = kInvalidStreamPtr;
            }

            if (gSendStreamPool[i].rcvBuffer != NULL) {
                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Disposing receive buffer", i);
                DisposePtr(gSendStreamPool[i].rcvBuffer);
                gSendStreamPool[i].rcvBuffer = NULL;
            }
        }

        memset(gSendStreamPool, 0, sizeof(gSendStreamPool));
        gPoolInitialized = 0;
        log_debug_cat(LOG_CAT_MESSAGING, "TCP send stream pool cleaned up");
    }

    /* Dispose listen buffer */
    if (gTCPListenRcvBuffer != NULL) {
        log_debug_cat(LOG_CAT_MESSAGING, "Disposing TCP listen stream receive buffer.");
        DisposePtr(gTCPListenRcvBuffer);
        gTCPListenRcvBuffer = NULL;
    }

    gTCPStreamRcvBufferSize = 0;
    memset((Ptr)&gListenAsrEvent, 0, sizeof(ASR_Event_Info));
    gTCPListenState = TCP_STATE_UNINITIALIZED;

    log_debug_cat(LOG_CAT_MESSAGING, "TCP Messaging Subsystem cleanup finished.");
}

void StartPassiveListen(void)
{
    OSErr err;

    if (gTCPListenState != TCP_STATE_IDLE) {
        log_error_cat(LOG_CAT_MESSAGING, "StartPassiveListen: Cannot listen, current state is %d (not IDLE).", gTCPListenState);
        return;
    }

    if (gTCPListenStream == kInvalidStreamPtr) {
        log_error_cat(LOG_CAT_MESSAGING, "CRITICAL (StartPassiveListen): Listen stream is NULL. Cannot listen.");
        gTCPListenState = TCP_STATE_ERROR;
        return;
    }

    if (gListenAsyncOperationInProgress) {
        log_debug_cat(LOG_CAT_MESSAGING, "StartPassiveListen: Another async operation is already in progress.");
        return;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Attempting asynchronous TCPListenAsync on port %u...", PORT_TCP);

    err = MacTCPImpl_TCPListenAsync(gTCPListenStream, PORT_TCP, &gListenAsyncHandle);

    if (err == noErr) {
        log_debug_cat(LOG_CAT_MESSAGING, "TCPListenAsync successfully initiated.");
        gTCPListenState = TCP_STATE_LISTENING;
        gListenAsyncOperationInProgress = true;
    } else {
        log_app_event("Error: TCPListenAsync failed: %d.", err);
        gTCPListenState = TCP_STATE_IDLE;
    }
}

void ProcessTCPStateMachine(GiveTimePtr giveTime)
{
    int i;

    /* Handle ASR events for listen stream */
    HandleListenASREvents(giveTime);

    /* Process send stream pool - handle ASR events and state machines for each entry */
    if (gPoolInitialized) {
        for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
            /* Handle ASR events for this pool entry */
            HandlePoolEntryASREvents(i, giveTime);

            /* Process state machine for this pool entry */
            ProcessPoolEntryStateMachine(i, giveTime);

            /* Check for stale connections and timeout */
            CheckPoolEntryTimeout(i);
        }
    }

    /* Process message queue - allocate pool entries for queued messages */
    ProcessMessageQueue(giveTime);

    /* Process listen stream state using the dispatcher */
    dispatch_listen_state_handler(gTCPListenState, giveTime);

    if (giveTime) giveTime();
}

static void HandleListenASREvents(GiveTimePtr giveTime)
{
    if (!gListenAsrEvent.eventPending) return;

    ASR_Event_Info currentEvent = gListenAsrEvent;
    gListenAsrEvent.eventPending = false;

    log_debug_cat(LOG_CAT_MESSAGING, "Listen ASR Event: Code %u, Reason %u (State: %d)",
                  currentEvent.eventCode, currentEvent.termReason, gTCPListenState);

    switch (currentEvent.eventCode) {
    case TCPDataArrival:
        if (gTCPListenState == TCP_STATE_CONNECTED_IN) {
            if (gListenNoCopyRdsPendingReturn) {
                log_app_event("Listen ASR: TCPDataArrival while RDS buffers still pending return!");
                MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                gListenNoCopyRdsPendingReturn = false;
            }

            NetworkTCPInfo tcpInfo;
            if (MacTCPImpl_TCPStatus(gTCPListenStream, &tcpInfo) != noErr) {
                log_error_cat(LOG_CAT_MESSAGING, "Listen ASR: TCPDataArrival, but GetStatus failed.");
                MacTCPImpl_TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
                break;
            }

            Boolean urgentFlag, markFlag;
            OSErr rcvErr = MacTCPImpl_TCPReceiveNoCopy(gTCPListenStream, (Ptr)gListenNoCopyRDS,
                           MAX_RDS_ENTRIES, TCP_RECEIVE_CMD_TIMEOUT_S,
                           &urgentFlag, &markFlag, giveTime);

            if (rcvErr == noErr) {
                log_debug_cat(LOG_CAT_MESSAGING, "Listen TCPNoCopyRcv successful. Processing data.");
                if (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL) {
                    ProcessIncomingTCPData(gListenNoCopyRDS, tcpInfo.remoteHost, tcpInfo.remotePort);
                    gListenNoCopyRdsPendingReturn = true;

                    OSErr bfrReturnErr = MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                    if (bfrReturnErr == noErr) {
                        gListenNoCopyRdsPendingReturn = false;
                    } else {
                        log_app_event("CRITICAL: Listen TCPBfrReturn FAILED: %d", bfrReturnErr);
                        gTCPListenState = TCP_STATE_ERROR;
                        MacTCPImpl_TCPAbort(gTCPListenStream);
                    }
                }
            } else if (rcvErr == connectionClosing) {
                /* CRITICAL FIX: Check if buffers were allocated before error
                 * Per MacTCP Programmer's Guide p.3177: "You are responsible for calling
                 * TCPBfrReturn after every TCPNoCopyRcv command that is completed successfully" */
                if (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL) {
                    log_warning_cat(LOG_CAT_MESSAGING, "Returning buffers after connectionClosing error");
                    MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                    gListenNoCopyRdsPendingReturn = false;
                }
                log_app_event("Listen connection closing by peer.");
                MacTCPImpl_TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
            } else if (rcvErr != commandTimeout) {
                /* CRITICAL FIX: Check if buffers were allocated before error */
                if (gListenNoCopyRDS[0].length > 0 || gListenNoCopyRDS[0].ptr != NULL) {
                    log_warning_cat(LOG_CAT_MESSAGING, "Returning buffers after error %d", rcvErr);
                    MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
                    gListenNoCopyRdsPendingReturn = false;
                }
                log_app_event("Error during Listen TCPNoCopyRcv: %d", rcvErr);
                MacTCPImpl_TCPAbort(gTCPListenStream);
                gTCPListenState = TCP_STATE_IDLE;
            }
        }
        break;

    case TCPTerminate:
        log_app_event("Listen ASR: TCPTerminate. Reason: %u.", currentEvent.termReason);
        if (gListenNoCopyRdsPendingReturn) {
            MacTCPImpl_TCPReturnBuffer(gTCPListenStream, (Ptr)gListenNoCopyRDS, giveTime);
            gListenNoCopyRdsPendingReturn = false;
        }
        gListenAsyncOperationInProgress = false;
        gTCPListenState = TCP_STATE_IDLE;

        /* CRITICAL FIX: Restart listening immediately after terminate
         * Reason 6 (ULP close) is delivered when remote peer closes gracefully.
         * This can arrive AFTER we've already called TCPAbort and StartPassiveListen,
         * terminating the new listen operation. We must restart listening.
         *
         * Per MacTCP Programmer's Guide p.4306: "ULP close = connection closed gracefully"
         * This is expected behavior for our stateless protocol (one message per connection).
         *
         * Without this restart, the listen stream accepts only the first connection and
         * then stops accepting new connections, causing 96% message loss (1/24 vs 24/24). */
        StartPassiveListen();
        break;

    case TCPClosing:
        log_app_event("Listen ASR: Remote peer closed connection.");
        MacTCPImpl_TCPAbort(gTCPListenStream);
        gTCPListenState = TCP_STATE_IDLE;
        break;

    default:
        break;
    }
}

/* Handle ASR events for a specific pool entry
 * Reference: MacTCP Programmer's Guide Section 4-3 - ASR handler patterns
 */
static void HandlePoolEntryASREvents(int poolIndex, GiveTimePtr giveTime)
{
    TCPSendStreamPoolEntry *entry;
    ASR_Event_Info currentEvent;

    (void)giveTime; /* Unused parameter */

    if (!gPoolInitialized) return;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return;

    entry = &gSendStreamPool[poolIndex];

    if (!entry->asrEvent.eventPending) return;

    /* Copy and clear event */
    currentEvent = entry->asrEvent;
    entry->asrEvent.eventPending = false;

    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: ASR Event: Code %u, Reason %u (State: %d)",
                  poolIndex, currentEvent.eventCode, currentEvent.termReason, entry->state);

    switch (currentEvent.eventCode) {
    case TCPTerminate:
        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: TCPTerminate. Reason: %u.", poolIndex, currentEvent.termReason);

        /* Handle termination based on current state and reason */
        if (entry->state == TCP_STATE_CONNECTING_OUT) {
            /* Connection terminated during async connect (before established)
             * Reason 2 = remote initiated disconnect/refusal (RST packet)
             * This typically means:
             * - Port not listening (connection refused)
             * - Remote host not reachable
             * - Remote host actively rejected connection
             *
             * CRITICAL FIX: Do NOT clear async handles here!
             * Per MacTCP Programmer's Guide p.2-70: "If the ULP action is abort, the connection
             * is broken, all pending commands are returned, and a terminate notification is given."
             * When TCPTerminate fires, MacTCP has already set ioResult to error/completion code.
             * The state machine polling code must call MacTCPImpl_TCPCheckAsyncStatus() to properly
             * free async handles via FreeTCPAsyncHandle(). If we clear handles here, they leak! */
            if (currentEvent.termReason == 2) {
                log_app_event("Pool[%d]: Connection to %s refused (peer not listening)",
                              poolIndex, entry->peerIPStr);
            } else {
                log_app_event("Pool[%d]: Connection to %s terminated during connect (reason %u)",
                              poolIndex, entry->peerIPStr, currentEvent.termReason);
            }
            entry->state = TCP_STATE_IDLE;
            /* Handles intentionally NOT cleared - let polling code free them */
        } else if (entry->state == TCP_STATE_SENDING || entry->state == TCP_STATE_CONNECTED_OUT) {
            /* Remote peer closed connection during or after send
             * Reason 2 = remote initiated disconnect (normal for one-message-per-connection protocol)
             * Per NetworkingOpenTransport.txt: Receiver closes immediately after reading message.
             * This is expected behavior - treat as successful send.
             *
             * CRITICAL FIX: Do NOT clear async handles here!
             * Per MacTCP Programmer's Guide p.2-70: "If the ULP action is abort, the connection
             * is broken, all pending commands are returned, and a terminate notification is given."
             * When TCPTerminate fires, MacTCP has already set ioResult fields to completion codes.
             * The state machine polling code must call MacTCPImpl_TCPCheckAsyncStatus() to properly
             * free async handles via FreeTCPAsyncHandle(). If we clear handles here, they leak! */
            if (currentEvent.termReason == 2) {
                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Remote disconnect during send (expected behavior)", poolIndex);
            } else {
                log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Unexpected termination reason %u during send", poolIndex, currentEvent.termReason);
            }
            entry->state = TCP_STATE_IDLE;
            /* Handles intentionally NOT cleared - let polling code free them */
        } else if (entry->state == TCP_STATE_CLOSING_GRACEFUL || entry->state == TCP_STATE_IDLE) {
            /* Expected termination after graceful close or when already idle
             * CRITICAL FIX: Do NOT clear closeHandle here!
             * The state machine polling code (TCP_STATE_CLOSING_GRACEFUL handler) must poll
             * the closeHandle via MacTCPImpl_TCPCheckAsyncStatus() to properly free the async
             * handle. If we clear it here, FreeTCPAsyncHandle() never gets called, causing
             * async handle leaks that exhaust the handle pool.
             *
             * ASR just marks state as IDLE - polling code will detect completion and free handle */
            entry->state = TCP_STATE_IDLE;
            entry->connectHandle = NULL;
            entry->sendHandle = NULL;
            /* closeHandle intentionally NOT cleared - let polling code free it */
        } else {
            /* Unexpected termination in other states (ERROR, ABORTING, RELEASING, etc.)
             *
             * CRITICAL FIX: Do NOT clear async handles here!
             * Per MacTCP Programmer's Guide p.2-70: "If the ULP action is abort, the connection
             * is broken, all pending commands are returned, and a terminate notification is given."
             * When TCPTerminate fires, MacTCP has already set ioResult fields to completion codes.
             * The state machine polling code must call MacTCPImpl_TCPCheckAsyncStatus() to properly
             * free async handles via FreeTCPAsyncHandle(). If we clear handles here, they leak! */
            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: TCPTerminate in unexpected state %d", poolIndex, entry->state);
            entry->state = TCP_STATE_IDLE;
            /* Handles intentionally NOT cleared - let polling code free them */
        }
        break;

    default:
        break;
    }
}

/* Process the state machine for a specific pool entry
 * Reference: MacTCP Programmer's Guide Section 4-18 - async operation completion
 */
static void ProcessPoolEntryStateMachine(int poolIndex, GiveTimePtr giveTime)
{
    TCPSendStreamPoolEntry *entry;
    OSErr err;
    OSErr operationResult;
    void *resultData;
    int msgLen;

    (void)giveTime; /* Unused parameter - state machine doesn't require CPU yielding */

    if (!gPoolInitialized) return;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return;

    entry = &gSendStreamPool[poolIndex];

    switch (entry->state) {
    case TCP_STATE_CONNECTING_OUT:
        if (entry->connectHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->connectHandle, &operationResult, &resultData);

            if (err != 1) { /* Not pending anymore */
                entry->connectHandle = NULL;

                if (err == noErr && operationResult == noErr) {
                    log_info_cat(LOG_CAT_MESSAGING, "Pool[%d]: Connected to %s", poolIndex, entry->peerIPStr);
                    entry->state = TCP_STATE_CONNECTED_OUT;

                    /* Start async send */
                    msgLen = strlen(entry->message);
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Sending %d bytes...", poolIndex, msgLen);
                    entry->state = TCP_STATE_SENDING;
                    entry->sendStartTime = TickCount();

                    err = MacTCPImpl_TCPSendAsync(entry->stream, (Ptr)entry->message,
                                                    msgLen, true, &entry->sendHandle);
                    if (err != noErr) {
                        log_app_event("Pool[%d]: Async send to %s failed to start: %d",
                                      poolIndex, entry->peerIPStr, err);
                        MacTCPImpl_TCPAbort(entry->stream);
                        entry->state = TCP_STATE_IDLE;
                        entry->connectHandle = NULL;
                        entry->sendHandle = NULL;
                        entry->closeHandle = NULL;
                    }
                } else {
                    log_app_event("Pool[%d]: Connection to %s failed: %d",
                                  poolIndex, entry->peerIPStr, operationResult);
                    entry->state = TCP_STATE_IDLE;
                    entry->connectHandle = NULL;
                    entry->closeHandle = NULL;
                }
            }
        }
        break;

    case TCP_STATE_SENDING:
        if (entry->sendHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->sendHandle, &operationResult, &resultData);

            if (err != 1) { /* Not pending anymore */
                entry->sendHandle = NULL;

                if (err == noErr && operationResult == noErr) {
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Message sent successfully", poolIndex);

                    /* Close connection */
                    if (strcmp(entry->msgType, MSG_QUIT) == 0) {
                        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Sending QUIT - using abort for immediate close", poolIndex);
                        MacTCPImpl_TCPAbort(entry->stream);
                        entry->state = TCP_STATE_IDLE;
                        entry->connectHandle = NULL;
                        entry->sendHandle = NULL;
                        entry->closeHandle = NULL;
                    } else {
                        /* Check connection state before attempting graceful close
                         * Per MacTCP Programmer's Guide p.5070: connectionState values:
                         * 0=Closed, 8=Established, 10-20=various closing states
                         * Error -23008 (connectionDoesntExist) occurs when state=0 (already closed)
                         * Only attempt TCPClose if connection still active (state >= 8) */
                        NetworkTCPInfo tcpInfo;
                        OSErr statusErr = MacTCPImpl_TCPStatus(entry->stream, &tcpInfo);

                        if (statusErr != noErr || tcpInfo.connectionState == 0) {
                            /* Connection already closed or status failed - just abort to clean up */
                            log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Connection already closed (state %u) or status failed (%d), using abort",
                                          poolIndex, tcpInfo.connectionState, statusErr);
                            MacTCPImpl_TCPAbort(entry->stream);
                            entry->state = TCP_STATE_IDLE;
                            entry->connectHandle = NULL;
                            entry->sendHandle = NULL;
                            entry->closeHandle = NULL;
                        } else if (tcpInfo.connectionState >= 8) {
                            /* Connection still active - attempt async graceful close
                             * Code Review Section 2.3.1: Use async close to prevent pool blocking
                             * Allows pool entry to return to IDLE without waiting up to 30 seconds */
                            log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Connection active (state %u), starting async graceful close...",
                                          poolIndex, tcpInfo.connectionState);
                            entry->state = TCP_STATE_CLOSING_GRACEFUL;
                            err = MacTCPImpl_TCPCloseAsync(entry->stream, &entry->closeHandle);
                            if (err != noErr) {
                                log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Async close failed (%d), using abort", poolIndex, err);
                                MacTCPImpl_TCPAbort(entry->stream);
                                entry->state = TCP_STATE_IDLE;
                                entry->connectHandle = NULL;
                                entry->sendHandle = NULL;
                                entry->closeHandle = NULL;
                            }
                            /* Don't set IDLE here - TCP_STATE_CLOSING_GRACEFUL handler will poll for completion */
                        } else {
                            /* Connection in transitional state (2=Listen, 4-6=connecting) - just abort */
                            log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Connection in transitional state (%u), using abort",
                                          poolIndex, tcpInfo.connectionState);
                            MacTCPImpl_TCPAbort(entry->stream);
                            entry->state = TCP_STATE_IDLE;
                            entry->connectHandle = NULL;
                            entry->sendHandle = NULL;
                            entry->closeHandle = NULL;
                        }
                    }
                } else {
                    log_app_event("Pool[%d]: Send to %s failed: %d",
                                  poolIndex, entry->peerIPStr, operationResult);
                    MacTCPImpl_TCPAbort(entry->stream);
                    entry->state = TCP_STATE_IDLE;
                    entry->connectHandle = NULL;
                    entry->sendHandle = NULL;
                    entry->closeHandle = NULL;
                }
            }
        }
        break;

    case TCP_STATE_CLOSING_GRACEFUL:
        /* Async graceful close in progress - poll for completion
         * Code Review Section 2.3.1: Non-blocking close allows immediate pool reuse
         * This prevents pool entries from blocking for up to 30 seconds during close
         *
         * CRITICAL: Must call MacTCPImpl_TCPCheckAsyncStatus() to poll ioResult
         * Per MacTCP Programmer's Guide p.2-15: "Poll ioResult field... when value
         * changes from inProgress (1) to some other value, call has completed"
         * MacTCPImpl_TCPCheckAsyncStatus() automatically frees async handle (mactcp_impl.c:1520) */
        if (entry->closeHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->closeHandle, &operationResult, &resultData);

            if (err != 1) { /* Not pending anymore (ioResult != inProgress) - close completed or failed
                             * IMPORTANT: MacTCPImpl_TCPCheckAsyncStatus() has already freed the async handle
                             * at this point (via FreeTCPAsyncHandle() at mactcp_impl.c:1520) */
                entry->closeHandle = NULL;  /* Clear our pointer since handle is now freed */

                if (err == noErr && operationResult == noErr) {
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Async close completed successfully", poolIndex);
                } else {
                    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Async close completed with status %d (result %d)",
                                  poolIndex, err, operationResult);
                }

                /* Close complete - return to IDLE for reuse */
                entry->state = TCP_STATE_IDLE;
                entry->connectHandle = NULL;
                entry->sendHandle = NULL;
                /* closeHandle already set to NULL above */
            }
            /* else: Still pending (ioResult == 1), keep polling on next cycle */
        } else {
            /* No close handle - shouldn't happen but handle gracefully
             * ASR TCPTerminate event may have already cleared state */
            log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: CLOSING_GRACEFUL with no handle, returning to IDLE", poolIndex);
            entry->state = TCP_STATE_IDLE;
            entry->connectHandle = NULL;
            entry->sendHandle = NULL;
        }
        break;

    case TCP_STATE_IDLE:
        /* Normal idle state - available for new send request
         * CRITICAL: Check for ANY pending async handles that need cleanup
         * This handles the race condition where TCPTerminate ASR fires before we poll the handles
         * Per async handle leak fix: ASR never clears handles, polling code must free them */

        /* Check for pending connectHandle */
        if (entry->connectHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->connectHandle, &operationResult, &resultData);
            if (err != 1) {
                /* Connect operation completed - MacTCPImpl_TCPCheckAsyncStatus already freed handle */
                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: IDLE state cleaned up pending connectHandle (status %d)",
                              poolIndex, err);
                entry->connectHandle = NULL;
            }
        }

        /* Check for pending sendHandle */
        if (entry->sendHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->sendHandle, &operationResult, &resultData);
            if (err != 1) {
                /* Send operation completed - MacTCPImpl_TCPCheckAsyncStatus already freed handle */
                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: IDLE state cleaned up pending sendHandle (status %d)",
                              poolIndex, err);
                entry->sendHandle = NULL;
            }
        }

        /* Check for pending closeHandle */
        if (entry->closeHandle != NULL) {
            err = MacTCPImpl_TCPCheckAsyncStatus(entry->closeHandle, &operationResult, &resultData);
            if (err != 1) {
                /* Close operation completed - MacTCPImpl_TCPCheckAsyncStatus already freed handle */
                log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: IDLE state cleaned up pending closeHandle (status %d)",
                              poolIndex, err);
                entry->closeHandle = NULL;
            }
        }
        break;

    default:
        /* Unexpected states */
        if (entry->state != TCP_STATE_UNINITIALIZED) {
            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Unexpected state: %d", poolIndex, entry->state);
        }
        break;
    }
}

/* Check for stale connections and timeout them
 * Prevents pool entries from getting stuck in connecting/sending states
 */
static void CheckPoolEntryTimeout(int poolIndex)
{
    TCPSendStreamPoolEntry *entry;
    unsigned long currentTime;
    unsigned long elapsedTicks;

    if (!gPoolInitialized) return;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return;

    entry = &gSendStreamPool[poolIndex];
    currentTime = TickCount();

    /* Check for stale connections */
    if (entry->state == TCP_STATE_CONNECTING_OUT && entry->connectStartTime > 0) {
        elapsedTicks = currentTime - entry->connectStartTime;
        if (elapsedTicks > TCP_STREAM_CONNECTION_TIMEOUT_TICKS) {
            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Connection timeout to %s (%lu ticks)",
                            poolIndex, entry->peerIPStr, elapsedTicks);
            MacTCPImpl_TCPAbort(entry->stream);
            entry->state = TCP_STATE_IDLE;
            entry->connectHandle = NULL;
            entry->sendHandle = NULL;
            entry->closeHandle = NULL;
        }
    } else if (entry->state == TCP_STATE_SENDING && entry->sendStartTime > 0) {
        elapsedTicks = currentTime - entry->sendStartTime;
        if (elapsedTicks > TCP_STREAM_CONNECTION_TIMEOUT_TICKS) {
            log_warning_cat(LOG_CAT_MESSAGING, "Pool[%d]: Send timeout to %s (%lu ticks)",
                            poolIndex, entry->peerIPStr, elapsedTicks);
            MacTCPImpl_TCPAbort(entry->stream);
            entry->state = TCP_STATE_IDLE;
            entry->connectHandle = NULL;
            entry->sendHandle = NULL;
            entry->closeHandle = NULL;
        }
    }
}

void ProcessIncomingTCPData(wdsEntry rds[], ip_addr remote_ip_from_status, tcp_port remote_port_from_status)
{
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    char remoteIPStrConnected[INET_ADDRSTRLEN];

    if (remote_ip_from_status != 0) {
        MacTCPImpl_AddressToString(remote_ip_from_status, remoteIPStrConnected);
    } else {
        strcpy(remoteIPStrConnected, "unknown_ip");
    }

    log_debug_cat(LOG_CAT_MESSAGING, "ProcessIncomingTCPData from %s:%u", remoteIPStrConnected, remote_port_from_status);

    /* Process RDS entries with explicit bounds check to prevent runaway loop */
    for (int i = 0; i < MAX_RDS_ENTRIES && (rds[i].length > 0 || rds[i].ptr != NULL); ++i) {
        if (rds[i].length == 0 || rds[i].ptr == NULL) break;

        log_debug_cat(LOG_CAT_MESSAGING, "Processing RDS entry %d: Ptr 0x%lX, Len %u", i, (unsigned long)rds[i].ptr,
                      rds[i].length);

        csend_uint32_t msg_id;
        if (parse_message((const char *)rds[i].ptr, rds[i].length,
                          senderIPStrFromPayload, senderUsername, msgType, &msg_id, content) == 0) {
            log_debug_cat(LOG_CAT_MESSAGING,
                          "Parsed TCP message: ID %lu, Type '%s', FromUser '%s', FromIP(payload) '%s', Content(len %d) '%.30s...'",
                          (unsigned long)msg_id, msgType, senderUsername, senderIPStrFromPayload, (int)strlen(content), content);

            handle_received_tcp_message(remoteIPStrConnected,
                                        senderUsername,
                                        msgType,
                                        content,
                                        &g_mac_tcp_callbacks,
                                        NULL);

            if (strcmp(msgType, MSG_QUIT) == 0) {
                log_app_event("QUIT message processed from %s.", remoteIPStrConnected);
            }
        } else {
            log_error_cat(LOG_CAT_MESSAGING, "Failed to parse TCP message chunk from %s (length %u).",
                          remoteIPStrConnected, rds[i].length);
        }
    }
}

TCPStreamState GetTCPListenStreamState(void)
{
    return gTCPListenState;
}

TCPStreamState GetTCPSendStreamState(void)
{
    int i;
    int idleCount = 0;

    if (!gPoolInitialized) {
        return TCP_STATE_UNINITIALIZED;
    }

    /* Count IDLE entries - if all IDLE, return IDLE, otherwise return BUSY state */
    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        if (gSendStreamPool[i].state == TCP_STATE_IDLE) {
            idleCount++;
        }
    }

    /* If all idle, report IDLE, otherwise report that we have active connections */
    if (idleCount == TCP_SEND_STREAM_POOL_SIZE) {
        return TCP_STATE_IDLE;
    } else {
        /* Return a generic "busy" state */
        return TCP_STATE_CONNECTING_OUT;
    }
}

/* Start an async send operation on a specific pool entry
 * Reference: MacTCP Programmer's Guide Section 4-18 "TCPActiveOpen" - async connection
 */
static OSErr StartAsyncSendOnPoolEntry(int poolIndex, const char *peerIPStr,
                                       const char *message_content, const char *msg_type)
{
    TCPSendStreamPoolEntry *entry;
    OSErr err = noErr;
    ip_addr targetIP = 0;
    char messageBuffer[BUFFER_SIZE];
    int formattedLen;

    if (!gPoolInitialized) return notOpenErr;
    if (poolIndex < 0 || poolIndex >= TCP_SEND_STREAM_POOL_SIZE) return paramErr;

    entry = &gSendStreamPool[poolIndex];

    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: StartAsyncSend: Request to send '%s' to %s",
                  poolIndex, msg_type, peerIPStr);

    /* Validate parameters */
    if (gMacTCPRefNum == 0) return notOpenErr;
    if (entry->stream == NULL) return invalidStreamPtr;
    if (peerIPStr == NULL || msg_type == NULL) return paramErr;

    /* Must be in IDLE state */
    if (entry->state != TCP_STATE_IDLE) {
        log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Stream not idle (state %d)", poolIndex, entry->state);
        return connectionExists;
    }

    /* Parse target IP */
    if (strcmp(peerIPStr, gMyLocalIPStr) == 0) {
        targetIP = gMyLocalIP;
    } else {
        err = ParseIPv4(peerIPStr, &targetIP);
        if (err != noErr) {
            log_app_event("Pool[%d]: Invalid IP address %s", poolIndex, peerIPStr);
            return err;
        }
    }

    /* Format message */
    formattedLen = format_message(messageBuffer, sizeof(messageBuffer), msg_type,
                                  generate_message_id(), gMyUsername, gMyLocalIPStr, message_content);
    if (formattedLen < 0) {
        log_app_event("Pool[%d]: format_message failed for type '%s'", poolIndex, msg_type);
        return paramErr;
    }

    /* Store operation info in pool entry */
    strncpy(entry->peerIPStr, peerIPStr, INET_ADDRSTRLEN - 1);
    entry->peerIPStr[INET_ADDRSTRLEN - 1] = '\0';
    strncpy(entry->message, messageBuffer, BUFFER_SIZE - 1);
    entry->message[BUFFER_SIZE - 1] = '\0';
    strncpy(entry->msgType, msg_type, 31);
    entry->msgType[31] = '\0';
    entry->targetIP = targetIP;
    entry->targetPort = PORT_TCP;
    entry->connectStartTime = TickCount();

    /* Start async connect */
    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Starting async connection to %s:%u...",
                  poolIndex, peerIPStr, PORT_TCP);
    entry->state = TCP_STATE_CONNECTING_OUT;

    err = MacTCPImpl_TCPConnectAsync(entry->stream, targetIP, PORT_TCP, &entry->connectHandle);
    if (err != noErr) {
        log_app_event("Pool[%d]: Async connection to %s failed to start: %d", poolIndex, peerIPStr, err);
        entry->state = TCP_STATE_IDLE;
        entry->connectHandle = NULL;
        return err;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Pool[%d]: Async connect initiated", poolIndex);
    return noErr;
}

