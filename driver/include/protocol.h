#ifndef CMP90HX_PROTOCOL_H
#define CMP90HX_PROTOCOL_H
#define CMP_DEVICE_TYPE 0x8337
#define CMP_IOCTL(fn) CTL_CODE(CMP_DEVICE_TYPE, fn, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define CMP_FILTER_STATUS CMP_IOCTL(0x80b)
#define CMP_FILTER_ARM_PROBE CMP_IOCTL(0x80c)
#define CMP_FILTER_PROBE_STATUS CMP_IOCTL(0x80d)
#define CMP_FILTER_UNLOCK_STATUS CMP_IOCTL(0x80e)
#define CMP_FILTER_PCI_STATUS CMP_IOCTL(0x80f)
#define CMP_SESSION_RUN CMP_IOCTL(0x810)
#define CMP_SESSION_STATUS CMP_IOCTL(0x811)
typedef struct _CMP_SESSION_INFO {
    ULONG Version, Bdf, Started, Busy, Quarantined, Retained, Runs, Status;
} CMP_SESSION_INFO;
C_ASSERT(sizeof(CMP_SESSION_INFO) == 32);
typedef struct _CMP_FILTER_PCI_SNAPSHOT {
    ULONG Version, Bdf, Bytes, Reserved;
    UCHAR Config[256];
} CMP_FILTER_PCI_SNAPSHOT;
C_ASSERT(sizeof(CMP_FILTER_PCI_SNAPSHOT) == 272);
#define CMP_FILTER_UNLOCK_LOG_BYTES 32768
typedef struct _CMP_FILTER_UNLOCK_RESULT {
    ULONG Version, Stage, Status, Bdf;
    LONG CoreReturn;
    ULONG Flags, ElapsedMs, Handoffs; /* bit7: PnP START; bit8: bus function-reset experiment */
    ULONGLONG DeviceAddress, Physical;
    ULONG LiveAllocations, LinkGpu, LinkBridge, LogBytes;
    CHAR Log[CMP_FILTER_UNLOCK_LOG_BYTES];
} CMP_FILTER_UNLOCK_RESULT;
C_ASSERT(sizeof(CMP_FILTER_UNLOCK_RESULT) == 32832);
typedef struct _CMP_FILTER_PROBE {
    ULONG Version, Stage, Status, Bdf;
    ULONGLONG Time, Bar0, DeviceAddress, Physical;
    ULONG Bytes, Flags, Irql, ElapsedMs;
    ULONG Boot0, GfwBoot, GfwProgress, GspCpu, Sec2Cpu, Command;
    ULONG Bcr, DmaCommand, InMatches, OutMatches;
    ULONG Reason, FailureIndex, Expected, Actual, PioMatches;
    ULONG CleanupReason, CleanupIndex, CleanupExpected, CleanupActual;
    ULONG DmemPortBefore, DmemPortAfter;
    ULONG Original[8], Observed[8], CleanupObserved[8], Reserved;
    ULONG ResetStage, ResetStatus, ResetFlags, ResetEngineBefore;
    ULONG ResetEngineAfter, ResetBcrAfter, ResetHwcfgAfter, ResetCpuAfter;
    ULONG ResetDmaCtlAfter, ResetRiscvAfter;
} CMP_FILTER_PROBE;
C_ASSERT(sizeof(CMP_FILTER_PROBE) == 288);
#define CMP_FILTER_EVENTS 64
typedef struct _CMP_FILTER_EVENT {
    ULONGLONG Sequence, Time;
    ULONG Phase, Major, Minor, Status, Value, Irql;
} CMP_FILTER_EVENT;
typedef struct _CMP_FILTER_DIAGNOSTICS {
    ULONG Version, Count;
    ULONGLONG Latest;
    ULONG DeviceCount, Bdf, Ready, Reserved;
    CMP_FILTER_EVENT Events[CMP_FILTER_EVENTS];
} CMP_FILTER_DIAGNOSTICS;
C_ASSERT(sizeof(CMP_FILTER_EVENT) == 40);
C_ASSERT(sizeof(CMP_FILTER_DIAGNOSTICS) == 2592);
typedef struct _CMP_TARGET { ULONG Gpu, Bridge; } CMP_TARGET;
#define CMP_ARENA_BYTES (32u * 1024u * 1024u)
#endif
