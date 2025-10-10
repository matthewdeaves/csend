/*
 * MacTCP Network Initialization and Management
 *
 * This module handles the complete lifecycle of MacTCP networking for the
 * Classic Mac P2P Messenger. It coordinates the initialization and shutdown
 * of all networking components in the proper sequence to ensure reliable
 * operation and clean resource management.
 *
 * KEY RESPONSIBILITIES:
 *
 * 1. MACTCP DRIVER INITIALIZATION:
 *    Opens the MacTCP driver (.IPP) and obtains the local IP configuration.
 *    Handles driver availability detection and version compatibility.
 *
 * 2. COMPONENT INITIALIZATION SEQUENCING:
 *    Coordinates startup of UDP discovery and TCP messaging subsystems in
 *    the correct order with proper error handling and rollback.
 *
 * 3. UPP (UNIVERSAL PROCEDURE POINTER) MANAGEMENT:
 *    Creates and manages UPPs for ASR (interrupt-level) callbacks.
 *    Essential for CFM (Code Fragment Manager) compatibility.
 *
 * 4. RESOURCE LIFECYCLE MANAGEMENT:
 *    Ensures all allocated resources are properly freed during shutdown,
 *    preventing memory leaks and system instability.
 *
 * 5. COOPERATIVE MULTITASKING SUPPORT:
 *    Provides system yielding functions for maintaining system responsiveness
 *    during network operations.
 *
 * ARCHITECTURAL PATTERNS:
 *
 * - Centralized Initialization: Single entry point for all networking setup
 * - Error Rollback: Failed initialization cleans up partial state
 * - Resource Tracking: All allocated resources tracked for cleanup
 * - Defensive Programming: Extensive parameter and state validation
 *
 * CLASSIC MAC CONSIDERATIONS:
 *
 * - Memory Management: Uses Mac Memory Manager for all allocations
 * - Error Handling: Follows Mac OS error code conventions
 * - Threading Model: Single-threaded with cooperative multitasking
 * - Resource Forks: Manages classic Mac resource-based configuration
 *
 * References:
 * - MacTCP Programmer's Guide (1989)
 * - Inside Macintosh Volume VI: Networking
 * - Technical Note TN1083: MacTCP and System 7.0
 */

#include "network_init.h"
#include "mactcp_impl.h"
#include "../shared/logging.h"
#include "../shared/logging.h"
#include "discovery.h"
#include "messaging.h"
#include "common_defs.h"
#include <Devices.h>
#include <Errors.h>
#include <string.h>
#include <stdlib.h>
#include <Memory.h>
#include <Events.h>
#include <Resources.h>
#include <OSUtils.h>
#include <MixedMode.h>

/*
 * MACTCP DRIVER IDENTIFICATION
 *
 * MacTCP appears to the system as a device driver with the name ".IPP"
 * (Internet Protocol Package). This is a Pascal string (length-prefixed)
 * as required by the Classic Mac Device Manager.
 *
 * The \p prefix is a compiler extension that creates a Pascal string
 * with the length byte automatically calculated and prepended.
 */
const unsigned char kTCPDriverName[] = "\p.IPP";

/*
 * DOMAIN NAME RESOLVER (DNR) FUNCTION DECLARATIONS
 *
 * These functions are provided by Apple's Domain Name Resolver library,
 * which is part of the MacTCP installation. They handle hostname-to-IP
 * address resolution and IP-to-string conversion.
 *
 * Note: These are external functions from Apple's DNR library, not
 * implemented in this codebase.
 */
extern OSErr OpenResolver(char *fileName);          /* Initialize DNR */
extern OSErr CloseResolver(void);                   /* Shutdown DNR */
extern OSErr AddrToStr(unsigned long addr, char *addrStr); /* IP to string */

/*
 * GLOBAL NETWORK STATE VARIABLES
 *
 * These globals maintain the core networking state for the application.
 * In Classic Mac programming, globals are commonly used due to the
 * single-threaded execution model and simplified state management.
 *
 * Memory Layout Considerations:
 * - All globals are in application global space (A5 world)
 * - Access from ASRs requires proper A5 setup
 * - Static initialization ensures known initial state
 */
