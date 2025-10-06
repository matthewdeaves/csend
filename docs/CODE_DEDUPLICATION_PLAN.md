# Code Deduplication & Refactoring Plan for CSend

## Executive Summary
After thorough analysis, I've identified **significant code duplication** between MacTCP and OpenTransport builds. Many files are 100% identical and can be immediately moved to a shared location. The project currently has **5,608 lines** in MacTCP and **2,887 lines** in OpenTransport, with substantial overlap.

## Key Findings

### 100% Identical Files (Can be shared immediately)
1. **UI Layer** (5 files, ~1,134 lines):
   - `dialog_input.c/h` (180 lines)
   - `dialog_messages.c/h` (333 lines)
   - `dialog_peerlist.c/h` (291 lines)
   - `logging.c/h` (41 lines)
   - `peer.c/h` (55 lines)

### Nearly Identical Files (Minor differences)
2. **Main Dialog** - `dialog.c` differs only in broadcast logic:
   - MacTCP: 316 lines (complex state machine, queue)
   - OpenTransport: 254 lines (simpler, event-driven)
   - **62 lines difference** - mostly broadcast handling

3. **Main Event Loop** - `main.c` differs in initialization:
   - MacTCP: 446 lines
   - OpenTransport: 523 lines
   - Differences: network init, endpoint creation, log file names

### Architecture-Specific Files (Cannot be shared)
4. **Network Implementation**:
   - MacTCP: `mactcp_impl.c` (1,440 lines) + `network_abstraction.c` (280 lines) + `tcp_state_handlers.c` (226 lines) + `network_init.c` (234 lines) + `DNR.c` (382 lines) = **2,562 lines**
   - OpenTransport: `opentransport_impl.c` (891 lines) = **891 lines**

5. **Discovery** - Different APIs but similar logic:
   - MacTCP: 551 lines (uses abstraction layer)
   - OpenTransport: 147 lines (direct OT calls)

6. **Messaging** - Different state management:
   - MacTCP: 833 lines (state machine, queuing)
   - OpenTransport: 172 lines (event-driven)

## Code Duplication Analysis

### File-by-File Comparison (MD5 Checksums)

| File | MacTCP | OpenTransport | Status |
|------|--------|---------------|--------|
| `dialog_input.c` | `c5276a80e03bbc1e518e040167d9a416` | `c5276a80e03bbc1e518e040167d9a416` | ✅ **IDENTICAL** |
| `dialog_messages.c` | `b3d0961b77ce507859aa8bb1904f4bd8` | `b3d0961b77ce507859aa8bb1904f4bd8` | ✅ **IDENTICAL** |
| `dialog_peerlist.c` | `836732f7ae845bea3ccfcf0293e36f50` | `836732f7ae845bea3ccfcf0293e36f50` | ✅ **IDENTICAL** |
| `logging.c` | `0ede72aaef867c2a8fa9135c5370c7e9` | `0ede72aaef867c2a8fa9135c5370c7e9` | ✅ **IDENTICAL** |
| `peer.c` | `e6b177154a5674f7d8b5ef1a23453944` | `e6b177154a5674f7d8b5ef1a23453944` | ✅ **IDENTICAL** |
| `dialog.h` | `bd4aa643e627d9a9214f86b07a33d792` | `bd4aa643e627d9a9214f86b07a33d792` | ✅ **IDENTICAL** |
| `dialog_input.h` | `90a8e2ce94b27e5670147143666f9693` | `90a8e2ce94b27e5670147143666f9693` | ✅ **IDENTICAL** |
| `dialog_messages.h` | `5286a8abea5117e8154ba6591e9da0a4` | `5286a8abea5117e8154ba6591e9da0a4` | ✅ **IDENTICAL** |
| `dialog_peerlist.h` | `4177893148b82a902c65c13aa954606b` | `4177893148b82a902c65c13aa954606b` | ✅ **IDENTICAL** |
| `logging.h` | `6266a022a574451d3cffa00e425d5c5a` | `6266a022a574451d3cffa00e425d5c5a` | ✅ **IDENTICAL** |
| `peer.h` | `14bc3782d18001cc8893eb04fbd819ba` | `14bc3782d18001cc8893eb04fbd819ba` | ✅ **IDENTICAL** |
| `dialog.c` | Different | Different | ⚠️ **NEARLY IDENTICAL** (broadcast logic differs) |
| `main.c` | Different | Different | ⚠️ **NEARLY IDENTICAL** (network init differs) |
| `discovery.c` | Different | Different | 🔴 **DIFFERENT** (API layer) |
| `messaging.c` | Different | Different | 🔴 **DIFFERENT** (state management) |

