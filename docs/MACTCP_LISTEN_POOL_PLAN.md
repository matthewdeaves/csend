# MacTCP Listen Stream Pool Implementation Plan

> **⚠️ PLAN INVALIDATED - DO NOT IMPLEMENT**
>
> **Date**: 2025-10-15
> **Status**: REJECTED after MacTCP documentation review
>
> **Why This Won't Work**:
>
> This plan assumes multiple TCP streams can simultaneously listen on the same local port with wildcard remote addresses. However, MacTCP Programmer's Guide explicitly states:
>
> > "**A TCP stream supports one connection at a time.**" (lines 2769-2772)
>
> While wildcard passive opens (remote=0.0.0.0:0) DO work, MacTCP does NOT support multiple concurrent passive opens on the same local port. The second `TCPPassiveOpen` call would likely return `duplicateSocket` error. The `duplicateSocket` error definition is ambiguous about whether it applies to:
> - Active connections only (4-tuple matching), OR
> - Port binding at the stream level (which would block multiple listeners)
>
> Additionally, UDP documentation clearly states (lines 1088, 1258-1259):
> > "a stream is already open using this local UDP port"
>
> This indicates port binding occurs at the stream level, not just for established connections.
>
> **Critical Evidence**:
> - MacTCP limit: 64 total TCP streams system-wide
> - One connection per stream at a time (not one connection per stream per remote host)
> - No documentation examples of multiple concurrent listeners on same port
> - Send pool works because it uses **active opens** (outbound), not passive opens (inbound)
>
> **Alternative Solution**: See `MACTCP_RESET_DELAY_ELIMINATION_PLAN.md` for the correct approach using immediate stream reuse without reset delays.

---

# Original Plan (For Reference Only)

## Problem Analysis

### Current Behavior
- **Single listen stream** (`gTCPListenStream`) handles all incoming TCP connections
- **One connection at a time**: MacTCP streams can only handle one connection at a time
- **100ms reset delay**: After processing a connection, must wait `TCP_STREAM_RESET_DELAY_TICKS` (6 ticks = 100ms) before accepting next connection
- **Result**: During burst sends (3 messages in rapid succession), only the first message gets through
  - Message 1: ✓ Accepted and processed
  - Message 2: ✗ Connection refused (stream in reset delay)
  - Message 3: ✗ Connection refused (stream in reset delay)

### Test Results
- **MacTCP**: 4/48 messages received (8% success rate)
- **OpenTransport**: 24/24 messages received (100% success rate)
- **POSIX**: Would handle all messages (multi-threaded listener with queue)

## Solution: Listen Stream Pool

### Key Insight from MacTCP Programmer's Guide

**MacTCP supports multiple streams listening on the same port!**

From MacTCP Programmer's Guide (lines 3695-3699):
> "If the remote IP address and remote TCP port are 0, a connection is accepted from any remote TCP. If they are nonzero, a connection is accepted only from that particular remote TCP."

The `duplicateSocket` error applies to the **TCP 4-tuple** (local IP, local port, remote IP, remote port), not just the local port. Multiple streams can call `TCPPassiveOpen` on port 8080 with wildcard remote addresses (0.0.0.0:0).

### Architecture

Create a **pool of N listen streams** (similar to existing send stream pool):

```
Listen Stream Pool (4 streams):
┌─────────────────────────────────────────────────────────────┐
│ Stream 0: LISTENING → (accept from 10.1.1.5) → PROCESSING → │
│           (reset delay 100ms) → IDLE → LISTENING             │
├─────────────────────────────────────────────────────────────┤
│ Stream 1: LISTENING → (accept from 10.1.1.6) → PROCESSING   │
├─────────────────────────────────────────────────────────────┤
│ Stream 2: LISTENING → ready to accept                        │
├─────────────────────────────────────────────────────────────┤
│ Stream 3: LISTENING → ready to accept                        │
└─────────────────────────────────────────────────────────────┘
```

