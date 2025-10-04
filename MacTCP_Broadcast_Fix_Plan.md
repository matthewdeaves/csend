# MacTCP Broadcast Reception Fix Plan

## Root Cause Analysis (Verified Against MacTCP Documentation)

### Critical Bug Found: TCPStatus Failing After PassiveOpen Completion

**Evidence from Logs:**
```
1925-10-04 10:56:54 TCPStatus failed after listen accept
1925-10-04 10:57:15 Incoming TCP connection established from 10.188.1.19:40770.
1925-10-04 10:57:15 Listen connection closed by peer (periodic check).
1925-10-04 10:57:38 TCPStatus failed after listen accept
```

**Flow:**
1. ✅ MacTCP correctly calls `TCPListenAsync` (PassiveOpen) - works!
2. ✅ OpenTransport Mac connects to MacTCP - connection accepted!
3. ❌ **BUG**: `TCPStatus()` call fails immediately after PassiveOpen completes
4. ❌ State goes back to IDLE instead of CONNECTED_IN
5. ❌ Connection is never properly established, so data is never received

**Location**: `classic_mac/tcp_state_handlers.c:102-106`

```c
if (gNetworkOps->TCPStatus(gTCPListenStream, &tcpInfo) == noErr) {
    handle_connection_accepted(tcpInfo.remoteHost, tcpInfo.remotePort, giveTime);
} else {
    log_app_event("TCPStatus failed after listen accept");  // ← FAILING HERE
    gTCPListenState = TCP_STATE_IDLE;  // ← Goes back to IDLE!
}
```

### Secondary Issue: Incorrect "Send stream in unexpected state" Warnings

**Not a bug, just noisy logging**. The send stream IS correctly in IDLE state when not actively sending.

**Location**: `classic_mac/messaging.c:692-701`

## Fixes Required

### Fix 1: Investigate and Fix TCPStatus Failure (CRITICAL)

**File**: `classic_mac/mactcp_impl.c:818-846`

**Hypothesis**: The MacTCP `TCPStatus` call (PBControlSync with csCode=TCPStatus) is failing or returning invalid data immediately after `PassiveOpen` completes.

**Per MacTCP Programmer's Guide**:
- "TCPPassiveOpen listens for an incoming connection. The command is completed when a connection is established or when an error occurs."
- When PassiveOpen completes successfully, connection SHOULD be established
- `connectionState` field should be >= 8 (established)

**Fix Options**:
1. **Add error code logging** to see WHY TCPStatus is failing
2. **Check if async completion is being checked too early** - may need small delay
3. **Verify connectionState interpretation** - check if line 841-842 logic is correct
4. **Check if stream is in transitional state** - may need to retry TCPStatus

### Fix 2: Remove TCP_STATE_IDLE from Unexpected States (MINOR)

**File**: `classic_mac/messaging.c:692-701`

**Change**: Move `TCP_STATE_IDLE` to a valid case (do nothing), remove from warning case

```c
case TCP_STATE_IDLE:
    /* Normal idle state - waiting for send request */
    break;

case TCP_STATE_UNINITIALIZED:
case TCP_STATE_LISTENING:
// ... other truly unexpected states
    log_warning_cat(LOG_CAT_MESSAGING, "Send stream in unexpected state: %d", gTCPSendState);
    break;
```

## Implementation Steps

### Phase 1: Diagnose TCPStatus Failure
1. Add detailed error logging to `MacTCPImpl_TCPStatus()` to capture actual error code
2. Log the `connectionState` value even when TCPStatus succeeds
3. Add logging to `process_listen_async_completion()` to see exact timing

### Phase 2: Fix TCPStatus Issue
Based on diagnostics, likely one of:
- **Option A**: Add retry logic if TCPStatus fails immediately after PassiveOpen
- **Option B**: Add small delay before calling TCPStatus
- **Option C**: Fix connectionState interpretation logic
- **Option D**: Handle specific MacTCP error codes differently

### Phase 3: Clean Up Logging
- Fix send stream IDLE warning
- Remove excessive debug output

## Expected Outcome

After fixes:
- ✅ MacTCP will properly transition to CONNECTED_IN state after accepting connections
- ✅ OpenTransport Mac broadcast messages will be received and displayed
- ✅ All three peers (OT Mac @10.188.1.103, MacTCP Mac @10.188.1.213, POSIX @10.188.1.19) will see each other's broadcasts
- ✅ No more "Send stream in unexpected state" spam