### Line Count Analysis

| Component | MacTCP Lines | OT Lines | Shared Lines | Duplication |
|-----------|--------------|----------|--------------|-------------|
| UI (identical) | 1,134 | 1,134 | 1,134 | 100% |
| Peer/Logging (identical) | 96 | 96 | 96 | 100% |
| Dialog (nearly identical) | 316 | 254 | ~200 | ~75% |
| Main (nearly identical) | 446 | 523 | ~300 | ~65% |
| Discovery | 551 | 147 | ~50 | ~25% |
| Messaging | 833 | 172 | ~80 | ~20% |
| Network impl | 2,562 | 891 | 0 | 0% |
| **TOTAL** | **5,608** | **2,887** | **~1,860** | **~35%** |

## Proposed Directory Structure

**NOTE**: As part of this refactoring, `classic_mac/` will be renamed to `classic_mac_mactcp/` to follow the naming convention of `classic_mac_ot/`.

```
shared/
  ├── classic_mac/          # NEW: Shared Classic Mac code (all builds)
  │   ├── ui/              # UI layer (both builds)
  │   │   ├── dialog_input.c/h
  │   │   ├── dialog_messages.c/h
  │   │   ├── dialog_peerlist.c/h
  │   │   └── dialog_base.c/h  # Extracted common dialog code
  │   ├── peer.c/h          # Peer management
  │   └── logging.c/h       # Classic Mac logging
  │
  ├── (existing shared POSIX/Mac protocol code)
  │   ├── protocol.c/h
  │   ├── discovery.c/h
  │   ├── messaging.c/h
  │   ├── peer.c/h
  │   ├── logging.c/h
  │   └── time_utils.c/h

classic_mac_mactcp/      # MacTCP-specific only (renamed from classic_mac/)
  ├── mactcp_impl.c/h
  ├── network_abstraction.c/h
  ├── tcp_state_handlers.c/h
  ├── network_init.c/h
  ├── DNR.c
  ├── main.c             # MacTCP-specific initialization
  ├── dialog.c/h         # MacTCP-specific broadcast logic
  ├── discovery.c/h      # MacTCP-specific discovery
  └── messaging.c/h      # MacTCP-specific messaging

classic_mac_ot/          # OpenTransport-specific only
  ├── opentransport_impl.c/h
  ├── main.c             # OT-specific initialization
  ├── dialog.c/h         # OT-specific broadcast logic
  ├── discovery.c/h      # OT-specific discovery
  └── messaging.c/h      # OT-specific messaging
```

## Phased Implementation Plan

### Phase 1: Move 100% Identical UI Files (LOW RISK)
**Goal**: Rename `classic_mac/` to `classic_mac_mactcp/` and extract dialog UI components to `shared/classic_mac/ui/`

**Files to Move** (1,134 lines):
- `dialog_input.c/h`
- `dialog_messages.c/h`
- `dialog_peerlist.c/h`

**Steps**:
1. Rename `classic_mac/` directory to `classic_mac_mactcp/`
2. Create `shared/classic_mac/` directory
3. Create `shared/classic_mac/ui/` subdirectory
4. Move `dialog_input.c/h` from `classic_mac_mactcp/` to `shared/classic_mac/ui/`
5. Move `dialog_messages.c/h` from `classic_mac_mactcp/` to `shared/classic_mac/ui/`
6. Move `dialog_peerlist.c/h` from `classic_mac_mactcp/` to `shared/classic_mac/ui/`
7. Delete duplicate files from `classic_mac_ot/`
8. Update `Makefile.retro68.mactcp` to reference `classic_mac_mactcp/`:
   ```makefile
   SHARED_CLASSIC_MAC_UI_DIR = shared/classic_mac/ui
   SHARED_CLASSIC_MAC_UI_C_FILES = $(wildcard $(SHARED_CLASSIC_MAC_UI_DIR)/*.c)
   # Add to compile list
   ```
9. Update `Makefile.retro68.ot` similarly
10. Update include paths in both Makefiles to add `-Ishared/classic_mac/ui`
11. Update all `#include` paths in source files to reference new locations
12. Build MacTCP: `make -f Makefile.retro68.mactcp clean && make -f Makefile.retro68.mactcp`
13. Build OpenTransport: `make -f Makefile.retro68.ot clean && make -f Makefile.retro68.ot`
14. Test manually on real hardware:
    - **POSIX**: Ubuntu machine running `build/posix/csend_posix`
    - **MacTCP**: PowerPC Mac running `csend-mac.APPL`
    - **OpenTransport**: Separate PowerPC Mac running `csend-mac-ot.APPL`
    - User performs testing and provides log files for AI review

