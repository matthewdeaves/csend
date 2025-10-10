/*
 * MacTCP Implementation for P2P Messaging
 *
 * This implementation follows patterns from MacTCP Programmer's Guide (1989):
 * - Asynchronous operation management for non-blocking I/O
 * - Proper MacTCP driver initialization and cleanup
 * - WDS (Write Data Structure) usage for scatter-gather I/O
 * - RDS (Read Data Structure) usage for no-copy receives
 * - DNR (Domain Name Resolver) integration
 *
 * Key architectural patterns:
 * - Resource pooling for async operation handles
 * - Callback-based notification system
 * - Error handling following MacTCP conventions
 * - Memory management using Mac Memory Manager
 *
 * Performance considerations:
 * - Async operations prevent blocking main thread
 * - Connection pooling reduces setup/teardown overhead
 * - Buffer management minimizes memory fragmentation
 */

#include "mactcp_impl.h"
#include "network_init.h"
#include "../shared/logging.h"
#include <MacTCP.h>
#include <Devices.h>
#include <Errors.h>
#include <Files.h>
#include <OSUtils.h>
#include <Events.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations */

/*
 * DNR (Domain Name Resolver) hostInfo structure
 * Based on MacTCP Programmer's Guide Chapter 3
 *
 * This structure is returned by StrToAddr() calls and contains
 * resolved address information. The addr array can hold multiple
 * IP addresses for multihomed hosts (load balancing/redundancy).
 */
struct hostInfo {
    OSErr           rtnCode;        /* DNR operation result code */
    char            cname[255];     /* Canonical (official) hostname */
    unsigned short  addrType;       /* Address family (always AF_INET for IP) */
    unsigned short  addrLen;        /* Length of each address (4 for IPv4) */
    ip_addr         addr[4];        /* Up to 4 IP addresses for multihomed hosts */
};

/* DNR function declarations */
extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);
extern OSErr StrToAddr(char *hostName, struct hostInfo *rtnStruct,
                       long resultProc, char *userData);


/*
 * WDS (Write Data Structure) setup macro
 *
 * WDS enables scatter-gather I/O by allowing MacTCP to send data from
 * multiple non-contiguous memory locations in a single operation.
 * Per MacTCP Programmer's Guide p.2-23: "The WDS is a linked list of
 * data pointers and lengths that tells MacTCP where to get the data."
 *
 * For simple sends, we use a 2-entry WDS with the second entry as sentinel.
 * More complex protocols might chain multiple data segments.
 */
#define SETUP_SINGLE_WDS(wds, data, length) do { \
    (wds)[0].length = (length);  /* Data length in bytes */ \
    (wds)[0].ptr = (data);       /* Pointer to data buffer */ \
    (wds)[1].length = 0;         /* Sentinel: zero length terminates list */ \
    (wds)[1].ptr = NULL;         /* Sentinel: NULL pointer */ \
} while(0)

/*
 * UDP Endpoint Implementation Structure
 *
 * Encapsulates MacTCP UDP stream state and associated resources.
 * Each UDP endpoint maintains its own receive buffer and stream handle.
 * This design allows multiple concurrent UDP operations (discovery + messaging).
 *
 * Memory management: recvBuffer is allocated if not provided by caller,
 * and automatically freed when endpoint is released.
 */
typedef struct {
    StreamPtr stream;           /* MacTCP UDP stream handle */
    udp_port localPort;         /* Local port number (host byte order) */
    Ptr recvBuffer;             /* Receive buffer (may be app-provided or allocated) */
    unsigned short bufferSize;  /* Size of receive buffer in bytes */
    Boolean isCreated;          /* Track creation state for cleanup */
} MacTCPUDPEndpoint;

/*
 * Async Operation Tracking Structure
 *
 * MacTCP async operations use Parameter Blocks (iopb) that remain active
 * until completion. We must track these to:
 * 1. Poll completion status (ioResult field)
 * 2. Manage associated resources (WDS arrays, buffers)
 * 3. Prevent memory leaks on early termination
 *
 * Design follows MacTCP Programmer's Guide Chapter 4 async patterns.
 */
typedef struct {
    UDPiopb pb;                  /* MacTCP parameter block - must remain valid until completion */
    Boolean inUse;               /* Track allocation state */
    UDPEndpointRef endpoint;     /* Associated endpoint for context */
    Boolean isReturnBuffer;      /* Operation type: true=buffer return, false=receive */
    Boolean isSend;              /* Operation type: true=send, false=receive */
    wdsEntry *wdsArray;          /* WDS allocation for send ops (needs cleanup) */
} MacTCPAsyncOp;


/* TCP async operation tracking */
typedef enum {
    TCP_ASYNC_CONNECT,
    TCP_ASYNC_SEND,
    TCP_ASYNC_RECEIVE,
    TCP_ASYNC_CLOSE,
    TCP_ASYNC_LISTEN
} TCPAsyncOpType;

typedef struct {
    TCPiopb pb;
    Boolean inUse;
    StreamPtr stream;
    TCPAsyncOpType opType;
    Ptr dataBuffer;           /* For send/receive operations */
    unsigned short dataLength;
    rdsEntry *rdsArray;       /* For receive operations */
    short rdsCount;
} TCPAsyncOp;

