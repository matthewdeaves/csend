//====================================
// FILE: ./classic_mac/mactcp_impl.c
//====================================

#include "mactcp_impl.h"
#include "mactcp_network.h"  /* Add this to get kTCPDriverName declaration */
#include "../shared/logging.h"
#include <MacTCP.h>
#include <Devices.h>
#include <Errors.h>
#include <Files.h>
#include <string.h>
#include <stdio.h>

/* DNR function declarations */
extern OSErr OpenResolver(char *fileName);
extern OSErr CloseResolver(void);
extern OSErr AddrToStr(unsigned long addr, char *addrStr);
extern OSErr StrToAddr(char *hostName, struct hostInfo *rtnStruct, long resultProc, char *userData);

/* Remove the duplicate definition of kTCPDriverName - it's now in mactcp_network.c */

/* Implementation of network operations for MacTCP */

static OSErr MacTCPImpl_Initialize(short *refNum, ip_addr *localIP, char *localIPStr)
{
    OSErr err;
    ParamBlockRec pbOpen;
    CntrlParam cntrlPB;
    
    log_debug("MacTCPImpl_Initialize: Opening MacTCP driver");
    
    /* Open MacTCP driver */
    memset(&pbOpen, 0, sizeof(ParamBlockRec));
    pbOpen.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pbOpen.ioParam.ioPermssn = fsCurPerm;
    
    err = PBOpenSync(&pbOpen);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to open MacTCP driver: %d", err);
        return err;
    }
    
    *refNum = pbOpen.ioParam.ioRefNum;
    log_debug("MacTCPImpl_Initialize: MacTCP driver opened, refNum: %d", *refNum);
    
    /* Get local IP address */
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = *refNum;
    cntrlPB.csCode = ipctlGetAddr;
    
    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to get IP address: %d", err);
        return err;
    }
    
    BlockMoveData(&cntrlPB.csParam[0], localIP, sizeof(ip_addr));
    
    /* Initialize DNR */
    err = OpenResolver(NULL);
    if (err != noErr) {
        log_app_event("MacTCPImpl_Initialize: Failed to open resolver: %d", err);
        return err;
    }
    
    /* Convert IP to string */
    err = AddrToStr(*localIP, localIPStr);
    if (err != noErr) {
        log_debug("MacTCPImpl_Initialize: AddrToStr failed: %d", err);
        /* Fallback formatting */
        sprintf(localIPStr, "%lu.%lu.%lu.%lu",
                (*localIP >> 24) & 0xFF,
                (*localIP >> 16) & 0xFF,
                (*localIP >> 8) & 0xFF,
                *localIP & 0xFF);
    }
    
    log_app_event("MacTCPImpl_Initialize: Success. Local IP: %s", localIPStr);
    return noErr;
}

static void MacTCPImpl_Shutdown(short refNum)
{
    log_debug("MacTCPImpl_Shutdown: Closing resolver");
    CloseResolver();
    
    /* Note: We don't close the MacTCP driver as other apps may be using it */
    log_debug("MacTCPImpl_Shutdown: Complete (driver remains open for system)");
}

static OSErr MacTCPImpl_TCPCreate(short refNum, NetworkStreamRef *streamRef,
                                 unsigned long rcvBufferSize, Ptr rcvBuffer,
                                 NetworkNotifyProcPtr notifyProc)
{
    TCPiopb pb;
    OSErr err;
    
    memset(&pb, 0, sizeof(TCPiopb));
    pb.ioCRefNum = refNum;
    pb.csCode = TCPCreate;
    pb.csParam.create.rcvBuff = rcvBuffer;
    pb.csParam.create.rcvBuffLen = rcvBufferSize;
    pb.csParam.create.notifyProc = (TCPNotifyProcPtr)notifyProc;
    
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err == noErr) {
        *streamRef = (NetworkStreamRef)pb.tcpStream;
        log_debug("MacTCPImpl_TCPCreate: Created stream 0x%lX", (unsigned long)*streamRef);
    } else {
        *streamRef = NULL;
        log_debug("MacTCPImpl_TCPCreate: Failed: %d", err);
    }
    
    return err;
}