**Risk**: VERY LOW - Files are byte-for-byte identical
**Benefit**: Eliminate 1,134 duplicate lines (14% reduction)
**Testing Time**: ~30 minutes (hardware setup + testing)

---

### Phase 2: Move Peer & Logging (LOW RISK)
**Goal**: Extract peer management and logging to `shared/classic_mac/`

**Files to Move** (96 lines):
- `peer.c/h`
- `logging.c/h`

**Steps**:
1. Move `peer.c/h` from `classic_mac_mactcp/` to `shared/classic_mac/`
2. Move `logging.c/h` from `classic_mac_mactcp/` to `shared/classic_mac/`
3. Delete duplicate files from `classic_mac_ot/`
4. Update Makefiles to compile from `shared/classic_mac/`
5. Update include paths: `-Ishared/classic_mac`
6. Build both versions
7. Test on real hardware (Ubuntu + 2 PowerPC Macs):
   - Peer list updates correctly
   - Logging to file works
   - Peer addition/removal
   - User provides log files for AI review

**Risk**: VERY LOW - Files are identical
**Benefit**: Eliminate 96 duplicate lines (1% reduction)
**Testing Time**: ~15 minutes (hardware testing)

**Total After Phase 1-2**: 1,230 lines eliminated (15% reduction)

---

### Phase 3: Extract Common Dialog Logic (MEDIUM RISK)
**Goal**: Create `dialog_base.c` with shared dialog initialization/cleanup

**Analysis**:
Both `dialog.c` files share ~200 lines of identical initialization code. The only difference is in `HandleSendButtonClick()` which handles broadcast logic differently:
- **MacTCP**: Checks TCP state machine, queues messages if busy
- **OpenTransport**: Direct OT calls, simpler event-driven model

**Common Functions** (can be extracted):
- `InitDialog()` - Dialog creation and control setup (~80 lines, 95% identical)
- `CleanupDialog()` - Cleanup and disposal (100% identical)
- `ActivateDialogTE()` - TextEdit activation (100% identical)
- `UpdateDialogControls()` - Control updates (100% identical)
- `InvalidateInputTE()`, `InvalidateMessagesTE()`, `InvalidatePeerList()` (100% identical)

**Platform-Specific** (keep separate):
- `HandleSendButtonClick()` - Broadcast/send logic (~60 lines differ)

**Steps**:
1. Create `shared/classic_mac/ui/dialog_base.c/h`
2. Extract common functions to `dialog_base.c`:
   ```c
   // dialog_base.h
   Boolean DialogBase_Init(void);
   void DialogBase_Cleanup(void);
   void DialogBase_ActivateTE(Boolean activating);
   void DialogBase_UpdateControls(void);
   void DialogBase_InvalidateInput(void);
   void DialogBase_InvalidateMessages(void);
   void DialogBase_InvalidatePeerList(void);
   ```
3. Update `classic_mac_mactcp/dialog.c` to call base functions
4. Update `classic_mac_ot/dialog.c` to call base functions
5. Keep `HandleSendButtonClick()` in each platform's `dialog.c`
6. Build both versions
7. Test extensively on real hardware (3 machines):
   - Dialog initialization
   - TextEdit activation/deactivation
   - Control updates
   - Broadcast button (platform-specific logic)
   - Direct send (platform-specific logic)
   - User provides log files for AI review

**Risk**: MEDIUM - Dialog is core UI, must test carefully
**Benefit**: Eliminate ~200 duplicate lines (2.5% reduction)
**Testing Time**: ~1 hour (thorough hardware testing needed)

**Total After Phase 3**: 1,430 lines eliminated (17.5% reduction)

---

### Phase 4: Unify Main Initialization (MEDIUM RISK)
**Goal**: Extract common main event loop to `shared/classic_mac/main_base.c`

**Analysis**:
Both `main.c` files have identical structure:
- Toolbox initialization
- AppleEvent handler installation
- Main WaitNextEvent loop
- Event dispatching (menu, mouse, keyboard, update)

**Differences**:
- MacTCP: Calls `InitializeNetworking()` (MacTCP-specific)
- OpenTransport: Calls `InitOTForApp()`, creates endpoints
- Log file names differ
- Network initialization sequence differs

**Common Code** (~300 lines):
- `InitializeToolbox()` - MaxApplZone, InitGraf, InitFonts, etc.
- `InstallAppleEventHandlers()` - Quit handler
- `RunEventLoop()` - Main WaitNextEvent loop
- Event handlers: `HandleMenuEvent()`, `HandleMouseDown()`, `HandleKeyDown()`, `HandleUpdateEvent()`