/*
 * Async Operation Pool Configuration
 *
 * Pool sizes chosen based on expected concurrent operations:
 * - UDP: Discovery broadcasts + occasional direct messages = 4 operations
 * - TCP: 1 listen + 4 pool connections + 3 buffer operations = 8 operations
 *
 * Static allocation avoids fragmentation in Classic Mac's non-virtual memory.
 * Alternative: Dynamic allocation with NewPtr, but adds complexity.
 */
#define MAX_ASYNC_OPS 4          /* UDP operations pool size */
#define MAX_TCP_ASYNC_OPS 8      /* TCP operations pool size */

/* Global operation pools - static allocation for predictable memory usage */
static MacTCPAsyncOp gAsyncOps[MAX_ASYNC_OPS];
static TCPAsyncOp gTCPAsyncOps[MAX_TCP_ASYNC_OPS];

/* Initialization flags - ensure pools are ready before use */
static Boolean gAsyncOpsInitialized = false;
static Boolean gTCPAsyncOpsInitialized = false;

/* Helper functions */
static void InitializeAsyncOps(void)
{
    if (!gAsyncOpsInitialized) {
        memset(gAsyncOps, 0, sizeof(gAsyncOps));
        gAsyncOpsInitialized = true;
    }
}

static void InitializeTCPAsyncOps(void)
{
    if (!gTCPAsyncOpsInitialized) {
        memset(gTCPAsyncOps, 0, sizeof(gTCPAsyncOps));
        gTCPAsyncOpsInitialized = true;
    }
}

static MacTCPAsyncHandle AllocateAsyncHandle(void)
{
    int i;

    InitializeAsyncOps();

    for (i = 0; i < MAX_ASYNC_OPS; i++) {
        if (!gAsyncOps[i].inUse) {
            gAsyncOps[i].inUse = true;
            return (MacTCPAsyncHandle)&gAsyncOps[i];
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "AllocateAsyncHandle: No free async operation slots");
    return NULL;
}

static void FreeAsyncHandle(MacTCPAsyncHandle handle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)handle;

    if (op >= &gAsyncOps[0] && op < &gAsyncOps[MAX_ASYNC_OPS]) {
        /* Free any allocated WDS for send operations */
        if (op->isSend && op->wdsArray != NULL) {
            DisposePtr((Ptr)op->wdsArray);
            op->wdsArray = NULL;
        }
        op->inUse = false;
        op->endpoint = NULL;
        op->isReturnBuffer = false;
        op->isSend = false;
    }
}

static MacTCPAsyncHandle AllocateTCPAsyncHandle(void)
{
    int i;

    InitializeTCPAsyncOps();

    for (i = 0; i < MAX_TCP_ASYNC_OPS; i++) {
        if (!gTCPAsyncOps[i].inUse) {
            gTCPAsyncOps[i].inUse = true;
            return (MacTCPAsyncHandle)&gTCPAsyncOps[i];
        }
    }

    log_debug_cat(LOG_CAT_NETWORKING, "AllocateTCPAsyncHandle: No free TCP async operation slots");
    return NULL;
}

static void FreeTCPAsyncHandle(MacTCPAsyncHandle handle)
{
    TCPAsyncOp *op = (TCPAsyncOp *)handle;

    if (op >= &gTCPAsyncOps[0] && op < &gTCPAsyncOps[MAX_TCP_ASYNC_OPS]) {
        op->inUse = false;
        op->stream = NULL;
        op->dataBuffer = NULL;
        op->dataLength = 0;
        op->rdsArray = NULL;
        op->rdsCount = 0;
    }
}





static MacTCPUDPEndpoint *AllocateUDPEndpoint(void)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)NewPtrClear(sizeof(MacTCPUDPEndpoint));
    if (endpoint == NULL) {
        log_debug_cat(LOG_CAT_NETWORKING, "AllocateUDPEndpoint: Failed to allocate memory");
    }
    return endpoint;
}

static void FreeUDPEndpoint(MacTCPUDPEndpoint *endpoint)
{
    if (endpoint != NULL) {
        if (endpoint->recvBuffer != NULL) {
            DisposePtr(endpoint->recvBuffer);
        }
        DisposePtr((Ptr)endpoint);
    }
}

/* Helper functions for TCP async operations */
static TCPAsyncOp *setup_tcp_async_operation(MacTCPAsyncHandle *handle,
        StreamPtr stream,
        TCPAsyncOpType type)
{
    TCPAsyncOp *op;

    if (!handle) return NULL;

    *handle = AllocateTCPAsyncHandle();
    if (*handle == NULL) {
        return NULL;
    }

    op = (TCPAsyncOp *)*handle;
    op->stream = stream;
    op->opType = type;

    return op;
}

static OSErr finalize_tcp_async_operation(TCPAsyncOp *op, OSErr err,
        MacTCPAsyncHandle *handle,
        const char *operation_name)
{
    (void)op;  /* Currently unused, but kept for potential future use */

    if (err != noErr && handle && *handle) {
        FreeTCPAsyncHandle(*handle);
        *handle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "%s: PBControlAsync failed: %d",
                      operation_name, err);
    }
    return err;
}

/* Implementation of network operations for MacTCP */

/*
 * Initialize MacTCP driver and networking subsystem
 *
 * Per MacTCP Programmer's Guide Chapter 2, initialization sequence:
 * 1. Open MacTCP driver (.IPP)
 * 2. Get local IP address using ipctlGetAddr control call
 * 3. Initialize DNR (Domain Name Resolver)
 * 4. Set up async operation pools
 *
 * Returns: noErr on success, MacTCP error codes on failure
 * Common errors: -23 (fnOpnErr) if driver not found
 *                -192 (resNotFound) if MacTCP not installed
 */
