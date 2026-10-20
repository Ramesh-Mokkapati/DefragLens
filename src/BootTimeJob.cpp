// BootTimeJob.cpp
// Schedules, cancels and reports on the one-time boot-time pass. Scheduling means three
// things: copy DefragLensBoot.exe into System32, write the job file, and add a
// "DefragLensBoot" line to the BootExecute registry value. The boot-time program undoes the
// registry entry and deletes the job itself as soon as it starts.

#include "pch.h"
#include "BootTimeJob.h"

namespace
{
    // BootExecute (REG_MULTI_SZ) lists the native programs smss.exe runs at startup; the default
    // is "autocheck autochk *" (Windows' disk check).
    const LPCTSTR kSessionManagerKey = _T("SYSTEM\\CurrentControlSet\\Control\\Session Manager");
    const LPCTSTR kBootExecuteValue = _T("BootExecute");
    const LPCTSTR kEntryName = _T("DefragLensBoot");
    const LPCTSTR kExeName = _T("DefragLensBoot.exe");

    // %SystemRoot%\DefragLensBoot: holds the job file and the boot-time log.
    CString BootFolder()
    {
        TCHAR windowsDir[MAX_PATH] = {};
        GetWindowsDirectory(windowsDir, MAX_PATH);
        return CString(windowsDir) + _T("\\DefragLensBoot");
    }

    // What the next boot-time pass should do (UTF-16 text, one key=value per line).
    CString JobPath()
    {
        return BootFolder() + _T("\\job.txt");
    }

    // True for a BootExecute line that runs our program ("DefragLensBoot", optionally with arguments).
    bool IsOurEntry(const CString& entry)
    {
        CString trimmed = entry;
        trimmed.Trim();
        int nameLength = (int)_tcslen(kEntryName);
        return trimmed.Left(nameLength).CompareNoCase(kEntryName) == 0 &&
            (trimmed.GetLength() == nameLength || trimmed[nameLength] == _T(' '));
    }

    // Reads BootExecute as a list of lines. A missing value counts as an empty list.
    bool ReadBootExecute(std::vector<CString>& entries, CString& message)
    {
        entries.clear();
        HKEY hKey = nullptr;
        LSTATUS status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, kSessionManagerKey, 0, KEY_QUERY_VALUE, &hKey);
        if (status != ERROR_SUCCESS)
        {
            message.Format(_T("Cannot open the Session Manager registry key (error %ld)."), status);
            return false;
        }

        DWORD type = 0, bytes = 0;
        status = RegQueryValueEx(hKey, kBootExecuteValue, nullptr, &type, nullptr, &bytes);
        if (status == ERROR_SUCCESS && type == REG_MULTI_SZ && bytes > 0)
        {
            std::vector<TCHAR> buffer(bytes / sizeof(TCHAR) + 2, 0);
            status = RegQueryValueEx(hKey, kBootExecuteValue, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
            for (LPCTSTR p = buffer.data(); status == ERROR_SUCCESS && *p; p += _tcslen(p) + 1)
            {
                entries.emplace_back(p);
            }
        }
        else if (status == ERROR_FILE_NOT_FOUND)
        {
            status = ERROR_SUCCESS;   // no BootExecute value at all: treat as empty
        }

        RegCloseKey(hKey);
        if (status != ERROR_SUCCESS)
        {
            message.Format(_T("Cannot read BootExecute (error %ld)."), status);
            return false;
        }

        return true;
    }

    // Writes the list back as REG_MULTI_SZ: each line null-terminated, plus a final null.
    bool WriteBootExecute(const std::vector<CString>& entries, CString& message)
    {
        std::vector<TCHAR> multi;
        for (const CString& entry : entries)
        {
            multi.insert(multi.end(), (LPCTSTR)entry, (LPCTSTR)entry + entry.GetLength());
            multi.push_back(0);
        }

        multi.push_back(0);
        if (multi.size() < 2)
        {
            multi.push_back(0);
        }

        HKEY hKey = nullptr;
        LSTATUS status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, kSessionManagerKey, 0, KEY_SET_VALUE, &hKey);
        if (status == ERROR_SUCCESS)
        {
            status = RegSetValueEx(hKey, kBootExecuteValue, 0, REG_MULTI_SZ,
                reinterpret_cast<const BYTE*>(multi.data()), (DWORD)(multi.size() * sizeof(TCHAR)));
            RegCloseKey(hKey);
        }