**Steps**:
1. Create `shared/classic_mac/main_base.c/h`
2. Define network initialization callback interface:
   ```c
   typedef struct {
       OSErr (*init_network)(char* ipBuf, size_t ipSize, char* userBuf, size_t userSize);
       void (*shutdown_network)(void);
       void (*poll_network)(void);  // Called in event loop
   } NetworkCallbacks;
   ```
3. Extract common functions to `main_base.c`
4. Create `MainBase_Run()` that takes callbacks:
   ```c
   int MainBase_Run(const char* logFileName,
                    const NetworkCallbacks* netCallbacks);
   ```
5. Update platform-specific `main.c` files:
   ```c
   // classic_mac_mactcp/main.c
   static OSErr mactcp_init(char* ip, size_t ipSize, char* user, size_t userSize) {
       return InitializeNetworking();
   }

   int main() {
       NetworkCallbacks callbacks = {
           .init_network = mactcp_init,
           .shutdown_network = ShutdownNetworkAbstraction,
           .poll_network = ProcessTCPStateMachine
       };
       return MainBase_Run("csend_mac.log", &callbacks);
   }
   ```
6. Build both versions
7. Test on real hardware (3 machines):
   - Application launches
   - Menus work
   - Events handled correctly
   - Network initializes
   - Quit works properly
   - User provides log files for AI review

**Risk**: MEDIUM - Event loop is critical, but well-structured
**Benefit**: Eliminate ~300 duplicate lines (3.5% reduction)
**Testing Time**: ~1 hour (hardware testing)

**Total After Phase 4**: 1,730 lines eliminated (21% reduction)

---

### Phase 5: Abstract Discovery Interface (HIGH RISK)
**Goal**: Create unified discovery API with platform-specific backends

**Analysis**:
Both discovery modules perform the same logical operations:
- Send discovery broadcast (UDP to 255.255.255.255)
- Send discovery response (UDP to specific peer)
- Process incoming discovery packets
- Periodic discovery broadcasts

**Current State**:
- MacTCP: 551 lines, uses network abstraction layer
- OpenTransport: 147 lines, direct OT UDP calls
- Different internal implementation but same external behavior

**Proposed Interface**:
```c
// shared/classic_mac/discovery_interface.h
typedef struct {
    OSErr (*init)(void);
    void (*shutdown)(void);
    OSErr (*send_broadcast)(const char* username, const char* localIP);
    OSErr (*send_response)(const char* username, const char* localIP,
                          uint32_t destIP, uint16_t destPort);
    void (*poll)(uint32_t myLocalIP);  // Check for incoming packets
    void (*process_periodic)(const char* username, const char* localIP);
} DiscoveryOps;

extern const DiscoveryOps* gDiscoveryOps;

// Public API (calls through ops table)
OSErr Discovery_Init(void);
void Discovery_Shutdown(void);
OSErr Discovery_Broadcast(const char* username, const char* localIP);
// ... etc
```

**Steps**:
1. Create `shared/classic_mac/discovery_interface.h`
2. Create `shared/classic_mac/discovery_interface.c` with dispatcher functions
3. Rename `classic_mac_mactcp/discovery.c` to `classic_mac_mactcp/discovery_mactcp.c`
4. Rename `classic_mac_ot/discovery.c` to `classic_mac_ot/discovery_ot.c`
5. Update each implementation to expose ops table:
   ```c
   // classic_mac_mactcp/discovery_mactcp.c
   static OSErr mactcp_discovery_init(void) { ... }
   static OSErr mactcp_discovery_broadcast(...) { ... }

   const DiscoveryOps g_mactcp_discovery_ops = {
       .init = mactcp_discovery_init,
       .send_broadcast = mactcp_discovery_broadcast,
       // ...
   };
   ```
6. Update callers to use `Discovery_*()` API instead of direct calls
7. Build both versions
8. Test extensively on real hardware (3 machines):
   - Peer discovery works
   - Broadcast reception
   - Response sending
   - Periodic discovery
   - Cross-platform discovery (MacTCP ↔ OT ↔ POSIX)
   - User provides log files for AI review

**Risk**: HIGH - Network discovery is critical for peer detection
**Benefit**: Unified API, easier to add future network backends
**Testing Time**: ~2 hours (thorough cross-platform hardware testing)

---

### Phase 6: Abstract Messaging Interface (HIGH RISK)
**Goal**: Create unified messaging API