OSErr MacTCPImpl_Initialize(short *refNum, ip_addr *localIP, char *localIPStr)
{
    OSErr err;
    ParamBlockRec pbOpen;    /* Parameter block for PBOpen */
    CntrlParam cntrlPB;      /* Parameter block for PBControl */

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Initialize: Opening MacTCP driver");

    /* Open MacTCP driver (.IPP)
     * Uses Device Manager PBOpen call with driver name ".IPP"
     * This establishes communication path to MacTCP stack */
    memset(&pbOpen, 0, sizeof(ParamBlockRec));
    pbOpen.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;  /* ".IPP" driver name */
    pbOpen.ioParam.ioPermssn = fsCurPerm;                  /* Current permission level */

    err = PBOpenSync(&pbOpen);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to open MacTCP driver: %d", err);
        return err;
    }

    *refNum = pbOpen.ioParam.ioRefNum;
    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Initialize: MacTCP driver opened, refNum: %d", *refNum);

    /* Get local IP address using MacTCP control call
     * ipctlGetAddr returns the machine's current IP configuration
     * in network byte order (big-endian) */
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = *refNum;
    cntrlPB.csCode = ipctlGetAddr;  /* MacTCP control code for "get IP address" */

    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to get IP address: %d", err);
        return err;
    }

    BlockMoveData(&cntrlPB.csParam[0], localIP, sizeof(ip_addr));

    /* Initialize DNR (Domain Name Resolver)
     * DNR provides hostname-to-IP resolution services
     * NULL parameter uses default resolver configuration
     * Critical for any hostname-based networking */
    err = OpenResolver(NULL);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to open resolver: %d", err);
        return err;
    }

    /* Convert IP to string */
    err = AddrToStr(*localIP, localIPStr);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Initialize: AddrToStr failed: %d", err);
        /* Fallback formatting */
        sprintf(localIPStr, "%lu.%lu.%lu.%lu",
                (*localIP >> 24) & 0xFF,
                (*localIP >> 16) & 0xFF,
                (*localIP >> 8) & 0xFF,
                *localIP & 0xFF);
    }

    /* Initialize async operations */
    InitializeAsyncOps();
    InitializeTCPAsyncOps();

    log_app_event("MacTCPImpl_Initialize: Success. Local IP: %s", localIPStr);
    return noErr;
}

void MacTCPImpl_Shutdown(short refNum)
{
    (void)refNum; /* Unused parameter */

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Shutdown: Closing resolver");
    CloseResolver();

    /* Note: We don't close the MacTCP driver as other apps may be using it */
    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_Shutdown: Complete (driver remains open for system)");
}

/*
 * Stream Notification Callback Management
 *
 * MacTCP limitation: ASR (Asynchronous Service Routine) callbacks only receive
 * the StreamPtr parameter, but our application needs different handlers for
 * different stream types (listen, send pool, discovery).
 *
 * Solution: Maintain a mapping table of StreamPtr -> application callback.
 * The global MacTCPNotifyWrapper() dispatches to the correct handler.
 *
 * Size calculation:
 * - 1 listen stream
 * - 4 connection pool streams
 * - 1 discovery stream
 * - 2 extra for safety margin
 * = 8 total stream notifiers
 */
#define MAX_STREAM_NOTIFIERS 8

typedef struct {
    StreamPtr stream;                /* MacTCP stream handle (unique identifier) */
    NetworkNotifyProcPtr notifyProc; /* Application callback for this stream */
} StreamNotifierEntry;

/* Global notification dispatch table */
static StreamNotifierEntry gStreamNotifiers[MAX_STREAM_NOTIFIERS];
static int gStreamNotifierCount = 0;

/* Register a stream's notify proc */
static void RegisterStreamNotifier(StreamPtr stream, NetworkNotifyProcPtr notifyProc)
{
    int i;

    /* Check if stream already registered (update case) */
    for (i = 0; i < gStreamNotifierCount; i++) {
        if (gStreamNotifiers[i].stream == stream) {
            gStreamNotifiers[i].notifyProc = notifyProc;
            return;
        }
    }

    /* Add new entry if room available */
    if (gStreamNotifierCount < MAX_STREAM_NOTIFIERS) {
        gStreamNotifiers[gStreamNotifierCount].stream = stream;
        gStreamNotifiers[gStreamNotifierCount].notifyProc = notifyProc;
        gStreamNotifierCount++;
    }
}

/* Pascal wrapper that dispatches to correct C notify proc based on stream */
static pascal void MacTCPNotifyWrapper(StreamPtr tcpStream, unsigned short eventCode,
                                       Ptr userDataPtr, unsigned short terminReason,
                                       ICMPReport *icmpMsg)
{
    int i;

    /* Find the notify proc for this stream */
    for (i = 0; i < gStreamNotifierCount; i++) {
        if (gStreamNotifiers[i].stream == tcpStream) {
            if (gStreamNotifiers[i].notifyProc) {
                gStreamNotifiers[i].notifyProc((void *)tcpStream, eventCode, userDataPtr,
                                               terminReason, (struct ICMPReport *)icmpMsg);
            }
            return;
        }
    }

    /* Stream not found - this shouldn't happen but log it */
    log_warning_cat(LOG_CAT_NETWORKING, "MacTCPNotifyWrapper: Unknown stream 0x%lX",
                   (unsigned long)tcpStream);
}

