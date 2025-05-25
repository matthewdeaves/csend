//====================================
// FILE: ./classic_mac/network_abstraction.h
//====================================

#ifndef NETWORK_ABSTRACTION_H
#define NETWORK_ABSTRACTION_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"

/* Network implementation types */
typedef enum {
    NETWORK_IMPL_NONE = 0,
    NETWORK_IMPL_MACTCP,
    NETWORK_IMPL_OPENTRANSPORT
} NetworkImplementation;

/* Forward declarations for implementation-specific types */
typedef void *NetworkStreamRef;
typedef void *NetworkEndpointRef;
typedef void *NetworkAsyncHandle;

/* Callback types */
typedef void (*NetworkNotifyProcPtr)(NetworkStreamRef stream, unsigned short eventCode,
                                     Ptr userDataPtr, unsigned short terminReason,
                                     struct ICMPReport *icmpMsg);
typedef void (*NetworkGiveTimeProcPtr)(void);

/* Network operation result codes (implementation-agnostic) */
typedef enum {
    NETWORK_SUCCESS = 0,
    NETWORK_ERROR_NOT_INITIALIZED = -1,
    NETWORK_ERROR_INVALID_PARAM = -2,
    NETWORK_ERROR_NO_MEMORY = -3,
    NETWORK_ERROR_TIMEOUT = -4,
    NETWORK_ERROR_CONNECTION_FAILED = -5,
    NETWORK_ERROR_CONNECTION_CLOSED = -6,
    NETWORK_ERROR_BUSY = -7,
    NETWORK_ERROR_NOT_SUPPORTED = -8,
    NETWORK_ERROR_UNKNOWN = -99
} NetworkError;

/* TCP connection info */
typedef struct {
    ip_addr localHost;
    ip_addr remoteHost;
    tcp_port localPort;
    tcp_port remotePort;
    Boolean isConnected;
    Boolean isListening;
} NetworkTCPInfo;

/* UDP endpoint info */
typedef struct {
    ip_addr localHost;
    udp_port localPort;
    Boolean isBound;
} NetworkUDPInfo;

/* Network operations function table */
typedef struct {
    /* System-level operations */
    OSErr(*Initialize)(short *refNum, ip_addr *localIP, char *localIPStr);
    void (*Shutdown)(short refNum);

    /* TCP operations */
    OSErr(*TCPCreate)(short refNum, NetworkStreamRef *streamRef,
                      unsigned long rcvBufferSize, Ptr rcvBuffer,
                      NetworkNotifyProcPtr notifyProc);
    OSErr(*TCPRelease)(short refNum, NetworkStreamRef streamRef);
    OSErr(*TCPListen)(NetworkStreamRef streamRef, tcp_port localPort,
                      Byte timeout, Boolean async);
    OSErr(*TCPConnect)(NetworkStreamRef streamRef, ip_addr remoteHost,
                       tcp_port remotePort, Byte timeout,
                       NetworkGiveTimeProcPtr giveTime);
    OSErr(*TCPSend)(NetworkStreamRef streamRef, Ptr data, unsigned short length,
                    Boolean push, Byte timeout, NetworkGiveTimeProcPtr giveTime);
    OSErr(*TCPReceiveNoCopy)(NetworkStreamRef streamRef, Ptr rdsPtr,
                             short maxEntries, Byte timeout,
                             Boolean *urgent, Boolean *mark,
                             NetworkGiveTimeProcPtr giveTime);
    OSErr(*TCPReturnBuffer)(NetworkStreamRef streamRef, Ptr rdsPtr,
                            NetworkGiveTimeProcPtr giveTime);
    OSErr(*TCPClose)(NetworkStreamRef streamRef, Byte timeout,
                     NetworkGiveTimeProcPtr giveTime);
    OSErr(*TCPAbort)(NetworkStreamRef streamRef);
    OSErr(*TCPStatus)(NetworkStreamRef streamRef, NetworkTCPInfo *info);

    /* UDP operations */
    OSErr(*UDPCreate)(short refNum, NetworkEndpointRef *endpointRef,
                      udp_port localPort, Ptr recvBuffer,
                      unsigned short bufferSize);
    OSErr(*UDPRelease)(short refNum, NetworkEndpointRef endpointRef);
    OSErr(*UDPSend)(NetworkEndpointRef endpointRef, ip_addr remoteHost,
                    udp_port remotePort, Ptr data, unsigned short length);
    OSErr(*UDPReceive)(NetworkEndpointRef endpointRef, ip_addr *remoteHost,
                       udp_port *remotePort, Ptr buffer,
                       unsigned short *length, Boolean async);
    OSErr(*UDPReturnBuffer)(NetworkEndpointRef endpointRef, Ptr buffer,
                            unsigned short bufferSize, Boolean async);

    /* Async UDP operations */
    OSErr(*UDPReceiveAsync)(NetworkEndpointRef endpointRef,
                            NetworkAsyncHandle *asyncHandle);
    OSErr(*UDPCheckAsyncStatus)(NetworkAsyncHandle asyncHandle,
                                ip_addr *remoteHost, udp_port *remotePort,
                                Ptr *dataPtr, unsigned short *dataLength);
    OSErr(*UDPReturnBufferAsync)(NetworkEndpointRef endpointRef,
                                 Ptr buffer, unsigned short bufferSize,
                                 NetworkAsyncHandle *asyncHandle);
    OSErr(*UDPCheckReturnStatus)(NetworkAsyncHandle asyncHandle);
    void (*UDPCancelAsync)(NetworkAsyncHandle asyncHandle);

    /* Utility operations */
    OSErr(*ResolveAddress)(const char *hostname, ip_addr *address);
    OSErr(*AddressToString)(ip_addr address, char *addressStr);

    /* Implementation info */
    const char *(*GetImplementationName)(void);
    Boolean(*IsAvailable)(void);
} NetworkOperations;

/* Global network state */
extern NetworkOperations *gNetworkOps;
extern NetworkImplementation gCurrentNetworkImpl;

/* Public API functions */
OSErr InitNetworkAbstraction(void);
void ShutdownNetworkAbstraction(void);
NetworkImplementation GetCurrentNetworkImplementation(void);
const char *GetNetworkImplementationName(void);

/* Error translation and handling */
NetworkError TranslateOSErrToNetworkError(OSErr err);
const char *GetNetworkErrorString(NetworkError err);
const char *GetMacTCPErrorString(OSErr err);
void LogNetworkError(const char *context, OSErr err);

#endif /* NETWORK_ABSTRACTION_H */