**Analysis**:
Both messaging modules expose the same high-level operations but with vastly different internal architecture:

**MacTCP** (833 lines):
- Complex TCP state machine
- Message queueing when TCP busy
- Async state tracking
- Connection pooling

**OpenTransport** (172 lines):
- Event-driven model
- Direct OT calls
- Simpler endpoint management

**Common External API**:
- Send message to peer
- Broadcast message to all peers
- Send quit message
- Message reception

**Proposed Interface**:
```c
// shared/classic_mac/messaging_interface.h
typedef struct {
    OSErr (*init)(void);
    void (*shutdown)(void);
    OSErr (*send_to_peer)(const char* peerIP, const char* message,
                         const char* msgType);
    OSErr (*broadcast)(const char* message);
    OSErr (*send_quit)(void);
    void (*poll)(void);  // Process state machine / check events
} MessagingOps;

extern const MessagingOps* gMessagingOps;

// Public API
OSErr Messaging_Init(void);
OSErr Messaging_SendToPeer(const char* ip, const char* msg, const char* type);
OSErr Messaging_Broadcast(const char* msg);
// ... etc
```

**Steps**:
1. Create `shared/classic_mac/messaging_interface.h`
2. Create `shared/classic_mac/messaging_interface.c` with dispatcher
3. Rename `classic_mac_mactcp/messaging.c` to `classic_mac_mactcp/messaging_mactcp.c`
4. Rename `classic_mac_ot/messaging.c` to `classic_mac_ot/messaging_ot.c`
5. Update each to expose ops table
6. Update `dialog.c` to use `Messaging_*()` API
7. Build both versions
8. Test extensively on real hardware (3 machines):
   - Direct messaging
   - Broadcast messaging
   - Message queuing (MacTCP)
   - Connection handling
   - Cross-platform messaging
   - User provides log files for AI review

**Risk**: HIGH - Core functionality, different internal models
**Benefit**: Unified API, cleaner dialog.c integration
**Testing Time**: ~2 hours (hardware testing)

---

## Testing Strategy

### Build Verification (All Phases)
For each phase, verify clean builds:
```bash
# MacTCP build
make -f Makefile.retro68.mactcp clean
make -f Makefile.retro68.mactcp
# Check: no errors, no warnings

# OpenTransport build
make -f Makefile.retro68.ot clean
make -f Makefile.retro68.ot
# Check: no errors, no warnings

# POSIX build (ensure no breakage)
make clean
make
# Check: no errors, no warnings
```

### Functional Testing (All Phases)
**Testing Environment**: Real hardware setup required
- **Machine 1**: Ubuntu PC running POSIX build (`build/posix/csend_posix`)
- **Machine 2**: PowerPC Mac running MacTCP build (`csend-mac.APPL`)
- **Machine 3**: PowerPC Mac running OpenTransport build (`csend-mac-ot.APPL`)
- All machines on same local network
- User performs testing and provides log files for AI review

**UI Testing** (PowerPC Macs):
1. Launch application
2. Verify dialog window appears correctly
3. Check all UI elements render:
   - Input field
   - Messages area
   - Peer list
   - Send button
   - Broadcast checkbox
   - Debug checkbox

**Peer Discovery Testing**:
1. Launch all 3 builds on separate machines
2. Verify peers discover each other within 5 seconds
3. Check peer list updates in all instances
4. Verify IP addresses and usernames display correctly

**Messaging Testing**:
1. Direct message: Select peer, type message, click Send
2. Verify message appears in recipient's message area
3. Broadcast message: Check broadcast box, type, send
4. Verify all peers receive broadcast
5. Test message from MacTCP → OpenTransport
6. Test message from OpenTransport → MacTCP
7. Test message from Mac → POSIX

**Cross-Platform Testing** (Critical):
1. Run 3 instances: Ubuntu (POSIX), PowerPC Mac (MacTCP), PowerPC Mac (OpenTransport)
2. Verify all 3 discover each other
3. Send messages in all directions:
   - MacTCP → OpenTransport ✓
   - MacTCP → POSIX ✓
   - OpenTransport → MacTCP ✓
   - OpenTransport → POSIX ✓
   - POSIX → MacTCP ✓
   - POSIX → OpenTransport ✓
4. Test broadcast reaches all 3 peers

**Stress Testing**:
1. Rapid message sending (10 messages/second)
2. Long messages (near BUFFER_SIZE limit)
3. Special characters
4. Multiple peers (if available)
5. Peer disconnect/reconnect