OSErr MacTCPImpl_TCPCreate(short refNum, StreamPtr *streamRef,
                                  unsigned long rcvBufferSize, Ptr rcvBuffer,
                                  NetworkNotifyProcPtr notifyProc)
{
    TCPiopb pb;
    OSErr err;

    /* Don't store globally - will register after stream is created */

    memset(&pb, 0, sizeof(TCPiopb));
    pb.ioCRefNum = refNum;
    pb.csCode = TCPCreate;
    pb.csParam.create.rcvBuff = rcvBuffer;
    pb.csParam.create.rcvBuffLen = rcvBufferSize;
    pb.csParam.create.notifyProc = MacTCPNotifyWrapper;

    err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr) {
        *streamRef = (StreamPtr)pb.tcpStream;
        /* Register this stream's notify proc for dispatch */
        RegisterStreamNotifier(*streamRef, notifyProc);
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPCreate: Created stream 0x%lX (registered notifier)",
                     (unsigned long)*streamRef);
    } else {
        *streamRef = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPCreate: Failed: %d", err);
    }

    return err;
}

OSErr MacTCPImpl_TCPRelease(short refNum, StreamPtr streamRef)
{
    TCPiopb pb;

    memset(&pb, 0, sizeof(TCPiopb));
    pb.ioCRefNum = refNum;
    pb.csCode = TCPRelease;
    pb.tcpStream = (StreamPtr)streamRef;

    return PBControlSync((ParmBlkPtr)&pb);
}

OSErr MacTCPImpl_TCPListen(StreamPtr streamRef, tcp_port localPort,
                                  Byte timeout, Boolean async)
{
    TCPiopb pb;
    OSErr err;

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPPassiveOpen;
    pb.csParam.open.ulpTimeoutValue = 20; /* Default ULP timeout */
    pb.csParam.open.ulpTimeoutAction = 1;
    pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = timeout;
    pb.csParam.open.localPort = localPort;
    pb.csParam.open.localHost = 0;
    pb.csParam.open.remoteHost = 0;
    pb.csParam.open.remotePort = 0;

    if (async) {
        pb.ioCompletion = nil;
        pb.ioCRefNum = gMacTCPRefNum;
        pb.ioResult = 1;
        err = PBControlAsync((ParmBlkPtr)&pb);
    } else {
        err = PBControlSync((ParmBlkPtr)&pb);
    }

    return err;
}

/* Async TCP Listen implementation */
OSErr MacTCPImpl_TCPListenAsync(StreamPtr streamRef, tcp_port localPort,
                                       MacTCPAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;

    if (streamRef == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Setup async operation */
    op = setup_tcp_async_operation(asyncHandle, streamRef, TCP_ASYNC_LISTEN);
    if (!op) {
        return memFullErr;
    }

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPPassiveOpen;
    op->pb.csParam.open.ulpTimeoutValue = 20; /* Default ULP timeout */
    op->pb.csParam.open.ulpTimeoutAction = 1;
    op->pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    op->pb.csParam.open.commandTimeoutValue = 0; /* Non-blocking */
    op->pb.csParam.open.localPort = localPort;
    op->pb.csParam.open.localHost = 0;
    op->pb.csParam.open.remoteHost = 0;
    op->pb.csParam.open.remotePort = 0;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    err = finalize_tcp_async_operation(op, err, asyncHandle, "MacTCPImpl_TCPListenAsync");

    if (err == noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPListenAsync: Started async listen on port %u", localPort);
    }

    return err;
}

OSErr MacTCPImpl_TCPConnect(StreamPtr streamRef, ip_addr remoteHost,
                                   tcp_port remotePort, Byte timeout,
                                   NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPActiveOpen;
    pb.csParam.open.ulpTimeoutValue = timeout ? timeout : 30;
    pb.csParam.open.ulpTimeoutAction = 1;
    pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.open.remoteHost = remoteHost;
    pb.csParam.open.remotePort = remotePort;
    pb.csParam.open.localPort = 0; /* Let MacTCP assign a random ephemeral port */
    pb.csParam.open.localHost = 0;
    pb.csParam.open.commandTimeoutValue = timeout;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    return PBControlSync((ParmBlkPtr)&pb);
}

/* Async TCP Connect implementation */
OSErr MacTCPImpl_TCPConnectAsync(StreamPtr streamRef, ip_addr remoteHost,
                                        tcp_port remotePort, MacTCPAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;

    if (streamRef == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Setup async operation */
    op = setup_tcp_async_operation(asyncHandle, streamRef, TCP_ASYNC_CONNECT);
    if (!op) {
        return memFullErr;
    }

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPActiveOpen;
    /* Per MacTCP Programmer's Guide p.2832: ulpTimeoutValue is when connection fails
     * For LAN connections, 3 seconds is reasonable - much shorter than default 30s
     * This prevents pool entries from blocking for extended periods */
    op->pb.csParam.open.ulpTimeoutValue = 3;  /* 3 second timeout for LAN */
    op->pb.csParam.open.ulpTimeoutAction = 1;  /* 1 = abort on timeout */
    op->pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
    op->pb.csParam.open.remoteHost = remoteHost;
    op->pb.csParam.open.remotePort = remotePort;
    op->pb.csParam.open.localPort = 0; /* Let MacTCP assign port */
    op->pb.csParam.open.localHost = 0;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    err = finalize_tcp_async_operation(op, err, asyncHandle, "MacTCPImpl_TCPConnectAsync");

    if (err == noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPConnectAsync: Started async connect to %lu:%u",
                      remoteHost, remotePort);
    }

    return err;
}

OSErr MacTCPImpl_TCPSend(StreamPtr streamRef, Ptr data, unsigned short length,
                                Boolean push, Byte timeout, NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    wdsEntry wds[2];
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL || data == NULL) {
        return paramErr;
    }

    /* Set up WDS */
    SETUP_SINGLE_WDS(wds, data, length);

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPSend;
    pb.csParam.send.ulpTimeoutValue = timeout ? timeout : 30;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag = push;
    pb.csParam.send.urgentFlag = false;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    return PBControlSync((ParmBlkPtr)&pb);
}

