#ifndef TCP_H
#define TCP_H

#include <MacTypes.h>
#include <MacTCP.h>      // Primary MacTCP definitions
#include "common_defs.h" // For BUFFER_SIZE, PORT_TCP etc.

/* --- Configuration --- */
// Note: MacTCP documentation often uses constants like these implicitly.
// Define them explicitly for clarity based on common practice in examples.
#define kTCPDefaultTimeout 0 // Use 0 for infinite timeout in blocking examples, adjust if needed

// #define kTCPAbortOnTimeout true // Standard behavior - Defined implicitly via value 1 below

/* --- Public Function Prototypes --- */

/**
 * @brief Initializes the single TCP stream for listening.
 *
 * Allocates necessary buffers, creates the TCP stream using TCPCreate (synchronously),
 * and posts the initial asynchronous TCPPassiveOpen request.
 *
 * @param macTCPRefNum The reference number for the open MacTCP driver.
 * @return OSErr noErr if successful, or an appropriate MacTCP or Memory Manager error code.
 */
OSErr InitTCPListener(short macTCPRefNum);

/**
 * @brief Cleans up the TCP listener stream and associated resources.
 *
 * Cancels any pending operations, releases the TCP stream using TCPRelease (synchronously),
 * and disposes of allocated buffers.
 *
 * @param macTCPRefNum The reference number for the open MacTCP driver.
 */
void CleanupTCPListener(short macTCPRefNum);

/**
 * @brief Polls for completion of asynchronous TCP operations (Listen, Receive, Close, Release).
 *
 * This function should be called periodically in the main event loop.
 * It checks the ioResult of pending parameter blocks and calls the appropriate
 * handler function upon completion. It also handles re-issuing the listen
 * command when the stream becomes available again.
 *
 * @param macTCPRefNum The reference number for the open MacTCP driver.
 * @param myLocalIP The local IP address (used for logging/ignoring self-connections).
 */
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP);


/* --- Extern Globals --- */
// These store the details of the *current* connection being handled by the listener stream.

/** IP address of the remote host currently connected to the listener stream. */
extern ip_addr gCurrentConnectionIP;

/** TCP port of the remote host currently connected to the listener stream. */
extern tcp_port gCurrentConnectionPort;

/** Flag indicating if the listener stream is currently handling an active connection (receiving data). */
extern Boolean gIsConnectionActive;

#endif // TCP_H