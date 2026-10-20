// SystemFiles.cpp
// Read-only fragmentation report for the files Windows keeps locked, plus the fixes the
// System Files dialog offers (page file size, hibernation file). The report never opens the
// files: it finds each file's MFT record and decodes its run list directly.
//
// NTFS on-disk layout used here (all little-endian):
//   file record:      "FILE" signature, offset of the first attribute at 0x14
//   attribute header: type (4 bytes), length (4), non-resident flag (1 byte at +8),
//                     name length (+9); resident value length at +16, value offset at +20;
//                     non-resident lowest VCN at +16, mapping-pairs offset at +32
//   attribute list entry: type (4), entry length (2 at +4), name length (1 at +6),
//                     segment reference (8 at +16)

#include "pch.h"
#include "SystemFiles.h"

namespace
{
    // Where Windows keeps the page file configuration (HKLM).
    const LPCTSTR kMemoryManagementKey = _T("SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management");
    const LPCTSTR kPagingFilesValue = _T("PagingFiles");
    // Our own backup of the original PagingFiles value, under HKCU\Software\DefragLens\DefragLens.
    const LPCTSTR kSettingsSection = _T("Settings");
    const LPCTSTR kPagingBackupEntry = _T("PagingFilesBackup");
    // Marks a stored backup so an empty original value can still be told apart from "no backup".
    const LPCTSTR kBackupPrefix = _T("v1|");

    // NTFS attribute type codes and the 48-bit record-number part of a file reference.
    const DWORD kAttrAttributeList = 0x20;
    const DWORD kAttrData = 0x80;
    const DWORD kAttrEnd = 0xFFFFFFFF;
    const ULONGLONG kRecordNumberMask = 0x0000FFFFFFFFFFFFULL;

    // Reads a little-endian value from a byte buffer; returns 0 past the end instead of overrunning.
    template <class T>
    T ReadAt(const std::vector<BYTE>& buffer, size_t offset)
    {
        T value = {};
        if (offset + sizeof(T) <= buffer.size())
        {
            memcpy(&value, buffer.data() + offset, sizeof(T));
        }

        return value;
    }