short gMacTCPRefNum = 0;                            /* MacTCP driver reference number */
ip_addr gMyLocalIP = 0;                             /* Local IP address (network byte order) */
char gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0";    /* Local IP as string */
char gMyUsername[GLOBAL_USERNAME_BUFFER_SIZE] = "MacTCP"; /* Default username */

/*
 * UNIVERSAL PROCEDURE POINTERS (UPPs) FOR ASR HANDLERS
 *
 * UPPs are a CFM (Code Fragment Manager) mechanism that enables calling
 * between different code fragments with different calling conventions.
 * They're essential for ASR callbacks because:
 *
 * 1. ASRs execute in system context with Pascal calling convention
 * 2. Our handlers are C functions with C calling convention
 * 3. UPPs provide the necessary translation layer
 *
 * SEPARATE UPPs FOR DIFFERENT STREAM TYPES:
 * We use separate UPPs for listen and send streams to:
 * - Enable different handling logic for each stream type
 * - Simplify debugging by identifying callback source
 * - Prevent callback confusion in complex scenarios
 *
 * UPP LIFECYCLE:
 * 1. Created with NewTCPNotifyUPP() during initialization
 * 2. Passed to MacTCP when creating streams
 * 3. Called by MacTCP when network events occur
 * 4. Disposed with DisposeRoutineDescriptor() during cleanup
 *
 * MEMORY MANAGEMENT:
 * UPPs allocate system memory and must be explicitly disposed to
 * prevent memory leaks. They're tracked globally for cleanup.
 */
static TCPNotifyUPP gTCPListenASR_UPP = NULL;  /* UPP for listen stream ASR */
static TCPNotifyUPP gTCPSendASR_UPP = NULL;    /* UPP for send stream pool ASRs */

/*
 * COMPLETE NETWORKING SUBSYSTEM INITIALIZATION
 *
 * This function orchestrates the startup of all networking components
 * in the correct sequence. Each step depends on the previous steps
 * completing successfully, so careful error handling and rollback
 * is essential.
 *
 * INITIALIZATION SEQUENCE:
 * 1. Initialize MacTCP driver and get local IP configuration
 * 2. Initialize UDP discovery subsystem for peer finding
 * 3. Create UPPs for TCP ASR handlers (interrupt callbacks)
 * 4. Initialize TCP messaging subsystem with connection pool
 * 5. Start listening for incoming connections
 *
 * ERROR HANDLING STRATEGY:
 * If any step fails, we must carefully clean up all resources
 * allocated in previous steps. This prevents memory leaks and
 * system instability from partial initialization.
 *
 * BUFFER SIZE SELECTION:
 * TCP receive buffer size affects both performance and memory usage:
 * - Larger buffers: Better performance, more memory usage
 * - Smaller buffers: Less memory, more system calls
 * - Chosen size balances performance with Classic Mac memory constraints
 *
 * RETURNS:
 * - noErr: All networking components initialized successfully
 * - Various error codes: Specific component initialization failed
 */