/* Async TCP Send implementation */
OSErr MacTCPImpl_TCPSendAsync(StreamPtr streamRef, Ptr data, unsigned short length,
                                     Boolean push, MacTCPAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;
    wdsEntry *wds;

    if (streamRef == NULL || data == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Allocate async handle */
    *asyncHandle = AllocateTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (TCPAsyncOp *)*asyncHandle;
    op->stream = streamRef;
    op->opType = TCP_ASYNC_SEND;
    op->dataBuffer = data;
    op->dataLength = length;

    /* Allocate WDS array - needs to persist for async operation */
    wds = (wdsEntry *)NewPtrClear(sizeof(wdsEntry) * 2);
    if (wds == NULL) {
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        return memFullErr;
    }

    /* Set up WDS */
    SETUP_SINGLE_WDS(wds, data, length);

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPSend;
    op->pb.csParam.send.ulpTimeoutValue = 30;  /* Default timeout */
    op->pb.csParam.send.ulpTimeoutAction = 1;
    op->pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    op->pb.csParam.send.pushFlag = push;
    op->pb.csParam.send.urgentFlag = false;
    op->pb.csParam.send.wdsPtr = (Ptr)wds;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Store WDS pointer for cleanup */
    op->rdsArray = (rdsEntry *)wds;  /* Reuse rdsArray field */

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        DisposePtr((Ptr)wds);
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPSendAsync: PBControlAsync failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPSendAsync: Started async send of %u bytes", length);
    return noErr;
}

OSErr MacTCPImpl_TCPReceiveNoCopy(StreamPtr streamRef, Ptr rdsPtr,
        short maxEntries, Byte timeout,
        Boolean *urgent, Boolean *mark,
        NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    OSErr err;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL || rdsPtr == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPNoCopyRcv;
    pb.csParam.receive.commandTimeoutValue = timeout;
    pb.csParam.receive.rdsPtr = rdsPtr;
    pb.csParam.receive.rdsLength = maxEntries;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    err = PBControlSync((ParmBlkPtr)&pb);

    if (err == noErr) {
        if (urgent) *urgent = pb.csParam.receive.urgentFlag;
        if (mark) *mark = pb.csParam.receive.markFlag;
    }

    return err;
}

/* Async TCP Receive implementation */
OSErr MacTCPImpl_TCPReceiveAsync(StreamPtr streamRef, Ptr rdsPtr,
                                        short maxEntries, MacTCPAsyncHandle *asyncHandle)
{
    TCPAsyncOp *op;
    OSErr err;

    if (streamRef == NULL || rdsPtr == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    /* Allocate async handle */
    *asyncHandle = AllocateTCPAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (TCPAsyncOp *)*asyncHandle;
    op->stream = streamRef;
    op->opType = TCP_ASYNC_RECEIVE;
    op->rdsArray = (rdsEntry *)rdsPtr;
    op->rdsCount = maxEntries;

    /* Set up parameter block */
    memset(&op->pb, 0, sizeof(TCPiopb));
    op->pb.tcpStream = (StreamPtr)streamRef;
    op->pb.csCode = TCPNoCopyRcv;
    op->pb.csParam.receive.commandTimeoutValue = 0;  /* Non-blocking */
    op->pb.csParam.receive.rdsPtr = rdsPtr;
    op->pb.csParam.receive.rdsLength = maxEntries;
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.ioResult = 1;

    /* Start async operation */
    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeTCPAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPReceiveAsync: PBControlAsync failed: %d", err);
        return err;
    }

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPReceiveAsync: Started async receive");
    return noErr;
}

OSErr MacTCPImpl_TCPReturnBuffer(StreamPtr streamRef, Ptr rdsPtr,
                                        NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL || rdsPtr == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPRcvBfrReturn;
    pb.csParam.receive.rdsPtr = rdsPtr;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call */
    return PBControlSync((ParmBlkPtr)&pb);
}

OSErr MacTCPImpl_TCPClose(StreamPtr streamRef, Byte timeout,
                                 NetworkGiveTimeProcPtr giveTime)
{
    TCPiopb pb;
    (void)giveTime; /* Unused parameter */

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPClose;
    pb.csParam.close.ulpTimeoutValue = timeout ? timeout : 30;
    pb.csParam.close.ulpTimeoutAction = 1;
    pb.csParam.close.validityFlags = timeoutValue | timeoutAction;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call - blocks until complete or timeout */
    return PBControlSync((ParmBlkPtr)&pb);
}

OSErr MacTCPImpl_TCPAbort(StreamPtr streamRef)
{
    TCPiopb pb;

    if (streamRef == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPAbort;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* For abort, we always use sync to ensure immediate effect */
    return PBControlSync((ParmBlkPtr)&pb);
}

OSErr MacTCPImpl_TCPStatus(StreamPtr streamRef, NetworkTCPInfo *info)
{
    TCPiopb pb;
    OSErr err;

    if (streamRef == NULL || info == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPStatus;
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;

    /* Use synchronous call */
    err = PBControlSync((ParmBlkPtr)&pb);

    if (err == noErr) {
        info->localHost = pb.csParam.status.localHost;
        info->localPort = pb.csParam.status.localPort;
        info->remoteHost = pb.csParam.status.remoteHost;
        info->remotePort = pb.csParam.status.remotePort;
        info->connectionState = pb.csParam.status.connectionState;
        info->isConnected = (pb.csParam.status.connectionState >= 8); /* established */
        info->isListening = (pb.csParam.status.connectionState == 2); /* listening */
    }

    return err;
}

/* UDP Operations */

OSErr MacTCPImpl_UDPCreate(short refNum, UDPEndpointRef *endpointRef,
                                  udp_port localPort, Ptr recvBuffer,
                                  unsigned short bufferSize)
{
    OSErr err;
    UDPiopb pb;
    MacTCPUDPEndpoint *endpoint;

    if (endpointRef == NULL) {
        return paramErr;
    }

    /* Allocate endpoint structure */
    endpoint = AllocateUDPEndpoint();
    if (endpoint == NULL) {
        return memFullErr;
    }

    /* If no buffer provided, allocate one */
    if (recvBuffer == NULL) {
        endpoint->recvBuffer = NewPtrClear(bufferSize);
        if (endpoint->recvBuffer == NULL) {
            FreeUDPEndpoint(endpoint);
            return memFullErr;
        }
    } else {
        endpoint->recvBuffer = recvBuffer;
    }

    endpoint->bufferSize = bufferSize;
    endpoint->localPort = localPort;

    /* Create UDP endpoint */
    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = refNum;
    pb.csCode = UDPCreate;
    pb.csParam.create.rcvBuff = endpoint->recvBuffer;
    pb.csParam.create.rcvBuffLen = bufferSize;
    pb.csParam.create.notifyProc = nil;
    pb.csParam.create.localPort = localPort;

    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPCreate: Failed: %d", err);
        FreeUDPEndpoint(endpoint);
        return err;
    }

    /* Store the actual stream pointer returned by MacTCP */
    endpoint->stream = pb.udpStream;
    endpoint->isCreated = true;
    *endpointRef = (UDPEndpointRef)endpoint;

    log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPCreate: Success. Stream: 0x%lX (endpoint: 0x%lX), Port: %u",
                  (unsigned long)endpoint->stream, (unsigned long)endpoint, pb.csParam.create.localPort);

    return noErr;
}

OSErr MacTCPImpl_UDPRelease(short refNum, UDPEndpointRef endpointRef)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;
    OSErr err;

    if (endpoint == NULL || !endpoint->isCreated) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = refNum;
    pb.csCode = UDPRelease;
    pb.udpStream = endpoint->stream;

    err = PBControlSync((ParmBlkPtr)&pb);

    /* Free endpoint regardless of error */
    endpoint->isCreated = false;
    FreeUDPEndpoint(endpoint);

    return err;
}

