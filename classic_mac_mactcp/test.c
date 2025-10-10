/*
 * Classic Mac Test Implementation for P2P Messenger
 *
 * This module implements the automated test feature for the Classic Mac
 * platform, providing a bridge between the cross-platform test logic
 * and Classic Mac-specific messaging and timing functions.
 *
 * TEST ARCHITECTURE OVERVIEW:
 *
 * The test system uses a callback-based design to maintain platform
 * independence while allowing each platform to implement platform-specific
 * operations:
 *
 * 1. SHARED TEST LOGIC:
 *    - Cross-platform test sequence and validation
 *    - Message generation and timing coordination
 *    - Success/failure criteria evaluation
 *    - Test progress reporting and logging
 *
 * 2. PLATFORM-SPECIFIC CALLBACKS:
 *    - Message sending (broadcast and direct)
 *    - Peer discovery and enumeration
 *    - Timing delays and synchronization
 *    - Platform-specific error handling
 *
 * CLASSIC MAC IMPLEMENTATION STRATEGY:
 *
 * 1. CODE REUSE PRINCIPLE:
 *    The test callbacks deliberately reuse the same messaging functions
 *    that the UI uses for sending messages. This ensures:
 *    - Test validates actual application code paths
 *    - No duplicate/alternate implementations that could behave differently
 *    - Realistic testing of the complete messaging stack
 *    - Consistent error handling and state management
 *
 * 2. COOPERATIVE TIMING:
 *    Classic Mac's cooperative multitasking requires careful timing:
 *    - Use Delay() function for precise timing delays
 *    - Convert milliseconds to Mac ticks (60 Hz timer)
 *    - Yield CPU time during delays for system responsiveness
 *
 * 3. RESOURCE MANAGEMENT:
 *    - No dynamic allocation in test code (uses stack variables)
 *    - Reuses existing application state and peer management
 *    - Clean integration with existing logging system
 *
 * TEST VALIDATION:
 *
 * The test performs a complete message flow validation:
 * - Broadcasts test messages to all discovered peers
 * - Sends direct messages to individual peers
 * - Validates message delivery and peer responses
 * - Generates clear START/END markers for log analysis
 * - Reports success/failure statistics
 *
 * This provides confidence that the networking stack is functioning
 * correctly and can reliably exchange messages between peers.
 *
 * References:
 * - docs/PERFORM_TEST_IMPLEMENTATION_PLAN.md
 * - Inside Macintosh Volume IV: Timer Manager
 */

#include "test.h"
#include "../shared/test.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "../shared/protocol.h"
#include "messaging.h"
#include <Timer.h>  /* Classic Mac Timer Manager for Delay() function */

/*
 * CLASSIC MAC TIMING DELAY IMPLEMENTATION
 *
 * Implements precise timing delays using the Mac Timer Manager's Delay()
 * function. This is the standard method for implementing delays in
 * Classic Mac applications while maintaining system responsiveness.
 *
 * TIMING CONVERSION:
 * Classic Mac timing is based on "ticks" from the 60 Hz system timer:
 * - 1 tick = 1/60 second ≈ 16.67 milliseconds
 * - Formula: ticks = (milliseconds * 60) / 1000
 *
 * COOPERATIVE MULTITASKING:
 * The Delay() function automatically yields CPU time to other applications
 * and system processes during the delay period. This maintains system
 * responsiveness and follows Classic Mac cooperative multitasking principles.
 *
 * PRECISION:
 * Delays are accurate to within one tick (±16.67ms), which is sufficient
 * for test timing requirements. The finalTicks parameter receives the
 * actual ending tick count but is not used in this implementation.
 */
static void mac_delay_ms(int milliseconds, void *context)
{
    unsigned long finalTicks;  /* Receives actual end time (unused) */
    (void)context;            /* Context not needed for Classic Mac timing */

    /* Convert milliseconds to Mac ticks and delay */
    /* Note: Integer division provides sufficient accuracy for test delays */
    Delay((milliseconds * 60) / 1000, &finalTicks);
}

