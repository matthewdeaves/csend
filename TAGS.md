## [v1.2.0](https://github.com/matthewdeaves/csend/tree/v1.2.0)

Revolutionary architectural improvements for Classic Mac networking, introducing a network abstraction layer and dual-stream TCP architecture. This release fundamentally reimagines the networking stack to provide better reliability, maintainability, and future extensibility while eliminating the complex single-stream juggling of previous versions.

**Network Architecture Revolution (Classic Mac):**

*   **Network Abstraction Layer:**
    *   Introduced a comprehensive network abstraction layer (`network_abstraction.c`/`.h`) that decouples high-level networking code from MacTCP implementation details.
    *   All network operations now go through a unified function table interface (`NetworkOperations`), making the codebase network-implementation agnostic.
    *   Prepared infrastructure for future OpenTransport support without requiring changes to application logic.
    *   Implemented proper error translation and detailed error reporting through the abstraction layer.

*   **Dual-Stream TCP Architecture:**
    *   Completely redesigned TCP handling to use two independent streams: one dedicated to listening for incoming connections, another for outgoing connections.
    *   Eliminated the error-prone pattern of aborting/restarting the listen operation when sending messages.
    *   Each stream has its own receive buffer, ASR (Asynchronous Status Routine) handler, and state tracking.
    *   Listen stream continuously accepts incoming connections without interruption.
    *   Send stream handles outgoing connections independently, dramatically improving reliability.

*   **Advanced Message Queue System:**
    *   Implemented a proper message queue for handling multiple outgoing messages, particularly beneficial for broadcast operations.
    *   Queue automatically processes pending messages when the send stream becomes idle.
    *   Broadcast messages to multiple peers now queue gracefully instead of potentially failing due to stream conflicts.

**UDP Discovery Enhancements (Classic Mac):**

*   **Asynchronous UDP Operations:**
    *   Migrated UDP operations to use fully asynchronous receive and buffer return through the network abstraction layer.
    *   Implemented proper async operation tracking with dedicated handle management.
    *   Improved buffer management with automatic retry on buffer availability.
    *   Better handling of concurrent UDP operations without blocking.

**Stability and Reliability Improvements:**

*   **ASR Event Handling:**
    *   Separate ASR handlers for listen and send streams prevent event interference.
    *   Improved ASR event queueing and processing to handle rapid network events.
    *   Added periodic polling as a failsafe for data arrival detection.

*   **Connection State Management:**
    *   Implemented proper connection reset delays to ensure MacTCP has time to clean up resources.
    *   Better handling of connection termination scenarios (graceful close vs. abort).
    *   Improved error recovery with automatic state transitions on failures.

*   **Resource Management:**
    *   Proper allocation and cleanup of network endpoints through abstraction layer.
    *   Improved memory management for async operations.
    *   Better tracking of pending operations to prevent resource leaks.

**Code Architecture Improvements:**

*   **Modular Design:**
    *   Clear separation between network abstraction, implementation, and application logic.
    *   MacTCP-specific code isolated in `mactcp_impl.c`, making it easy to add alternative implementations.
    *   Reduced coupling between modules through well-defined interfaces.

*   **Enhanced Debugging:**
    *   More detailed logging throughout the network stack.
    *   Better error context preservation and reporting.
    *   Clearer state machine transitions visible in debug output.

This version represents a complete architectural overhaul of the Classic Mac networking stack, providing a robust foundation for reliable peer-to-peer communication while preparing for future network stack support. The dual-stream architecture eliminates the most significant source of networking errors in previous versions, while the abstraction layer ensures long-term maintainability and portability.

---

## [v0.1.1](https://github.com/matthewdeaves/csend/tree/v0.1.1)

Major stability enhancements for Classic Mac networking, focusing on MacTCP driver interaction and TCP connection management. This release resolves critical shutdown errors, improves TCP listening reliability, and integrates standard Apple Menu and Quit functionality for better system citizenship.

**User Interface & System Integration (Classic Mac):**

*   **Standard Menu Support:**
    *   Added a functional Apple Menu.
    *   Implemented a File menu with a Quit option (`Cmd+Q` shortcut).
*   **Graceful System Shutdown/Restart Handling:**
    *   The application now correctly handles system shutdown/restart Apple Events (`kAEQuitApplication`), ensuring proper network and resource cleanup occurs, identical to a user-initiated Quit.
*   **Bigger Window:**
    *   Made the fixed window larger to fill a 640x480 display.

**Networking Core (Classic Mac):**

*   **MacTCP Driver Lifecycle Correction:**
    *   Resolved persistent `-24 (closeErr)` during application shutdown by ensuring the MacTCP driver (`.IPP`) is no longer incorrectly closed by the application. The driver is now correctly left open for system-wide use, as per MacTCP documentation.
*   **TCP Re-Listen Stability (`duplicateSocketErr` Fix):**
    *   Addressed `-23007 (duplicateSocketErr / connectionExists)` when re-listening for incoming TCP connections.
    *   Implemented a `POST_ABORT_COOLDOWN` state and timer in the TCP state machine, allowing MacTCP sufficient time to release the port before a new `TCPPassiveOpenAsync` is initiated. This ensures reliable, continuous server functionality.
*   **UPP Management for TCP ASR:**
    *   Ensured the `TCPNotifyUPP` for the Asynchronous Notification Routine is created once during network initialization and correctly disposed of during cleanup, preventing potential resource leaks or errors.
*   **Robust TCP Send/Receive Cycle:**
    *   Improved logic for handling active TCP sends by correctly aborting pending passive listens and re-initiating them after the send operation completes, ensuring a smooth transition between listening and sending states.
*   **Buffer Sizing and Definitions:**
    *   Standardised string buffer sizes (`INET_ADDRSTRLEN`, username lengths) across modules, aligning with `common_defs.h` and ensuring correct null-terminator handling.
    *   Added a local definition for `kTCPDriverName` to resolve compilation issues.