## References Used

- `resources/Books/MacTCP_Programmers_Guide_1989.txt`
  - Lines 2163-2207: TCPPassiveOpen documentation
  - Lines 2330-2602: TCPPassiveOpen parameter reference
  - Connection state documentation

## CRITICAL UPDATE: MacTCP CAN Receive TCP - Specific OT Mac Issue!

### Evidence from Logs

**MacTCP Mac SUCCESSFULLY received TCP from POSIX:**
```
1925-10-04 10:57:15 ubuntu@10.188.1.19: hello fom ubuntu ← RECEIVED!
```

**POSIX SUCCESSFULLY received TCP from both Macs:**
```
2025-10-04 10:53:30 User@10.188.1.103: hello to all (OT Mac) ← RECEIVED!
2025-10-04 10:54:14 User@10.188.1.103: test broadcast (OT Mac) ← RECEIVED!
2025-10-04 10:54:38 MacUser@10.188.1.213: hello from mactcp (MacTCP) ← RECEIVED!
```

**MacTCP Mac DID NOT receive from OT Mac:**
- NO "hello to all" message
- NO "test broadcast" message
- Only UDP discovery packets were received

### Revised Root Cause

The problem is **NOT** a general TCP reception failure in MacTCP. The problem is **specific to connections from OpenTransport Mac (10.188.1.103)**.

**Working**: POSIX (10.188.1.19) → MacTCP (10.188.1.213) ✅
**Broken**: OT Mac (10.188.1.103) → MacTCP (10.188.1.213) ❌

This suggests a **compatibility issue** between OpenTransport's TCP implementation and MacTCP's listen/accept logic.

## Regression Analysis

### Comparison with Commit f961569 (Oct 4, 2025)

**Key Finding**: The MacTCP listen/receive code **has NOT changed** since commit f961569!

**Changes Made Since f961569**:
- ✅ **QUIT message handling** (UDP broadcast) - Added in `shared/discovery.c`, `classic_mac/discovery.c`
- ✅ **peer_shared_mark_inactive()** - Added in `shared/peer.c`
- ✅ **BroadcastQuitMessage()** - Added to Classic Mac
- ✅ **MSG_QUIT handling in discovery logic** - Properly handles quit over UDP

**Files Unchanged**:
- `classic_mac/messaging.c` - NO CHANGES
- `classic_mac/tcp_state_handlers.c` - NO CHANGES
- `classic_mac/mactcp_impl.c` - NO CHANGES
- All TCP listen/accept/receive code - NO CHANGES

### Critical Conclusion

**The TCPStatus bug existed in f961569 too** - it was simply not tested or noticed at that time!

This is NOT a regression. This is a **pre-existing bug** that was present when the MacTCP code was originally written. The QUIT message changes we made are working correctly and did not introduce this issue.

**Implication**: We cannot "revert" to fix this - we must debug and fix the root cause of why TCPStatus fails after PassiveOpen completes.

## Hypothesis: OpenTransport vs MacTCP Compatibility Issue

### Possible Causes

1. **Connection timing difference**: OT Mac might connect faster than POSIX, hitting the TCPStatus bug window
2. **TCP option negotiation**: OT might negotiate different TCP options that MacTCP can't handle
3. **Connection state race**: OT's async behavior might trigger edge case in MacTCP's state machine
4. **Port/addressing difference**: Something specific about how OT formats the connection

### Key Question

Why does "TCPStatus failed after listen accept" correlation with OT Mac connections but not POSIX connections?

Looking at logs:
- 10:56:54 - TCPStatus failed
- 10:57:15 - Connection from POSIX (10.188.1.19) succeeded ← This works!
- 10:57:15 - Connection immediately closed
- 10:57:38 - TCPStatus failed again

## Next Steps

1. ~~Compare current MacTCP code to working commit~~ ✅ DONE - No changes to TCP code
2. ~~Identify what changed that broke TCPStatus~~ ✅ DONE - Nothing changed, bug pre-existed
3. **NEW**: Compare exact timestamps - does TCPStatus failure correlate with OT connection attempts?
4. **NEW**: Add logging to see what IP addresses trigger TCPStatus failures
5. **NEW**: Check if OT Mac uses different TCP options vs POSIX
6. **NEW**: Test theory: Does MacTCP reject ALL OT connections, or just some?
7. Implement fix based on findings