    // Fetches one MFT file record through FSCTL_GET_NTFS_FILE_RECORD. NTFS applies the record's
    // update-sequence fix-ups before returning it, so it can be parsed directly.
    bool ReadFileRecord(HANDLE hVolume, ULONGLONG recordNumber, DWORD recordSize, std::vector<BYTE>& record)
    {
        NTFS_FILE_RECORD_INPUT_BUFFER in = {};
        in.FileReferenceNumber.QuadPart = (LONGLONG)(recordNumber & kRecordNumberMask);
        std::vector<BYTE> out(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + recordSize);
        DWORD returned = 0;
        if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in), out.data(), (DWORD)out.size(),
            &returned, nullptr))
        {
            return false;
        }

        // NTFS returns the nearest in-use record at or below the one asked for; make sure it is ours.
        auto* result = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(out.data());
        if (((ULONGLONG)result->FileReferenceNumber.QuadPart & kRecordNumberMask) != (recordNumber & kRecordNumberMask))
        {
            return false;
        }

        DWORD length = (std::min)(result->FileRecordLength, recordSize);
        record.assign(result->FileRecordBuffer, result->FileRecordBuffer + length);
        return record.size() >= 48 && memcmp(record.data(), "FILE", 4) == 0;
    }

    // Decodes an NTFS mapping-pairs array into extents.
    // Each run starts with a header byte: low nibble = size of the length field, high nibble = size
    // of the offset field. The offset is signed and relative to the previous run's LCN; an offset
    // size of 0 means a sparse run with no clusters. A 0 header byte ends the list.
    void DecodeRuns(const std::vector<BYTE>& rec, size_t pos, size_t end, LONGLONG vcn, std::vector<FileExtent>& extents)
    {
        LONGLONG lcn = 0;
        while (pos < end && rec[pos] != 0)
        {
            BYTE header = rec[pos++];
            int lengthBytes = header & 0x0F;
            int offsetBytes = header >> 4;
            if (lengthBytes == 0 || lengthBytes > 8 || offsetBytes > 8 || pos + lengthBytes + offsetBytes > end)
            {
                break;
            }

            LONGLONG runLength = 0;
            for (int i = 0; i < lengthBytes; ++i)
            {
                runLength |= (LONGLONG)rec[pos + i] << (8 * i);
            }

            pos += lengthBytes;
            if (offsetBytes == 0)
            {
                extents.push_back({ vcn, -1, runLength });   // sparse run
            }
            else
            {
                LONGLONG delta = 0;
                for (int i = 0; i < offsetBytes; ++i)
                {
                    delta |= (LONGLONG)rec[pos + i] << (8 * i);
                }

                if (rec[pos + offsetBytes - 1] & 0x80)   // sign-extend
                {
                    for (int i = offsetBytes; i < 8; ++i)
                    {
                        delta |= (LONGLONG)0xFF << (8 * i);
                    }
                }

                pos += offsetBytes;
                lcn += delta;
                extents.push_back({ vcn, lcn, runLength });
            }

            vcn += runLength;
        }
    }

    // Collects the unnamed $DATA runs in one file record. When extraRecords is given, also lists the
    // other records that an $ATTRIBUTE_LIST says hold more of the $DATA attribute.
    void ParseRecord(const std::vector<BYTE>& rec, ULONGLONG baseRecord, std::vector<FileExtent>& extents,
        std::vector<ULONGLONG>* extraRecords, bool& approximate)
    {
        size_t offset = ReadAt<WORD>(rec, 0x14);
        while (offset + 16 <= rec.size())
        {
            DWORD type = ReadAt<DWORD>(rec, offset);
            DWORD length = ReadAt<DWORD>(rec, offset + 4);
            if (type == kAttrEnd || length < 16 || offset + length > rec.size())
            {
                break;
            }

            BYTE nonResident = rec[offset + 8];
            BYTE nameLength = rec[offset + 9];
            if (type == kAttrData && nameLength == 0 && nonResident)
            {
                LONGLONG lowestVcn = ReadAt<LONGLONG>(rec, offset + 16);
                WORD runsOffset = ReadAt<WORD>(rec, offset + 32);
                DecodeRuns(rec, offset + runsOffset, offset + length, lowestVcn, extents);
            }
            else if (type == kAttrAttributeList && extraRecords)
            {
                if (nonResident)
                {
                    approximate = true;   // the list itself is stored outside the record
                }
                else
                {
                    size_t entry = offset + ReadAt<WORD>(rec, offset + 20);
                    size_t listEnd = entry + ReadAt<DWORD>(rec, offset + 16);
                    listEnd = (std::min)(listEnd, offset + (size_t)length);
                    while (entry + 26 <= listEnd)
                    {
                        DWORD entryType = ReadAt<DWORD>(rec, entry);
                        WORD entryLength = ReadAt<WORD>(rec, entry + 4);
                        BYTE entryNameLength = rec[entry + 6];
                        ULONGLONG segment = ReadAt<ULONGLONG>(rec, entry + 16) & kRecordNumberMask;
                        if (entryLength == 0)
                        {
                            break;
                        }

                        if (entryType == kAttrData && entryNameLength == 0 && segment != (baseRecord & kRecordNumberMask) &&
                            std::find(extraRecords->begin(), extraRecords->end(), segment) == extraRecords->end())
                        {
                            extraRecords->push_back(segment);
                        }

                        entry += entryLength;
                    }
                }
            }

            offset += length;
        }
    }

    // All extents of a file's unnamed $DATA attribute. Large or very fragmented files spread their
    // run list over several records, listed in an $ATTRIBUTE_LIST; those records are read too.
    // `approximate` is set when part of the list could not be read (the count is then a minimum).
    bool ReadDataExtents(HANDLE hVolume, ULONGLONG recordNumber, DWORD recordSize, std::vector<FileExtent>& extents,
        bool& approximate)
    {
        extents.clear();
        approximate = false;
        std::vector<BYTE> record;
        if (!ReadFileRecord(hVolume, recordNumber, recordSize, record))
        {
            return false;
        }

        std::vector<ULONGLONG> extraRecords;
        ParseRecord(record, recordNumber, extents, &extraRecords, approximate);
        for (ULONGLONG extra : extraRecords)
        {
            if (ReadFileRecord(hVolume, extra, recordSize, record))
            {
                ParseRecord(record, recordNumber, extents, nullptr, approximate);
            }
            else
            {
                approximate = true;
            }
        }

        std::sort(extents.begin(), extents.end(),
            [](const FileExtent& a, const FileExtent& b) { return a.vcn < b.vcn; });
        return true;
    }

    // Fills in the fragment count for one report row.
    void FillReport(HANDLE hVolume, ULONGLONG recordNumber, DWORD recordSize, SystemFileReport& report)
    {
        std::vector<FileExtent> extents;
        bool approximate = false;
        if (ReadDataExtents(hVolume, recordNumber, recordSize, extents, approximate))
        {
            report.readable = true;
            report.approximate = approximate;
            report.fragments = CountFragments(extents);
        }
    }

    // The "Assessment" column: what the numbers mean for this file and which fix applies.
    CString Assess(SystemFileId id, const SystemFileReport& r)
    {
        CString text;
        if (!r.present)
        {
            switch (id)
            {
            case SysHiberFile:
                return _T("Not present: hibernation and Fast Startup are turned off.");

            case SysMft:
                return _T("Not available.");

            default:
                return _T("Not on this volume.");
            }
        }

        if (!r.readable)
        {
            return _T("Its location could not be read.");
        }

        // The MFT's first piece can never move, so two pieces is already the best possible.
        bool fragmented = id == SysMft ? r.fragments > 2 : r.fragments > 1;
        switch (id)
        {
        case SysPageFile:
            if (fragmented)
            {
                text.Format(_T("%lu fragments. A fixed size stops it growing in more pieces; ")
                    _T("rebuilding it needs the steps below."), r.fragments);
            }
            else
            {
                text = _T("Contiguous. A fixed size keeps it that way.");
            }

            break;

        case SysHiberFile:
            if (fragmented)
            {
                text.Format(_T("%lu fragments. Recreate it so Windows allocates it again."), r.fragments);
            }
            else
            {
                text = _T("Contiguous.");
            }

            break;

        case SysSwapFile:
            if (fragmented)
            {
                text.Format(_T("%lu fragments. Windows manages it with the page file; there is no separate fix."), r.fragments);
            }
            else
            {
                text = _T("Contiguous.");
            }

            break;

        default:
            if (fragmented)
            {
                text.Format(_T("%lu fragments. Consolidate it to bring the movable part together."), r.fragments);
            }
            else if (r.fragments == 2)
            {
                text = _T("2 fragments. The first piece is fixed in place, so this is as good as it gets.");
            }
            else
            {
                text = _T("Contiguous.");
            }

            break;
        }

        if (r.approximate)
        {
            text += _T(" (Count is a minimum: part of the run list could not be read.)");
        }

        return text;
    }

    // Writes the PagingFiles REG_MULTI_SZ value: each entry null-terminated, plus a final null.
    bool WritePagingFiles(const std::vector<CString>& entries, CString& message)
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
        LSTATUS status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, kMemoryManagementKey, 0, KEY_SET_VALUE, &hKey);
        if (status == ERROR_SUCCESS)
        {
            status = RegSetValueEx(hKey, kPagingFilesValue, 0, REG_MULTI_SZ,
                reinterpret_cast<const BYTE*>(multi.data()), (DWORD)(multi.size() * sizeof(TCHAR)));
            RegCloseKey(hKey);
        }

        if (status != ERROR_SUCCESS)
        {
            message.Format(_T("Could not write the page file setting (error %ld)."), status);
            return false;
        }

        return true;
    }

    // Runs powercfg.exe without a console window and waits up to two minutes for it.
    bool RunPowerCfg(LPCTSTR arguments, DWORD& exitCode)
    {
        // A 32-bit build on 64-bit Windows sees SysWOW64 as System32; Sysnative reaches the real one.
        TCHAR windowsDir[MAX_PATH] = {};
        GetWindowsDirectory(windowsDir, MAX_PATH);
        BOOL wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow64);
        CString command;
        command.Format(_T("\"%s\\%s\\powercfg.exe\" %s"), windowsDir, wow64 ? _T("Sysnative") : _T("System32"), arguments);
        std::vector<TCHAR> commandLine((LPCTSTR)command, (LPCTSTR)command + command.GetLength() + 1);

        STARTUPINFO si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcess(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            exitCode = GetLastError();
            return false;
        }

        bool finished = WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return finished && exitCode == 0;
    }
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