**Log Verification** (AI Review):
1. User collects log files from all 3 machines:
   - `csend_posix.log` (Ubuntu)
   - `csend_mac.log` (MacTCP PowerPC Mac)
   - `csend_classic_mac_ot_ppc.log` (OpenTransport PowerPC Mac)
2. User provides logs to AI for review
3. AI verifies no error messages
4. AI compares log output before/after refactoring
5. AI checks for memory leaks or unusual patterns

### Regression Testing Checklist
After each phase, verify:
- ✅ Application launches without crashes
- ✅ Dialog UI renders correctly
- ✅ Peer discovery works
- ✅ Direct messaging works
- ✅ Broadcast messaging works
- ✅ Quit/cleanup works without crashes
- ✅ Log files created with correct timestamps
- ✅ No new compiler warnings
- ✅ No increase in binary size (>5%)
- ✅ Cross-platform communication intact

---

## Expected Benefits

### Immediate Benefits (Phase 1-2)
- **Code Reduction**: 1,230 lines (15% reduction)
- **Risk**: Very low
- **Effort**: ~2-3 hours
- **Maintenance**: UI bug fixes apply to both builds automatically

### Medium-Term Benefits (Phase 3-4)
- **Code Reduction**: Additional 500 lines (6% reduction)
- **Risk**: Medium
- **Effort**: ~4-6 hours
- **Maintenance**: Dialog and main loop unified

### Long-Term Benefits (Phase 5-6)
- **Architecture**: Proper network abstraction
- **Extensibility**: Easy to add future network backends
- **Risk**: High
- **Effort**: ~8-12 hours
- **Maintenance**: Significant long-term benefit if actively developed

### Total Potential Savings
- **Lines Eliminated**: ~1,730 lines (21% reduction)
- **Maintenance Burden**: Significantly reduced
- **Code Quality**: Improved separation of concerns
- **Testing**: Reduced duplication in test scenarios

### Quantified Benefits

| Metric | Before | After (Phase 1-2) | After (Phase 3-4) | After (Phase 5-6) |
|--------|--------|-------------------|-------------------|-------------------|
| Total Lines | 8,495 | 7,265 | 6,765 | 6,765 |
| Duplicate Lines | 1,860 | 630 | 130 | 0 |
| Duplication % | 22% | 8.7% | 1.9% | 0% |
| Shared UI Lines | 0 | 1,230 | 1,430 | 1,430 |
| Shared Interface Lines | 0 | 0 | 0 | 400 |

---

## Risk Assessment

### Phase 1-2: LOW RISK ⚠️ GREEN
- Files are byte-for-byte identical
- No logic changes required
- Only Makefile and path updates
- Easy to revert if issues found
- **Recommendation**: PROCEED IMMEDIATELY

### Phase 3-4: MEDIUM RISK ⚠️ YELLOW
- Requires code refactoring
- Dialog and main loop are critical components
- Need careful testing of event handling
- Broadcast logic must remain platform-specific
- **Recommendation**: PROCEED AFTER Phase 1-2, with thorough testing

### Phase 5-6: HIGH RISK ⚠️ RED
- Major architectural changes
- Network code is critical
- Different internal models must coexist
- Extensive cross-platform testing required
- **Recommendation**: PROCEED ONLY IF:
  - Project will be actively maintained long-term
  - Plan to add more network backends
  - Team has time for thorough testing
  - Can accept risk of introducing bugs

---

## Recommendations

### Recommended Execution Order

#### Conservative Approach (Lowest Risk)
1. ✅ **Execute Phase 1** (UI files) - 1 hour
2. ✅ **Test thoroughly** - 30 minutes
3. ✅ **Execute Phase 2** (peer/logging) - 30 minutes
4. ✅ **Test thoroughly** - 15 minutes
5. ⏸️ **STOP and evaluate**
6. 🤔 **Decision point**: Is 15% reduction sufficient?
   - **YES**: Stop here, commit changes
   - **NO**: Continue to Phase 3

**Total Time**: ~2.5 hours
**Total Benefit**: 1,230 lines eliminated (15%)
**Risk**: Very low

#### Moderate Approach (Balanced)
1. ✅ Execute Phase 1-2 (as above)
2. ✅ **Execute Phase 3** (dialog base) - 2 hours
3. ✅ **Test extensively** - 1 hour
4. ⏸️ **STOP and evaluate**
5. 🤔 **Decision point**: Proceed to Phase 4?
   - **YES**: Continue
   - **NO**: Stop, commit

**Total Time**: ~5.5 hours
**Total Benefit**: 1,430 lines eliminated (17.5%)
**Risk**: Medium