OSErr InitializeNetworking(void)
{
    OSErr err;
    unsigned long tcpStreamBufferSize;  /* Size of TCP receive buffers */

    log_info_cat(LOG_CAT_NETWORKING, "InitializeNetworking: Starting MacTCP initialization");

    /*
     * MACTCP DRIVER INITIALIZATION
     *
     * This is the foundation of all networking - opens the MacTCP driver,
     * obtains local IP configuration, and initializes the DNR.
     *
     * Common failure reasons:
     * - MacTCP not installed or wrong version
     * - Network interface not configured
     * - Insufficient system resources
     * - Conflicting network software
     */
    err = MacTCPImpl_Initialize(&gMacTCPRefNum, &gMyLocalIP, gMyLocalIPStr);
    if (err != noErr) {
        log_app_event("Fatal Error: MacTCP initialization failed: %d", err);
        return err;
    }

    log_info_cat(LOG_CAT_NETWORKING, "InitializeNetworking: MacTCP initialized successfully");

    if (gMyLocalIP == 0) {
        log_app_event("Critical Warning: Local IP address is 0.0.0.0. Check network configuration.");
    }

    /* Initialize UDP Discovery */
    err = InitUDPDiscoveryEndpoint(gMacTCPRefNum);
    if (err != noErr) {
        log_app_event("Fatal: UDP Discovery initialization failed (%d).", err);
        MacTCPImpl_Shutdown(gMacTCPRefNum);
        gMacTCPRefNum = 0;
        return err;
    }
    log_info_cat(LOG_CAT_DISCOVERY, "UDP Discovery Endpoint Initialized.");

    /* Initialize TCP Messaging with dual streams */
    tcpStreamBufferSize = PREFERRED_TCP_STREAM_RCV_BUFFER_SIZE;
    if (tcpStreamBufferSize < MINIMUM_TCP_STREAM_RCV_BUFFER_SIZE) {
        tcpStreamBufferSize = MINIMUM_TCP_STREAM_RCV_BUFFER_SIZE;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "Initializing TCP with stream receive buffer size: %lu bytes.", tcpStreamBufferSize);

    /*
     * UPP CREATION FOR ASR HANDLERS
     *
     * Universal Procedure Pointers enable MacTCP to call our C functions
     * from its Pascal-based interrupt-level code. This is essential for
     * receiving network event notifications.
     *
     * UPP CREATION PROCESS:
     * 1. NewTCPNotifyUPP() creates a code stub that:
     *    - Converts Pascal calling convention to C
     *    - Sets up proper stack frame
     *    - Handles register preservation
     *    - Calls our actual handler function
     * 2. Returns UPP handle for use with MacTCP
     *
     * MEMORY ALLOCATION:
     * UPPs allocate system memory for the code stub. If allocation fails,
     * we must clean up all previously allocated resources.
     */
    if (gTCPListenASR_UPP == NULL) {
        gTCPListenASR_UPP = NewTCPNotifyUPP(TCP_Listen_ASR_Handler);
        if (gTCPListenASR_UPP == NULL) {
            log_app_event("Fatal: Failed to create UPP for TCP_Listen_ASR_Handler.");
            /* Clean up in reverse order of initialization */
            CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
            MacTCPImpl_Shutdown(gMacTCPRefNum);
            gMacTCPRefNum = 0;
            return memFullErr;
        }
        log_debug_cat(LOG_CAT_NETWORKING, "TCP Listen ASR UPP created at 0x%lX.", (unsigned long)gTCPListenASR_UPP);
    }

    if (gTCPSendASR_UPP == NULL) {
        gTCPSendASR_UPP = NewTCPNotifyUPP(TCP_Send_ASR_Handler);
        if (gTCPSendASR_UPP == NULL) {
            log_app_event("Fatal: Failed to create UPP for TCP_Send_ASR_Handler.");
            DisposeRoutineDescriptor(gTCPListenASR_UPP);
            gTCPListenASR_UPP = NULL;
            CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
            MacTCPImpl_Shutdown(gMacTCPRefNum);
            gMacTCPRefNum = 0;
            return memFullErr;
        }
        log_debug_cat(LOG_CAT_NETWORKING, "TCP Send ASR UPP created at 0x%lX.", (unsigned long)gTCPSendASR_UPP);
    }

    err = InitTCP(gMacTCPRefNum, tcpStreamBufferSize, gTCPListenASR_UPP, gTCPSendASR_UPP);
    if (err != noErr) {
        log_app_event("Fatal: TCP messaging initialization failed (%d).", err);
        if (gTCPListenASR_UPP != NULL) {
            DisposeRoutineDescriptor(gTCPListenASR_UPP);
            gTCPListenASR_UPP = NULL;
        }
        if (gTCPSendASR_UPP != NULL) {
            DisposeRoutineDescriptor(gTCPSendASR_UPP);
            gTCPSendASR_UPP = NULL;
        }
        CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
        MacTCPImpl_Shutdown(gMacTCPRefNum);
        gMacTCPRefNum = 0;
        return err;
    }

    log_info_cat(LOG_CAT_MESSAGING, "TCP Messaging Initialized with connection pool.");
    log_app_event("Networking initialization complete. Local IP: %s using MacTCP",
                  gMyLocalIPStr);

    return noErr;
}