OSErr MacTCPImpl_UDPSend(UDPEndpointRef endpointRef, ip_addr remoteHost,
                                udp_port remotePort, Ptr data, unsigned short length)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;
    wdsEntry wds[2];

    if (endpoint == NULL || !endpoint->isCreated || data == NULL) {
        return paramErr;
    }

    /* Set up WDS */
    SETUP_SINGLE_WDS(wds, data, length);

    /* Send datagram */
    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum; /* Use global ref */
    pb.csCode = UDPWrite;
    pb.udpStream = endpoint->stream;
    pb.csParam.send.remoteHost = remoteHost;
    pb.csParam.send.remotePort = remotePort;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    pb.csParam.send.checkSum = true;

    return PBControlSync((ParmBlkPtr)&pb);
}

OSErr MacTCPImpl_UDPReceive(UDPEndpointRef endpointRef, ip_addr *remoteHost,
                                   udp_port *remotePort, Ptr buffer,
                                   unsigned short *length, Boolean async)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;
    OSErr err;

    if (endpoint == NULL || !endpoint->isCreated || buffer == NULL || length == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;
    pb.csCode = UDPRead;
    pb.udpStream = endpoint->stream;
    pb.csParam.receive.rcvBuff = buffer;
    pb.csParam.receive.rcvBuffLen = *length;
    pb.csParam.receive.timeOut = async ? 0 : 1; /* 0 for async, 1 sec for sync */

    if (async) {
        err = PBControlAsync((ParmBlkPtr)&pb);
        if (err == noErr) {
            /* For async, caller needs to poll ioResult */
            return 1; /* Indicate pending */
        }
    } else {
        err = PBControlSync((ParmBlkPtr)&pb);
    }

    if (err == noErr) {
        if (remoteHost) *remoteHost = pb.csParam.receive.remoteHost;
        if (remotePort) *remotePort = pb.csParam.receive.remotePort;
        *length = pb.csParam.receive.rcvBuffLen;
    }

    return err;
}