#### Aggressive Approach (Maximum Benefit)
1. ✅ Execute Phase 1-4 (as above)
2. ✅ **Execute Phase 5** (discovery interface) - 3 hours
3. ✅ **Test cross-platform** - 2 hours
4. ✅ **Execute Phase 6** (messaging interface) - 3 hours
5. ✅ **Test cross-platform** - 2 hours
6. ✅ **Final integration testing** - 2 hours

**Total Time**: ~17.5 hours
**Total Benefit**: Full abstraction, future-proof architecture
**Risk**: High

### My Recommendation: **CONSERVATIVE + PHASE 3**

Execute Phase 1-3, then evaluate:
- **Phase 1-2**: Very safe, immediate 15% reduction
- **Phase 3**: Reasonable risk, good architectural benefit
- **Phase 4-6**: Only if planning major future development

**Rationale**:
- Phase 1-2 are "free wins" - identical files, zero risk
- Phase 3 provides good architectural benefit for modest risk
- Phase 4-6 require significant effort and testing
- Phase 4-6 only valuable if actively adding features

**Decision Criteria for Phase 4-6**:
- ✅ **PROCEED** if:
  - Planning to add more Classic Mac features
  - Want to support additional network stacks
  - Have time for 15+ hours of work
  - Can dedicate 4+ hours to testing

- ⛔ **STOP** if:
  - Project is in maintenance mode
  - No plans for major new features
  - Limited testing resources
  - 15% code reduction is sufficient

---

## Implementation Checklist

### Phase 1 Checklist
- [ ] Create `shared/classic_mac/ui/` directory
- [ ] Move `dialog_input.c/h`
- [ ] Move `dialog_messages.c/h`
- [ ] Move `dialog_peerlist.c/h`
- [ ] Update `Makefile.retro68.mactcp`
- [ ] Update `Makefile.retro68.ot`
- [ ] Build MacTCP version
- [ ] Build OpenTransport version
- [ ] Test UI rendering
- [ ] Test input field
- [ ] Test messages area
- [ ] Test peer list
- [ ] Commit changes

### Phase 2 Checklist
- [ ] Move `peer.c/h` to `shared/classic_mac/`
- [ ] Move `logging.c/h` to `shared/classic_mac/`
- [ ] Update Makefiles
- [ ] Build both versions
- [ ] Test peer management
- [ ] Test logging
- [ ] Commit changes

### Phase 3 Checklist
- [ ] Create `shared/classic_mac/ui/dialog_base.c/h`
- [ ] Extract common functions
- [ ] Update `classic_mac/dialog.c`
- [ ] Update `classic_mac_ot/dialog.c`
- [ ] Update Makefiles
- [ ] Build both versions
- [ ] Test dialog initialization
- [ ] Test broadcast (MacTCP)
- [ ] Test broadcast (OpenTransport)
- [ ] Test direct send
- [ ] Commit changes

### Phase 4 Checklist
- [ ] Create `shared/classic_mac/main_base.c/h`
- [ ] Define callback interface
- [ ] Extract common functions
- [ ] Update `classic_mac/main.c`
- [ ] Update `classic_mac_ot/main.c`
- [ ] Update Makefiles
- [ ] Build both versions
- [ ] Test application launch
- [ ] Test event loop
- [ ] Test menu handling
- [ ] Test network initialization
- [ ] Test quit
- [ ] Commit changes

### Phase 5 Checklist
- [ ] Create `shared/classic_mac/discovery_interface.h`
- [ ] Create `shared/classic_mac/discovery_interface.c`
- [ ] Rename/refactor MacTCP discovery
- [ ] Rename/refactor OpenTransport discovery
- [ ] Update callers
- [ ] Update Makefiles
- [ ] Build both versions
- [ ] Test peer discovery
- [ ] Test broadcast/response
- [ ] Test cross-platform discovery
- [ ] Commit changes

### Phase 6 Checklist
- [ ] Create `shared/classic_mac/messaging_interface.h`
- [ ] Create `shared/classic_mac/messaging_interface.c`
- [ ] Rename/refactor MacTCP messaging
- [ ] Rename/refactor OpenTransport messaging
- [ ] Update dialog.c to use interface
- [ ] Update Makefiles
- [ ] Build both versions
- [ ] Test direct messaging
- [ ] Test broadcast messaging
- [ ] Test cross-platform messaging
- [ ] Test message queuing
- [ ] Commit changes

---

## Appendix: Technical Details

### A. File Size Breakdown