This version represents a significant step forward in the robustness and correctness of the Classic Mac networking implementation, leading to a more stable user experience.

---

## [v0.1.0](https://github.com/matthewdeaves/csend/tree/v0.1.0)

This version deserves a point release number now. I've done several rounds of refactoring to better organise and structure of both code bases and the shared code.

## I. Major Refactoring: Shared Logging System

A new, more robust shared logging system has been implemented, replacing the previous separate logging mechanisms in `posix/` and `classic_mac/`.

### 1. New Shared Logging Files:

*   **`shared/logging.h`** (replaces `shared/logging_shared.h`)
    *   Defines `platform_logging_callbacks_t` struct with function pointers for `get_timestamp` and `display_debug_log`. This allows platform-specific implementations for how timestamps are generated and how debug messages are shown in the UI.
    *   Declares new public API:
        *   `void log_init(const char *log_file_name_suggestion, platform_logging_callbacks_t *callbacks);`
        *   `void log_shutdown(void);`
        *   `void log_debug(const char *format, ...);` (for detailed internal/debug logs)
        *   `void log_app_event(const char *format, ...);` (for general application events, potentially user-visible or important internal milestones)
    *   `set_debug_output_enabled()` and `is_debug_output_enabled()` remain.
    *   Includes `<stdarg.h>` and `<stddef.h>`.
*   **`shared/logging.c`** (replaces `shared/logging_shared.c`)
    *   Implements the new shared logging API.
    *   Manages a global log file (`g_log_file`).
    *   `log_init()`: Opens the log file (filename can be suggested, defaults based on platform) and stores platform callbacks. Writes a "Log Session Started" message.
    *   `log_shutdown()`: Writes a "Log Session Ended" message and closes the log file.
    *   `log_debug()`: Writes formatted messages prefixed with "[DEBUG]" to the log file. If debug output is enabled and a `display_debug_log` callback is provided, it calls the callback to show the message in the platform's UI.
    *   `log_app_event()`: Writes formatted messages to the log file (without a "[DEBUG]" prefix). These are generally not displayed in the UI via the callback, intended more for persistent event tracking.
    *   Uses a `fallback_get_timestamp()` if no platform callback is provided.
    *   Uses `vsprintf` for `__MACOS__` and `vsnprintf` otherwise for message formatting within the logging functions to prevent buffer overflows.

### 2. POSIX Logging Adaptation (`posix/logging.c`, `posix/logging.h`):

*   **`posix/logging.h`**:
    *   Removed old `log_message()` and `display_user_message()` declarations.
    *   Added declarations for `posix_platform_get_timestamp()` and `posix_platform_display_debug_log()`.
*   **`posix/logging.c`**:
    *   Removed old logging implementations.
    *   Implements `posix_platform_get_timestamp()` using `time()`, `localtime()`, and `strftime()`.
    *   Implements `posix_platform_display_debug_log()` using `printf()` to console.
*   **`posix/main.c`**: Calls `log_init()` with these POSIX-specific callbacks and `log_shutdown()` on exit.
*   **All other POSIX files**: Replaced calls to old `log_message()` with `log_debug()` or `log_app_event()`. Replaced `display_user_message()` with `terminal_display_app_message()` (from `ui_terminal.c`) which itself now also calls `log_app_event()`.

### 3. Classic Mac Logging Adaptation (`classic_mac/logging.c`, `classic_mac/logging.h`):

*   **`classic_mac/logging.h`**:
    *   Removed old logging function declarations (`InitLogFile`, `CloseLogFile`, `log_message`, etc.) and globals (`kLogFileName`, `gLogFile`).
    *   Added declarations for `classic_mac_platform_get_timestamp()` and `classic_mac_platform_display_debug_log()`.
*   **`classic_mac/logging.c`**:
    *   Removed old logging implementations.
    *   Implements `classic_mac_platform_get_timestamp()` using `GetTime()` and `sprintf()`.
    *   Implements `classic_mac_platform_display_debug_log()` which appends the formatted message to the `gMessagesTE` TextEdit field in the dialog. It uses a flag `g_classic_mac_logging_to_te_in_progress` to prevent recursion.
*   **`classic_mac/main.c`**: Calls `log_init()` with these Classic Mac specific callbacks and `log_shutdown()` on exit.
*   **All other Classic Mac files**: Replaced calls to old `log_message()` and `log_to_file_only()` with `log_debug()` or `log_app_event()`. Direct UI messages are now typically handled by `AppendToMessagesTE` after an appropriate `log_app_event` or `log_debug` call.

## II. File Renaming and Reorganization

### 1. Shared Directory:

*   `discovery_logic.h` -> `discovery.h`
*   `discovery_logic.c` -> `discovery.c`
*   `logging_shared.h` -> `logging.h` (covered above)
*   `logging_shared.c` -> `logging.c` (covered above)
*   `messaging_logic.h` -> `messaging.h`
*   `messaging_logic.c` -> `messaging.c`
*   `peer_shared.h` -> `peer.h`
*   `peer_shared.c` -> `peer.c`

### 2. Classic Mac Directory:

*   `discovery.h` -> `mactcp_discovery.h`
*   `discovery.c` -> `mactcp_discovery.c`
*   `network.h` -> `mactcp_network.h`
*   `network.c` -> `mactcp_network.c`
*   `peer_mac.h` -> `peer.h`
*   `peer_mac.c` -> `peer.c`
*   `tcp.h` -> `mactcp_messaging.h`
*   `tcp.c` -> `mactcp_messaging.c`

### 3. POSIX Directory:

*   **`posix/main.c`**: New file. The `main()` function was moved here from `posix/peer.c`.
*   **`posix/peer.c`**: No longer contains `main()`.

## III. Changes in `shared/` Files (excluding logging)

### 1. `shared/discovery.c` (formerly `discovery_logic.c`):

*   Includes new shared `logging.h`.
*   All `log_message()` calls changed to `log_debug()`.

### 2. `shared/messaging.c` (formerly `messaging_logic.c`):

