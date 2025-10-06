# POSIX vs. Classic Mac Code Sharing Plan

## 1. Executive Summary

This document outlines a plan to further refactor the `csend` codebase to increase code sharing between the POSIX and Classic Mac builds. The primary focus is to consolidate the peer management wrappers, which are currently duplicated with minor differences.

The analysis shows that the peer management code in `posix/peer.c` and `shared/classic_mac/peer_mac.c` is nearly identical. The only significant difference is the use of `pthread_mutex` in the POSIX version for thread safety, which is absent in the single-threaded Classic Mac version.

This plan proposes creating a single, unified peer management wrapper that uses a platform-abstracted locking mechanism. This will eliminate redundant code and improve maintainability.

## 2. Analysis of Code Duplication

### 2.1. Peer Management (`peer.c` vs. `peer_mac.c`)

- **`shared/peer.c`**: Contains the core, platform-agnostic peer list management algorithms (`peer_shared_*` functions). This is well-designed.
- **`posix/peer.c`**: A thin wrapper around `peer_shared_*` functions that adds `pthread_mutex` locking to ensure thread safety in the multi-threaded POSIX environment.
- **`shared/classic_mac/peer_mac.c`**: A thin wrapper around the same `peer_shared_*` functions, but without any locking, as the Classic Mac environment is single-threaded.

**Conclusion**: The logic is identical, with the only variable being the locking mechanism. This is a prime candidate for consolidation.

### 2.2. Logging (`logging.c` vs. `logging_mac.c`)

- **`shared/logging.c`**: Provides a flexible logging framework using a `platform_logging_callbacks_t` struct to abstract platform-specific actions (getting a timestamp, displaying the log message).
- **`posix/logging.c`**: Implements the callbacks for POSIX (using `printf` and standard time functions).
- **`shared/classic_mac/logging_mac.c`**: Implements the callbacks for Classic Mac (using `AppendToMessagesTE` and Mac-specific time functions).

**Conclusion**: The logging system is already optimally refactored. The core logic is shared, and the platform-specific implementations are cleanly separated via callbacks. No further action is needed here.

### 2.3. Discovery and Messaging

- The discovery and messaging systems follow a similar, well-designed pattern.
- Core, platform-agnostic logic resides in `shared/discovery.c` and `shared/messaging.c`.
- Platform-specific implementations (POSIX sockets, MacTCP async calls, OpenTransport events) are in their respective directories (`posix/`, `classic_mac_mactcp/`, `classic_mac_ot/`).

**Conclusion**: The current architecture for discovery and messaging is sound and does not require further consolidation at this time.

## 3. Proposed Refactoring Plan: Unifying Peer Management

The goal is to create a single peer management wrapper that can be used by both POSIX and Classic Mac builds, abstracting the locking mechanism.

### Phase 1: Implement Locking Abstraction (Low Risk)

**Goal**: Create a simple locking abstraction and a unified peer wrapper that uses it.

**Steps**:

1.  **Create `shared/platform_sync.h`**: This new header will define a platform-agnostic mutex structure and functions.

    ```c
    // shared/platform_sync.h
    #ifndef PLATFORM_SYNC_H
    #define PLATFORM_SYNC_H

    #ifdef __MACOS__
    // Classic Mac is single-threaded, so locks are no-ops.
    typedef struct { int dummy; } platform_mutex_t;
    #define platform_mutex_init(m) do {} while(0)
    #define platform_mutex_lock(m) do {} while(0)
    #define platform_mutex_unlock(m) do {} while(0)
    #define platform_mutex_destroy(m) do {} while(0)
    #else
    // POSIX uses pthreads.
    #include <pthread.h>
    typedef pthread_mutex_t platform_mutex_t;
    #define platform_mutex_init(m) pthread_mutex_init(m, NULL)
    #define platform_mutex_lock(m) pthread_mutex_lock(m)
    #define platform_mutex_unlock(m) pthread_mutex_unlock(m)
    #define platform_mutex_destroy(m) pthread_mutex_destroy(m)
    #endif

    #endif // PLATFORM_SYNC_H
    ```

2.  **Create `shared/peer_wrapper.c` and `shared/peer_wrapper.h`**: This will be the new, unified peer management wrapper.

    -   It will manage a `peer_manager_t` instance and a `platform_mutex_t`.
    -   It will expose functions like `pw_init()`, `pw_add_or_update()`, `pw_prune_timed_out()`, etc.
    -   Each function will wrap the corresponding `peer_shared_*` call with `platform_mutex_lock()` and `platform_mutex_unlock()`.

3.  **Refactor POSIX Build**:
    -   Modify `posix/peer.c` and `posix/main.c` to use the new `peer_wrapper` functions.
    -   The `app_state_t` struct will no longer need to hold the `peer_manager` or the `peers_mutex`, as this will be managed internally by the wrapper.
    -   Remove the now-redundant `add_peer` and `prune_peers` functions from `posix/peer.c`.

4.  **Refactor Classic Mac Builds**:
    -   Modify `shared/classic_mac/peer_mac.c` and `shared/classic_mac/peer_mac.h` to be simple pass-throughs to the new `peer_wrapper` functions. This avoids extensive changes in the many files that include `peer_mac.h`.
    -   Alternatively, directly replace usages of `peer_mac.h` functions with `peer_wrapper.h` functions. The former is less intrusive.

5.  **Update Makefiles**:
    -   Add `shared/peer_wrapper.c` to the compilation list for all builds.
    -   Ensure `shared/platform_sync.h` is in the include path.

**Benefit**:
-   Eliminates ~150 lines of duplicated wrapper code.
-   Centralizes peer management logic.
-   Improves maintainability: bug fixes or changes to peer handling only need to be made in one place.

**Risk**: Low. The logic is a straightforward consolidation. The use of preprocessor directives for the locking mechanism is a standard and safe technique.

### 4. Testing Strategy

After implementing the refactoring, the following tests must be performed:

1.  **Build Verification**:
    -   Confirm that the POSIX build compiles cleanly (`make clean && make`).
    -   Confirm that both Classic Mac builds compile cleanly (`make -f Makefile.retro68.mactcp clean && make -f Makefile.retro68.mactcp` and `make -f Makefile.retro68.ot clean && make -f Makefile.retro68.ot`).

2.  **Functional Testing (Multi-Platform)**:
    -   Run `csend` on a POSIX machine, a MacTCP machine, and an OpenTransport machine simultaneously.
    -   **Peer Discovery**: Verify that all three instances discover each other and populate their peer lists correctly.
    -   **Peer Timeout**: Let the applications run and verify that inactive peers are correctly pruned from the list after the timeout period.
    -   **Messaging**: Send messages between all platforms to ensure that communication is unaffected.
    -   **Quit**: Verify that when one client quits, it is correctly marked as inactive on the other clients.

3.  **Stress Testing (POSIX)**:
    -   On the POSIX build, create a simple test script to simulate multiple threads rapidly adding or updating peers to ensure the `pthread_mutex` locking is working correctly and prevents race conditions.

By following this plan, we can improve the codebase by reducing duplication and centralizing the peer management logic, with minimal risk.