**Key Benefits**:
- While Stream 0 is in reset delay, Streams 1-3 continue accepting connections
- Reset delays overlap - continuous availability
- Can handle up to N simultaneous connections
- No global bottleneck

## Implementation Details

### 1. Configuration Constants (`classic_mac_mactcp/network_init.h`)

Add listen pool size constant:

```c
#ifdef __MAC_SE__
/* Mac SE / System 6 configuration - optimized for limited memory */
#define TCP_LISTEN_STREAM_POOL_SIZE 2  /* 2 listen streams for SE */
#else
/* Standard configuration - System 7+ with ample memory */
#define TCP_LISTEN_STREAM_POOL_SIZE 4  /* 4 listen streams standard */
#endif
```

**Memory Impact**:
- Standard: 4 streams × 16 KB buffer = 64 KB
- Mac SE: 2 streams × 8 KB buffer = 16 KB

### 2. Data Structures (`classic_mac_mactcp/messaging.h`)

Replace single `gTCPListenStream` with pool:

```c
/* TCP Listen Stream Pool Entry
 * Each entry represents one reusable listen stream that can accept one connection at a time
 * Pool operates independently - each stream has own state and reset delay
 * Reference: MacTCP Programmer's Guide Section 4-41 "TCPPassiveOpen" wildcard mode
 */
typedef struct {
    StreamPtr stream;                      /* TCP stream handle */
    Ptr rcvBuffer;                         /* Dedicated receive buffer */
    TCPStreamState state;                  /* Current state (IDLE, LISTENING, etc.) */
    Boolean needsReset;                    /* Reset delay flag */
    unsigned long resetTime;               /* When reset delay started (TickCount) */
    MacTCPAsyncHandle asyncHandle;         /* Async handle for TCPPassiveOpen */
    ASR_Event_Info asrEvent;               /* ASR event tracking */
    wdsEntry rds[MAX_RDS_ENTRIES];         /* RDS for no-copy receives */
    Boolean rdsPendingReturn;              /* Buffer return tracking */
    int poolIndex;                         /* Index in pool (for debugging) */
} TCPListenStreamPoolEntry;
```

### 3. Initialization (`classic_mac_mactcp/messaging.c`)

Initialize pool in `InitTCP()`:

```c
static TCPListenStreamPoolEntry gListenStreamPool[TCP_LISTEN_STREAM_POOL_SIZE];

OSErr InitTCP(short macTCPRefNum, unsigned long streamReceiveBufferSize,
              TCPNotifyUPP listenAsrUPP, TCPNotifyUPP sendAsrUPP)
{
    int i;

    /* Initialize listen stream pool */
    log_info_cat(LOG_CAT_MESSAGING, "Initializing TCP listen stream pool (%d streams)...",
                 TCP_LISTEN_STREAM_POOL_SIZE);

    for (i = 0; i < TCP_LISTEN_STREAM_POOL_SIZE; i++) {
        TCPListenStreamPoolEntry *entry = &gListenStreamPool[i];

        /* Allocate receive buffer */
        entry->rcvBuffer = AllocateNetworkBuffer(streamReceiveBufferSize);
        if (!entry->rcvBuffer) {
            /* Cleanup previously allocated streams */
            goto cleanup;
        }

        /* Create TCP stream */
        err = MacTCPImpl_TCPCreate(macTCPRefNum, &entry->stream,
                                   streamReceiveBufferSize,
                                   entry->rcvBuffer, listenAsrUPP, (Ptr)entry);
        if (err != noErr) {
            goto cleanup;
        }

        entry->state = TCP_STATE_IDLE;
        entry->poolIndex = i;
        entry->needsReset = false;
        entry->asyncHandle = NULL;
        entry->rdsPendingReturn = false;

        log_debug_cat(LOG_CAT_MESSAGING, "Listen pool[%d]: Stream created", i);
    }

    log_info_cat(LOG_CAT_MESSAGING, "TCP listen stream pool initialized (%d streams)",
                 TCP_LISTEN_STREAM_POOL_SIZE);
    return noErr;

cleanup:
    /* Cleanup code... */
}
```