        if (status != ERROR_SUCCESS)
        {
            message.Format(_T("Cannot write BootExecute (error %ld)."), status);
            return false;
        }

        return true;
    }

    // True if any line runs our program.
    bool HasEntry(const std::vector<CString>& entries)
    {
        for (const CString& entry : entries)
        {
            if (IsOurEntry(entry))
            {
                return true;
            }
        }

        return false;
    }

    // Removes our line(s) from BootExecute and keeps every other entry exactly as it was.
    bool RemoveEntry(CString& message)
    {
        std::vector<CString> entries;
        if (!ReadBootExecute(entries, message))
        {
            return false;
        }

        if (!HasEntry(entries))
        {
            return true;
        }

        std::vector<CString> kept;
        for (const CString& entry : entries)
        {
            if (!IsOurEntry(entry))
            {
                kept.push_back(entry);
            }
        }

        return WriteBootExecute(kept, message);
    }

    // Asks NTFS whether the volume is flagged for repair (the flag chkdsk clears).
    bool IsVolumeDirty(TCHAR drive, bool& dirty, CString& message)
    {
        CString device;
        device.Format(_T("\\\\.\\%c:"), drive);
        HANDLE hVolume = CreateFile(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hVolume == INVALID_HANDLE_VALUE)
        {
            message.Format(_T("Cannot open volume %c: (error %lu)."), drive, GetLastError());
            return false;
        }

        ULONG flags = 0;
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(hVolume, FSCTL_IS_VOLUME_DIRTY, nullptr, 0, &flags, sizeof(flags), &returned, nullptr);
        DWORD error = GetLastError();
        CloseHandle(hVolume);
        if (!ok)
        {
            message.Format(_T("Cannot check whether %c: needs repair (error %lu)."), drive, error);
            return false;
        }

        dirty = (flags & VOLUME_IS_DIRTY) != 0;
        return true;
    }

    // Writes job.txt for the boot-time program, as UTF-16LE with a byte-order mark. The settings
    // lines come first so the program knows the volume before reading the "file=" lines.
    bool WriteJobFile(const BootTimeJob& job, CString& message)
    {
        CString text;
        text.Format(_T("version=1\r\nvolume=%c:\r\ndryrun=%d\r\nsystemfiles=%d\r\nmft=%d\r\nminutes=%lu\r\n"),
            job.drive, job.dryRun ? 1 : 0, job.systemFiles ? 1 : 0, job.mft ? 1 : 0, job.minutes);
        for (const CString& file : job.files)
        {
            text += _T("file=") + file + _T("\r\n");
        }

        HANDLE hFile = CreateFile(JobPath(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            message.Format(_T("Cannot write %s (error %lu)."), (LPCTSTR)JobPath(), GetLastError());
            return false;
        }

        const WCHAR bom = 0xFEFF;
        DWORD written = 0;
        BOOL ok = WriteFile(hFile, &bom, sizeof(bom), &written, nullptr) &&
            WriteFile(hFile, (LPCTSTR)text, (DWORD)(text.GetLength() * sizeof(TCHAR)), &written, nullptr) &&
            FlushFileBuffers(hFile);
        DWORD error = GetLastError();
        CloseHandle(hFile);
        if (!ok)
        {
            DeleteFile(JobPath());
            message.Format(_T("Cannot write %s (error %lu)."), (LPCTSTR)JobPath(), error);
            return false;
        }

        return true;
    }
}

// ---------------------------------------------------------------------------
// Public functions (see BootTimeJob.h)
// ---------------------------------------------------------------------------

// Scheduled means both parts exist: our BootExecute line and a job file for it to read.
bool IsBootTimeScheduled()
{
    std::vector<CString> entries;
    CString ignored;
    return ReadBootExecute(entries, ignored) && HasEntry(entries) &&
        GetFileAttributes(JobPath()) != INVALID_FILE_ATTRIBUTES;
}

// An entry without a job does nothing at boot except start and exit, but it should not stay
// registered: remove it.
void CleanUpStaleBootEntry()
{
    if (GetFileAttributes(JobPath()) == INVALID_FILE_ATTRIBUTES)
    {
        CString ignored;
        RemoveEntry(ignored);
    }
}

