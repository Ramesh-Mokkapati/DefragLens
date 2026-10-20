// SystemFiles.h
// Read-only report on the system files Windows keeps locked (page file, swap file, hibernation
// file, MFT), and the fixes the System Files dialog can apply after confirmation.

#pragma once
#include "DefragEngine.h"

// Rows of the report, in display order.
enum SystemFileId
{
    SysPageFile = 0,
    SysHiberFile,
    SysSwapFile,
    SysMft,
    SysFileCount
};

// One row of the report.
struct SystemFileReport
{
    CString name;               // file name as shown, e.g. "pagefile.sys"
    bool present = false;       // exists on this volume
    bool readable = false;      // its extents could be read from the MFT
    bool approximate = false;   // part of the run list lives where we cannot read it; fragments is a lower bound
    ULONGLONG size = 0;         // bytes
    DWORD fragments = 0;
    CString assessment;         // plain-language verdict and suggested fix
};

// Reports pagefile.sys, hiberfil.sys, swapfile.sys (root of the volume) and $MFT, indexed by
// SystemFileId. Locations are read straight from the file records with FSCTL_GET_NTFS_FILE_RECORD,
// so this works while Windows holds the files open and never opens them for data access.
bool QuerySystemFiles(const CString& root, std::vector<SystemFileReport>& out, CString& error);

// Page file configuration: HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PagingFiles
std::vector<CString> GetPagingFilesSetting();
// Fixes the page file on `drive` at sizeMB (minimum = maximum); the original value is saved first.
bool SetFixedPageFile(TCHAR drive, ULONGLONG sizeMB, CString& message);
// True when a saved original setting is waiting to be restored.
bool HasPageFileBackup();
// Puts the saved original PagingFiles value back.
bool RestorePageFileSetting(CString& message);

// Runs "powercfg /hibernate off" then "powercfg /hibernate on" so Windows allocates the file again.
bool RecreateHibernationFile(CString& message);