*   Includes new shared `logging.h`.
*   All `log_message()` calls changed to `log_debug()`.

### 3. `shared/peer.c` (formerly `peer_shared.c`):

*   Includes new shared `logging.h`.
*   All `log_message()` calls changed to `log_debug()`.

### 4. `shared/protocol.h`:

*   Introduced `csend_uint32_t` typedef: `UInt32` for `__MACOS__`, `uint32_t` otherwise. This is used for the magic number in message protocol.

### 5. `shared/protocol.c`:

*   Includes new shared `logging.h`.
*   All `log_message()` calls changed to `log_debug()`.
*   Uses `csend_uint32_t` for magic number variables.
*   Removed `my_htonl` and `my_ntohl` macros.
*   For `__MACOS__`, new static inline identity functions `csend_plat_htonl` and `csend_plat_ntohl` are defined and then `#define htonl(x) csend_plat_htonl(x)` etc. are used. For non-Mac, standard `htonl/ntohl` from `<arpa/inet.h>` are used. This ensures the magic number is explicitly processed for network byte order.
*   **Potential Issue:** `snprintf` calls in `format_message` are not conditionally compiled for Classic Mac, which might lack a standard `snprintf`. This was also an issue in the old version.

## IV. Changes in `posix/` Files

### 1. `posix/main.c` (New):

*   Contains the `main` function moved from `posix/peer.c`.
*   Initialises and shuts down the new shared logging system with POSIX-specific callbacks.
*   Uses `log_app_event()` for high-level application status messages.
*   Uses `terminal_display_app_message()` for messages directly visible to the user on the console.
*   Improved thread cancellation and joining logic during startup failure or shutdown.

### 2. `posix/peer.c`:

*   `main()` function removed.
*   `cleanup_app_state()` now sets `g_state = NULL;`.

### 3. `posix/discovery.c`, `posix/messaging.c`, `posix/signal_handler.c`:

*   Updated to use the new shared logging functions (`log_debug`, `log_app_event`).
*   `posix/messaging.c`: `posix_tcp_display_text_message` now calls both `log_app_event` (for file log) and `terminal_display_app_message` (for console UI).

### 4. `posix/ui_terminal.h`:

*   Added `terminal_display_app_message(const char *format, ...)` declaration.

### 5. `posix/ui_terminal.c`:

*   Implemented `terminal_display_app_message()` which prepends a timestamp to messages printed to the console.
*   Replaced most `display_user_message()` calls with `terminal_display_app_message()` and often added corresponding `log_app_event()` calls for file logging.
*   Updated to use new shared logging functions (`log_debug`).

## V. Changes in `classic_mac/` Files

### 1. General:

*   All files updated to use the new shared logging functions (`log_debug`, `log_app_event`).
*   Includes for renamed headers (e.g., `mactcp_network.h` instead of `network.h`).

### 2. `classic_mac/main.c`:

*   Initialises and shuts down the new shared logging system with Classic Mac specific callbacks.
*   The shutdown sequence for sending QUIT messages is now directly in `main.c`:
    *   It iterates through active peers.
    *   Calls the generalised `MacTCP_SendMessageSync()` with `MSG_QUIT`.
    *   Adds a `kQuitMessageDelayTicks` delay between sending QUIT messages to different peers.
    *   Logs and displays a summary of QUIT messages sent.
*   `IdleInputTE()` is now called in the main event loop.
*   `mouseDown` handling for `gMessagesScrollBar` (thumb drag): Now calls `ScrollMessagesTEToValue()` for cleaner scroll logic.
*   `DialogSelect` handling for `kBroadcastCheckbox`: Calls `DialogPeerList_DeselectAll()` when broadcast is checked.
*   `HandleInputTEKeyDown()` is called for `keyDown` and `autoKey` events.

### 3. `classic_mac/dialog.c`:

*   `HandleSendButtonClick()` updated to use the new `MacTCP_SendMessageSync()` signature, passing `MSG_TEXT`, `gMyUsername`, and `gMyLocalIPStr`.

### 4. `classic_mac/dialog_input.h` & `classic_mac/dialog_input.c`:

*   Added `IdleInputTE()` function (calls `TEIdle()`).
*   Added `HandleInputTEKeyDown()` function (calls `TEKey()` if appropriate).

### 5. `classic_mac/dialog_messages.h` & `classic_mac/dialog_messages.c`:

*   Added `ScrollMessagesTEToValue(short newScrollValue)` to directly scroll the TE to a specific line value.
*   `MyScrollAction()` now explicitly ignores `kControlIndicatorPart` (thumb drags), as this is handled in `main.c`.
*   `AppendToMessagesTE()`: When auto-scrolling, now uses `SetControlValue` and `ScrollMessagesTEToValue`.
*   `HandleMessagesTEUpdate()`: Added `EraseRect(&(**gMessagesTE).viewRect)` before `TEUpdate` to prevent visual artifacts.

### 6. `classic_mac/dialog_peerlist.h` & `classic_mac/dialog_peerlist.c`:

*   `GetSelectedPeerInfo()` renamed to `DialogPeerList_GetSelectedPeer()`.
*   Added `DialogPeerList_DeselectAll()` to deselect any item in the list and update the view.
*   `CleanupPeerListControl()` now resets `gLastSelectedCell.v = -1;`.

### 7. `classic_mac/mactcp_messaging.h` (formerly `tcp.h`):

*   `TCP_SendTextMessageSync()` renamed to `MacTCP_SendMessageSync()`.
*   Signature of `MacTCP_SendMessageSync()` changed to:
    `OSErr MacTCP_SendMessageSync(const char *peerIPStr, const char *message_content, const char *msg_type, const char *local_username, const char *local_ip_str, GiveTimePtr giveTime);`
    This makes it more general for sending different message types.

### 8. `classic_mac/mactcp_messaging.c` (formerly `tcp.c`):