static OSErr MacTCPImpl_TCPRelease(short refNum, NetworkStreamRef streamRef)
{
    TCPiopb pb;
    
    memset(&pb, 0, sizeof(TCPiopb));
    pb.ioCRefNum = refNum;
    pb.csCode = TCPRelease;
    pb.tcpStream = (StreamPtr)streamRef;
    
    return PBControlSync((ParmBlkPtr)&pb);
}

static OSErr MacTCPImpl_TCPStatus(NetworkStreamRef streamRef, NetworkTCPInfo *info)
{
    TCPiopb pb;
    OSErr err;
    
    if (streamRef == NULL || info == NULL) {
        return paramErr;
    }
    
    memset(&pb, 0, sizeof(TCPiopb));
    pb.tcpStream = (StreamPtr)streamRef;
    pb.csCode = TCPStatus;
    
    /* Use async call with immediate poll for safety */
    pb.ioCompletion = nil;
    pb.ioResult = 1;
    
    err = PBControlAsync((ParmBlkPtr)&pb);
    if (err != noErr) {
        return err;
    }
    
    /* Poll for completion */
    while (pb.ioResult > 0) {
        /* Yield time */
    }
    
    err = pb.ioResult;
    if (err == noErr) {
        info->localHost = pb.csParam.status.localHost;
        info->localPort = pb.csParam.status.localPort;
        info->remoteHost = pb.csParam.status.remoteHost;
        info->remotePort = pb.csParam.status.remotePort;
        info->isConnected = (pb.csParam.status.connectionState >= 8); /* established */
        info->isListening = (pb.csParam.status.connectionState == 2); /* listening */
    }
    
    return err;
}

static OSErr MacTCPImpl_AddressToString(ip_addr address, char *addressStr)
{
    return AddrToStr(address, addressStr);
}

static const char* MacTCPImpl_GetImplementationName(void)
{
    return "MacTCP";
}

static Boolean MacTCPImpl_IsAvailable(void)
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

/* TODO: Implement remaining operations - for now, return not implemented */
static OSErr NotImplemented(void)
{
    return unimpErr;
}

/* Static operations table for MacTCP */
static NetworkOperations gMacTCPOperations = {
    /* System operations */
    MacTCPImpl_Initialize,
    MacTCPImpl_Shutdown,
    
    /* TCP operations */
    MacTCPImpl_TCPCreate,
    MacTCPImpl_TCPRelease,
    (void*)NotImplemented,  /* TCPListen - TODO */
    (void*)NotImplemented,  /* TCPConnect - TODO */
    (void*)NotImplemented,  /* TCPSend - TODO */
    (void*)NotImplemented,  /* TCPReceiveNoCopy - TODO */
    (void*)NotImplemented,  /* TCPReturnBuffer - TODO */
    (void*)NotImplemented,  /* TCPClose - TODO */
    (void*)NotImplemented,  /* TCPAbort - TODO */
    MacTCPImpl_TCPStatus,
    
    /* UDP operations */
    (void*)NotImplemented,  /* UDPCreate - TODO */
    (void*)NotImplemented,  /* UDPRelease - TODO */
    (void*)NotImplemented,  /* UDPSend - TODO */
    (void*)NotImplemented,  /* UDPReceive - TODO */
    (void*)NotImplemented,  /* UDPReturnBuffer - TODO */
    
    /* Utility operations */
    (void*)NotImplemented,  /* ResolveAddress - TODO */
    MacTCPImpl_AddressToString,
    
    /* Implementation info */
    MacTCPImpl_GetImplementationName,
    MacTCPImpl_IsAvailable
};

/* Get MacTCP operations table */
NetworkOperations* GetMacTCPOperations(void)
{
    return &gMacTCPOperations;
}