OSErr MacTCPImpl_UDPReturnBuffer(UDPEndpointRef endpointRef, Ptr buffer,
                                        unsigned short bufferSize, Boolean async)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    UDPiopb pb;

    if (endpoint == NULL || !endpoint->isCreated || buffer == NULL) {
        return paramErr;
    }

    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;
    pb.ioCRefNum = gMacTCPRefNum;
    pb.csCode = UDPBfrReturn;
    pb.udpStream = endpoint->stream;
    pb.csParam.receive.rcvBuff = buffer;
    pb.csParam.receive.rcvBuffLen = bufferSize;

    if (async) {
        return PBControlAsync((ParmBlkPtr)&pb);
    } else {
        return PBControlSync((ParmBlkPtr)&pb);
    }
}

/* Async UDP operations */

OSErr MacTCPImpl_UDPSendAsync(UDPEndpointRef endpointRef, ip_addr remoteHost,
                                     udp_port remotePort, Ptr data, unsigned short length,
                                     MacTCPAsyncHandle *asyncHandle)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    MacTCPAsyncOp *op;
    OSErr err;
    wdsEntry *wds;

    if (endpoint == NULL || !endpoint->isCreated || data == NULL || asyncHandle == NULL) {
        return paramErr;
    }

    *asyncHandle = AllocateAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (MacTCPAsyncOp *)*asyncHandle;
    op->endpoint = endpointRef;
    op->isReturnBuffer = false;
    op->isSend = true;

    /* Allocate WDS array - needs to persist for async operation */
    wds = (wdsEntry *)NewPtrClear(sizeof(wdsEntry) * 2);
    if (wds == NULL) {
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        return memFullErr;
    }

    /* Set up WDS */
    SETUP_SINGLE_WDS(wds, data, length);

    /* Store WDS for cleanup */
    op->wdsArray = wds;

    /* Set up async UDP write */
    memset(&op->pb, 0, sizeof(UDPiopb));
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.csCode = UDPWrite;
    op->pb.udpStream = endpoint->stream;
    op->pb.csParam.send.remoteHost = remoteHost;
    op->pb.csParam.send.remotePort = remotePort;
    op->pb.csParam.send.wdsPtr = (Ptr)wds;
    op->pb.csParam.send.checkSum = true;
    op->pb.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        DisposePtr((Ptr)wds);
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPSendAsync: PBControlAsync failed: %d", err);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPSendAsync: Started async send of %u bytes to %lu:%u",
                      length, remoteHost, remotePort);
    }

    return err;
}

OSErr MacTCPImpl_UDPCheckSendStatus(MacTCPAsyncHandle asyncHandle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse || !op->isSend) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    /* Operation complete - free the handle (which also frees WDS) */
    FreeAsyncHandle(asyncHandle);

    return ioResult;
}

OSErr MacTCPImpl_UDPReceiveAsync(UDPEndpointRef endpointRef,
                                        MacTCPAsyncHandle *asyncHandle)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    MacTCPAsyncOp *op;
    OSErr err;

    if (!endpoint || !endpoint->isCreated || !asyncHandle) {
        return paramErr;
    }

    *asyncHandle = AllocateAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (MacTCPAsyncOp *)*asyncHandle;
    op->endpoint = endpointRef;
    op->isReturnBuffer = false;

    /* Set up async UDP read */
    memset(&op->pb, 0, sizeof(UDPiopb));
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.csCode = UDPRead;
    op->pb.udpStream = endpoint->stream;
    op->pb.csParam.receive.rcvBuff = endpoint->recvBuffer;
    op->pb.csParam.receive.rcvBuffLen = endpoint->bufferSize;
    op->pb.csParam.receive.timeOut = 0; /* Non-blocking */
    op->pb.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReceiveAsync: PBControlAsync failed: %d", err);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReceiveAsync: Started async read");
    }

    return err;
}

OSErr MacTCPImpl_UDPCheckAsyncStatus(MacTCPAsyncHandle asyncHandle,
        ip_addr *remoteHost, udp_port *remotePort,
        Ptr *dataPtr, unsigned short *dataLength)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    if (ioResult == noErr) {
        if (remoteHost) *remoteHost = op->pb.csParam.receive.remoteHost;
        if (remotePort) *remotePort = op->pb.csParam.receive.remotePort;
        if (dataPtr) *dataPtr = op->pb.csParam.receive.rcvBuff;
        if (dataLength) *dataLength = op->pb.csParam.receive.rcvBuffLen;
    }

    /* Mark as available */
    FreeAsyncHandle(asyncHandle);

    return ioResult;
}