/*
 * TEST BROADCAST MESSAGE CALLBACK
 *
 * This function implements broadcast messaging for the test system by
 * deliberately reusing the same messaging code paths that the UI uses.
 * This ensures the test validates the actual application functionality
 * rather than testing separate code paths.
 *
 * DESIGN PRINCIPLE: CODE REUSE FOR REALISTIC TESTING
 * - Calls MacTCP_QueueMessage() - same function as UI send button
 * - Uses same peer enumeration as UI peer list
 * - Same error handling and logging as interactive operations
 * - Tests the complete messaging stack including queueing and state management
 *
 * BROADCAST ALGORITHM:
 * 1. Get current list of active peers
 * 2. Iterate through all peers
 * 3. Queue message for each peer using application messaging functions
 * 4. Track success/failure statistics
 * 5. Return overall result
 *
 * ERROR HANDLING:
 * - Individual peer failures are logged but don't stop the broadcast
 * - Overall failure only if no peers reachable or system error
 * - Matches the behavior of the UI broadcast functionality
 *
 * RETURN VALUES:
 * - 0: Success (all reachable peers received message)
 * - -1: Failure (system error or no peers could receive message)
 */
static int test_send_broadcast(const char *message, void *context)
{
    int i;                     /* Loop counter for peer iteration */
    int sent_count = 0;        /* Successfully queued messages */
    int failed_count = 0;      /* Failed message attempts */
    int total_active_peers;    /* Current number of discovered peers */
    OSErr sendErr;             /* MacTCP operation result */

    (void)context;             /* Context not used in Classic Mac implementation */

    /* Get current peer count - same function UI uses */
    total_active_peers = pw_get_active_peer_count();

    /*
     * PEER AVAILABILITY VALIDATION
     *
     * Check if any peers are available for testing. No peers is not
     * considered an error condition - it's a valid test scenario that
     * indicates the discovery system found no other instances.
     */
    if (total_active_peers == 0) {
        log_app_event("Test: No active peers to broadcast to");
        return 0;  /* Success - no peers to message is valid state */
    }

    /*
     * BROADCAST MESSAGE TO ALL PEERS
     *
     * Iterate through the peer list and send the test message to each peer
     * using the identical messaging infrastructure that the UI uses.
     * This provides realistic testing of the networking stack.
     */
    for (i = 0; i < total_active_peers; i++) {
        peer_t peer;  /* Peer information structure */

        /* Get peer information - same function UI peer list uses */
        pw_get_peer_by_index(i, &peer);

        /*
         * CRITICAL: Use SAME messaging function as UI
         *
         * This calls MacTCP_QueueMessage() which is the exact same function
         * that the UI send button calls. This ensures we're testing the
         * real application code paths, not alternate test-only paths.
         */
        sendErr = MacTCP_QueueMessage(peer.ip, message, MSG_TEXT);

        if (sendErr == noErr) {
            sent_count++;
        } else {
            failed_count++;
            log_app_event("Test broadcast failed for %s@%s: %d",
                         peer.username, peer.ip, sendErr);
        }
    }

    return (failed_count == 0) ? 0 : -1;
}

/*
 * TEST DIRECT MESSAGE CALLBACK
 *
 * Implements direct (peer-to-peer) messaging for the test system using
 * the same messaging infrastructure as the UI. This provides targeted
 * message testing to specific peers.
 *
 * DESIGN CONSISTENCY:
 * Uses identical call to MacTCP_QueueMessage() as the UI's direct message
 * functionality, ensuring test validates real application behavior.
 *
 * USAGE IN TEST SEQUENCE:
 * After broadcast messages, the test sends direct messages to individual
 * peers to validate both broadcast and unicast messaging work correctly.
 *
 * PARAMETERS:
 * - peer_ip: Target peer's IP address (string format)
 * - message: Test message content
 * - context: Platform context (unused on Classic Mac)
 *
 * RETURN VALUES:
 * - 0: Message queued successfully
 * - -1: Message queueing failed (network error, queue full, etc.)
 */
static int test_send_direct(const char *peer_ip, const char *message, void *context)
{
    (void)context;  /* Context not used in Classic Mac implementation */

    /*
     * CRITICAL: Use identical messaging function as UI
     *
     * This is the exact same call that the UI makes for direct messages,
     * ensuring the test validates the real application messaging behavior.
     */
    return (MacTCP_QueueMessage(peer_ip, message, MSG_TEXT) == noErr) ? 0 : -1;
}