```
MacTCP Build (classic_mac_mactcp/ - renamed from classic_mac/):
├── UI Layer (1,230 lines)
│   ├── dialog.c/h (316 lines)
│   ├── dialog_input.c/h (180 lines)
│   ├── dialog_messages.c/h (333 lines)
│   ├── dialog_peerlist.c/h (291 lines)
│   ├── peer.c/h (55 lines)
│   └── logging.c/h (41 lines)
├── Network Layer (2,562 lines)
│   ├── mactcp_impl.c/h (1,440 lines)
│   ├── network_abstraction.c/h (280 lines)
│   ├── tcp_state_handlers.c/h (226 lines)
│   ├── network_init.c/h (234 lines)
│   └── DNR.c (382 lines)
├── Application Logic (1,830 lines)
│   ├── main.c (446 lines)
│   ├── discovery.c/h (551 lines)
│   └── messaging.c/h (833 lines)
└── TOTAL: 5,608 lines

OpenTransport Build (classic_mac_ot/):
├── UI Layer (1,230 lines)
│   ├── dialog.c/h (254 lines)
│   ├── dialog_input.c/h (180 lines)
│   ├── dialog_messages.c/h (333 lines)
│   ├── dialog_peerlist.c/h (291 lines)
│   ├── peer.c/h (55 lines)
│   └── logging.c/h (41 lines)
├── Network Layer (891 lines)
│   └── opentransport_impl.c/h (891 lines)
├── Application Logic (842 lines)
│   ├── main.c (523 lines)
│   ├── discovery.c/h (147 lines)
│   └── messaging.c/h (172 lines)
└── TOTAL: 2,887 lines

Shared Code (shared/):
├── protocol.c/h (3,504 lines)
├── discovery.c/h (3,575 lines)
├── messaging.c/h (2,158 lines)
├── peer.c/h (4,217 lines)
├── logging.c/h (6,986 lines)
└── time_utils.c/h (1,853 lines)
└── TOTAL: 1,014 lines

GRAND TOTAL: 8,495 lines (MacTCP + OT + Shared)
```

### B. Makefile Structure

Current Makefiles use similar patterns:

**Makefile.retro68.mactcp**:
```makefile
MAC_DIR = classic_mac_mactcp
SHARED_DIR = shared
MAC_C_FILES = $(wildcard $(MAC_DIR)/*.c)
SHARED_C_FILES = $(wildcard $(SHARED_DIR)/*.c)
CFLAGS_MAC = -D__MACOS__ -Iclassic_mac_mactcp -Ishared
```

**Makefile.retro68.ot**:
```makefile
MAC_OT_DIR = classic_mac_ot
SHARED_DIR = shared
MAC_OT_C_FILES = $(wildcard $(MAC_OT_DIR)/*.c)
SHARED_C_FILES = $(wildcard $(SHARED_DIR)/*.c)
CFLAGS_MAC = -D__MACOS__ -DUSE_OPENTRANSPORT -Iclassic_mac_ot -Ishared
```

After Phase 1-2, add:
```makefile
SHARED_CLASSIC_MAC_DIR = shared/classic_mac
SHARED_CLASSIC_MAC_UI_DIR = shared/classic_mac/ui
SHARED_CLASSIC_MAC_C_FILES = $(wildcard $(SHARED_CLASSIC_MAC_DIR)/*.c)
SHARED_CLASSIC_MAC_UI_C_FILES = $(wildcard $(SHARED_CLASSIC_MAC_UI_DIR)/*.c)
CFLAGS_MAC += -Ishared/classic_mac -Ishared/classic_mac/ui
```

### C. Compiler Defines

Use compiler defines to handle platform-specific code:

```c
#ifdef USE_OPENTRANSPORT
    // OpenTransport-specific code
#else
    // MacTCP-specific code
#endif
```

For dialog.c broadcast differences (in platform-specific files):
```c
// classic_mac_mactcp/dialog.c
void HandleSendButtonClick(void) {
    // Common code...

    // MacTCP: Check state, queue if busy
    TCPStreamState currentState = GetTCPSendStreamState();
    if (currentState != TCP_STATE_IDLE) {
        // Queue message...
    } else {
        // Send immediately...
    }

    // Common code...
}

// classic_mac_ot/dialog.c
void HandleSendButtonClick(void) {
    // Common code...

    // OpenTransport: Direct OT broadcast
    OSErr err = BroadcastMessage(inputCStr);

    // Common code...
}
```

---

## Document Metadata

- **Author**: Claude (Anthropic)
- **Created**: 2025-10-06
- **Project**: CSend P2P Messenger
- **Version**: 1.0
- **Status**: Planning Phase
- **Last Updated**: 2025-10-06
