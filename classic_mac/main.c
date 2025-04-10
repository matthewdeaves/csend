#include <Types.h>
#include <Quickdraw.h> // Still needed for InitGraf
#include <OSUtils.h>   // For OSErr, TickCount, SysBeep
#include <Events.h>    // For Button()
#include <Memory.h>    // For NewPtrClear, DisposePtr
#include <Devices.h>   // For PBControlSync, PBOpenSync
#include <stdio.h>     // For printf
#include <string.h>    // For memset, strcpy, strcmp

// MacTCP specific includes
#include <MacTCP.h>

// --- DNR Includes and Definitions ---
#define __MACTCPCOMMONTYPES__ // Prevent redefinition of common types
#include <AddressXlation.h>

// --- Constants ---
#define kBroadcastIP    0xFFFFFFFFUL // 255.255.255.255 as unsigned long
#define kTargetPort     54321       // Example UDP port for broadcast
#define kMessage        "Hello from Simple MacTCP Broadcaster!"
#define kDelayInterval  (10 * 60)   // 10 seconds in ticks (60 ticks per second)
#define kMinUDPBufSize  2048        // Minimum receive buffer size for UDPCreate

// Define INET_ADDRSTRLEN if not provided by headers
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

// MacTCP Control Code for GetMyIPAddr (Defined in MacTCP.h)
// #define ipctlGetAddr    15

// --- Globals ---
short       gMacTCPRefNum = 0;      // Ref num for the .IPP driver
StreamPtr   gUDPStream = 0L;        // Pointer to our UDP stream (Use 0L instead of nil)
Boolean     gDone = false;          // Flag to control the main loop (not used in console loop)
Ptr         gUDPRecvBuffer = nil;   // Pointer to the buffer allocated for UDPCreate (nil is ok for Ptr)
ip_addr     gMyLocalIP = 0;         // Store the local IP globally (network byte order)
char        gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0"; // Store the string version globally

// --- Function Prototypes ---
void InitializeMinimalToolbox(void);
OSErr OpenMacTCPDriverAndResolver(void);
void GetAndDisplayLocalIPAddress(void);
OSErr CreateUDPStream(void);
void SendBroadcast(void);
void ConsoleEventLoop(void);
void Cleanup(void);

// --- Implementation ---

int main(void) {
    OSErr err;

    InitializeMinimalToolbox();
    printf("Minimal Toolbox Initialized.\n");

    err = OpenMacTCPDriverAndResolver();
    if (err != noErr) {
        printf("Fatal Error: Could not open MacTCP driver or Resolver: %d\n", err);
        return 1;
    }
    printf("MacTCP Driver Opened (RefNum: %d) and Resolver Initialized.\n", gMacTCPRefNum);

    GetAndDisplayLocalIPAddress();

    err = CreateUDPStream();
    if (err != noErr) {
        printf("Fatal Error: Could not create UDP stream: %d\n", err);
        Cleanup();
        return 1;
    }
    printf("UDP Stream Created (StreamPtr: %lu).\n", gUDPStream);

    printf("Starting broadcast loop (every %d seconds). Click mouse or press key to quit.\n", kDelayInterval / 60);

    SendBroadcast();
    ConsoleEventLoop();
    Cleanup();
    printf("Application finished.\n");
    return 0;
}

/**
 * @brief Initializes minimal Macintosh Toolbox managers for console app.
 */
void InitializeMinimalToolbox(void) {
    InitGraf(&qd.thePort);
    InitCursor();
    FlushEvents(everyEvent, 0);
}

/**
 * @brief Opens the MacTCP driver (.IPP) and initializes the DNR.
 */
OSErr OpenMacTCPDriverAndResolver(void) {
    OSErr err;
    ParamBlockRec pb;

    pb.ioParam.ioNamePtr = (StringPtr)"\p.IPP";
    pb.ioParam.ioPermssn = fsCurPerm;
    printf("Attempting PBOpenSync for .IPP driver...\n");
    err = PBOpenSync(&pb);
    if (err != noErr) {
        printf("Error: PBOpenSync failed. Error: %d\n", err);
        gMacTCPRefNum = 0;
        return err;
    }
    gMacTCPRefNum = pb.ioParam.ioRefNum;
    printf("PBOpenSync succeeded (RefNum: %d).\n", gMacTCPRefNum);

    printf("Attempting OpenResolver...\n");
    err = OpenResolver(NULL);
    if (err != noErr) {
        printf("Error: OpenResolver failed. Error: %d\n", err);
        return err;
    }
    printf("OpenResolver succeeded.\n");

    return noErr;
}

/**
 * @brief Gets the local IP address using ipctlGetAddr and converts it to string using AddrToStr.
 */
void GetAndDisplayLocalIPAddress(void) {
    IPParamBlock pb;
    OSErr err;

    if (gMacTCPRefNum == 0) {
        printf("  Error: MacTCP driver not open, cannot get IP address.\n");
        return;
    }

    memset(&pb, 0, sizeof(IPParamBlock));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;
    pb.csCode = ipctlGetAddr;

    printf("Getting local IP address (binary)...\n");
    err = PBControlSync((ParmBlkPtr)&pb);

    if (err == noErr) {
        // *** REVERT TO THE WORKING CAST FROM OLD CODE ***
        // Cast the address where csParam starts to an ip_addr pointer.
        ip_addr* ourAddrPtr = (ip_addr*)&(pb.csParam);
        gMyLocalIP = *ourAddrPtr; // Dereference the pointer

        printf("  Successfully got binary IP: %lu\n", gMyLocalIP);

        printf("  Converting binary IP to string using AddrToStr...\n");
        err = AddrToStr(gMyLocalIP, gMyLocalIPStr);

        if (err != noErr) {
             printf("  Warning: AddrToStr failed with error %d. IP string may be invalid.\n", err);
             strcpy(gMyLocalIPStr, "?.?.?.?");
        } else if (gMyLocalIPStr[0] == '\0' || strcmp(gMyLocalIPStr, "0.0.0.0") == 0) {
             printf("  Warning: AddrToStr returned empty or zero string. IP string may be invalid.\n");
             strcpy(gMyLocalIPStr, "?.?.?.?");
        } else {
             printf("  AddrToStr conversion successful.\n");
        }
        printf("  Local IP Address: %s\n", gMyLocalIPStr);

    } else {
        printf("  Error getting local IP address (binary) using ipctlGetAddr: %d\n", err);
        gMyLocalIP = 0;
        strcpy(gMyLocalIPStr, "ERROR");
    }
}

