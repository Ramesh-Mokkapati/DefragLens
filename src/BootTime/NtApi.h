// NtApi.h
// Declarations for the boot-time program (DefragLensBoot.exe).

#pragma once

// Native (pre-Win32) API surface used by the boot-time program. Only ntdll.dll is available
// when smss.exe runs BootExecute entries, so nothing here may call kernel32 or the CRT.
//
// windows.h is included only for its types and constants. The SDK's winternl.h declares a few
// ntdll functions (NtCreateFile, NtClose, NtQuerySystemTime, ...); the rest are declared below.
// ntdll.lib from the Windows SDK provides the imports for all of them.

// Get NTSTATUS codes from ntstatus.h rather than the partial set in winnt.h.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>
#include <winioctl.h>   // FSCTL_* codes and their input/output structures

// Success and informational codes are >= 0; warnings and errors are negative.
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

// Pseudo-handle for the current process.
#define NtCurrentProcess() ((HANDLE)(LONG_PTR)-1)

// Values from wdm.h / ntifs.h
// NtCreateFile dispositions and options.
const ULONG kFileOpen = 0x00000001;              // FILE_OPEN
const ULONG kFileOverwriteIf = 0x00000005;       // FILE_OVERWRITE_IF
const ULONG kFileOpenIf = 0x00000003;            // FILE_OPEN_IF
const ULONG kFileDirectoryFile = 0x00000001;     // FILE_DIRECTORY_FILE
const ULONG kFileNonDirectoryFile = 0x00000040;  // FILE_NON_DIRECTORY_FILE
const ULONG kFileSynchronousIo = 0x00000020;     // FILE_SYNCHRONOUS_IO_NONALERT
const ULONG kFileDeleteOnClose = 0x00001000;     // FILE_DELETE_ON_CLOSE
const ULONG kFileOpenForBackup = 0x00004000;     // FILE_OPEN_FOR_BACKUP_INTENT
const ULONG kFileOpenReparsePoint = 0x00200000;  // FILE_OPEN_REPARSE_POINT

// NtQueryValueKey information class, and privilege numbers for RtlAdjustPrivilege.
const ULONG kKeyValuePartialInformation = 2;
const ULONG kSeBackupPrivilege = 17;
const ULONG kSeRestorePrivilege = 18;
const ULONG kSeManageVolumePrivilege = 28;

// KEY_VALUE_PARTIAL_INFORMATION: a registry value's type and data.
struct NtKeyValuePartialInformation
{
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
};

extern "C"
{
    // Boot screen and process
    NTSYSAPI NTSTATUS NTAPI NtDisplayString(PUNICODE_STRING String);
    NTSYSAPI NTSTATUS NTAPI NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

    // File and volume I/O
    NTSYSAPI NTSTATUS NTAPI NtFsControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
        PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode, PVOID InputBuffer,
        ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);
    NTSYSAPI NTSTATUS NTAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
        PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);
    NTSYSAPI NTSTATUS NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
        PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);

    // Registry (used to remove our BootExecute entry)
    NTSYSAPI NTSTATUS NTAPI NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
    NTSYSAPI NTSTATUS NTAPI NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG KeyValueInformationClass,
        PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
    NTSYSAPI NTSTATUS NTAPI NtSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type,
        PVOID Data, ULONG DataSize);
    NTSYSAPI NTSTATUS NTAPI NtFlushKey(HANDLE KeyHandle);
    NTSYSAPI NTSTATUS NTAPI NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock);

    // Privileges and memory
    NTSYSAPI NTSTATUS NTAPI RtlAdjustPrivilege(ULONG Privilege, BOOLEAN Enable, BOOLEAN CurrentThread, PBOOLEAN Enabled);
    NTSYSAPI PVOID NTAPI RtlCreateHeap(ULONG Flags, PVOID HeapBase, SIZE_T ReserveSize, SIZE_T CommitSize,
        PVOID Lock, PVOID Parameters);
    NTSYSAPI PVOID NTAPI RtlAllocateHeap(PVOID HeapHandle, ULONG Flags, SIZE_T Size);
    NTSYSAPI BOOLEAN NTAPI RtlFreeHeap(PVOID HeapHandle, ULONG Flags, PVOID BaseAddress);
}
