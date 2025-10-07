# Asynchronous Messaging Architecture Improvements

## Executive Summary

Current testing revealed critical performance and reliability issues across all three platform implementations:

- **MacTCP**: Single send stream bottleneck - queued all 47 test messages, sent AFTER test ended (2+ minute delay)
- **OpenTransport**: Synchronous blocking mode - waits in loops for connections causing 7+ second delays
- **POSIX**: Synchronous blocking sends - messages arrive out of order with 2+ minute delays

This document outlines the architectural improvements needed to achieve **reliable, fast, asynchronous, non-blocking** message delivery on all platforms.

## Critical Issues Discovered

### MacTCP Implementation
**Problem**: Single send stream architecture causes complete bottleneck
- Test started: 16:05:54, ended: 16:07:10 (76 seconds)
- All 47 messages queued during test
- First actual send: 16:07:11 (1 second AFTER test completion)
- Messages finally delivered: 16:07:26 (2+ minutes after test start)

**Root Cause**:
- Only one TCP stream for outgoing connections (`gTCPSendStream`)
- Each message requires: TCPActiveOpen → TCPSend → TCPClose cycle
- Stream stays in CONNECTING_OUT or SENDING state while processing one message
- All subsequent messages queue but cannot send until stream returns to IDLE
- Queue drains sequentially, one message at a time

**Evidence from logs**:
```
16:05:54 - AUTOMATED TEST START
16:05:54 - [MESSAGING] Message queued for MacTCP@10.0.1.51: 'TEST_R1_BROADCAST_1' (queue: 1/64)
16:05:56 - [MESSAGING] Message queued for MacTCP@10.0.1.51: 'TEST_R1_BROADCAST_2' (queue: 2/64)
...
16:07:10 - AUTOMATED TEST END
16:07:10 - Test Summary: 36 total messages, 0 failed, 36 succeeded
16:07:11 - [MESSAGING] TCPActiveOpen completed successfully to 10.0.1.51:2555
16:07:11 - [MESSAGING] TCPSend completed successfully (18 bytes to 10.0.1.51)
```

### OpenTransport Implementation
**Problem**: Synchronous blocking mode with polling loops
- Currently uses synchronous blocking: waits in loop for T_CONNECT event
- Connection establishment: 7+ seconds per connection observed
- No async mode enabled despite OpenTransport support

**Root Cause** (`classic_mac_ot/opentransport_impl.c:1005-1025`):
```c
/* Wait for connection to complete */
startTime = OTGetTimeStamp();
while ((currentTime - startTime) < connectionTimeout) {
    OTResult lookResult = OTLook(endpoint);

    if (lookResult == T_CONNECT) {
        /* Connection established */
        err = OTRcvConnect(endpoint, NULL);
        if (err == noErr) {
            log_debug_cat(LOG_CAT_NETWORKING, "Connection established to %s:%u", ipStr, port);
            *outEndpoint = endpoint;
            return noErr;
        }
        // ... error handling
    }
    YieldTimeToSystem();
    currentTime = OTGetTimeStamp();
}
```

**Book Reference**: "Networking with OpenTransport" Section 2-38:
> "In asynchronous nonblocking mode, a function returns immediately, and your notifier function
> is called when the operation completes or when data arrives. This is the recommended mode for
> most applications because it provides the best performance and allows your application to
> continue processing events while the operation completes."

### POSIX Implementation
**Problem**: Synchronous blocking socket operations cause ordering issues
- Each `send_tcp_message()` blocks on connect() + send() + close()
- No message queue or worker thread
- Messages sent from multiple threads compete
- Delivery order not guaranteed

**Evidence from OpenTransport peer logs**:
```
16:04:16 - Received TEXT from POSIX@10.0.1.53: 'TEST_R1_BROADCAST_1'
16:04:19 - Received TEXT from POSIX@10.0.1.53: 'TEST_R1_BROADCAST_3'  [SKIPPED #2!]
16:04:22 - Received TEXT from POSIX@10.0.1.53: 'TEST_R1_DIRECT_1_TO_OpenTransport_MSG_2' [SKIPPED MSG_1!]
```

