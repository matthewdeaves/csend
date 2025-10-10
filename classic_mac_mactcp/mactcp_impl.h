//====================================
// FILE: ./classic_mac/mactcp_impl.h
//====================================

#ifndef MACTCP_IMPL_H
#define MACTCP_IMPL_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"

/* MacTCP async operation handle - opaque pointer to TCPiopb or UDPiopb */
typedef void* MacTCPAsyncHandle;

/* UDP endpoint - opaque pointer to UDPiopb */
typedef void* UDPEndpointRef;

/* TCP connection info */
typedef struct {
    ip_addr localHost;
    ip_addr remoteHost;
    tcp_port localPort;
    tcp_port remotePort;
    unsigned short connectionState;  /* 0=Closed, 2=Listen, 8=Established, 10-20=Closing */
    Boolean isConnected;
    Boolean isListening;
} NetworkTCPInfo;

/* Give time callback - matches TCL callback signature */
typedef void (*NetworkGiveTimeProcPtr)(void);

/* Notification callback - matches TCPNotifyUPP signature */
typedef void (*NetworkNotifyProcPtr)(StreamPtr tcpStream, unsigned short eventCode, Ptr userDataPtr,
                                     unsigned short terminReason, struct ICMPReport *icmpMsg);

/* System-level operations */
OSErr MacTCPImpl_Initialize(short *refNum, ip_addr *localIP, char *localIPStr);
void MacTCPImpl_Shutdown(short refNum);

/* TCP operations */
OSErr MacTCPImpl_TCPCreate(short refNum, StreamPtr *streamRef,
                           unsigned long rcvBufferSize, Ptr rcvBuffer,
                           NetworkNotifyProcPtr notifyProc);
OSErr MacTCPImpl_TCPRelease(short refNum, StreamPtr streamRef);
OSErr MacTCPImpl_TCPListen(StreamPtr streamRef, tcp_port localPort,
                           Byte timeout, Boolean async);
OSErr MacTCPImpl_TCPConnect(StreamPtr streamRef, ip_addr remoteHost,
                            tcp_port remotePort, Byte timeout,
                            NetworkGiveTimeProcPtr giveTime);
OSErr MacTCPImpl_TCPSend(StreamPtr streamRef, Ptr data, unsigned short length,
                         Boolean push, Byte timeout, NetworkGiveTimeProcPtr giveTime);
OSErr MacTCPImpl_TCPReceiveNoCopy(StreamPtr streamRef, Ptr rdsPtr,
                                  short maxEntries, Byte timeout,
                                  Boolean *urgent, Boolean *mark,
                                  NetworkGiveTimeProcPtr giveTime);
OSErr MacTCPImpl_TCPReturnBuffer(StreamPtr streamRef, Ptr rdsPtr,
                                 NetworkGiveTimeProcPtr giveTime);
OSErr MacTCPImpl_TCPClose(StreamPtr streamRef, Byte timeout,
                          NetworkGiveTimeProcPtr giveTime);
OSErr MacTCPImpl_TCPAbort(StreamPtr streamRef);
OSErr MacTCPImpl_TCPStatus(StreamPtr streamRef, NetworkTCPInfo *info);

/* Async TCP operations */
OSErr MacTCPImpl_TCPListenAsync(StreamPtr streamRef, tcp_port localPort,
                                MacTCPAsyncHandle *asyncHandle);
OSErr MacTCPImpl_TCPConnectAsync(StreamPtr streamRef, ip_addr remoteHost,
                                 tcp_port remotePort, MacTCPAsyncHandle *asyncHandle);
OSErr MacTCPImpl_TCPSendAsync(StreamPtr streamRef, Ptr data, unsigned short length,
                              Boolean push, MacTCPAsyncHandle *asyncHandle);
OSErr MacTCPImpl_TCPReceiveAsync(StreamPtr streamRef, Ptr rdsPtr,
                                 short maxEntries, MacTCPAsyncHandle *asyncHandle);
OSErr MacTCPImpl_TCPCheckAsyncStatus(MacTCPAsyncHandle asyncHandle,
                                     OSErr *operationResult, void **resultData);
void MacTCPImpl_TCPCancelAsync(MacTCPAsyncHandle asyncHandle);

/* UDP operations */
OSErr MacTCPImpl_UDPCreate(short refNum, UDPEndpointRef *endpointRef,
                           udp_port localPort, Ptr recvBuffer,
                           unsigned short bufferSize);
OSErr MacTCPImpl_UDPRelease(short refNum, UDPEndpointRef endpointRef);
OSErr MacTCPImpl_UDPSend(UDPEndpointRef endpointRef, ip_addr remoteHost,
                         udp_port remotePort, Ptr data, unsigned short length);
OSErr MacTCPImpl_UDPReceive(UDPEndpointRef endpointRef, ip_addr *remoteHost,
                            udp_port *remotePort, Ptr buffer,
                            unsigned short *length, Boolean async);
OSErr MacTCPImpl_UDPReturnBuffer(UDPEndpointRef endpointRef, Ptr buffer,
                                 unsigned short bufferSize, Boolean async);

/* Async UDP operations */
OSErr MacTCPImpl_UDPSendAsync(UDPEndpointRef endpointRef, ip_addr remoteHost,
                              udp_port remotePort, Ptr data, unsigned short length,
                              MacTCPAsyncHandle *asyncHandle);
OSErr MacTCPImpl_UDPCheckSendStatus(MacTCPAsyncHandle asyncHandle);
OSErr MacTCPImpl_UDPReceiveAsync(UDPEndpointRef endpointRef,
                                 MacTCPAsyncHandle *asyncHandle);
OSErr MacTCPImpl_UDPCheckAsyncStatus(MacTCPAsyncHandle asyncHandle,
                                     ip_addr *remoteHost, udp_port *remotePort,
                                     Ptr *dataPtr, unsigned short *dataLength);
OSErr MacTCPImpl_UDPReturnBufferAsync(UDPEndpointRef endpointRef,
                                      Ptr buffer, unsigned short bufferSize,
                                      MacTCPAsyncHandle *asyncHandle);
OSErr MacTCPImpl_UDPCheckReturnStatus(MacTCPAsyncHandle asyncHandle);
void MacTCPImpl_UDPCancelAsync(MacTCPAsyncHandle asyncHandle);

/* Utility operations */
OSErr MacTCPImpl_ResolveAddress(const char *hostname, ip_addr *address);
OSErr MacTCPImpl_AddressToString(ip_addr address, char *addressStr);

/* Implementation info */
const char *MacTCPImpl_GetImplementationName(void);
Boolean MacTCPImpl_IsAvailable(void);

#endif /* MACTCP_IMPL_H */