// Checks that a boot-time pass can run safely, then installs the program, writes the job and
// registers it. Any failure part-way removes the job again, so nothing is half-scheduled.
bool ScheduleBootTime(const BootTimeJob& job, CString& message)
{
    // A boot-time program must match Windows' own architecture: nothing 32-bit can run that early on
    // 64-bit Windows, and this build only carries the 32-bit one.
    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64)
    {
        message = _T("This is the 32-bit (Win32) build running on 64-bit Windows. Boot-time programs must match ")
            _T("Windows' architecture, so use the x64 build of DefragLens for Boot-Time Defrag.");
        return false;
    }

    // 1. Refuse a volume that needs repair: Windows' disk check runs first at boot and must fix it.
    bool dirty = false;
    if (!IsVolumeDirty(job.drive, dirty, message))
    {
        return false;
    }

    if (dirty)
    {
        message.Format(_T("%c: is marked as needing repair. Run \"chkdsk %c: /f\" and restart before scheduling a boot-time pass."),
            job.drive, job.drive);
        return false;
    }

    // 2. Install the native program where smss.exe looks for BootExecute images.
    TCHAR modulePath[MAX_PATH] = {};
    GetModuleFileName(nullptr, modulePath, MAX_PATH);
    PathRemoveFileSpec(modulePath);
    CString source = CString(modulePath) + _T("\\") + kExeName;
    if (GetFileAttributes(source) == INVALID_FILE_ATTRIBUTES)
    {
        message.Format(_T("%s was not found next to DefragLens.exe. Build the whole solution, which builds both programs."), kExeName);
        return false;
    }

    TCHAR systemDir[MAX_PATH] = {};
    GetSystemDirectory(systemDir, MAX_PATH);
    CString target = CString(systemDir) + _T("\\") + kExeName;
    if (!CopyFile(source, target, FALSE))
    {
        message.Format(_T("Cannot copy %s to %s (error %lu)."), kExeName, systemDir, GetLastError());
        return false;
    }

    // 3. Write the job, then register. The job is written first so an entry never exists without one.
    CreateDirectory(BootFolder(), nullptr);
    if (!WriteJobFile(job, message))
    {
        return false;
    }

    std::vector<CString> entries;
    if (!ReadBootExecute(entries, message))
    {
        DeleteFile(JobPath());
        return false;
    }

    if (!HasEntry(entries))
    {
        entries.emplace_back(kEntryName);   // after autocheck, so Windows' disk check always runs first
        if (!WriteBootExecute(entries, message))
        {
            DeleteFile(JobPath());
            return false;
        }
    }

    message.Format(_T("Scheduled for the next restart%s. Use Restart, not Shut down: with Fast Startup on, ")
        _T("Shut down skips this stage."), job.dryRun ? _T(" (dry run)") : _T(""));
    return true;
}

// Unregisters first, then deletes the job; the copied program can stay in System32 harmlessly.
bool CancelBootTime(CString& message)
{
    if (!RemoveEntry(message))
    {
        return false;
    }

    DeleteFile(JobPath());
    message = _T("The scheduled boot-time pass was cancelled.");
    return true;
}

// boot.log is written by the boot-time program (UTF-16LE) and replaced on every run.
CString GetBootLogPath()
{
    return BootFolder() + _T("\\boot.log");
}

// The log's last-write time and a one-line summary of it, for the main window and the dialog.
bool GetLastBootLog(CTime& when, CString& summary)
{
    HANDLE hFile = CreateFile(GetBootLogPath(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    FILETIME written = {};
    GetFileTime(hFile, nullptr, nullptr, &written);
    when = CTime(written);

    LARGE_INTEGER size = {};
    GetFileSizeEx(hFile, &size);
    std::vector<BYTE> data((size_t)(std::min)(size.QuadPart, 4LL * 1024 * 1024) + 2, 0);
    DWORD read = 0;
    ReadFile(hFile, data.data(), (DWORD)data.size() - 2, &read, nullptr);
    CloseHandle(hFile);

    CString text(reinterpret_cast<LPCWSTR>(data.data()), (int)(read / sizeof(WCHAR)));
    text.TrimLeft(L'\xFEFF');

    // Prefer the summary line; otherwise the last line before "Finished".
    summary.Empty();
    CString previous;
    int pos = 0;
    for (CString line = text.Tokenize(_T("\r\n"), pos); pos != -1; line = text.Tokenize(_T("\r\n"), pos))
    {
        if (line.Left(8) == _T("Summary:"))
        {
            summary = line;
        }

        if (line.Left(8) != _T("Finished"))
        {
            previous = line;
        }
    }

    if (summary.IsEmpty())
    {
        summary = previous;
    }

    return true;
}