*   `TCP_SendTextMessageSync()` renamed and reimplemented as `MacTCP_SendMessageSync()` with the new signature. It now handles formatting the message using `format_message()` with the provided type, username, IP, and content.
*   `TCP_SendQuitMessagesSync()` function was removed. Quit messages are now sent by `classic_mac/main.c` calling `MacTCP_SendMessageSync` with `MSG_QUIT`.
*   `MacTCP_SendMessageSync` has specific handling for `kConnectionExistsErr` when sending `MSG_QUIT`, treating it as a non-fatal error in that context.

### 9. `classic_mac/peer.h` (formerly `peer_mac.h`):

*   Removed `GetSelectedPeerInfo()` declaration (functionality moved to `dialog_peerlist.c/h`).

### 10. `classic_mac/peer.c` (formerly `peer_mac.c`):

*   Removed `GetSelectedPeerInfo()` implementation.
*   `GetPeerByIndex()`: Changed `active_index <= 0` check to `active_index < 0`. This means an index of `0` is now considered valid for the first active peer.

## VI. Other Minor Changes & Observations

*   **`shared/protocol.h` `csend_uint32_t`**: This is a good step for explicit type sizing.
*   **Error Handling in `MacTCP_SendMessageSync`**: Specific handling for `kConnectionExistsErr` when sending `MSG_QUIT` is a nuanced improvement.
*   **Classic Mac UI Updates**: Several small improvements to UI responsiveness and correctness, like `EraseRect` in `HandleMessagesTEUpdate` and better scrollbar handling.
*   **Consistency**: Renaming of files and functions (e.g. `DialogPeerList_GetSelectedPeer`) improves consistency.

---

## [v0.0.12](https://github.com/matthewdeaves/csend/tree/v0.0.12)

This release focuses on significant GUI bug fixes and usability enhancements for the Classic Macintosh client, addressing key interaction issues identified in v0.0.11. The core networking and shared logic remain stable. We have reached feature parity with the POSIX version.

1.  **Classic Mac GUI - Checkbox Functionality Restored:**
    *   **Previous State (v0.0.11):** The "Show Debug" and "Broadcast" checkboxes would log state changes internally but failed to update their visual appearance (i.e., the 'X' would not appear or disappear correctly).
    *   **v0.0.12 Fix:**
        *   Modified the `DialogSelect` event handling in `classic_mac/main.c`.
        *   When a checkbox item is clicked, `SetControlValue` is now explicitly called to toggle the control's stored value *before* its new state is read for application logic.
        *   `InvalRect` is then used on the checkbox's item rectangle to ensure the Dialog Manager correctly redraws the control with its updated visual state during the subsequent update event.
    *   **Benefit:** Checkboxes now function as expected, providing clear visual feedback to the user and enabling reliable use of the "Show Debug" and "Broadcast" features.

2.  **Classic Mac GUI - Input Text Field Visuals Corrected:**
    *   **Previous State (v0.0.11):**
        *   The border around the message input TextEdit field was not always visible immediately when the dialog appeared.
        *   After sending a message, the input field's text content was cleared internally, but the old text often remained visually on screen until further interaction.
    *   **v0.0.12 Fixes:**
        *   **Initial Border:** Ensured the input field's border is drawn reliably upon dialog initialization by adding an explicit call to `UpdateDialogControls()` at the end of `InitDialog` in `classic_mac/dialog.c`. This forces an immediate draw of user items, including the input field's frame.
        *   **Text Clearing:** Modified `ClearInputText` in `classic_mac/dialog_input.c` to call `HandleInputTEUpdate` after clearing the TextEdit content. `HandleInputTEUpdate` now explicitly calls `EraseRect` on the TextEdit field's view rectangle before `TEUpdate`, ensuring the old text is visually removed.
        *   The input field continues to use an inset TextEdit view rectangle within a framed user item for robust border drawing.
    *   **Benefit:** The message input field now has a consistent border from the moment the dialog appears and clears its visual content correctly after a message is sent, improving the user experience.

3.  **Classic Mac GUI - Peer List Selection Robustness:**
    *   **Previous State (v0.0.11):** Selecting a peer in the list was unreliable; the selection highlight could be lost, and sending messages to the selected peer often failed.
    *   **v0.0.12 Fix:**
        *   Refined `HandlePeerListClick` in `classic_mac/dialog_peerlist.c`. It now uses `LLastClick` to identify the cell targeted by the user's click and then `LGetSelect(false, ...)` to definitively confirm if that specific cell is indeed selected by the List Manager.
        *   The internal tracking variable `gLastSelectedCell` is updated based on this confirmed selection.
        *   `UpdatePeerDisplayList` was enhanced to better preserve and re-apply selections based on the underlying peer data when the list is rebuilt.
    *   **Benefit:** Peer selection is now stable and reliable. Users can confidently select a peer, and the selection persists visually and functionally for sending messages.

4.  **Classic Mac GUI - Messages Scrollbar Stability:**
    *   **Previous State (v0.0.11 behavior after initial fixes):** A Type 1 Address Error (crash) occurred when dragging the thumb of the messages scrollbar.
    *   **v0.0.12 Fix:**
        *   Restructured the `mouseDown` event handling for the scrollbar in `classic_mac/main.c` to align with a previously stable approach.
        *   Thumb drags are now handled directly: `TrackControl` is called with a `nil` action proc, and upon its return, the scrollbar's new value is immediately used to calculate and apply the text scrolling via `ScrollMessagesTE`.
        *   Clicks on scrollbar arrows or page regions continue to use the `MyScrollAction` procedure.
    *   **Benefit:** The messages scrollbar is now stable and functions correctly for all interaction types (arrows, page regions, and thumb dragging).

This version significantly polishes the Classic Macintosh client's user interface, making it more reliable and intuitive to use for its core chat functionalities.

---

## [v0.0.11](https://github.com/matthewdeaves/csend/tree/v0.0.11)

This version marks a major milestone, for the Classic Macintosh client, by fully implementing TCP message handling and integrating shared discovery and messaging logic. The Classic Mac client has the underpinnings of a functional peer-to-peer chat application capable of discovering, sending to, and receiving messages from other peers.

