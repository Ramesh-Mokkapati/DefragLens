// BootTimeJob.h
// Main-program side of Boot-Time Defrag: scheduling, cancelling and reading the result of the
// one-time pass that DefragLensBoot.exe performs at the next restart.

#pragma once

// Schedules the native DefragLensBoot.exe to run once at the next restart, from the BootExecute
// registry value (where Windows' own disk check runs), before services start or the page file opens.
//
// Files:  %SystemRoot%\System32\DefragLensBoot.exe     the program (copied from next to DefragLens.exe)
//         %SystemRoot%\DefragLensBoot\job.txt          what to do; deleted by the program as it starts
//         %SystemRoot%\DefragLensBoot\boot.log         what it did

// What the boot-time pass should do; written to job.txt by ScheduleBootTime.
struct BootTimeJob
{
    TCHAR drive = 0;            // volume letter, e.g. 'C'
    bool dryRun = true;         // check and log only, move nothing
    bool systemFiles = false;   // pagefile.sys, swapfile.sys, hiberfil.sys
    bool mft = false;           // consolidate the MFT (all but its first piece)
    DWORD minutes = 10;         // time limit for the whole pass
    std::vector<CString> files; // full paths of files to defragment (on `drive`)
};

// True when a pass is registered in BootExecute and its job file exists.
bool IsBootTimeScheduled();
// Checks the volume, installs the program, writes the job and registers it. `message` explains
// the outcome either way.
bool ScheduleBootTime(const BootTimeJob& job, CString& message);
// Unregisters the pass and deletes its job file.
bool CancelBootTime(CString& message);

// Removes a BootExecute entry left without a job (e.g. the boot-time program could not edit the registry).
void CleanUpStaleBootEntry();

// Full path of boot.log.
CString GetBootLogPath();
// Time of the last boot-time log and its summary line; false when there is no log.
bool GetLastBootLog(CTime& when, CString& summary);