OSErr MacTCPImpl_UDPReturnBufferAsync(UDPEndpointRef endpointRef,
        Ptr buffer, unsigned short bufferSize,
        MacTCPAsyncHandle *asyncHandle)
{
    MacTCPUDPEndpoint *endpoint = (MacTCPUDPEndpoint *)endpointRef;
    MacTCPAsyncOp *op;
    OSErr err;

    if (!endpoint || !endpoint->isCreated || !buffer || !asyncHandle) {
        return paramErr;
    }

    *asyncHandle = AllocateAsyncHandle();
    if (*asyncHandle == NULL) {
        return memFullErr;
    }

    op = (MacTCPAsyncOp *)*asyncHandle;
    op->endpoint = endpointRef;
    op->isReturnBuffer = true;

    /* Set up async buffer return */
    memset(&op->pb, 0, sizeof(UDPiopb));
    op->pb.ioCompletion = nil;
    op->pb.ioCRefNum = gMacTCPRefNum;
    op->pb.csCode = UDPBfrReturn;
    op->pb.udpStream = endpoint->stream;
    op->pb.csParam.receive.rcvBuff = buffer;
    op->pb.csParam.receive.rcvBuffLen = bufferSize;
    op->pb.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&op->pb);
    if (err != noErr) {
        FreeAsyncHandle(*asyncHandle);
        *asyncHandle = NULL;
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReturnBufferAsync: PBControlAsync failed: %d", err);
    } else {
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPReturnBufferAsync: Started async buffer return");
    }

    return err;
}

OSErr MacTCPImpl_UDPCheckReturnStatus(MacTCPAsyncHandle asyncHandle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse || !op->isReturnBuffer) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    /* Mark as available */
    FreeAsyncHandle(asyncHandle);

    return ioResult;
}

void MacTCPImpl_UDPCancelAsync(MacTCPAsyncHandle asyncHandle)
{
    MacTCPAsyncOp *op = (MacTCPAsyncOp *)asyncHandle;

    if (op && op->inUse) {
        /* Note: MacTCP doesn't provide a way to cancel async operations */
        /* We just mark it as free and let it complete in the background */
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_UDPCancelAsync: Marking handle as free (can't cancel MacTCP async)");
        FreeAsyncHandle(asyncHandle);
    }
}

OSErr MacTCPImpl_ResolveAddress(const char *hostname, ip_addr *address)
{
    /* For now, just try to parse as IP address */
    return ParseIPv4(hostname, address);
}

OSErr MacTCPImpl_AddressToString(ip_addr address, char *addressStr)
{
    return AddrToStr(address, addressStr);
}

const char *MacTCPImpl_GetImplementationName(void)
{
    return "MacTCP";
}

Boolean MacTCPImpl_IsAvailable(void)
{
    /* Check if MacTCP driver can be opened */
    ParamBlockRec pb;
    OSErr err;

    memset(&pb, 0, sizeof(ParamBlockRec));
    pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pb.ioParam.ioPermssn = fsCurPerm;

    err = PBOpenSync(&pb);
    if (err == noErr) {
        /* Driver opened, close it immediately */
        pb.ioParam.ioRefNum = pb.ioParam.ioRefNum;
        PBCloseSync(&pb);
        return true;
    }

    /* If already open, that's OK too */
    return (err == opWrErr);
}

/* TCP Async Status Checking */
OSErr MacTCPImpl_TCPCheckAsyncStatus(MacTCPAsyncHandle asyncHandle,
        OSErr *operationResult, void **resultData)
{
    TCPAsyncOp *op = (TCPAsyncOp *)asyncHandle;
    OSErr ioResult;

    if (!op || !op->inUse || !operationResult) {
        return paramErr;
    }

    ioResult = op->pb.ioResult;
    if (ioResult > 0) {
        return 1; /* Still pending */
    }

    /* Operation complete */
    *operationResult = ioResult;

    /* Return operation-specific data if requested */
    if (resultData) {
        switch (op->opType) {
        case TCP_ASYNC_CONNECT:
            /* For connect, no additional data */
            *resultData = NULL;
            break;

        case TCP_ASYNC_SEND:
            /* For send, return bytes sent (in csParam.send.sendLength) */
            *resultData = (void *)(unsigned long)op->pb.csParam.send.sendLength;
            break;

        case TCP_ASYNC_RECEIVE:
            /* For receive, resultData should point to a structure with urgent/mark flags */
            /* Caller should interpret based on context */
            *resultData = (void *)&op->pb.csParam.receive;
            break;

        case TCP_ASYNC_CLOSE:
            /* For close, no additional data */
            *resultData = NULL;
            break;

        case TCP_ASYNC_LISTEN:
            /* For listen, return pointer to open params containing remote host/port */
            *resultData = (void *)&op->pb.csParam.open;
            break;

        default:
            *resultData = NULL;
            break;
        }
    }

    /* Clean up any allocated resources */
    if (op->opType == TCP_ASYNC_SEND && op->rdsArray) {
        /* Free the WDS array we allocated */
        DisposePtr((Ptr)op->rdsArray);
        op->rdsArray = NULL;
    }

    /* Free the async handle */
    FreeTCPAsyncHandle(asyncHandle);

    return noErr;
}

/* TCP Async Cancel */
void MacTCPImpl_TCPCancelAsync(MacTCPAsyncHandle asyncHandle)
{
    TCPAsyncOp *op = (TCPAsyncOp *)asyncHandle;

    if (op && op->inUse) {
        /* Note: MacTCP doesn't provide a way to cancel async operations */
        /* We just mark it as free and let it complete in the background */
        log_debug_cat(LOG_CAT_NETWORKING, "MacTCPImpl_TCPCancelAsync: Marking handle as free (can't cancel MacTCP async)");

        /* Clean up any allocated resources */
        if (op->opType == TCP_ASYNC_SEND && op->rdsArray) {
            /* Don't free WDS until operation completes */
            /* This is a memory leak risk, but safer than crashing */
        }

        /* Mark as not in use but don't clear the pb - let it complete */
        op->inUse = false;
    }
}