1.  **Shared Discovery Logic Implementation & Integration:**
    *   **v0.0.10 (Conceptual):** Classic Mac only sent UDP discovery broadcasts. Receiving and processing discovery packets was a TODO.
    *   **v0.0.11:**
        *   Introduces `shared/discovery_logic.c` and `shared/discovery_logic.h`. This C file contains the platform-independent logic for parsing incoming UDP packets (`MSG_DISCOVERY`, `MSG_DISCOVERY_RESPONSE`). It uses a `discovery_platform_callbacks_t` struct to allow platform-specific actions (sending responses, adding/updating peers, notifying UI).
        *   **Classic Mac (`classic_mac/discovery.c`):**
            *   Now implements asynchronous UDP receive polling (`StartAsyncUDPRead`, `PollUDPListener`, `ReturnUDPBufferAsync`).
            *   When a UDP packet is received, it's passed to `discovery_logic_process_packet`.
            *   Implements the Mac-specific callbacks:
                *   `mac_send_discovery_response`: Uses `SendDiscoveryResponseSync` to send a `MSG_DISCOVERY_RESPONSE`.
                *   `mac_add_or_update_peer`: Calls the existing `AddOrUpdatePeer` (which uses `peer_shared`).
                *   `mac_notify_peer_list_updated`: Calls `UpdatePeerDisplayList` to refresh the GUI.
        *   **POSIX (`posix/discovery.c`):**
            *   Refactored to use the new `shared/discovery_logic.c`.
            *   The `discovery_thread` now calls `discovery_logic_process_packet`.
            *   Implements POSIX-specific callbacks:
                *   `posix_send_discovery_response`: Sends UDP response using `sendto`.
                *   `posix_add_or_update_peer`: Calls `add_peer` (which uses `peer_shared`).
                *   `posix_notify_peer_list_updated`: Currently a no-op log message for the terminal UI.
    *   **Benefit:** Peer discovery (both sending and receiving/processing) is now consistently handled across platforms using shared core logic, with platform-specifics neatly isolated in callbacks. Classic Mac can now fully participate in discovery.

2.  **Shared TCP Messaging Logic & Full Classic Mac TCP Implementation:**
    *   **v0.0.10 (Conceptual):** Classic Mac had placeholder send logic (`DoSendAction`) but no actual TCP send/receive. POSIX had its own TCP handling.
    *   **v0.0.11:**
        *   Introduces `shared/messaging_logic.c` and `shared/messaging_logic.h`. This C file contains platform-independent logic for handling parsed TCP messages (`MSG_TEXT`, `MSG_QUIT`). It uses a `tcp_platform_callbacks_t` struct for platform-specific actions.
        *   **Classic Mac (`classic_mac/tcp.c`):**
            *   **Major rewrite and feature completion.** This file now implements a single-stream TCP management strategy for Classic Mac.
            *   **Listening (Passive Open):** Uses `TCPPassiveOpen` asynchronously (polled in `PollTCP`) to listen for incoming connections.
            *   **Receiving:** When a connection is established, `TCPRcv` (polled synchronously) reads data. The received data is parsed via `shared/protocol.c` and then passed to `handle_received_tcp_message` from `shared/messaging_logic.c`.
            *   **Sending (Active Open):** `TCP_SendTextMessageSync` and `TCP_SendQuitMessagesSync` are now fully implemented.
                *   If a passive listen is active, it's `TCPAbort`ed.
                *   `TCPActiveOpen` (polled synchronously) connects to the peer.
                *   `TCPSend` (polled synchronously) sends the formatted message.
                *   `TCPAbort` closes the connection.
                *   The system then returns to passive listening.
            *   Implements the Mac-specific `tcp_platform_callbacks_t`:
                *   `mac_tcp_add_or_update_peer`: Updates peer list via `AddOrUpdatePeer` & refreshes GUI.
                *   `mac_tcp_display_text_message`: Appends the message to the `gMessagesTE` in the dialog.
                *   `mac_tcp_mark_peer_inactive`: Marks the peer as inactive using `MarkPeerInactive` & refreshes GUI.
        *   **POSIX (`posix/messaging.c`):**
            *   Refactored to use the new `shared/messaging_logic.c`.
            *   The `listener_thread` now calls `handle_received_tcp_message` after parsing an incoming TCP message.
            *   Implements POSIX-specific callbacks (e.g., `posix_tcp_display_text_message` logs to console).
    *   **Benefit:** TCP message handling is now shared. Classic Mac can send and receive TEXT and QUIT messages, making it a fully interactive chat client. The complex MacTCP stream management is encapsulated.

3.  **Classic Mac GUI and Integration Enhancements:**
    *   **`classic_mac/dialog.c` (`HandleSendButtonClick`):**
        *   Now correctly calls `TCP_SendTextMessageSync` to send messages to selected peers.
        *   The "Broadcast" checkbox affects local display but currently *does not* implement network broadcast for TEXT messages (which would be iterative unicast TCP anyway).
    *   **`classic_mac/dialog_peerlist.c`:** `UpdatePeerDisplayList` and `GetSelectedPeerInfo` are more robust in handling selections and updating the UI when peer list changes (e.g., due to discovery or QUIT messages).
    *   **`classic_mac/main.c`:**
        *   Now calls `PollTCP` in the main event loop to handle incoming TCP connections and process received data.
        *   On quit (`gDone = true`), it now calls `TCP_SendQuitMessagesSync` to notify other peers.
    *   **`classic_mac/peer_mac.c`:** `MarkPeerInactive` correctly interfaces with `shared/peer_shared.c` to set the peer's active flag to 0.

4.  **Protocol Definition `MSG_MAGIC_NUMBER`:**
    *   **v0.0.11:** A `MSG_MAGIC_NUMBER` (0x43534443UL, "CSDC") is added to `shared/protocol.h` and used in `shared/protocol.c` to prefix all messages.
    *   `format_message` now prepends this magic number (network byte order).
    *   `parse_message` now first checks for and validates this magic number before attempting to parse the rest of the message.
    *   **Benefit:** Provides a simple way to quickly identify and discard packets that are not part of this application's protocol, improving robustness, especially for UDP where unrelated broadcasts might be received.

