// FILE: ./classic_mac/discovery.h
#ifndef DISCOVERY_H
#define DISCOVERY_H

// --- Base Mac OS Includes ---
#include <MacTypes.h>
#include <Devices.h> // For ParamBlockRec base structure

// --- Include the REAL MacTCP Header ---
// This brings in all the standard enums, basic types, UPPs, nested structs etc.
#include <MacTCP.h>

// --- Define OUR Correctly Padded UDP Parameter Block ---
// We use a distinct name 'MyUDPiopb' to avoid conflict with the UDPiopb
// potentially defined (incorrectly for our driver binary) in MacTCP.h.

struct MyUDPiopb {
    /* Standard ParamBlockRec Header (first 26 bytes) */
    struct QElem   *qLink;        /* Offset 0, Size 4 */
    short          qType;         /* Offset 4, Size 2 */
    short          ioTrap;        /* Offset 6, Size 2 */
    Ptr            ioCmdAddr;     /* Offset 8, Size 4 */
    ProcPtr        ioCompletion;  /* Offset 12, Size 4 - Use generic ProcPtr for base PB */
    OSErr          ioResult;      /* Offset 16, Size 2 - Use OSErr                      */
    Ptr            ioNamePtr;     /* Offset 18, Size 4 - Official MacTCP.h uses Ptr       */
    short          ioVRefNum;     /* Offset 22, Size 2 */
    short          ioCRefNum;     /* Offset 24, Size 2 */

    /* --- MacTCP Specific Part Starts Here --- */

    /* *** ADD PADDING HERE TO MATCH MPW C ALIGNMENT (Driver Expectation) *** */
    char              _pad_to_offset_28[2]; // <-- THE FIX!

    /* Fields below should now align correctly relative to the driver */
    short             csCode;         /* Offset 28 */
    StreamPtr         udpStream;      /* Offset 30 */ /* StreamPtr type comes from MacTCP.h */

    /* Union uses nested structs defined in the included MacTCP.h */
    union {                         /* Offset 34 */
        struct UDPCreatePB  create;   /* Defined in MacTCP.h */
        struct UDPSendPB    send;     /* Defined in MacTCP.h */
        struct UDPReceivePB receive;  /* Defined in MacTCP.h */
        struct UDPMTUPB     mtu;      /* Defined in MacTCP.h */
    } csParam;
};

// Define the type and pointer type using our unique name
typedef struct MyUDPiopb MyUDPiopb;
typedef MyUDPiopb *MyUDPiopbPtr;

// --- Keep Lowercase Defines for Compatibility ---
// Maps the lowercase names used in discovery.c to the uppercase enums from MacTCP.h
#define udpCreate     UDPCreate     // Defined in MacTCP.h enum
#define udpRead       UDPRead       // Defined in MacTCP.h enum
#define udpBfrReturn  UDPBfrReturn  // Defined in MacTCP.h enum
#define udpWrite      UDPWrite      // Defined in MacTCP.h enum
#define udpRelease    UDPRelease    // Defined in MacTCP.h enum
// e.g. #define udpMaxMTU UDPMaxMTUSize // Add if needed

// --- Project Includes (needed after base types) ---
#include "common_defs.h" // For PORT_UDP, DISCOVERY_INTERVAL, BUFFER_SIZE, INET_ADDRSTRLEN
#include "peer_mac.h"    // For AddOrUpdatePeer

// --- Project Constants ---
#define BROADCAST_IP 0xFFFFFFFFUL // Standard broadcast address (255.255.255.255)
#define kMinUDPBufSize  2048        // Minimum receive buffer size for UDPCreate

// --- Global Variables (External Declarations) ---
extern StreamPtr gUDPStream; // Pointer to our UDP stream for discovery (StreamPtr from MacTCP.h)
extern Ptr     gUDPRecvBuffer; // Pointer to the buffer allocated for UDPCreate
extern unsigned long gLastBroadcastTimeTicks; // Time of last broadcast in Ticks

// --- Function Prototypes ---

/**
 * @brief Initializes the UDP endpoint for discovery broadcasts and receives using PBControl.
 * @param macTCPRefNum The driver reference number obtained from PBOpen.
 * @return OSErr noErr on success, or an error code on failure.
 */
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum);

/**
 * @brief Sends a UDP discovery broadcast message using PBControl.
 * @param macTCPRefNum The driver reference number.
 * @param myUsername The username to include in the broadcast.
 * @param myLocalIPStr The local IP address string to include.
 * @return OSErr noErr on success, or an error code on failure.
 */
OSErr SendDiscoveryBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);

/**
 * @brief Checks if it's time to send the next discovery broadcast and sends if needed.
 * @param macTCPRefNum The driver reference number.
 * @param myUsername The username to include in the broadcast.
 * @param myLocalIPStr The local IP address string to include.
 */
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);

/**
 * @brief Checks for and processes incoming UDP packets (e.g., discovery responses).
 *        Uses PBControlSync with a short timeout for non-blocking checks.
 * @param macTCPRefNum The driver reference number.
 * @param myLocalIP The local IP address (numeric) to ignore self-messages.
 */
void CheckUDPReceive(short macTCPRefNum, ip_addr myLocalIP); // ip_addr from MacTCP.h

/**
 * @brief Cleans up the UDP discovery endpoint resources.
 * @param macTCPRefNum The driver reference number.
 */
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum);


#endif // DISCOVERY_H