bool QuerySystemFiles(const CString& root, std::vector<SystemFileReport>& out, CString& error)
{
    out.assign(SysFileCount, SystemFileReport());
    out[SysPageFile].name = _T("pagefile.sys");
    out[SysHiberFile].name = _T("hiberfil.sys");
    out[SysSwapFile].name = _T("swapfile.sys");
    out[SysMft].name = _T("$MFT");

    CString device;
    device.Format(_T("\\\\.\\%c:"), root[0]);
    // Read-only volume handle: the report never changes anything.
    HANDLE hVolume = CreateFile(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVolume == INVALID_HANDLE_VALUE)
    {
        error.Format(_T("Cannot open volume %s (error %lu)."), (LPCTSTR)root, GetLastError());
        return false;
    }

    NTFS_VOLUME_DATA_BUFFER volume = {};
    DWORD returned = 0;
    if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &volume, sizeof(volume), &returned, nullptr))
    {
        CloseHandle(hVolume);
        error = _T("The system file report needs an NTFS volume.");
        return false;
    }

    // Usually 1 KB (4 KB on some newer volumes).
    const DWORD recordSize = volume.BytesPerFileRecordSegment;

    // $MFT is always file record 0.
    out[SysMft].present = true;
    out[SysMft].size = (ULONGLONG)volume.MftValidDataLength.QuadPart;
    FillReport(hVolume, 0, recordSize, out[SysMft]);

    // The other files are locked, so get their file IDs by listing the root directory instead of opening them.
    HANDLE hRoot = CreateFile(root, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hRoot != INVALID_HANDLE_VALUE)
    {
        std::vector<BYTE> buffer(64 * 1024);
        // Enumerate the root folder in batches; each entry carries the file ID (= MFT record reference).
        FILE_INFO_BY_HANDLE_CLASS infoClass = FileIdBothDirectoryRestartInfo;
        while (GetFileInformationByHandleEx(hRoot, infoClass, buffer.data(), (DWORD)buffer.size()))
        {
            infoClass = FileIdBothDirectoryInfo;
            BYTE* p = buffer.data();
            for (;;)
            {
                auto* info = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(p);
                CString name(info->FileName, (int)(info->FileNameLength / sizeof(WCHAR)));
                for (int id = SysPageFile; id < SysMft; ++id)
                {
                    if (name.CompareNoCase(out[id].name) == 0)
                    {
                        out[id].present = true;
                        out[id].size = (ULONGLONG)info->EndOfFile.QuadPart;
                        FillReport(hVolume, (ULONGLONG)info->FileId.QuadPart, recordSize, out[id]);
                    }
                }

                if (info->NextEntryOffset == 0)
                {
                    break;
                }

                p += info->NextEntryOffset;
            }
        }

        CloseHandle(hRoot);
    }

    CloseHandle(hVolume);
    for (int id = 0; id < SysFileCount; ++id)
    {
        out[id].assessment = Assess((SystemFileId)id, out[id]);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Page file setting
// ---------------------------------------------------------------------------

// The PagingFiles entries as stored, e.g. "C:\pagefile.sys 4096 4096" (fixed size in MB),
// "C:\pagefile.sys 0 0" (system managed for that drive) or "?:\pagefile.sys" (system managed).
std::vector<CString> GetPagingFilesSetting()
{
    std::vector<CString> entries;
    HKEY hKey = nullptr;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, kMemoryManagementKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
    {
        return entries;
    }

    DWORD type = 0, bytes = 0;
    if (RegQueryValueEx(hKey, kPagingFilesValue, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && bytes > 0)
    {
        std::vector<TCHAR> buffer(bytes / sizeof(TCHAR) + 2, 0);
        if (RegQueryValueEx(hKey, kPagingFilesValue, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) == ERROR_SUCCESS)
        {
            for (LPCTSTR p = buffer.data(); *p; p += _tcslen(p) + 1)
            {
                entries.emplace_back(p);
            }
        }
    }

    RegCloseKey(hKey);
    return entries;
}

// True once SetFixedPageFile has saved the original setting and it has not been restored yet.
bool HasPageFileBackup()
{
    return !AfxGetApp()->GetProfileString(kSettingsSection, kPagingBackupEntry).IsEmpty();
}

// Sets the page file on `drive` to a fixed size (minimum = maximum), so it never grows in new
// pieces. Other drives' entries are kept. Takes effect after a restart.
bool SetFixedPageFile(TCHAR drive, ULONGLONG sizeMB, CString& message)
{
    std::vector<CString> current = GetPagingFilesSetting();

    // Keep the original setting (only the first time) so it can be restored later.
    if (!HasPageFileBackup())
    {
        CString backup = kBackupPrefix;
        for (size_t i = 0; i < current.size(); ++i)
        {
            backup += (i ? _T("|") : _T("")) + current[i];
        }

        AfxGetApp()->WriteProfileString(kSettingsSection, kPagingBackupEntry, backup);
    }

    // Replace this drive's entry and the "?:\pagefile.sys" entry that means "system managed".
    std::vector<CString> updated;
    CString mine;
    mine.Format(_T("%c:\\pagefile.sys %I64u %I64u"), drive, sizeMB, sizeMB);
    updated.push_back(mine);
    for (const CString& entry : current)
    {
        CString trimmed = entry;
        trimmed.Trim();
        if (trimmed.IsEmpty() || trimmed[0] == _T('?') || _totupper(trimmed[0]) == _totupper(drive))
        {
            continue;
        }

        updated.push_back(entry);
    }

    if (!WritePagingFiles(updated, message))
    {
        return false;
    }

    message.Format(_T("The page file on %c: is now a fixed %I64u MB. Restart Windows for the change to take effect."),
        drive, sizeMB);
    return true;
}

// Writes back the PagingFiles value saved before the first change, then forgets the backup.
bool RestorePageFileSetting(CString& message)
{
    CString backup = AfxGetApp()->GetProfileString(kSettingsSection, kPagingBackupEntry);
    if (backup.Left(3) != kBackupPrefix)
    {
        message = _T("There is no saved page file setting to restore.");
        return false;
    }

    std::vector<CString> entries;
    CString body = backup.Mid(3);
    int pos = 0;
    for (CString entry = body.Tokenize(_T("|"), pos); pos != -1; entry = body.Tokenize(_T("|"), pos))
    {
        entries.push_back(entry);
    }

    if (!WritePagingFiles(entries, message))
    {
        return false;
    }

    AfxGetApp()->WriteProfileString(kSettingsSection, kPagingBackupEntry, nullptr);   // delete the backup
    message = _T("The original page file setting is restored. Restart Windows for the change to take effect.");
    return true;
}

// ---------------------------------------------------------------------------
// Hibernation file
// ---------------------------------------------------------------------------

// Turning hibernation off deletes hiberfil.sys; turning it back on makes Windows allocate a new
// one, in one piece when enough contiguous free space exists.
bool RecreateHibernationFile(CString& message)
{
    DWORD exitCode = 0;
    if (!RunPowerCfg(_T("/hibernate off"), exitCode))
    {
        message.Format(_T("\"powercfg /hibernate off\" failed (code %lu). Nothing was changed."), exitCode);
        return false;
    }

    if (!RunPowerCfg(_T("/hibernate on"), exitCode))
    {
        message.Format(_T("Hibernation was turned off but could not be turned back on (code %lu). ")
            _T("Run \"powercfg /hibernate on\" from an administrator command prompt to restore hibernation and Fast Startup."),
            exitCode);
        return false;
    }

    message = _T("The hibernation file was deleted and allocated again by Windows.");
    return true;
}