5.  **Build System & Makefile Updates:**
    *   **`Makefile.retro68` (Classic Mac):**
        *   Object file lists and dependencies adjusted.
    *   **`Makefile` (POSIX):**
        *   Object file lists and dependencies adjusted.

v0.0.11 brings the Classic Macintosh version closer to feature-parity with the POSIX version in terms of core communication capabilities. Both platforms now utilise shared logic for discovery processing and TCP message handling. The Classic Mac version now fully supports sending and receiving chat messages and QUIT notifications. Key remaining differences lie in the UI (GUI vs. CLI) and threading models.

---

## [v0.0.10](https://github.com/matthewdeaves/csend/tree/v0.0.10)

1.  **Major Refactoring: Shared Peer Management Logic:**
    *   **v0.0.9:** Peer management logic (like defining `peer_t`, adding/updating peers, checking timeouts) was implemented within the platform-specific `posix/peer.c` and absent in `classic_mac`.
    *   **v0.0.10:** Introduces `shared/peer_shared.c` and `shared/peer_shared.h`. This centralises the core, platform-independent logic for managing the peer list:
        *   The `peer_t` structure is now defined in `shared/common_defs.h`.
        *   Functions like `peer_shared_init_list`, `peer_shared_find_by_ip`, `peer_shared_find_empty_slot`, `peer_shared_update_entry`, `peer_shared_add_or_update`, and `peer_shared_prune_timed_out` handle the fundamental operations on a `peer_t` array.
        *   Platform-specific time handling (`TickCount` vs `time`) is managed within `peer_shared.c` using `#ifdef __MACOS__`.
    *   **Benefit:** This significantly reduces code duplication, ensures consistent peer handling logic across platforms, and makes future modifications easier.

2.  **Platform-Specific Peer Wrappers:**
    *   **v0.0.10:**
        *   **POSIX (`posix/peer.c`):** This file was refactored to become a *wrapper* around the shared logic. It still manages the `app_state_t` (which contains the `peers` array and the `peers_mutex`), but functions like `add_peer` and `prune_peers` now primarily handle locking/unlocking the mutex and then call the corresponding `peer_shared_...` functions.
        *   **Classic Mac (`classic_mac/peer_mac.c`, `classic_mac/peer_mac.h`):** These *new* files provide the Classic Mac interface to the shared peer logic. They define a global `gPeerList` array and implement functions (`InitPeerList`, `AddOrUpdatePeer`, `PruneTimedOutPeers`) that directly call the `peer_shared_...` functions on this global list. A new function `GetPeerByIndex` was added specifically for the Classic Mac UI to retrieve peer data based on its visible index in the list.
    *   **Benefit:** Maintains a clear separation between the core peer logic (shared) and how each platform integrates with it (global list vs. state struct with mutex).

3.  **Logging Refactoring:**
    *   **v0.0.9:** Logging was handled by `shared/utils.c` and `shared/utils.h`, likely printing only to `stdout` (suitable for POSIX but not Classic Mac GUI).
    *   **v0.0.10:**
        *   `shared/utils.c/h` were removed.
        *   Logging functionality was moved into platform-specific implementations:
            *   `posix/logging.c/h`: Provides `log_message` which prints timestamped messages to `stdout`.
            *   `classic_mac/logging.c/h`: Provides `log_message` (renamed from `LogToDialog`) which writes to a file (`csend_log.txt`) and, if the dialog is initialised, also appends the message to the dialog's message area (`gMessagesTE`).
    *   **Benefit:** Allows each platform to log messages in the most appropriate way (console for POSIX, file/dialog for Classic Mac) while maintaining a consistent function name (`log_message`).

4.  **Classic Mac UDP Discovery Implementation:**
    *   **v0.0.9:** UDP discovery logic was likely absent or incomplete in the Classic Mac build. The `classic_mac/network.c` handled general TCP/IP setup but not UDP broadcasts specifically.
    *   **v0.0.10:**
        *   Introduced `classic_mac/discovery.c` and `classic_mac/discovery.h`.
        *   These files implement UDP broadcast functionality specifically for Classic Mac using MacTCP `PBControl` calls (`udpCreate`, `udpWrite`, `udpRelease`).
        *   `InitUDPBroadcastEndpoint` sets up the UDP stream.
        *   `SendDiscoveryBroadcast` formats and sends the discovery message.
        *   `CheckSendBroadcast` handles the periodic sending based on `DISCOVERY_INTERVAL` and `TickCount`.
        *   `CleanupUDPBroadcastEndpoint` releases the UDP stream.
        *   The UDP-related logic was removed from `classic_mac/network.c`.
    *   **Benefit:** Implements the core discovery sending mechanism for the Classic Mac platform, separating it cleanly from general TCP/IP setup.

5.  **Classic Mac Integration Updates:**
    *   **`classic_mac/main.c`:** Updated to initialise the new peer list (`InitPeerList`), initialise the UDP endpoint (`InitUDPBroadcastEndpoint`), and call `CheckSendBroadcast` and `PruneTimedOutPeers` during the idle part of the event loop.
    *   **`classic_mac/dialog.c`:** `DoSendAction` was updated to integrate with the new peer management. It now includes logic (using the new `GetPeerByIndex`) to identify the target peer IP when sending a non-broadcast message (though the actual network send call is still a TODO). It also uses the global `gMyUsername` and `gMyLocalIPStr`. The logging function name was updated from `LogToDialog` to `log_message`.