### 4. State Machine Updates (`classic_mac_mactcp/tcp_state_handlers.c`)

**Key Changes**:
- Convert from single global state to per-entry state
- Each pool entry has independent reset delay timer
- Dispatch based on stream pointer (identify which pool entry)

**Updated Functions**:
- `dispatch_listen_state_handler()`: Takes pool entry pointer instead of global state
- `handle_listen_idle_state()`: Check entry-specific reset delay
- `handle_connection_accepted()`: Process on specific pool entry
- `StartPassiveListen()`: Called for each IDLE entry

**Main Processing Loop** (`ProcessTCPStateMachine`):

```c
void ProcessTCPStateMachine(GiveTimePtr giveTime)
{
    int i;

    /* Process each listen stream in pool */
    for (i = 0; i < TCP_LISTEN_STREAM_POOL_SIZE; i++) {
        TCPListenStreamPoolEntry *entry = &gListenStreamPool[i];
        dispatch_listen_state_handler(entry, giveTime);
    }

    /* Process send stream pool (existing code) */
    for (i = 0; i < TCP_SEND_STREAM_POOL_SIZE; i++) {
        /* ... existing send pool processing ... */
    }
}
```

### 5. ASR Handler Updates (`classic_mac_mactcp/messaging.c`)

Update `TCP_Listen_ASR_Handler()` to identify which pool entry:

```c
pascal void TCP_Listen_ASR_Handler(StreamPtr tcpStream, unsigned short eventCode,
                                   Ptr userDataPtr, unsigned short terminReason,
                                   struct ICMPReport *icmpMsg)
{
    TCPListenStreamPoolEntry *entry = (TCPListenStreamPoolEntry *)userDataPtr;

    /* Validate pool entry */
    if (!entry || entry->poolIndex < 0 || entry->poolIndex >= TCP_LISTEN_STREAM_POOL_SIZE) {
        log_error_cat(LOG_CAT_MESSAGING, "Listen ASR: Invalid pool entry");
        return;
    }

    /* Store ASR event for this specific pool entry */
    entry->asrEvent.eventPending = true;
    entry->asrEvent.eventCode = eventCode;
    entry->asrEvent.termReason = terminReason;
    /* ... */
}
```

### 6. Passive Open for Each Stream

Each IDLE stream independently calls `TCPPassiveOpen`:

```c
void StartPassiveListen(TCPListenStreamPoolEntry *entry)
{
    OSErr err;

    if (entry->state != TCP_STATE_IDLE || entry->stream == kInvalidStreamPtr) {
        return;
    }

    /* Start async passive open with wildcard remote address */
    err = MacTCPImpl_TCPListenAsync(entry->stream, PORT_TCP, &entry->asyncHandle);

    if (err == noErr) {
        entry->state = TCP_STATE_LISTENING;
        log_debug_cat(LOG_CAT_MESSAGING, "Listen pool[%d]: Started async listen on port %u",
                      entry->poolIndex, PORT_TCP);
    } else {
        log_error_cat(LOG_CAT_MESSAGING, "Listen pool[%d]: TCPListenAsync failed: %d",
                      entry->poolIndex, err);
        entry->needsReset = true;
        entry->resetTime = TickCount();
    }
}
```

## Expected Performance Improvements

### Message Delivery Rate
- **Before**: 8% success rate (4/48 messages during burst test)
- **After**: 100% success rate (48/48 messages)

### Concurrent Connection Capacity
- **Before**: 1 connection, others get "Connection Refused"
- **After**: Up to 4 concurrent connections (standard), 2 on Mac SE

### Reset Delay Impact
- **Before**: Global 100ms blackout period after each message
- **After**: Independent reset delays - only affects one stream

### Burst Handling Example

**Scenario**: 3 messages arrive within 10ms

**Before (1 stream)**:
- T+0ms: Message 1 arrives → Stream accepts → processes → closes
- T+1ms: Message 2 arrives → **Connection Refused** (stream resetting)
- T+2ms: Message 3 arrives → **Connection Refused** (stream resetting)
- T+100ms: Stream ready again