/*
 * COMPLETE NETWORKING SUBSYSTEM CLEANUP
 *
 * Performs orderly shutdown of all networking components in reverse order
 * of initialization. This ensures that no component tries to use resources
 * that have already been freed.
 *
 * CLEANUP SEQUENCE:
 * 1. Shutdown TCP messaging (closes connections, frees streams)
 * 2. Shutdown UDP discovery (closes UDP endpoint)
 * 3. Dispose UPPs (frees system memory for code stubs)
 * 4. Shutdown MacTCP driver (closes driver, frees resources)
 * 5. Clear global state variables
 *
 * DEFENSIVE CLEANUP:
 * Each cleanup step includes NULL checks to handle partial initialization
 * scenarios where some components may not have been initialized.
 *
 * RESOURCE LEAK PREVENTION:
 * All dynamically allocated resources (UPPs, streams, buffers) are
 * explicitly freed and their handles set to NULL to prevent reuse.
 */
void CleanupNetworking(void)
{
    log_app_event("Cleaning up Networking...");

    /*
     * TCP MESSAGING CLEANUP
     *
     * Must be first because TCP streams may have active connections
     * that need graceful shutdown before disposing lower-level resources.
     */
    CleanupTCP(gMacTCPRefNum);
    log_debug_cat(LOG_CAT_MESSAGING, "TCP Messaging Cleaned up.");

    /* Clean up UDP Discovery */
    CleanupUDPDiscoveryEndpoint(gMacTCPRefNum);
    log_debug_cat(LOG_CAT_DISCOVERY, "UDP Discovery Cleaned up.");

    /*
     * UPP DISPOSAL
     *
     * UPPs must be explicitly disposed to free the system memory used
     * for the code stubs. DisposeRoutineDescriptor() is the correct
     * function for disposing UPPs created with NewTCPNotifyUPP().
     *
     * IMPORTANT SEQUENCING:
     * UPPs must be disposed AFTER all streams that use them are closed.
     * Disposing UPPs while streams are active will cause crashes when
     * MacTCP tries to call the disposed callback.
     */
    if (gTCPListenASR_UPP != NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "Disposing TCP Listen ASR UPP at 0x%lX.", (unsigned long)gTCPListenASR_UPP);
        DisposeRoutineDescriptor(gTCPListenASR_UPP);
        gTCPListenASR_UPP = NULL;
    }

    if (gTCPSendASR_UPP != NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "Disposing TCP Send ASR UPP at 0x%lX.", (unsigned long)gTCPSendASR_UPP);
        DisposeRoutineDescriptor(gTCPSendASR_UPP);
        gTCPSendASR_UPP = NULL;
    }

    /* Shutdown MacTCP */
    MacTCPImpl_Shutdown(gMacTCPRefNum);

    gMacTCPRefNum = 0;
    gMyLocalIP = 0;
    gMyLocalIPStr[0] = '\0';

    log_app_event("Networking cleanup complete.");
}

/*
 * COOPERATIVE MULTITASKING SUPPORT
 *
 * Classic Mac OS uses cooperative multitasking where applications must
 * voluntarily yield CPU time to allow other applications and system
 * processes to run. This is essential during network operations that
 * might take significant time.
 *
 * IMPLEMENTATION:
 * WaitNextEvent() with a short timeout (1 tick) allows:
 * - Other applications to run
 * - System processes to execute
 * - Event processing to continue
 * - Network stack to process packets
 *
 * PARAMETERS:
 * - eventMask: 0 = don't retrieve any events
 * - theEvent: event record (unused in our case)
 * - sleep: 1 tick = ~17ms minimum yield time
 * - mouseRgn: NULL = no special mouse region handling
 *
 * USAGE PATTERN:
 * Called during long-running operations like:
 * - Waiting for network connections
 * - Processing large amounts of data
 * - Cleanup operations with delays
 *
 * This maintains system responsiveness and prevents the application
 * from appearing "frozen" during network operations.
 */