6.  **Build System Updates (Makefiles):**
    *   **`Makefile` (POSIX):** Updated to reflect the change from `shared/utils.c` to `posix/logging.c`. Object file lists and dependencies were adjusted accordingly.
    *   **`Makefile.classicmac`:** Significantly updated to:
        *   Include the new Classic Mac source files (`discovery.c`, `peer_mac.c`) and the new shared file (`peer_shared.c`) in the compilation and linking steps.
        *   Remove the old shared utility object (`shared_utils.o`).
        *   Correctly specify include paths (`-Iclassic_mac`, `-Ishared`, `-I"$(CINCLUDES)"`, `-I"$(UNIVERSAL_CINCLUDES)"`).
        *   Add dynamic detection and checking for Retro68 include paths (`RIncludes`, `CIncludes`, `Universal/CIncludes`) relative to the compiler location, making the build more robust.
        *   Use the C++ linker driver (`CXX_MAC`) which is often necessary with Retro68 even for C projects.
        *   Remove the `-lRetroConsole` linker flag as the GUI app doesn't use the console output library.

v0.0.10 represents a significant step forward in code organization and feature implementation. The core peer management logic is now shared, reducing redundancy and improving consistency. Logging is handled appropriately for each platform. UDP discovery sending is now implemented for Classic Mac. The build system is more robust, especially for the Classic Mac target. The next features to build for the classic Mac build are:

1. On quit send a `MSG_QUIT` message to peers so they will remove the peer from their peer list
2. Support for receiving `MSG_DISCOVERY_RESPONSE` message and adding a peer to the peer list

---

## [v0.0.9](https://github.com/matthewdeaves/csend/tree/v0.0.9)

Further work on code sharing between POSIX and ANSI C builds through strategic refactoring to plain C, enabling better cross-platform compatibility while maintaining platform-specific optimizations where needed.

Core Refactoring

- Refactored peer management with new _update_peer_entry helper function
    - Centralised timestamp and username update logic
    - Reduced code duplication between updating existing peers and adding new ones
    - Ensured consistent handling of NULL or empty usernames
- Moved utility code to shared directory
    - Relocated utils.c and utils.h from posix/ to shared/
    - Created shared/common_defs.h for constants used by both platforms
    - Refactored format_message() to use dependency injection for local IP

Build System Improvements

- Enhanced Classic Mac build
    - Updated Makefile.classicmac to include shared source files
    - Added RetroConsole library linking for printf output
    - Changed linker from gcc to g++ to resolve C++ symbols
- Code organization
    - Moved application constants (MAX_PEERS, DISCOVERY_INTERVAL, PEER_TIMEOUT) to common header
    - Maintained protocol constants in protocol.h for better semantic cohesion

Development Tools

- Added code duplication detection
    - New cpd_check.sh script integrates PMD's Copy/Paste Detector
    - Automatically downloads and manages PMD dependencies
    - Helps identify refactoring opportunities across the codebase
- Enhanced Docker development environment
    - Added Docker context management for both Engine and Desktop environments
    - Implemented automatic detection of connectivity issues
    - Added dependency checks and installation prompts
    - Fixed context-related issues and improved error handling
    - Adjusted IP range for Docker containers to avoid conflicts

---

## [v0.0.8](https://github.com/matthewdeaves/csend/tree/v0.0.8)

Add Classic Mac GUI Shell and Build System

- Add initial Classic Mac GUI shell (`classic_mac/main.c`) including:
    - Toolbox initialization (QuickDraw, Fonts, Windows, Menus, TE, Dialogs).
    - Main event loop (`WaitNextEvent`).
    - Loading and displaying a basic dialog window (`GetNewDialog` from resource).
    - Basic event handling (mouse down for drag/close, update events).
- Add resource definitions (`classic_mac/csend.r`) for the dialog (`DLOG`/`DITL` ID 128).
- Add Classic Mac build system (`Makefile.classicmac`) using Retro68 toolchain:
    - Compiles `classic_mac/main.c` (shared code excluded for now).
    - Uses `Rez` to combine code, resources, and `Retro68APPL.r`.
    - Dynamically locates `RIncludes` based on compiler path.
    - Outputs `.APPL`, `.bin`, `.dsk` files to `build/classic_mac/`.
- Refactor POSIX `Makefile`:
    - Organise outputs into `build/posix/` (executable) and `build/obj/posix/` (objects).
    - Update `clean` target to remove entire `build/` directory.
- Update Docker configuration (`Dockerfile`, `docker-compose.yml`) to use the new POSIX executable path (`/app/build/posix/csend_posix`).

---

## [v0.0.7](https://github.com/matthewdeaves/csend/tree/v0.0.7)

Refactor: Structure project for cross-platform build

- Created posix/, classic_mac/, and shared/ directories.
- Moved existing POSIX source code into posix/.
- Moved protocol.[ch] into shared/ as first shared module.
- Updated #include paths in posix/ code for shared/protocol.h.
- Updated root Makefile to build POSIX target from new structure.
- Updated codemerge.sh to support platform selection (posix/classic/all).
- Updated Dockerfile and docker-compose.yml for new structure.

---

## [v0.0.6](https://github.com/matthewdeaves/csend/tree/v0.0.6)

A refactor to improve code organization and modularity