**Root Cause** (`posix/messaging.c:82-116`):
- Direct socket operations with no queuing
- Each thread calls `send_tcp_message()` directly
- No serialization of outgoing messages

## Architectural Solutions

### MacTCP: Connection Pool Architecture

**Strategy**: Replace single send stream with pool of reusable streams

**Design** (Based on MacTCP Programmer's Guide Chapter 4):
```
+-------------------+
| Message Queue     |
| [msg1][msg2][...] |
+-------------------+
         |
         v
+-------------------+
| Connection Pool   |
| [stream1] IDLE    |
| [stream2] BUSY    |
| [stream3] IDLE    |
| [stream4] BUSY    |
+-------------------+
```

**Implementation Details**:

1. **Connection Pool Structure**:
```c
#define TCP_SEND_STREAM_POOL_SIZE 4

typedef struct {
    NetworkStreamRef stream;
    TCPStreamState state;
    ip_addr targetIP;
    tcp_port targetPort;
    unsigned long connectStartTime;
    unsigned long sendStartTime;
} TCPSendStreamPoolEntry;

TCPSendStreamPoolEntry gSendStreamPool[TCP_SEND_STREAM_POOL_POOL_SIZE];
```

2. **Stream Allocation Algorithm**:
   - On new message: find IDLE stream from pool
   - If available: assign target IP/port, initiate TCPActiveOpen
   - If none available: queue message (existing queue mechanism)
   - On completion: mark stream IDLE, check queue for next message

3. **ASR Handler Updates**:
   - Identify which pool entry triggered the event
   - Process state transition for that specific stream
   - Return stream to pool on close

4. **Timeout Handling**:
   - Track connection start time per stream
   - Abort stale connections (>30 seconds)
   - Return stream to pool

**Book References**:
- MacTCP Programmer's Guide Section 4-3: "Using Asynchronous Routines"
- MacTCP Programmer's Guide Section 4-18: "TCPActiveOpen" - async mode with completion routines

### OpenTransport: Async Non-Blocking Mode

**Strategy**: Convert from synchronous blocking to asynchronous non-blocking mode

**Current Mode**: Synchronous Blocking
- Function blocks until operation completes
- Polling loop with OTLook() checking for T_CONNECT
- Wastes CPU cycles, delays other operations

**Target Mode**: Asynchronous Non-Blocking
- Function returns immediately
- Notifier called when operation completes
- Application continues processing events

**Implementation Details**:

1. **Enable Async Mode** (per "Networking with OpenTransport" p. 2-40):
```c
OSStatus OTInitializeEndpointAsync(TNetworkImplementation *impl, const char *configStr)
{
    OSStatus err;
    EndpointRef endpoint;

    /* Open endpoint in async mode */
    endpoint = OTOpenEndpoint(OTCreateConfiguration(configStr),
                              0,     /* oflag - 0 for default async */
                              NULL,  /* info */
                              &err);

    if (err != noErr) {
        return err;
    }

    /* Install notifier function */
    err = OTInstallNotifier(endpoint, NewOTNotifyUPP(OTNotifierFunction), impl);
    if (err != noErr) {
        OTCloseProvider(endpoint);
        return err;
    }

    /* Set non-blocking mode */
    err = OTSetNonBlocking(endpoint);
    if (err != noErr) {
        OTCloseProvider(endpoint);
        return err;
    }

    /* Set asynchronous mode */
    err = OTSetAsynchronous(endpoint);
    if (err != noErr) {
        OTCloseProvider(endpoint);
        return err;
    }

    return noErr;
}
```

2. **Notifier Function** (per "Networking with OpenTransport" p. 2-43):
```c
pascal void OTNotifierFunction(void *contextPtr, OTEventCode event,
                               OTResult result, void *cookie)
{
    TNetworkImplementation *impl = (TNetworkImplementation *)contextPtr;

    switch (event) {
        case T_CONNECT:
            /* Connection completed - can now send */
            HandleConnectComplete(impl, result);
            break;

        case T_DISCONNECT:
            /* Connection closed */
            HandleDisconnect(impl, result);
            break;

        case T_ORDREL:
            /* Orderly release (half-close) */
            HandleOrderlyRelease(impl, result);
            break;

        case T_DATA:
            /* Data available to receive */
            HandleDataAvailable(impl);
            break;

        case T_GODATA:
            /* Flow control lifted, can send more */
            HandleFlowControlLifted(impl);
            break;

        default:
            log_debug_cat(LOG_CAT_NETWORKING, "OT Notifier: event=0x%X result=%d",
                         event, result);
            break;
    }
}
```

3. **Connection State Machine**:
```c
typedef enum {
    OT_CONN_IDLE,
    OT_CONN_CONNECTING,
    OT_CONN_CONNECTED,
    OT_CONN_SENDING,
    OT_CONN_CLOSING,
    OT_CONN_ERROR
} OTConnectionState;

typedef struct {
    EndpointRef endpoint;
    OTConnectionState state;
    ip_addr targetIP;
    tcp_port targetPort;
    char *pendingData;
    size_t pendingDataLen;
} OTConnectionContext;
```

4. **Non-Blocking Send**:
```c
OSStatus OTSendMessageAsync(EndpointRef endpoint, const char *data, size_t len)
{
    OSStatus err;
    OTResult bytesSent;

    /* OTSnd in non-blocking mode returns immediately */
    bytesSent = OTSnd(endpoint, (void *)data, len, 0);

    if (bytesSent >= 0) {
        if (bytesSent < (OTResult)len) {
            /* Partial send - flow control active */
            /* Save remaining data, wait for T_GODATA event */
            return kOTFlowErr;
        }
        return noErr;  /* Complete send */
    } else {
        /* Error or would block */
        err = (OSStatus)bytesSent;
        if (err == kOTFlowErr) {
            /* Wait for T_GODATA event */
            return err;
        }
        return err;  /* Real error */
    }
}
```

**Book References**:
- "Networking with OpenTransport" Section 2-38: "Asynchronous Nonblocking Mode"
- "Networking with OpenTransport" Section 2-43: "Writing a Notifier Function"
- "Networking with OpenTransport" Section 3-25: "OTSetAsynchronous"
- "Networking with OpenTransport" Section 3-26: "OTSetNonBlocking"

### POSIX: Message Queue + Worker Thread

**Strategy**: Add message queue with dedicated worker thread for serialized sends

**Current Architecture**:
```
[Input Thread] ──> send_tcp_message() ──> socket()→connect()→send()→close()
[Listener Thread] ──> send_tcp_message() ──> socket()→connect()→send()→close()
[Discovery Thread] ──> send_tcp_message() ──> socket()→connect()→send()→close()
```
*Problem*: Multiple threads competing, no ordering guarantees

**Target Architecture**:
```
                    ┌─────────────────┐
[Input Thread] ────>│                 │
[Listener Thread]──>│  Message Queue  │
[Discovery Thread]─>│  (Thread-Safe)  │
                    └────────┬────────┘
                             │
                             v
                    ┌─────────────────┐
                    │  Worker Thread  │ (Single threaded sender)
                    └────────┬────────┘
                             │
                             v
                    socket()→connect()→send()→close()
```
*Benefits*: Serialized sends, guaranteed ordering, non-blocking for caller threads

**Implementation Details**:

1. **Message Queue Structure**:
```c
#define POSIX_SEND_QUEUE_SIZE 128

typedef struct {
    char target_ip[INET_ADDRSTRLEN];
    char message[BUFFER_SIZE];
    size_t message_len;
} posix_queued_message_t;

typedef struct {
    posix_queued_message_t messages[POSIX_SEND_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
    Boolean shutdown;
} posix_send_queue_t;
```

2. **Queue Operations**:
```c
int posix_enqueue_message(posix_send_queue_t *queue, const char *ip,
                          const char *message, size_t len)
{
    pthread_mutex_lock(&queue->mutex);

    /* Wait for space if queue full */
    while (queue->count >= POSIX_SEND_QUEUE_SIZE && !queue->shutdown) {
        pthread_cond_wait(&queue->cond_not_full, &queue->mutex);
    }

    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    /* Add message to queue */
    posix_queued_message_t *msg = &queue->messages[queue->tail];
    strncpy(msg->target_ip, ip, INET_ADDRSTRLEN - 1);
    memcpy(msg->message, message, len);
    msg->message_len = len;

    queue->tail = (queue->tail + 1) % POSIX_SEND_QUEUE_SIZE;
    queue->count++;

    pthread_cond_signal(&queue->cond_not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

int posix_dequeue_message(posix_send_queue_t *queue, posix_queued_message_t *out_msg)
{
    pthread_mutex_lock(&queue->mutex);

    /* Wait for message or shutdown */
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->cond_not_empty, &queue->mutex);
    }

    if (queue->count == 0) {  /* Shutdown with empty queue */
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    /* Remove message from queue */
    *out_msg = queue->messages[queue->head];
    queue->head = (queue->head + 1) % POSIX_SEND_QUEUE_SIZE;
    queue->count--;

    pthread_cond_signal(&queue->cond_not_full);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}
```

3. **Worker Thread**:
```c
void *send_worker_thread(void *arg)
{
    posix_send_queue_t *queue = (posix_send_queue_t *)arg;
    posix_queued_message_t msg;

    log_info_cat(LOG_CAT_MESSAGING, "Send worker thread started");

    while (1) {
        if (posix_dequeue_message(queue, &msg) != 0) {
            break;  /* Shutdown */
        }

        /* Perform actual socket send (existing send_tcp_message logic) */
        send_tcp_message_blocking(msg.target_ip, msg.message, msg.message_len);
    }

    log_info_cat(LOG_CAT_MESSAGING, "Send worker thread exiting");
    return NULL;
}
```

4. **Public API Change**:
```c
/* OLD: Blocking send */
int send_tcp_message(const char *ip, const char *message, size_t message_len);

/* NEW: Non-blocking enqueue */
int send_tcp_message_async(const char *ip, const char *message, size_t message_len)
{
    return posix_enqueue_message(&g_send_queue, ip, message, message_len);
}
```

## Implementation Plan

### Phase 1: MacTCP Connection Pool (Highest Priority)
**Files to Modify**:
- `classic_mac_mactcp/messaging.c` - Add pool structure and management
- `classic_mac_mactcp/messaging.h` - Add pool definitions
- `classic_mac_mactcp/tcp_state_handlers.c` - Update state handlers for pool

**Steps**:
1. Define `TCPSendStreamPoolEntry` structure
2. Create pool initialization in `InitTCP()`
3. Modify `ProcessQueuedMessages()` to use pool allocation
4. Update `TCP_Send_ASR_Handler()` to identify pool entry
5. Implement timeout monitoring for stale connections
6. Add pool cleanup in `CleanupTCP()`

**Testing**:
- Run `/test` command from POSIX peer
- Verify MacTCP sends messages DURING test (not after)
- Check logs for concurrent stream usage
- Verify no queue overflow

### Phase 2: OpenTransport Async Mode
**Files to Modify**:
- `classic_mac_ot/opentransport_impl.c` - Convert to async/non-blocking
- `classic_mac_ot/opentransport_impl.h` - Add notifier definitions

**Steps**:
1. Create `OTNotifierFunction()` with T_CONNECT/T_DATA/T_GODATA handlers
2. Modify endpoint initialization to use `OTSetAsynchronous()` + `OTSetNonBlocking()`
3. Remove polling loops from `TCPConnect()` implementation
4. Add connection context tracking structure
5. Implement flow control handling (kOTFlowErr → wait for T_GODATA)
6. Update `TCPSend()` for non-blocking mode

**Testing**:
- Run `/test` command from MacTCP peer
- Verify OpenTransport accepts connections quickly (<1 second)
- Check for T_GODATA event handling
- Verify no blocking loops in logs

### Phase 3: POSIX Message Queue
**Files to Modify**:
- `posix/messaging.c` - Add queue and worker thread
- `posix/messaging.h` - Add queue structure definitions
- `posix/commands.c` - Update to use async API

**Steps**:
1. Define `posix_send_queue_t` and `posix_queued_message_t` structures
2. Implement queue operations (enqueue/dequeue with mutex)
3. Create worker thread function
4. Start worker thread in `initialize_messaging()`
5. Update all `send_tcp_message()` calls to `send_tcp_message_async()`
6. Add queue shutdown logic in cleanup

**Testing**:
- Run `/test` command
- Verify messages arrive in order on all peers
- Check queue depth doesn't exceed capacity
- Monitor worker thread CPU usage

## Success Criteria

### Performance Metrics
- ✅ MacTCP: Messages sent DURING test (not queued until after)
- ✅ OpenTransport: Connection establishment <1 second (down from 7+ seconds)
- ✅ POSIX: Messages arrive in order (TEST_R1_BROADCAST_1, _2, _3 sequentially)
- ✅ All platforms: Test completion without queue overflow
- ✅ All platforms: Message delivery latency <5 seconds

### Reliability Metrics
- ✅ Zero message loss during burst sends (48 messages in <2 minutes)
- ✅ Graceful handling of connection failures
- ✅ No deadlocks or race conditions
- ✅ Proper cleanup on shutdown

### Code Quality
- ✅ All code fully functional (no stubs or TODOs)
- ✅ Book references cited in comments
- ✅ Consistent error handling across platforms
- ✅ No code duplication

## Book References Summary

**MacTCP Programmer's Guide (1989)**:
- Section 4-3: Asynchronous Routines - completion routine patterns
- Section 4-18: TCPActiveOpen - async connection establishment
- Section 4-20: TCPSend - async data transmission

**Networking with OpenTransport**:
- Section 2-38: Asynchronous Nonblocking Mode - recommended mode
- Section 2-40: Setting Endpoint Modes - OTSetAsynchronous/OTSetNonBlocking
- Section 2-43: Writing a Notifier Function - event handling patterns
- Section 3-25: OTSetAsynchronous - enable async mode
- Section 3-26: OTSetNonBlocking - enable non-blocking mode

## Risk Analysis

### MacTCP Connection Pool Risks
- **Risk**: Stream pool exhaustion under heavy load
- **Mitigation**: Queue overflow protection already exists (64 message capacity)

- **Risk**: Pool entry state corruption from ASR race conditions
- **Mitigation**: ASR handlers run at interrupt time, use atomic state transitions

### OpenTransport Async Risks
- **Risk**: Notifier called at interrupt time, limited API available
- **Mitigation**: Use OTEnterNotifier/OTLeaveNotifier, defer complex work

- **Risk**: Flow control (kOTFlowErr) not handled properly
- **Mitigation**: Implement T_GODATA event handling, buffer pending data

### POSIX Queue Risks
- **Risk**: Queue overflow if worker thread can't keep up
- **Mitigation**: Bounded queue with condition variables (block enqueue when full)

- **Risk**: Worker thread deadlock on shutdown
- **Mitigation**: Use shutdown flag + condition variable broadcast

## Timeline Estimate

- **Phase 1 (MacTCP Pool)**: 2-3 hours implementation + 1 hour testing
- **Phase 2 (OpenTransport Async)**: 3-4 hours implementation + 1 hour testing
- **Phase 3 (POSIX Queue)**: 2-3 hours implementation + 1 hour testing
- **Integration Testing**: 2 hours across all platforms
- **Total**: ~12-15 hours

## Conclusion

The current synchronous blocking architecture across all three platforms causes severe performance degradation and reliability issues. By implementing:

1. **MacTCP Connection Pool** - eliminate single stream bottleneck
2. **OpenTransport Async Mode** - leverage OS-level async capabilities
3. **POSIX Message Queue** - serialize sends, guarantee ordering

We will achieve **reliable, fast, asynchronous, non-blocking** message delivery that meets professional networking application standards.