/**
 * @brief Creates a UDP stream for sending broadcasts.
 */
OSErr CreateUDPStream(void) {
    UDPiopb pb;
    OSErr err;

    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == nil) {
        printf("  Error: Failed to allocate UDP receive buffer (memFullErr).\n");
        return memFullErr;
    }
    printf("  Allocated %ld bytes for UDP receive buffer at %p.\n", (long)kMinUDPBufSize, gUDPRecvBuffer);

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;
    pb.csCode = UDPCreate;
    pb.udpStream = 0L;
    pb.csParam.create.rcvBuff = gUDPRecvBuffer;
    pb.csParam.create.rcvBuffLen = kMinUDPBufSize;
    pb.csParam.create.notifyProc = nil;
    pb.csParam.create.localPort = 0;

    err = PBControlSync((ParmBlkPtr)&pb);

    if (err == noErr) {
        gUDPStream = pb.udpStream;
        printf("  UDPCreate successful. StreamPtr: %lu\n", gUDPStream);
    } else {
        printf("  UDPCreate failed with error: %d\n", err);
        if (gUDPRecvBuffer != nil) {
            DisposePtr(gUDPRecvBuffer);
            gUDPRecvBuffer = nil;
            printf("  Disposed UDP receive buffer due to UDPCreate error.\n");
        }
    }
    return err;
}

/**
 * @brief Sends a single UDP broadcast message.
 */
void SendBroadcast(void) {
    UDPiopb pb;
    wdsEntry wds[2];
    char message[] = kMessage;
    OSErr err;

    if (gUDPStream == 0L) {
        printf("Error: Cannot send broadcast, UDP Stream is not valid.\n");
        return;
    }

    wds[0].length = sizeof(message) - 1;
    wds[0].ptr = message;
    wds[1].length = 0;
    wds[1].ptr = nil;

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;
    pb.csCode = UDPWrite;
    pb.udpStream = gUDPStream;
    pb.csParam.send.remoteHost = kBroadcastIP;
    pb.csParam.send.remotePort = kTargetPort;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.csParam.send.checkSum = true;

    printf("Sending broadcast: \"%s\" to IP 255.255.255.255:%d...\n", message, kTargetPort);

    err = PBControlSync((ParmBlkPtr)&pb);

    if (err != noErr) {
        printf("  Error sending UDP broadcast: %d\n", err);
    } else {
        printf("  Broadcast sent successfully.\n");
    }
}

/**
 * @brief Simplified event loop for console app, handles timing and quitting.
 */
void ConsoleEventLoop(void) {
    long nextBroadcastTime = TickCount() + kDelayInterval;
    EventRecord event;

    while (true) {
        if (GetNextEvent(mDownMask | keyDownMask | autoKeyMask, &event)) {
             if (event.what == mouseDown) {
                 printf("Mouse clicked, quitting application.\n");
             } else {
                 printf("Key pressed, quitting application.\n");
             }
             break;
        }

        if (TickCount() >= nextBroadcastTime) {
            SendBroadcast();
            nextBroadcastTime = TickCount() + kDelayInterval;
        }

        WaitNextEvent(0, &event, 1, NULL); // Yield CPU time
    }
}

/**
 * @brief Releases MacTCP and DNR resources.
 */
void Cleanup(void) {
    UDPiopb pb;
    OSErr err;

    // Release UDP Stream
    if (gUDPStream != 0L) {
        printf("Releasing UDP Stream (StreamPtr: %lu)...\n", gUDPStream);
        memset(&pb, 0, sizeof(UDPiopb));
        pb.ioCompletion = nil;
        pb.ioCRefNum = gMacTCPRefNum;
        pb.csCode = UDPRelease;
        pb.udpStream = gUDPStream;
        pb.csParam.create.rcvBuff = gUDPRecvBuffer;
        pb.csParam.create.rcvBuffLen = kMinUDPBufSize;

        err = PBControlSync((ParmBlkPtr)&pb);
        if (err != noErr) {
            printf("  Warning: Error releasing UDP stream: %d.\n", err);
        } else {
            printf("  UDP Stream released successfully.\n");
            gUDPStream = 0L;
        }
    }

    // Dispose UDP Receive Buffer
    if (gUDPRecvBuffer != nil) {
         printf("Disposing UDP receive buffer at %p.\n", gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = nil;
    }

    // Close DNR
    printf("Attempting CloseResolver...\n");
    err = CloseResolver();
    if (err != noErr) {
        printf("  Warning: CloseResolver failed with error %d.\n", err);
    } else {
        printf("  CloseResolver succeeded.\n");
    }

    // MacTCP Driver
    printf("MacTCP driver (.IPP) remains open (standard practice).\n");
    gMacTCPRefNum = 0;
}