**After (4 streams)**:
- T+0ms: Message 1 arrives → Stream 0 accepts → processes → closes
- T+1ms: Message 2 arrives → Stream 1 accepts → processes → closes
- T+2ms: Message 3 arrives → Stream 2 accepts → processes → closes
- All messages delivered successfully!

## Testing Strategy

### 1. Unit Testing
- Verify each pool entry initializes correctly
- Test independent state transitions
- Verify reset delays don't block other streams

### 2. Integration Testing
- Run existing `/test` command (4 rounds × 12 messages)
- Expected: 48/48 messages received (100% vs current 8%)
- Compare with OpenTransport performance (should match)

### 3. Stress Testing
- Send 10+ concurrent messages
- Verify graceful degradation if pool exhausted
- Monitor memory usage (especially on Mac SE)

### 4. Regression Testing
- Verify normal message flow still works
- Test with 2-3 peers simultaneously
- Ensure no memory leaks during long sessions

## Implementation Phases

### Phase 1: Core Infrastructure (2-3 hours)
- [ ] Add pool size constants to `network_init.h`
- [ ] Define `TCPListenStreamPoolEntry` structure in `messaging.h`
- [ ] Convert global variables to pool array in `messaging.c`

### Phase 2: Pool Initialization (1-2 hours)
- [ ] Update `InitTCP()` to create pool
- [ ] Update `CleanupTCP()` to release pool
- [ ] Add pool allocation/deallocation helpers

### Phase 3: State Machine Updates (2-3 hours)
- [ ] Update `tcp_state_handlers.c` to work with pool entries
- [ ] Modify `dispatch_listen_state_handler()` signature
- [ ] Update all state handlers to use entry-specific state
- [ ] Implement per-entry reset delay logic

### Phase 4: ASR and Processing (1-2 hours)
- [ ] Update `TCP_Listen_ASR_Handler()` to identify pool entry
- [ ] Modify `ProcessTCPStateMachine()` to iterate pool
- [ ] Update `StartPassiveListen()` for pool entries

### Phase 5: Testing and Refinement (2-3 hours)
- [ ] Build and test on Mac SE (memory constraints)
- [ ] Run automated test suite
- [ ] Performance measurements
- [ ] Debug any issues

**Total Estimated Time**: 8-13 hours

## Risks and Mitigations

### Risk 1: MacTCP doesn't support multiple listeners on same port
**Mitigation**: Documentation explicitly supports wildcard passive opens. If issue occurs, we can verify with simple test program.

### Risk 2: Memory exhaustion on Mac SE
**Mitigation**: Use smaller pool (2 streams) and smaller buffers (8 KB) on SE builds.

### Risk 3: Complexity increases debugging difficulty
**Mitigation**: Add extensive logging with pool index in all messages. Use existing state machine patterns.

### Risk 4: ASR handler race conditions
**Mitigation**: ASR stores event data; main loop processes it synchronously. No threading issues (single-threaded Mac).

## References

### MacTCP Programmer's Guide
- **Section 4-41**: TCPPassiveOpen - wildcard remote address behavior
- **Section 4-36**: TCPCreate - stream vs connection distinction
- **Page 3176**: 64 TCP streams system-wide limit
- **Page 3695-3699**: Passive open with remote=0 accepts any remote TCP

### Existing Code Patterns
- **`classic_mac_mactcp/messaging.c`**: Send stream pool implementation (lines 144-999)
- **`classic_mac_mactcp/tcp_state_handlers.c`**: State machine dispatch pattern
- **`classic_mac_mactcp/network_init.h`**: Memory-aware configuration (lines 20-30)

## Success Criteria

1. ✓ MacTCP receives 100% of messages during burst test (match OpenTransport)
2. ✓ No "Connection Refused" errors during normal operation
3. ✓ Memory usage stays within Mac SE constraints (≤20 KB app heap)
4. ✓ No regression in normal (non-burst) messaging
5. ✓ Code maintains existing architecture patterns and style