- Extract utility [functions into utils.c](https://github.com/matthewdeaves/csend/blob/c86282b7e76a075df6ef3887829dd9c1b0f4bef8/utils.c) for better reusability
- Create dedicated [signal_handler.c](https://github.com/matthewdeaves/csend/blob/c86282b7e76a075df6ef3887829dd9c1b0f4bef8/signal_handler.c) for cleaner signal management
- Move messaging code into [messaging.c](https://github.com/matthewdeaves/csend/blob/c86282b7e76a075df6ef3887829dd9c1b0f4bef8/messaging.c)
- Add proper header files for all major components and streamline all includes
- Very detailed comments throughout the code to aid in learning (yes, I print and read my code with a coffee)

---

## [v0.0.5](https://github.com/matthewdeaves/csend/tree/v0.0.5)

A refactor and implementation of some development tools. Refactoring includes:

* move code related to discovery into separate [discovery.h](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/discovery.h) and [disocvery.c](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/discovery.c) files

Implementing developer tooling includes:

* Adding a [Dockerfile](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/Dockerfile), [docker-compose.yml](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/docker-compose.yml) and [p2p.sh](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/p2p.sh) enabling 3 Docker containers to be fired up each running an instance of the compiled binary for testing on local machine 

### Running Docker

Exceute `./p2p.sh start` in a terminal window (not within VSCode terminal) and the script will auto launch 3 separate terminal windows attached to each container for CLI use.

Execute `./p2p.sh stop` in any terminal window to shut them down.

---

## [v0.0.4](https://github.com/matthewdeaves/csend/tree/v0.0.4)

A very simple refactor to:

* move code related to the terminal user interface into separate files with functionally the same as [v0.0.3](https://github.com/matthewdeaves/csend/tree/v0.0.3) but with terminal UI code moved to [ui_terminal.h](https://github.com/matthewdeaves/csend/blob/390e72b0b2020471c7348b23957101f81da2588b/ui_terminal.h) and  [ui_terminal.c](https://github.com/matthewdeaves/csend/blob/390e72b0b2020471c7348b23957101f81da2588b/ui_terminal.c)

Improved developer tooling with:

* a [Makefile](https://github.com/matthewdeaves/csend/blob/e52e6e47febbaba3fb970071b84d6c0a0341260a/Makefile)
* a [codemerge.sh](https://github.com/matthewdeaves/csend/blob/dc84874cfd4fe14f15c6adbc4dafaabbf3e8d120/codemerge.sh) tool with options to combine all .c files into a single text file which can be used with LLMs to inspect the code. I find `./codemerge.sh -s dependency` gives good results with an LLM by organising the code with header files first

Compile with:

```
make
```

Run with:

```
./p2p_chat
```

View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.4)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.4)

---

## [v0.0.3](https://github.com/matthewdeaves/csend/tree/v0.0.3)

A rewrite to move to a single binary capable of sending and receiving messages with support for:
* peer to peer discovery over UDP 
* direct and broadcast messaging over TCP
* messages encapsulated with a simple messaging protocol

This is achieved by:

* implementing a [discovery_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L321)
* implementing a [listener_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L224)
* implementing a [user_input_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.c#L172)
* Moving code from previous versions into [network.c](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c) with explicit functions to initialise TCP sockets to [listen](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L53) and [send](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L165)
* Implementing a method to [initialise UDP sockets](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L142) to [broadcast discovery](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L142) messages
* Implementing a [main method](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.c#L310) to start threads
* implementing a very simple structured [message protocol](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/protocol.c)
* each thead having access to a [`struct app_state_t`](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.h#L44) to store key information for the peer such as the list of peers and a method to aid in multithreaded access to the list

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc -o p2p_chat peer.c network.c protocol.c -lpthread
```

Run with:

```
./p2p_chat
```

### Key Learnings

#### Message Protocol

It really is a very simple format which I think in future can be improved to use a more structured format with a struct and introduction of magic numbers at the start of the data and a checksum to help a peer verifiy a connection on a socket has the intended data and was sent by a valid peer. The message format is:

```
// Message format: TYPE|SENDER|CONTENT
// Example: TEXT|username@192.168.1.5|Hello, world!

```
The [parse_message()](https://github.com/matthewdeaves/csend/blob/a4cea91a61c4f70d5ce5de417bf0d7a5a40cc184/protocol.c#L38) function attempts to parse incomming messages according to the above format, 

#### Threads
Each peer maintains an array of network peers within a struct called [app_state_t]() defined in peer.h. This list of peers needs to be modified in a threadsafe manner as a peer can be added to the list via the discovery and listener threads.

To achieve this, app_state_t hold a variable of [pthread_mutex_t]() type called `peers_mutex`. This is a special type which allows the calling of functions to obtain and release a lock on that variable. For a thread to obtain the lock (which blocks execution/waits if it is not available)`pthread_mutex_lock()` is called. To release the lock `pthread_mutex_unlock()` is called. This means you have to remember to make a thread obtain the lock, do your thing and then release the lock when you are done. Both functions take an argument type `pthread_mutex_t`. For example:

```
pthread_mutex_lock(&state->peers_mutex);

// code to do modify the list of peers

pthread_mutex_unlock(&state->peers_mutex);
```

View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.3)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.3)

---

## [v0.0.2](https://github.com/matthewdeaves/csend/tree/v0.0.2)

A minor improvement to the server implementation this time in that it will run continuously listening for connections until the process is terminated (and a graceful shutdown performed). This is achieved by adding:
* [setup](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L54) and [handling](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L24) of SIGINT (Ctrl+C) and SIGTERM signals
* [non-blocking accept](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L108) of connections with a [1 second timeout via select()](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L118). This allows the servier to periodically check if it should continue to run.
* A main server [loop](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L103) which ties the above points together
* A [tidy up before shutdown](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L29), closing the server sockets before program termination

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc client.c -o client
gcc server.c -o server
```

Run with:

```
./server
```

and in another terminal (run as many of these as you like or repeat whilst server is running):

```
./client
```
View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.2)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.2)

---

## [v0.0.1](https://github.com/matthewdeaves/csend/tree/v0.0.1)

This is a very simple client and server setup using sockets on the localhost. You start the [server](https://github.com/matthewdeaves/csend/blob/v0.0.1/server.c) and it will open a port, bind a socket and  listen for a connections. When a client connects it will read a message, send a response and then close connections and terminate. The call to wait for a connection is [blocking](https://github.com/matthewdeaves/csend/blob/a572ac3b9acbecea0316d38763c26d933245f092/server.c#L57) (as in the program execution pauses until a connection is made on the port).

The [client](https://github.com/matthewdeaves/csend/blob/v0.0.1/client.c) is equally simple, opening a socket, connecting to the localhost, sending a message to the server, reading a response and terminating.

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc client.c -o client
gcc server.c -o server
```

Run with:

```
./server
```

and in another terminal:

```
./client
```
View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.1)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.1)

---