void YieldTimeToSystem(void)
{
    EventRecord event;
    (void)WaitNextEvent(0, &event, 1L, NULL);  /* Yield for 1 tick (~17ms) */
}

/*
 * IPv4 ADDRESS STRING PARSING
 *
 * Converts dotted-decimal IPv4 address strings (e.g., "192.168.1.1")
 * into 32-bit network byte order addresses for use with MacTCP.
 *
 * PARSING ALGORITHM:
 * 1. Copy input string to local buffer (safety)
 * 2. Split string on '.' delimiter using strtok_r()
 * 3. Parse each part as decimal number (0-255)
 * 4. Validate range and format of each part
 * 5. Combine parts into 32-bit address
 *
 * VALIDATION:
 * - Exactly 4 parts required
 * - Each part must be 0-255
 * - No non-numeric characters allowed
 * - No leading/trailing whitespace
 *
 * BYTE ORDER:
 * Result is in network byte order (big-endian) as required by MacTCP.
 * Format: (part[0] << 24) | (part[1] << 16) | (part[2] << 8) | part[3]
 *
 * ERROR HANDLING:
 * Returns paramErr for any parsing error and sets *out_addr to 0
 * for consistent error state.
 *
 * THREAD SAFETY:
 * Uses strtok_r() (reentrant version) to avoid static buffer issues
 * that could occur with strtok() in multi-context environments.
 */
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr)
{
    unsigned long parts[4];      /* Array to hold the 4 IP address parts */
    int i = 0;                   /* Current part index */
    char *token;                 /* Current token from strtok_r */
    char *rest_of_string;        /* Remaining string for strtok_r */
    char buffer[INET_ADDRSTRLEN]; /* Local copy of input string */

    /* Parameter validation */
    if (ip_str == NULL || out_addr == NULL) {
        return paramErr;
    }

    /*
     * SAFE STRING COPYING
     *
     * Create local copy to avoid modifying input string (strtok_r modifies
     * the string it parses). Ensure null termination for safety.
     */
    strncpy(buffer, ip_str, INET_ADDRSTRLEN - 1);
    buffer[INET_ADDRSTRLEN - 1] = '\0';

    /*
     * TOKENIZATION AND PARSING LOOP
     *
     * Split string on '.' delimiter and parse each part as decimal number.
     * strtok_r() is reentrant and safe for use in callback contexts.
     */
    rest_of_string = buffer;
    while ((token = strtok_r(rest_of_string, ".", &rest_of_string)) != NULL && i < 4) {
        char *endptr;  /* For strtoul error checking */

        /* Parse decimal number with error checking */
        parts[i] = strtoul(token, &endptr, 10);

        /*
         * VALIDATION CHECKS:
         * - endptr != '\0': Non-numeric characters present
         * - parts[i] > 255: Value out of valid byte range
         */
        if (*endptr != '\0' || parts[i] > 255) {
            log_error_cat(LOG_CAT_NETWORKING, "ParseIPv4: Invalid part '%s' in IP string '%s'", token, ip_str);
            *out_addr = 0;  /* Set error state */
            return paramErr;
        }
        i++;  /* Move to next part */
    }

    /*
     * FINAL VALIDATION
     *
     * Must have exactly 4 parts for valid IPv4 address.
     * Too few parts: incomplete address
     * Too many parts: extra dots in string
     */
    if (i != 4) {
        log_error_cat(LOG_CAT_NETWORKING, "ParseIPv4: Incorrect number of parts (%d) in IP string '%s'", i, ip_str);
        *out_addr = 0;
        return paramErr;
    }

    /*
     * NETWORK BYTE ORDER CONVERSION
     *
     * Combine 4 parts into 32-bit address in network byte order (big-endian).
     * Format: parts[0].parts[1].parts[2].parts[3]
     * Example: 192.168.1.1 becomes 0xC0A80101
     *
     * Bit layout:
     * parts[0] in bits 31-24 (most significant byte)
     * parts[1] in bits 23-16
     * parts[2] in bits 15-8
     * parts[3] in bits 7-0 (least significant byte)
     */
    *out_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return noErr;
}