/*
 * TEST PEER COUNT CALLBACK
 *
 * Returns the current number of active peers for test planning.
 * Uses the same peer counting function as the UI peer list display.
 *
 * This enables the shared test logic to determine how many peers
 * are available for testing and adjust the test sequence accordingly.
 */
static int test_get_peer_count(void *context)
{
    (void)context;  /* Context not used in Classic Mac implementation */
    return pw_get_active_peer_count();  /* Same function as UI peer count */
}

/*
 * TEST PEER ENUMERATION CALLBACK
 *
 * Retrieves peer information by index for test targeting.
 * Uses the same peer enumeration function as the UI peer list.
 *
 * This allows the shared test logic to iterate through the peer
 * list and send targeted messages to specific peers during testing.
 *
 * PARAMETERS:
 * - index: Zero-based peer index
 * - out_peer: Pointer to peer_t structure to fill
 * - context: Platform context (unused on Classic Mac)
 *
 * RETURN VALUES:
 * - 0: Success (peer information retrieved)
 * - Note: Function currently always succeeds (matches shared code expectations)
 */
static int test_get_peer_by_index(int index, peer_t *out_peer, void *context)
{
    (void)context;  /* Context not used in Classic Mac implementation */
    pw_get_peer_by_index(index, out_peer);  /* Same function as UI peer access */
    return 0;  /* Always success (matches current shared code design) */
}

/*
 * MAIN TEST ENTRY POINT FOR CLASSIC MAC
 *
 * This function is called from the UI (File menu "Perform Test") to
 * execute the complete automated test sequence. It sets up the callback
 * structure that bridges the cross-platform test logic with Classic Mac
 * platform-specific implementations.
 *
 * TEST EXECUTION SEQUENCE:
 * 1. Initialize test configuration with default parameters
 * 2. Set up callback functions for platform-specific operations
 * 3. Execute shared test logic via run_automated_test()
 * 4. Log completion for user feedback
 *
 * CALLBACK ARCHITECTURE:
 * The callback pattern allows the shared test logic to be completely
 * platform-independent while still accessing platform-specific
 * functionality like messaging, timing, and peer management.
 *
 * TEST VALIDATION:
 * - Validates complete message flow (broadcast + direct messages)
 * - Tests actual application code paths (not test-only code)
 * - Generates detailed logs with START/END markers for analysis
 * - Provides immediate feedback to user about network functionality
 *
 * USER INTERFACE INTEGRATION:
 * This function is designed to be called from the Classic Mac Dialog
 * Manager event handling when the user selects "Perform Test" from
 * the File menu. It provides a simple, synchronous interface for
 * UI integration.
 */
void PerformAutomatedTest(void)
{
    test_config_t config;      /* Test configuration parameters */
    test_callbacks_t callbacks; /* Platform-specific callback functions */

    log_app_event("PerformAutomatedTest: Starting automated test");

    /*
     * CONFIGURATION SETUP
     *
     * Get default test parameters from shared code. This includes
     * timing delays, message content, and test sequence parameters.
     */
    config = get_default_test_config();

    /*
     * CALLBACK FUNCTION SETUP
     *
     * Configure the callback structure with Classic Mac implementations
     * of each required operation. This bridges shared test logic with
     * platform-specific functionality.
     */
    callbacks.send_broadcast = test_send_broadcast;        /* Broadcast messaging */
    callbacks.send_direct = test_send_direct;              /* Direct messaging */
    callbacks.get_peer_count = test_get_peer_count;        /* Peer enumeration */
    callbacks.get_peer_by_index = test_get_peer_by_index;  /* Peer access */
    callbacks.delay_func = mac_delay_ms;                   /* Timing delays */
    callbacks.context = NULL;                              /* No context needed */

    /*
     * EXECUTE SHARED TEST LOGIC
     *
     * Run the complete test sequence using the shared, cross-platform
     * test implementation. This ensures consistent test behavior across
     * all platform implementations.
     */
    run_automated_test(&config, &callbacks);

    log_app_event("PerformAutomatedTest: Test completed");
}
