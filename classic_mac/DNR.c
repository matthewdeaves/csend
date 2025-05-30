#define MPW3 .0
#include <OSUtils.h>
#include <Errors.h>
#include <Files.h>
#include <Resources.h>
#include <Memory.h>
#include <Traps.h>
#include <Gestalt.h>
#include <Folders.h>
#include <MixedMode.h>
#define OPENRESOLVER 1
#define CLOSERESOLVER 2
#define STRTOADDR 3
#define ADDRTOSTR 4
#define ENUMCACHE 5
#define ADDRTONAME 6
#define HINFO 7
#define MXINFO 8
Handle codeHndl = nil;
UniversalProcPtr dnr = nil;
TrapType GetTrapType(theTrap)
unsigned long theTrap;
{
    if (BitAnd(theTrap, 0x0800) > 0)
        return (ToolTrap);
    else
        return (OSTrap);
}
Boolean TrapAvailable(trap)
unsigned long trap;
{
    TrapType trapType = ToolTrap;
    unsigned long numToolBoxTraps;
    if (NGetTrapAddress(_InitGraf, ToolTrap) == NGetTrapAddress(0xAA6E, ToolTrap))
        numToolBoxTraps = 0x200;
    else
        numToolBoxTraps = 0x400;
    trapType = GetTrapType(trap);
    if (trapType == ToolTrap) {
        trap = BitAnd(trap, 0x07FF);
        if (trap >= numToolBoxTraps)
            trap = _Unimplemented;
    }
    return (NGetTrapAddress(trap, trapType) != NGetTrapAddress(_Unimplemented, ToolTrap));
}
void GetSystemFolder(short *vRefNumP, long *dirIDP)
{
    SysEnvRec info;
    long wdProcID;
    SysEnvirons(1, &info);
    if (GetWDInfo(info.sysVRefNum, vRefNumP, dirIDP, &wdProcID) != noErr) {
        *vRefNumP = 0;
        *dirIDP = 0;
    }
}
void GetCPanelFolder(short *vRefNumP, long *dirIDP)
{
    Boolean hasFolderMgr = false;
    long feature;
    if (Gestalt(gestaltFindFolderAttr, &feature) == noErr)
        hasFolderMgr = true;
    if (!hasFolderMgr) {
        GetSystemFolder(vRefNumP, dirIDP);
        return;
    } else {
        if (FindFolder(kOnSystemDisk, kControlPanelFolderType, kDontCreateFolder, vRefNumP, dirIDP) != noErr) {
            *vRefNumP = 0;
            *dirIDP = 0;
        }
    }
}
short SearchFolderForDNRP(long targetType, long targetCreator, short vRefNum, long dirID)
{
    HParamBlockRec fi;
    Str255 filename;
    short refnum;
    fi.fileParam.ioCompletion = nil;
    fi.fileParam.ioNamePtr = filename;
    fi.fileParam.ioVRefNum = vRefNum;
    fi.fileParam.ioDirID = dirID;
    fi.fileParam.ioFDirIndex = 1;
    while (PBHGetFInfo(&fi, false) == noErr) {
        if (fi.fileParam.ioFlFndrInfo.fdType == targetType &&
                fi.fileParam.ioFlFndrInfo.fdCreator == targetCreator) {
            refnum = HOpenResFile(vRefNum, dirID, filename, fsRdPerm);
            if (GetIndResource('dnrp', 1) == NULL)
                CloseResFile(refnum);
            else
                return refnum;
        }
        fi.fileParam.ioFDirIndex++;
        fi.fileParam.ioDirID = dirID;
    }
    return (-1);
}
short OpenOurRF()
{
    short refnum;
    short vRefNum;
    long dirID;
    GetCPanelFolder(&vRefNum, &dirID);
    refnum = SearchFolderForDNRP('cdev', 'ztcp', vRefNum, dirID);
    if (refnum != -1) return (refnum);
    GetSystemFolder(&vRefNum, &dirID);
    refnum = SearchFolderForDNRP('cdev', 'mtcp', vRefNum, dirID);
    if (refnum != -1) return (refnum);
    GetCPanelFolder(&vRefNum, &dirID);
    refnum = SearchFolderForDNRP('cdev', 'mtcp', vRefNum, dirID);
    if (refnum != -1) return (refnum);
    return -1;
}
typedef OSErr(*OpenResolverProcPtr)(short selector, char *fileName);
enum {
    uppOpenResolverProcInfo = kCStackBased
                              | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                              | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                              | STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(char *)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr OpenResolverUPP;
#define NewOpenResolverProc(userRoutine) \
  (OpenResolverUPP) NewRoutineDescriptor(userRoutine, uppOpenResolverProcInfo, GetCurrentISA())
#define CallOpenResolverProc(userRoutine,selector,filename) \
  CallUniversalProc(userRoutine, uppOpenResolverProcInfo, selector, filename)
#else
typedef OpenResolverProcPtr OpenResolverUPP;
#define NewOpenResolverProc(userRoutine) \
  (OpenResolverUPP)(userRoutine)
#define CallOpenResolverProc(userRoutine,selector,filename) \
  (*userRoutine)(selector, filename)
#endif
OSErr OpenResolver(fileName)
char *fileName;
{
    short refnum;
    OSErr rc;
    if (dnr != nil)
        return (noErr);
    refnum = OpenOurRF();
    codeHndl = GetIndResource('dnrp', 1);
    if (codeHndl == nil) {
        return (ResError());
    }
    DetachResource(codeHndl);
    if (refnum != -1) {
        CloseWD(refnum);
        CloseResFile(refnum);
    }
    HLock(codeHndl);
    dnr = (UniversalProcPtr) * codeHndl;
    rc = CallOpenResolverProc(dnr, OPENRESOLVER, fileName);
    if (rc != noErr) {
        HUnlock(codeHndl);
        DisposeHandle(codeHndl);
        dnr = nil;
    }
    return (rc);
}
typedef OSErr(*CloseResolverProcPtr)(short selector);
enum {
    uppCloseResolverProcInfo = kCStackBased
                               | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                               | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr CloseResolverUPP;
#define NewCloseResolverProc(userRoutine) \
  (CloseResolverUPP) NewRoutineDescriptor(userRoutine, uppCloseResolverProcInfo, GetCurrentISA())
#define CallCloseResolverProc(userRoutine,selector) \
  CallUniversalProc(userRoutine, uppCloseResolverProcInfo, selector)
#else
typedef CloseResolverProcPtr CloseResolverUPP;
#define NewCloseResolverProc(userRoutine) \
  (CloseResolverUPP)(userRoutine)
#define CallCloseResolverProc(userRoutine,selector) \
  (*userRoutine)(selector)
#endif
OSErr CloseResolver()
{
    if (dnr == nil)
        return (notOpenErr);
    CallCloseResolverProc(dnr, CLOSERESOLVER);
    HUnlock(codeHndl);
    DisposeHandle(codeHndl);
    dnr = nil;
    return (noErr);
}
typedef OSErr(*StrToAddrProcPtr)(short selector, char *hostName, struct hostInfo *rtnStruct,
                                 long resultProc, char *userData);
enum {
    uppStrToAddrProcInfo = kCStackBased
                           | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                           | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                           | STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(char *)))
                           | STACK_ROUTINE_PARAMETER(3, SIZE_CODE(sizeof(struct hostInfo *)))
                           | STACK_ROUTINE_PARAMETER(4, SIZE_CODE(sizeof(long)))
                           | STACK_ROUTINE_PARAMETER(5, SIZE_CODE(sizeof(char *)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr StrToAddrUPP;
#define NewStrToAddrProc(userRoutine) \
  (StrToAddrUPP) NewRoutineDescriptor(userRoutine, uppStrToAddrProcInfo, GetCurrentISA())
#define CallStrToAddrProc(userRoutine,selector,hostName,rtnStruct,resultProc,userData) \
  CallUniversalProc(userRoutine, uppStrToAddrProcInfo, selector, hostName, rtnStruct, resultProc, userData)
#else
typedef StrToAddrProcPtr StrToAddrUPP;
#define NewStrToAddrProc(userRoutine) \
  (StrToAddrUPP)(userRoutine)
#define CallStrToAddrProc(userRoutine,selector,hostName,rtnStruct,resultProc,userData) \
  (*userRoutine)(selector, hostName, rtnStruct, resultProc, userData)
#endif
OSErr StrToAddr(hostName, rtnStruct, resultproc, userDataPtr)
char *hostName;
struct hostInfo *rtnStruct;
long resultproc;
char *userDataPtr;
{
    if (dnr == nil)
        return (notOpenErr);
    return (CallStrToAddrProc(dnr, STRTOADDR, hostName, rtnStruct, resultproc, userDataPtr));
}
typedef OSErr(*AddrToStrProcPtr)(short selector, long address, char *hostName);
enum {
    uppAddrToStrProcInfo = kCStackBased
                           | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                           | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                           | STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(unsigned long)))
                           | STACK_ROUTINE_PARAMETER(3, SIZE_CODE(sizeof(char *)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr AddrToStrUPP;
#define NewAddrToStrProc(userRoutine) \
  (AddrToStrUPP) NewRoutineDescriptor(userRoutine, uppAddrToStrProcInfo, GetCurrentISA())
#define CallAddrToStrProc(userRoutine,selector,address,hostName) \
  CallUniversalProc(userRoutine, uppAddrToStrProcInfo, selector, address, hostName)
#else
typedef AddrToStrProcPtr AddrToStrUPP;
#define NewAddrToStrProc(userRoutine) \
  (AddrToStrUPP)(userRoutine)
#define CallAddrToStrProc(userRoutine,selector,address,hostName) \
  (*userRoutine)(selector, address, hostName)
#endif
OSErr AddrToStr(addr, addrStr)
unsigned long addr;
char *addrStr;
{
    if (dnr == nil)
        return (notOpenErr);
    CallAddrToStrProc(dnr, ADDRTOSTR, addr, addrStr);
    return (noErr);
}
typedef OSErr(*EnumCacheProcPtr)(short selector, long result, char *userData);
enum {
    uppEnumCacheProcInfo = kCStackBased
                           | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                           | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                           | STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(long)))
                           | STACK_ROUTINE_PARAMETER(3, SIZE_CODE(sizeof(char *)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr EnumCacheUPP;
#define NewEnumCacheProc(userRoutine) \
  (EnumCacheUPP) NewRoutineDescriptor(userRoutine, uppEnumCacheProcInfo, GetCurrentISA())
#define CallEnumCacheProc(userRoutine,selector,result,userData) \
  CallUniversalProc(userRoutine, uppEnumCacheProcInfo, selector, result, userData)
#else
typedef EnumCacheProcPtr EnumCacheUPP;
#define NewEnumCacheProc(userRoutine) \
  (EnumCacheUPP)(userRoutine)
#define CallEnumCacheProc(userRoutine,selector,result,userData) \
  (*userRoutine)(selector, result, userData)
#endif
OSErr EnumCache(resultproc, userDataPtr)
long resultproc;
char *userDataPtr;
{
    if (dnr == nil)
        return (notOpenErr);
    return (CallEnumCacheProc(dnr, ENUMCACHE, resultproc, userDataPtr));
}
typedef OSErr(*AddrToNameProcPtr)(short selector, unsigned long addr, struct hostInfo *rtnStruct,
                                  long resultProc, char *userData);
enum {
    uppAddrToNameProcInfo = kCStackBased
                            | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                            | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                            | STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(unsigned long)))
                            | STACK_ROUTINE_PARAMETER(3, SIZE_CODE(sizeof(struct hostInfo *)))
                            | STACK_ROUTINE_PARAMETER(4, SIZE_CODE(sizeof(long)))
                            | STACK_ROUTINE_PARAMETER(5, SIZE_CODE(sizeof(char *)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr AddrToNameUPP;
#define NewAddrToNameProc(userRoutine) \
  (AddrToNameUPP) NewRoutineDescriptor(userRoutine, uppAddrToNameProcInfo, GetCurrentISA())
#define CallAddrToNameProc(userRoutine,selector,addr,rtnStruct,resultProc,userData) \
  CallUniversalProc(userRoutine, uppAddrToNameProcInfo, selector, addr, rtnStruct, resultProc, userData)
#else
typedef AddrToNameProcPtr AddrToNameUPP;
#define NewAddrToNameProc(userRoutine) \
  (AddrToNameUPP)(userRoutine)
#define CallAddrToNameProc(userRoutine,selector,addr,rtnStruct,resultProc,userData) \
  (*userRoutine)(selector, addr, rtnStruct, resultProc, userData)
#endif
OSErr AddrToName(addr, rtnStruct, resultproc, userDataPtr)
unsigned long addr;
struct hostInfo *rtnStruct;
long resultproc;
char *userDataPtr;
{
    if (dnr == nil)
        return (notOpenErr);
    return (CallAddrToNameProc(dnr, ADDRTONAME, addr, rtnStruct, resultproc, userDataPtr));
}
typedef OSErr(*HInfoProcPtr)(short selector, char *hostName, struct returnRec *returnRecPtr,
                             long resultProc, char *userData);
enum {
    uppHInfoProcInfo = kCStackBased
                       | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                       | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                       | STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(char *)))
                       | STACK_ROUTINE_PARAMETER(3, SIZE_CODE(sizeof(struct returnRec *)))
                       | STACK_ROUTINE_PARAMETER(4, SIZE_CODE(sizeof(long)))
                       | STACK_ROUTINE_PARAMETER(5, SIZE_CODE(sizeof(char *)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr HInfoUPP;
#define NewHInfoProc(userRoutine) \
  (HInfoUPP) NewRoutineDescriptor(userRoutine, uppHInfoProcInfo, GetCurrentISA())
#define CallHInfoProc(userRoutine,selector,hostName,returnRecPtr,resultProc,userData) \
  CallUniversalProc(userRoutine, uppHInfoProcInfo, selector, hostName, returnRecPtr, resultProc, userData)
#else
typedef HInfoProcPtr HInfoUPP;
#define NewHInfoProc(userRoutine) \
  (HInfoUPP)(userRoutine)
#define CallHInfoProc(userRoutine,selector,hostName,returnRecPtr,resultProc,userData) \
  CallUniversalProc(userRoutine, uppHInfoProcInfo, selector, hostName, returnRecPtr, resultProc, userData)
#endif
extern OSErr HInfo(hostName, returnRecPtr, resultProc, userDataPtr)
char *hostName;
struct returnRec *returnRecPtr;
long resultProc;
char *userDataPtr;
{
    if (dnr == nil)
        return (notOpenErr);
    return (CallHInfoProc(dnr, HINFO, hostName, returnRecPtr, resultProc, userDataPtr));
}
typedef OSErr(*MXInfoProcPtr)(short selector, char *hostName, struct returnRec *returnRecPtr,
                              long resultProc, char *userData);
enum {
    uppMXInfoProcInfo = kCStackBased
                        | RESULT_SIZE(SIZE_CODE(sizeof(short)))
                        | STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                        | STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(char *)))
                        | STACK_ROUTINE_PARAMETER(3, SIZE_CODE(sizeof(struct returnRec *)))
                        | STACK_ROUTINE_PARAMETER(4, SIZE_CODE(sizeof(long)))
                        | STACK_ROUTINE_PARAMETER(5, SIZE_CODE(sizeof(char *)))
};
#if USESROUTINEDESCRIPTORS
typedef UniversalProcPtr MXInfoUPP;
#define NewMXInfoProc(userRoutine) \
  (MXInfoUPP) NewRoutineDescriptor(userRoutine, uppMXInfoProcInfo, GetCurrentISA())
#define CallMXInfoProc(userRoutine,selector,hostName,returnRecPtr,resultProc,userData) \
  CallUniversalProc(userRoutine, selector, hostName, returnRecPtr, resultProc, userData)
#else
typedef MXInfoProcPtr MXInfoUPP;
#define NewMXInfoProc(userRoutine) \
  (MXInfoUPP)(userRoutine)
#define CallMXInfoProc(userRoutine,selector,hostName,returnRecPtr,resultProc,userData) \
  (*userRoutine)(selector, hostName, returnRecPtr, resultProc, userData)
#endif
extern OSErr MXInfo(hostName, returnRecPtr, resultProc, userDataPtr)
char *hostName;
struct returnRec *returnRecPtr;
long resultProc;
char *userDataPtr;
{
    if (dnr == nil)
        return (notOpenErr);
    return (CallMXInfoProc(dnr, MXINFO, hostName, returnRecPtr, resultProc, userDataPtr));
};
