// DefragEngine.cpp
// Implementation of CDefragEngine: volume access, analysis, defragmentation, boot-file
// packing, MFT consolidation and the thread-safe snapshots the UI draws from.
//
// Threading: Analyze/Defragment/BootOptimize run on one worker thread. Everything the UI
// reads (bitmap, file list, counters, status) is guarded by m_lock; slow I/O happens
// outside the lock so the UI never waits on the disk.

#include "pch.h"
#include "DefragEngine.h"

namespace
{
    // Minimum time between progress messages, so the UI repaints a few times a second at most.
    const ULONGLONG kNotifyIntervalMs = 150;
    const LONGLONG kMoveChunk = 16384;          // clusters per FSCTL_MOVE_FILE call
    const size_t kReloadBitmapEvery = 64;       // files moved between bitmap refreshes
    const LPCTSTR kLongPathPrefix = _T("\\\\?\\");

    // Enables a privilege the process token already holds (administrators have these, disabled by default).
    void EnablePrivilege(LPCTSTR name)
    {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        {
            return;
        }

        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValue(nullptr, name, &tp.Privileges[0].Luid))
        {
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        }

        CloseHandle(hToken);
    }

    // Volume position of the file's first allocated cluster, or -1 when nothing is allocated.
    LONGLONG FirstLcn(const std::vector<FileExtent>& extents)
    {
        for (const FileExtent& e : extents)
        {
            if (e.lcn >= 0)
            {
                return e.lcn;
            }
        }

        return -1;
    }

    // Reads %SystemRoot%\Prefetch\Layout.ini: the files Windows reads during startup, in that order.
    // Paths come back as "C:\..." with duplicates removed; device paths are mapped to drive letters.
    bool ReadBootLayout(std::vector<CString>& paths, CString& error)
    {
        paths.clear();
        TCHAR windowsDir[MAX_PATH] = {};
        GetWindowsDirectory(windowsDir, MAX_PATH);
        CString file = CString(windowsDir) + _T("\\Prefetch\\Layout.ini");

        // Backup semantics lets an administrator read the file despite its restrictive permissions.
        HANDLE hFile = CreateFile(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            {
                error.Format(_T("%s was not found. Windows only maintains it when the SysMain service is running ")
                    _T("and Windows is on a hard disk."), (LPCTSTR)file);
            }
            else
            {
                error.Format(_T("Cannot read %s (error %lu)."), (LPCTSTR)file, err);
            }

            return false;
        }

        LARGE_INTEGER size = {};
        GetFileSizeEx(hFile, &size);
        if (size.QuadPart <= 0 || size.QuadPart > 64LL * 1024 * 1024)
        {
            CloseHandle(hFile);
            error.Format(_T("%s is empty or unexpectedly large."), (LPCTSTR)file);
            return false;
        }

        std::vector<BYTE> data((size_t)size.QuadPart);
        DWORD read = 0;
        BOOL ok = ReadFile(hFile, data.data(), (DWORD)data.size(), &read, nullptr);
        CloseHandle(hFile);
        if (!ok)
        {
            error.Format(_T("Cannot read %s (error %lu)."), (LPCTSTR)file, GetLastError());
            return false;
        }

        CString text;
        // Windows writes Layout.ini as UTF-16LE with a byte-order mark; anything else is read as ANSI.
        if (read >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        {
            text = CString(reinterpret_cast<LPCWSTR>(data.data() + 2), (int)((read - 2) / sizeof(WCHAR)));
        }
        else
        {
            text = CString(CStringA(reinterpret_cast<LPCSTR>(data.data()), (int)read));
        }

        // "\Device\HarddiskVolume3" -> "C:"
        std::vector<std::pair<CString, CString>> devices;
        for (TCHAR letter = _T('A'); letter <= _T('Z'); ++letter)
        {
            TCHAR name[3] = { letter, _T(':'), 0 };
            TCHAR target[MAX_PATH] = {};
            if (QueryDosDevice(name, target, MAX_PATH))
            {
                devices.emplace_back(CString(target) + _T("\\"), CString(name) + _T("\\"));
            }
        }

        // Keep the first occurrence of each path so the list stays in Windows' boot order.
        std::set<std::wstring> seen;
        int pos = 0;
        for (CString line = text.Tokenize(_T("\r\n"), pos); pos != -1; line = text.Tokenize(_T("\r\n"), pos))
        {
            line.Trim();
            if (line.IsEmpty() || line[0] == _T('[') || line[0] == _T(';'))
            {
                continue;
            }

            if (line.Left(4) == _T("\\??\\"))
            {
                line = line.Mid(4);
            }
            else if (line.Left(8).CompareNoCase(_T("\\Device\\")) == 0)
            {
                for (const auto& device : devices)
                {
                    if (line.Left(device.first.GetLength()).CompareNoCase(device.first) == 0)
                    {
                        line = device.second + line.Mid(device.first.GetLength());
                        break;
                    }
                }
            }

            // Keep only absolute "X:\..." paths; this also drops entries such as "Version=1".
            if (line.GetLength() < 4 || line[1] != _T(':') || line[2] != _T('\\'))
            {
                continue;
            }

            CString key = line;
            key.MakeUpper();
            if (seen.insert(std::wstring(key)).second)
            {
                paths.push_back(line);
            }
        }

        if (paths.empty())
        {
            error.Format(_T("%s does not list any files."), (LPCTSTR)file);
            return false;
        }

        return true;
    }

    // Clusters actually stored on disk (sparse and compressed runs take none).
    LONGLONG AllocatedClusters(const std::vector<FileExtent>& extents)
    {
        LONGLONG total = 0;
        for (const FileExtent& e : extents)
        {
            if (e.lcn >= 0)
            {
                total += e.clusters;
            }
        }

        return total;
    }

    // Number of set bits in a byte (used clusters in one byte of the bitmap).
    int PopCount(BYTE b)
    {
        int n = 0;
        for (; b; b &= b - 1)
        {
            ++n;
        }

        return n;
    }
}

// Two extents count as one fragment when the second starts exactly where the first ends:
// NTFS sometimes splits a contiguous run into several extents.
DWORD CountFragments(const std::vector<FileExtent>& extents)
{
    DWORD fragments = 0;
    LONGLONG nextLcn = -2;
    for (const FileExtent& e : extents)
    {
        if (e.lcn < 0)
        {
            continue;
        }

        if (e.lcn != nextLcn)
        {
            ++fragments;
        }

        nextLcn = e.lcn + e.clusters;
    }

    return fragments;
}

// ---------------------------------------------------------------------------
// Volume access
// ---------------------------------------------------------------------------

CDefragEngine::~CDefragEngine()
{
    Close();
}

// Validates the file system, opens \\.\X: and loads the allocation bitmap. Any previous
// volume and its analysis are discarded first.
bool CDefragEngine::Open(const CString& root, CString& error)
{
    Close();

    // Volume maintenance for the defragmentation IOCTLs; backup and restore so files whose
    // permissions would otherwise refuse us can still be opened for their layout.
    EnablePrivilege(SE_MANAGE_VOLUME_NAME);
    EnablePrivilege(SE_BACKUP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);

    TCHAR fsName[MAX_PATH] = {};
    if (!GetVolumeInformation(root, nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH))
    {
        error.Format(_T("Cannot query volume %s (error %lu)."), (LPCTSTR)root, GetLastError());
        return false;
    }

    CString fs(fsName);
    // FSCTL_MOVE_FILE works on NTFS and the FAT family only (not ReFS or network shares).
    if (fs != _T("NTFS") && fs != _T("FAT32") && fs != _T("FAT") && fs != _T("exFAT"))
    {
        error.Format(_T("Volume %s uses %s, which does not support defragmentation."), (LPCTSTR)root, fsName);
        return false;
    }

    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
    if (!GetDiskFreeSpace(root, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
    {
        error.Format(_T("Cannot read the cluster size of %s (error %lu)."), (LPCTSTR)root, GetLastError());
        return false;
    }

    CString device;
    device.Format(_T("\\\\.\\%c:"), root[0]);
    // Read/write access to the volume device is what FSCTL_MOVE_FILE requires. Sharing read and
    // write means the volume is not locked: Windows and other programs keep using it normally.
    m_hVolume = CreateFile(device, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (m_hVolume == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        error.Format(_T("Cannot open volume %s (error %lu)."), (LPCTSTR)root, err);
        if (err == ERROR_ACCESS_DENIED)
        {
            error += _T(" Run the program as administrator.");
        }

        return false;
    }

    m_root = root;
    m_fileSystem = fs;
    m_mediaType = QueryMediaType();
    m_cursor = 0;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stats.bytesPerCluster = sectorsPerCluster * bytesPerSector;
    }

    if (!ReadVolumeBitmap())
    {
        error.Format(_T("Cannot read the allocation bitmap of %s (error %lu)."), (LPCTSTR)root, GetLastError());
        Close();
        return false;
    }

    SetStatus(_T("Ready. Click Analyze to examine the volume."));
    return true;
}

// Releases the volume and resets every result, so a closed engine looks like a new one.
void CDefragEngine::Close()
{
    if (m_hVolume != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hVolume);
        m_hVolume = INVALID_HANDLE_VALUE;
    }

    m_analyzed = false;
    m_mediaType = -1;
    m_root.Empty();
    m_fileSystem.Empty();

    std::lock_guard<std::mutex> guard(m_lock);
    m_bitmap.clear();
    m_files.clear();
    m_moved.clear();
    m_bootRanges.clear();
    m_failedPaths.clear();
    m_active = { -1, 0 };
    m_stats = VolumeStats();
    m_status.Empty();
}

// A failure adds the path once; a later success removes it, so the list only holds files that
// are still unmoved. Skipped files (no space, compressed) are not candidates for boot time.
void CDefragEngine::RecordMoveResult(const CString& path, MoveResult result)
{
    auto it = std::find_if(m_failedPaths.begin(), m_failedPaths.end(),
        [&path](const CString& p) { return p.CompareNoCase(path) == 0; });
    if (result == MoveFailed && it == m_failedPaths.end())
    {
        m_failedPaths.push_back(path);
    }
    else if (result == MoveDone && it != m_failedPaths.end())
    {
        m_failedPaths.erase(it);
    }
}

// Returns a copy so the caller can use it without holding the lock.
std::vector<CString> CDefragEngine::GetFailedPaths() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_failedPaths;
}

// A disk without a seek penalty is solid-state. Volumes spanning several disks give no answer (-1).
int CDefragEngine::QueryMediaType() const
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR desc = {};
    DWORD returned = 0;
    if (DeviceIoControl(m_hVolume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        &desc, sizeof(desc), &returned, nullptr) && returned >= sizeof(desc))
    {
        return desc.IncursSeekPenalty ? 0 : 1;
    }

    return -1;
}

// Reads the whole allocation bitmap (one bit per cluster). A first call with a small buffer
// returns the volume size; the second fetches everything. The new copy replaces m_bitmap,
// which also discards local reservations made by SetBits since the last read.
bool CDefragEngine::ReadVolumeBitmap()
{
    STARTING_LCN_INPUT_BUFFER in = {};
    std::vector<BYTE> buffer(sizeof(VOLUME_BITMAP_BUFFER) + 64 * 1024);
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(m_hVolume, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in),
        buffer.data(), (DWORD)buffer.size(), &returned, nullptr);
    if (!ok && GetLastError() != ERROR_MORE_DATA)
    {
        return false;
    }

    ULONGLONG total = (ULONGLONG)reinterpret_cast<VOLUME_BITMAP_BUFFER*>(buffer.data())->BitmapSize.QuadPart;
    size_t bytes = (size_t)((total + 7) / 8);
    if (!ok)
    {
        // First call only told us the size; fetch the whole bitmap in one go.
        buffer.assign(FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer) + bytes + 16, 0);
        in.StartingLcn.QuadPart = 0;
        if (!DeviceIoControl(m_hVolume, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in),
            buffer.data(), (DWORD)buffer.size(), &returned, nullptr))
        {
            return false;
        }
    }

    const BYTE* src = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(buffer.data())->Buffer;
    std::vector<BYTE> bits(src, src + bytes);
    if (total & 7)
    {
        bits.back() &= (BYTE)((1u << (total & 7)) - 1);   // clear bits past the end of the volume
    }

    // Count in-use clusters to derive the free-space figure.
    ULONGLONG used = 0;
    for (BYTE b : bits)
    {
        used += PopCount(b);
    }

    std::lock_guard<std::mutex> guard(m_lock);
    m_bitmap.swap(bits);
    m_stats.totalClusters = total;
    m_stats.freeClusters = total - used;
    return true;
}

// Reads a file's extent list in batches (ERROR_MORE_DATA means "call again from here").
// Returns true for files with no extents: small files live entirely inside their MFT record.
bool CDefragEngine::GetExtents(HANDLE hFile, std::vector<FileExtent>& extents)
{
    extents.clear();
    STARTING_VCN_INPUT_BUFFER in = {};
    std::vector<BYTE> out(16 * 1024);
    for (;;)
    {
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(hFile, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof(in),
            out.data(), (DWORD)out.size(), &returned, nullptr);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err != ERROR_MORE_DATA)
        {
            return err == ERROR_HANDLE_EOF;   // EOF: file is resident in the MFT / empty
        }

        auto* rp = reinterpret_cast<RETRIEVAL_POINTERS_BUFFER*>(out.data());
        LONGLONG vcn = rp->StartingVcn.QuadPart;
        for (DWORD i = 0; i < rp->ExtentCount; ++i)
        {
            LONGLONG next = rp->Extents[i].NextVcn.QuadPart;
            extents.push_back({ vcn, rp->Extents[i].Lcn.QuadPart, next - vcn });
            vcn = next;
        }

        if (ok || rp->ExtentCount == 0)
        {
            return true;
        }

        in.StartingVcn.QuadPart = vcn;
    }
}

// Replaces the status line shown under the progress bar.
void CDefragEngine::SetStatus(const CString& text)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_status = text;
}

// Tells the UI to refresh. Throttled so a fast loop cannot flood the message queue;
// `force` is used for the first and last update of an operation.
void CDefragEngine::Notify(int percent, bool force)
{
    ULONGLONG now = GetTickCount64();
    if (!force && now - m_lastNotify < kNotifyIntervalMs)
    {
        return;
    }

    m_lastNotify = now;
    if (m_hNotify)
    {
        ::PostMessage(m_hNotify, WM_DEFRAG_PROGRESS, (WPARAM)(std::max)(0, (std::min)(percent, 100)), 0);
    }
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

// Walks every folder and file on the volume, reads each one's extents and keeps the
// fragmented ones in m_files. Finishes by locating the boot files from Layout.ini.
// Returns false if stopped or if the bitmap cannot be read.
bool CDefragEngine::Analyze()
{
    m_analyzed = false;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_files.clear();
        m_moved.clear();
        m_bootRanges.clear();
        m_active = { -1, 0 };
        DWORD bpc = m_stats.bytesPerCluster;
        m_stats = VolumeStats();
        m_stats.bytesPerCluster = bpc;
    }

    SetStatus(_T("Reading volume bitmap..."));
    Notify(0, true);
    if (!ReadVolumeBitmap())
    {
        CString msg;
        msg.Format(_T("Cannot read the volume bitmap (error %lu)."), GetLastError());
        SetStatus(msg);
        return false;
    }

    // The number of files is unknown up front, so progress compares clusters seen so far
    // with clusters in use on the volume.
    ULONGLONG usedClusters;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        usedClusters = (std::max)(1ULL, m_stats.totalClusters - m_stats.freeClusters);
    }

    // Depth-first walk with an explicit stack; directories keep a trailing backslash.
    std::vector<CString> stack;
    stack.push_back(kLongPathPrefix + m_root);
    ULONGLONG scannedClusters = 0;

    while (!stack.empty() && !m_cancel)
    {
        CString dir = stack.back();
        stack.pop_back();
        AnalyzeEntry(dir, true, 0, scannedClusters);

        // FindExInfoBasic skips 8.3 short names; LARGE_FETCH reads directory entries in bigger batches.
        WIN32_FIND_DATA fd;
        HANDLE hFind = FindFirstFileEx(dir + _T("*"), FindExInfoBasic, &fd, FindExSearchNameMatch,
            nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            continue;
        }
        do
        {
            if (_tcscmp(fd.cFileName, _T(".")) == 0 || _tcscmp(fd.cFileName, _T("..")) == 0)
            {
                continue;
            }

            CString path = dir + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // Do not follow junctions/symlinks: they lead elsewhere or loop.
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                {
                    stack.push_back(path + _T("\\"));
                }

                continue;
            }

            // Cloud placeholders and offline files would be recalled if touched.
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                FILE_ATTRIBUTE_RECALL_ON_OPEN))
            {
                std::lock_guard<std::mutex> guard(m_lock);
                ++m_stats.filesSkipped;
                continue;
            }

            ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            AnalyzeEntry(path, false, size, scannedClusters);

            if (GetTickCount64() - m_lastNotify >= kNotifyIntervalMs)
            {
                CString msg;
                msg.Format(_T("Analyzing: %s"), (LPCTSTR)path.Mid(4));
                SetStatus(msg);
                Notify((int)(std::min)(99ULL, scannedClusters * 100 / usedClusters));
            }
        } while (!m_cancel && FindNextFile(hFind, &fd));

        FindClose(hFind);
    }

    bool completed = !m_cancel;
    if (completed)
    {
        // Identify the files Windows reads at startup; a missing Layout.ini is not an error here.
        std::vector<CString> bootPaths;
        CString bootError;
        if (ReadBootLayout(bootPaths, bootError))
        {
            SetStatus(_T("Checking boot files (Layout.ini)..."));
            Notify(99, true);
            std::vector<BootFile> bootFiles;
            CollectBootFiles(bootPaths, bootFiles);
            UpdateBootState(bootFiles);
        }
    }

    {
        std::lock_guard<std::mutex> guard(m_lock);
        RecomputeFragmentStats();
        if (completed)
        {
            double pct = m_stats.filesScanned ? 100.0 * m_stats.fragmentedFiles / m_stats.filesScanned : 0.0;
            m_status.Format(_T("Analysis complete: %I64u of %I64u files are fragmented (%.1f%%)."),
                m_stats.fragmentedFiles, m_stats.filesScanned, pct);
        }
        else
        {
            m_status = _T("Analysis stopped.");
        }
    }

    m_analyzed = completed;
    Notify(100, true);
    return completed;
}

// Opens one file or folder just for its attributes (no data access, so files in use are fine),
// reads its extents and updates the counters. Reparse points are opened as themselves, never
// followed. Anything that cannot be opened is counted as skipped.
void CDefragEngine::AnalyzeEntry(const CString& path, bool isDir, ULONGLONG size, ULONGLONG& scannedClusters)
{
    CString openPath = path;
    if (isDir && openPath.GetLength() > 7)   // keep "\\?\C:\" for the root, strip it elsewhere
    {
        openPath.TrimRight(_T('\\'));
    }

    HANDLE hFile = CreateFile(openPath, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    std::vector<FileExtent> extents;
    bool ok = hFile != INVALID_HANDLE_VALUE && GetExtents(hFile, extents);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
    }

    std::lock_guard<std::mutex> guard(m_lock);
    if (!ok)
    {
        ++m_stats.filesSkipped;
        return;
    }

    if (isDir)
    {
        ++m_stats.dirsScanned;
    }
    else
    {
        ++m_stats.filesScanned;
    }

    LONGLONG clusters = AllocatedClusters(extents);
    scannedClusters += clusters;
    DWORD fragments = CountFragments(extents);
    // Only fragmented entries are kept; contiguous ones just add to the counters.
    if (fragments > 1)
    {
        FragmentedFile f;
        f.path = openPath.Mid(4);
        f.size = isDir ? (ULONGLONG)clusters * m_stats.bytesPerCluster : size;
        f.clusters = clusters;
        f.fragments = fragments;
        f.extents = std::move(extents);
        m_files.push_back(std::move(f));
    }
}

// Recounts the fragmentation figures from m_files, ignoring files already defragmented.
// Caller must hold m_lock.
void CDefragEngine::RecomputeFragmentStats()
{
    m_stats.fragmentedFiles = 0;
    m_stats.totalFragments = 0;
    m_stats.fragmentedClusters = 0;
    for (const FragmentedFile& f : m_files)
    {
        if (f.done || f.fragments <= 1)
        {
            continue;
        }

        ++m_stats.fragmentedFiles;
        m_stats.totalFragments += f.fragments;
        m_stats.fragmentedClusters += f.clusters;
    }
}

// ---------------------------------------------------------------------------
// Defragmentation
// ---------------------------------------------------------------------------

// Defragments every analysed file with at least minFragments fragments (2 = all of them,
// higher = only the highly fragmented ones). Analyses first if needed. Each file is moved
// whole into one free run; files that cannot be moved are counted and listed for boot time.
bool CDefragEngine::Defragment(DWORD minFragments)
{
    if (!m_analyzed && !Analyze())
    {
        return false;
    }

    const bool highOnly = minFragments > 2;

    SetStatus(_T("Preparing to defragment..."));
    Notify(0, true);
    if (!ReadVolumeBitmap())
    {
        SetStatus(_T("Cannot read the volume bitmap."));
        return false;
    }

    // Smallest files first: they are the easiest to place and give the most benefit per move.
    std::vector<size_t> order;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        for (size_t i = 0; i < m_files.size(); ++i)
        {
            if (!m_files[i].done && m_files[i].fragments >= minFragments)
            {
                order.push_back(i);
            }
        }

        std::sort(order.begin(), order.end(),
            [this](size_t a, size_t b) { return m_files[a].clusters < m_files[b].clusters; });
        m_stats.filesDefragged = 0;
        m_stats.filesNotMoved = 0;
        m_moved.clear();
    }

    if (order.empty())
    {
        CString msg;
        if (highOnly)
        {
            msg.Format(_T("No files with %lu or more fragments; nothing to defragment."), minFragments);
        }
        else
        {
            msg = _T("No fragmented files; nothing to defragment.");
        }

        SetStatus(msg);
        Notify(100, true);
        return true;
    }

    m_cursor = 0;

    // The local bitmap knows about clusters we reserved but not about clusters our moves freed
    // or that other programs allocated, so it is re-read every kReloadBitmapEvery files.
    size_t sinceReload = 0;
    for (size_t k = 0; k < order.size() && !m_cancel; ++k)
    {
        size_t index = order[k];
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_status.Format(_T("Defragmenting %Iu of %Iu: %s"), k + 1, order.size(), (LPCTSTR)m_files[index].path);
        }

        m_percent = (int)(k * 100 / order.size());
        Notify(m_percent);

        MoveResult result = DefragFile(index);
        if (result == MoveFailed && !m_cancel)
        {
            // The target was probably taken since the last bitmap read; refresh and retry once.
            ReadVolumeBitmap();
            sinceReload = 0;
            result = DefragFile(index);
        }

        {
            std::lock_guard<std::mutex> guard(m_lock);
            RecordMoveResult(m_files[index].path, result);
            if (result == MoveDone)
            {
                m_files[index].done = true;
                ++m_stats.filesDefragged;
            }
            else if (result != MoveCancelled)
            {
                ++m_stats.filesNotMoved;
            }
        }

        if (++sinceReload >= kReloadBitmapEvery)
        {
            ReadVolumeBitmap();
            sinceReload = 0;
        }
    }

    ReadVolumeBitmap();
    bool completed = !m_cancel;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_active = { -1, 0 };
        RecomputeFragmentStats();
        CString scope;
        if (highOnly)
        {
            scope.Format(_T(" of files with %lu+ fragments"), minFragments);
        }

        m_status.Format(_T("Defragmentation%s %s: %I64u files defragmented, %I64u could not be moved, %I64u still fragmented."),
            (LPCTSTR)scope, completed ? _T("complete") : _T("stopped"),
            m_stats.filesDefragged, m_stats.filesNotMoved, m_stats.fragmentedFiles);
    }

    Notify(100, true);
    return completed;
}

// Defragments one analysed file and records its new layout for the map and list.
CDefragEngine::MoveResult CDefragEngine::DefragFile(size_t index)
{
    CString path;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        path = m_files[index].path;
    }

    std::vector<FileExtent> after;
    bool afterValid = false;
    MoveResult result = RelocateFile(path, -1, 0, after, afterValid);
    if (afterValid)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        FragmentedFile& f = m_files[index];
        f.fragments = CountFragments(after);
        f.extents = std::move(after);
    }

    return result;
}

// Moves all of a file's clusters into one run. With target < 0 the run is found here and
// already-contiguous files are left alone; with target >= 0 the caller has planned and
// reserved the slot (Boot Optimize). `after` receives the file's extents afterwards.
CDefragEngine::MoveResult CDefragEngine::RelocateFile(const CString& path, LONGLONG target, LONGLONG plannedClusters,
    std::vector<FileExtent>& after, bool& afterValid)
{
    afterValid = false;
    CString openPath = kLongPathPrefix + path;

    DWORD attrs = GetFileAttributes(openPath);
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return MoveFailed;
    }

    // Compressed and sparse files need compression-unit aligned moves; leave them alone.
    if (attrs & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE))
    {
        return MoveSkipped;
    }

    // FSCTL_MOVE_FILE needs only FILE_READ_ATTRIBUTES on the file, so files that other
    // programs have open can usually still be moved.
    HANDLE hFile = CreateFile(openPath, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return MoveFailed;
    }

    std::vector<FileExtent> extents;
    if (!GetExtents(hFile, extents))
    {
        CloseHandle(hFile);
        return MoveFailed;
    }

    LONGLONG needed = AllocatedClusters(extents);
    if (target < 0)
    {
        if (CountFragments(extents) <= 1)
        {
            CloseHandle(hFile);
            after = std::move(extents);
            afterValid = true;
            return MoveDone;   // already contiguous
        }

        target = FindFreeRun(needed);
        if (target < 0)
        {
            CloseHandle(hFile);
            return MoveSkipped;   // no contiguous free run big enough
        }
    }
    else
    {
        if (CountFragments(extents) == 1 && FirstLcn(extents) == target)
        {
            CloseHandle(hFile);
            after = std::move(extents);
            afterValid = true;
            return MoveDone;   // already in its slot
        }

        if (needed == 0 || needed > plannedClusters)
        {
            CloseHandle(hFile);
            return MoveSkipped;   // grew since it was planned; it would spill into the next slot
        }
    }

    {
        std::lock_guard<std::mutex> guard(m_lock);
        SetBits((ULONGLONG)target, (ULONGLONG)needed);   // reserve locally until the next bitmap read
        m_active = { target, needed };
    }

    // Extents are written back to back from the target, so the file ends up in a single run.
    MoveResult result = MoveExtents(hFile, extents, 0, target);
    afterValid = GetExtents(hFile, after);
    CloseHandle(hFile);

    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_active = { -1, 0 };
    }

    // Every call succeeded but the file is still in pieces: report it as not moved.
    if (result == MoveDone && afterValid && CountFragments(after) > 1)
    {
        result = MoveFailed;
    }

    return result;
}

// Moves extents[first..] in VCN order to consecutive clusters starting at dest.
// Sparse runs (lcn < 0) have no clusters and are skipped.
CDefragEngine::MoveResult CDefragEngine::MoveExtents(HANDLE hFile, const std::vector<FileExtent>& extents,
    size_t first, LONGLONG dest)
{
    for (size_t i = first; i < extents.size(); ++i)
    {
        const FileExtent& e = extents[i];
        if (e.lcn < 0)
        {
            continue;
        }

        for (LONGLONG done = 0; done < e.clusters;)
        {
            if (m_cancel)
            {
                return MoveCancelled;
            }

            // Chunks keep Stop responsive and the map moving on large files. Each FSCTL_MOVE_FILE call is
            // atomic: NTFS copies the clusters, then switches the file over in a journaled transaction,
            // so an interruption leaves the file whole in its old or new place.
            LONGLONG count = (std::min)(e.clusters - done, kMoveChunk);
            MOVE_FILE_DATA mfd = {};
            mfd.FileHandle = hFile;
            mfd.StartingVcn.QuadPart = e.vcn + done;
            mfd.StartingLcn.QuadPart = dest;
            mfd.ClusterCount = (DWORD)count;
            DWORD returned = 0;
            if (!DeviceIoControl(m_hVolume, FSCTL_MOVE_FILE, &mfd, sizeof(mfd), nullptr, 0, &returned, nullptr))
            {
                m_lastMoveError = GetLastError();
                return MoveFailed;
            }

            {
                std::lock_guard<std::mutex> guard(m_lock);
                if (!m_moved.empty() && m_moved.back().lcn + m_moved.back().clusters == dest)
                {
                    m_moved.back().clusters += count;
                }
                else
                {
                    m_moved.push_back({ dest, count });
                }
            }

            dest += count;
            done += count;
            Notify(m_percent);   // keep the map lively during large moves
        }
    }

    return MoveDone;
}

// ---------------------------------------------------------------------------
// Boot optimization
// ---------------------------------------------------------------------------

// Turns the Layout.ini path list into the boot files on this volume that can be moved, with
// their current extents. Folders, compressed, sparse and cloud/offline files are left out.
void CDefragEngine::CollectBootFiles(const std::vector<CString>& paths, std::vector<BootFile>& out) const
{
    out.clear();
    for (const CString& path : paths)
    {
        if (m_cancel)
        {
            break;
        }

        if (m_root.IsEmpty() || _totupper(path[0]) != _totupper(m_root[0]))
        {
            continue;   // on another volume
        }

        CString openPath = kLongPathPrefix + path;
        DWORD attrs = GetFileAttributes(openPath);
        const DWORD unmovable = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE |
            FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN;
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & unmovable))
        {
            continue;
        }

        HANDLE hFile = CreateFile(openPath, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        BootFile file;
        file.path = path;
        bool ok = GetExtents(hFile, file.extents);
        CloseHandle(hFile);
        file.clusters = AllocatedClusters(file.extents);
        if (ok && file.clusters > 0)   // files resident in the MFT have nothing to move
        {
            out.push_back(std::move(file));
        }
    }
}

// Publishes boot file counts and the ranges drawn in teal on the map.
void CDefragEngine::UpdateBootState(const std::vector<BootFile>& files)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_bootRanges.clear();
    m_stats.bootLayoutFound = true;
    m_stats.bootFiles = files.size();
    m_stats.bootFragmented = 0;
    for (const BootFile& f : files)
    {
        if (CountFragments(f.extents) > 1)
        {
            ++m_stats.bootFragmented;
        }

        for (const FileExtent& e : f.extents)
        {
            if (e.lcn >= 0)
            {
                m_bootRanges.push_back({ e.lcn, e.clusters });
            }
        }
    }
}

// Boot optimization:
//   1. read Layout.ini and keep the listed files that are on this volume and movable;
//   2. stop if they are already contiguous and back to back in boot order;
//   3. otherwise find the first free run (from the start of the volume) that holds them all
//      and move them in, one after another, in boot order;
//   4. if no run is big enough, defragment each file wherever it fits instead.
bool CDefragEngine::BootOptimize()
{
    m_percent = 0;
    SetStatus(_T("Reading the boot file list (Layout.ini)..."));
    Notify(0, true);

    std::vector<CString> paths;
    CString error;
    if (!ReadBootLayout(paths, error))
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stats.bootLayoutFound = false;
        m_bootRanges.clear();
        m_status = error;
        return false;
    }

    if (!ReadVolumeBitmap())
    {
        SetStatus(_T("Cannot read the volume bitmap."));
        return false;
    }

    SetStatus(_T("Locating boot files..."));
    std::vector<BootFile> files;
    CollectBootFiles(paths, files);
    UpdateBootState(files);
    if (files.empty())
    {
        CString msg;
        msg.Format(_T("Layout.ini lists no movable files on %s."), (LPCTSTR)m_root);
        SetStatus(msg);
        Notify(100, true);
        return true;
    }

    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stats.bootPlaced = 0;
        m_stats.bootNotMoved = 0;
        m_stats.bootPacked = false;
        m_moved.clear();
    }

    // Already contiguous and back to back in boot order: nothing to do.
    bool inOrder = true;
    for (size_t i = 0; i < files.size() && inOrder; ++i)
    {
        inOrder = CountFragments(files[i].extents) == 1 &&
            (i == 0 || FirstLcn(files[i].extents) == FirstLcn(files[i - 1].extents) + files[i - 1].clusters);
    }

    if (inOrder)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_stats.bootPacked = true;
        m_stats.bootPlaced = files.size();
        m_status.Format(_T("The %Iu boot files are already contiguous and packed in boot order; nothing to do."), files.size());
        Notify(100, true);
        return true;
    }

    // Analysed (fragmented) files by path, so the map and file list follow the moves.
    std::unordered_map<std::wstring, size_t> analysed;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        for (size_t i = 0; i < m_files.size(); ++i)
        {
            CString key = m_files[i].path;
            key.MakeUpper();
            analysed.emplace(std::wstring(key), i);
        }
    }

    // Pack everything into the first free run that can hold all of it (first fit from the
    // start of the volume, the fastest zone of a hard disk). Without one, fall back to
    // defragmenting each boot file wherever it fits.
    LONGLONG total = 0;
    for (const BootFile& f : files)
    {
        total += f.clusters;
    }

    m_cursor = 0;
    const LONGLONG start = FindFreeRun(total);
    const bool packing = start >= 0;
    if (packing)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        SetBits((ULONGLONG)start, (ULONGLONG)total);
    }

    // Walk the files in Layout.ini order; each one goes into the next slot of the reserved run.
    LONGLONG dest = start;
    for (size_t k = 0; k < files.size() && !m_cancel; ++k)
    {
        const BootFile& f = files[k];
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_status.Format(_T("Boot optimization %Iu of %Iu: %s"), k + 1, files.size(), (LPCTSTR)f.path);
        }

        m_percent = (int)(k * 100 / files.size());
        Notify(m_percent);

        std::vector<FileExtent> after;
        bool afterValid = false;
        MoveResult result = packing
            ? RelocateFile(f.path, dest, f.clusters, after, afterValid)
            : RelocateFile(f.path, -1, 0, after, afterValid);
        if (packing)
        {
            dest += f.clusters;   // keep each file's slot even if it failed, so the order holds
        }

        std::lock_guard<std::mutex> guard(m_lock);
        RecordMoveResult(f.path, result);
        if (result == MoveDone)
        {
            ++m_stats.bootPlaced;
        }
        else if (result != MoveCancelled)
        {
            ++m_stats.bootNotMoved;
        }

        if (afterValid)
        {
            CString key = f.path;
            key.MakeUpper();
            auto it = analysed.find(std::wstring(key));
            if (it != analysed.end())
            {
                FragmentedFile& analysedFile = m_files[it->second];
                analysedFile.fragments = CountFragments(after);
                analysedFile.extents = std::move(after);
                analysedFile.done = analysedFile.fragments <= 1;
            }
        }
    }

    ReadVolumeBitmap();
    // Re-read the real layout so the map and counters show what actually happened.
    std::vector<BootFile> refreshed;
    CollectBootFiles(paths, refreshed);
    UpdateBootState(refreshed);

    bool completed = !m_cancel;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_active = { -1, 0 };
        RecomputeFragmentStats();
        m_stats.bootPacked = packing && completed && m_stats.bootNotMoved == 0;
        if (packing)
        {
            m_status.Format(_T("Boot optimization %s: %I64u of %Iu boot files packed together in boot order from cluster %I64d; ")
                _T("%I64u could not be moved."),
                completed ? _T("complete") : _T("stopped"), m_stats.bootPlaced, files.size(), start, m_stats.bootNotMoved);
        }
        else
        {
            m_status.Format(_T("Boot optimization %s: no free area could hold all boot files together, so each was ")
                _T("defragmented separately. %I64u placed, %I64u could not be moved."),
                completed ? _T("complete") : _T("stopped"), m_stats.bootPlaced, m_stats.bootNotMoved);
        }
    }

    Notify(100, true);
    return completed;
}

// ---------------------------------------------------------------------------
// MFT consolidation
// ---------------------------------------------------------------------------

// Brings the MFT (the table of every file on an NTFS volume) closer to one piece by moving all
// of its extents after the first into a single free run. The first extent holds the records
// NTFS needs to mount the volume and cannot be moved, so two fragments is the best result.
// Called synchronously from the System Files dialog.
bool CDefragEngine::ConsolidateMft(CString& message)
{
    if (!IsOpen() || m_fileSystem != _T("NTFS"))
    {
        message = _T("MFT consolidation needs an NTFS volume.");
        return false;
    }

    // $MFT never appears in directory listings, but it can be opened by name for its attributes.
    HANDLE hMft = CreateFile(kLongPathPrefix + m_root + _T("$MFT"), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hMft == INVALID_HANDLE_VALUE)
    {
        message.Format(_T("Windows did not allow the MFT to be opened for moving (error %lu)."), GetLastError());
        return false;
    }

    std::vector<FileExtent> extents;
    if (!GetExtents(hMft, extents))
    {
        message.Format(_T("Cannot read the MFT's location (error %lu)."), GetLastError());
        CloseHandle(hMft);
        return false;
    }

    // The first extent holds the records NTFS itself needs and cannot move; gather the rest after it.
    const DWORD before = CountFragments(extents);
    size_t first = 0;
    while (first < extents.size() && extents[first].lcn < 0)
    {
        ++first;
    }

    LONGLONG rest = 0;
    for (size_t i = first + 1; i < extents.size(); ++i)
    {
        if (extents[i].lcn >= 0)
        {
            rest += extents[i].clusters;
        }
    }

    if (before <= 2 || rest == 0)
    {
        CloseHandle(hMft);
        message.Format(_T("The MFT is in %lu piece(s); nothing to consolidate (its first piece cannot be moved)."), before);
        return true;
    }

    m_cancel = false;
    m_percent = 0;
    if (!ReadVolumeBitmap())
    {
        CloseHandle(hMft);
        message = _T("Cannot read the volume bitmap.");
        return false;
    }

    m_cursor = 0;
    LONGLONG target = FindFreeRun(rest);
    if (target < 0)
    {
        CloseHandle(hMft);
        message.Format(_T("No free area is large enough to hold the rest of the MFT (%I64d clusters)."), rest);
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_lock);
        SetBits((ULONGLONG)target, (ULONGLONG)rest);
        m_moved.clear();
        m_active = { target, rest };
    }

    MoveResult result = MoveExtents(hMft, extents, first + 1, target);
    std::vector<FileExtent> after;
    bool afterValid = GetExtents(hMft, after);
    CloseHandle(hMft);

    {
        std::lock_guard<std::mutex> guard(m_lock);
        m_active = { -1, 0 };
    }

    ReadVolumeBitmap();
    DWORD afterFragments = afterValid ? CountFragments(after) : 0;
    if (result == MoveDone)
    {
        message.Format(_T("MFT consolidated: %lu fragments before, %lu after."), before, afterFragments);
    }
    else
    {
        message.Format(_T("The MFT could only be partly moved (error %lu): %lu fragments before, %lu after."),
            m_lastMoveError, before, afterFragments);
    }

    SetStatus(message);
    return result == MoveDone;
}

// ---------------------------------------------------------------------------
// Free-space bitmap
// ---------------------------------------------------------------------------

// Finds `clusters` contiguous free clusters and returns the first LCN, or -1 if there is no
// such run. Next fit: the search continues after the previous placement and wraps around once,
// which spreads moves across the volume and avoids rescanning full areas for every file.
LONGLONG CDefragEngine::FindFreeRun(LONGLONG clusters)
{
    std::lock_guard<std::mutex> guard(m_lock);
    const ULONGLONG total = m_stats.totalClusters;
    const ULONGLONG need = (ULONGLONG)clusters;
    if (need == 0 || need > total || m_bitmap.empty())
    {
        return -1;
    }

    // Scans [from, to) for a free run, skipping whole bitmap bytes that are entirely used or free.
    auto search = [&](ULONGLONG from, ULONGLONG to) -> LONGLONG
    {
        ULONGLONG runStart = 0, runLen = 0;
        for (ULONGLONG lcn = from; lcn < to;)
        {
            if ((lcn & 7) == 0 && lcn + 8 <= to)
            {
                BYTE b = m_bitmap[(size_t)(lcn >> 3)];
                if (b == 0xFF)
                {
                    runLen = 0;
                    lcn += 8;
                    continue;
                }

                if (b == 0x00)
                {
                    if (runLen == 0)
                    {
                        runStart = lcn;
                    }

                    runLen += 8;
                    lcn += 8;
                    if (runLen >= need)
                    {
                        return (LONGLONG)runStart;
                    }

                    continue;
                }
            }

            if (TestBit(lcn))
            {
                runLen = 0;
            }
            else
            {
                if (runLen == 0)
                {
                    runStart = lcn;
                }

                if (++runLen >= need)
                {
                    return (LONGLONG)runStart;
                }
            }

            ++lcn;
        }

        return -1;
    };

    // Search from the cursor to the end, then from the start up to where the first search began.
    ULONGLONG cursor = m_cursor < total ? m_cursor : 0;
    LONGLONG found = search(cursor, total);
    if (found < 0 && cursor > 0)
    {
        found = search(0, (std::min)(total, cursor + need));
    }

    if (found >= 0)
    {
        m_cursor = (ULONGLONG)found + need;
    }

    return found;
}

// Marks clusters as used in the local bitmap, reserving a target run before moving into it so
// the next search cannot pick the same clusters. Caller must hold m_lock.
void CDefragEngine::SetBits(ULONGLONG lcn, ULONGLONG count)
{
    ULONGLONG end = (std::min)(lcn + count, m_stats.totalClusters);
    for (ULONGLONG i = lcn; i < end; ++i)
    {
        BYTE& b = m_bitmap[(size_t)(i >> 3)];
        BYTE mask = (BYTE)(1u << (i & 7));
        if (!(b & mask))
        {
            b |= mask;
            --m_stats.freeClusters;
        }
    }
}

// True if any cluster in [from, to) is in use: bit by bit up to a byte boundary, then whole
// bytes, then the remaining bits. Caller must hold m_lock.
bool CDefragEngine::AnyBitSet(ULONGLONG from, ULONGLONG to) const
{
    while (from < to && (from & 7))
    {
        if (TestBit(from++))
        {
            return true;
        }
    }

    while (from + 8 <= to)
    {
        if (m_bitmap[(size_t)(from >> 3)])
        {
            return true;
        }

        from += 8;
    }

    while (from < to)
    {
        if (TestBit(from++))
        {
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// UI snapshots
// ---------------------------------------------------------------------------

// Builds the cluster map. Each cell covers total/cellCount clusters. Layers, later ones winning:
// used, fragmented, highly fragmented, defragmented in this pass, boot files, move in progress.
void CDefragEngine::BuildCellMap(std::vector<BYTE>& cells, size_t cellCount) const
{
    cells.assign(cellCount, CellFree);
    std::lock_guard<std::mutex> guard(m_lock);
    const ULONGLONG total = m_stats.totalClusters;
    if (total == 0 || cellCount == 0 || m_bitmap.empty())
    {
        return;
    }

    // Base layer: a cell is "used" if any of its clusters is in use.
    const double perCell = (double)total / cellCount;
    for (size_t c = 0; c < cellCount; ++c)
    {
        ULONGLONG start = (ULONGLONG)(c * perCell);
        ULONGLONG end = (ULONGLONG)((c + 1) * perCell);
        if (end <= start)
        {
            end = start + 1;
        }

        if (end > total)
        {
            end = total;
        }

        if (start < end && AnyBitSet(start, end))
        {
            cells[c] = CellUsed;
        }
    }

    // Paints every cell that a run of clusters touches.
    auto mark = [&](LONGLONG lcn, LONGLONG len, BYTE state)
    {
        if (lcn < 0 || len <= 0)
        {
            return;
        }

        size_t first = (size_t)(lcn / perCell);
        size_t last = (std::min)((size_t)((lcn + len - 1) / perCell), cellCount - 1);
        for (size_t c = first; c <= last; ++c)
        {
            cells[c] = state;
        }
    };

    // Two passes so a highly fragmented file is never hidden by an ordinary one sharing a cell.
    const DWORD high = m_highThreshold;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const FragmentedFile& f : m_files)
        {
            bool isHigh = f.fragments >= high;
            if (f.done || f.fragments <= 1 || isHigh != (pass == 1))
            {
                continue;
            }

            for (const FileExtent& e : f.extents)
            {
                mark(e.lcn, e.clusters, isHigh ? CellHighlyFragmented : CellFragmented);
            }
        }
    }

    // Overlays in priority order: moved in this pass, boot files, then the move in progress.
    for (const Range& r : m_moved)
    {
        mark(r.lcn, r.clusters, CellMoved);
    }

    for (const Range& r : m_bootRanges)
    {
        mark(r.lcn, r.clusters, CellBoot);
    }

    mark(m_active.lcn, m_active.clusters, CellActive);
}

// Copies of the shared state, taken under the lock.
VolumeStats CDefragEngine::GetStats() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_stats;
}

CString CDefragEngine::GetStatusText() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_status;
}

// Count and size of not-yet-defragmented files at or above the highly fragmented threshold.
void CDefragEngine::GetHighlyFragmented(ULONGLONG& files, ULONGLONG& bytes) const
{
    files = 0;
    bytes = 0;
    const DWORD high = m_highThreshold;
    std::lock_guard<std::mutex> guard(m_lock);
    for (const FragmentedFile& f : m_files)
    {
        if (!f.done && f.fragments >= high)
        {
            ++files;
            bytes += f.size;
        }
    }
}

// Most fragmented files first, limited to maxCount, copied without their extent lists
// (the UI only needs the path, size and fragment count).
void CDefragEngine::GetFragmentedFiles(std::vector<FragmentedFile>& out, size_t maxCount, DWORD minFragments) const
{
    out.clear();
    std::lock_guard<std::mutex> guard(m_lock);
    std::vector<const FragmentedFile*> list;
    for (const FragmentedFile& f : m_files)
    {
        if (!f.done && f.fragments > 1 && f.fragments >= minFragments)
        {
            list.push_back(&f);
        }
    }

    size_t n = (std::min)(maxCount, list.size());
    // Only the top n need ordering; a full volume can have hundreds of thousands of entries.
    std::partial_sort(list.begin(), list.begin() + n, list.end(),
        [](const FragmentedFile* a, const FragmentedFile* b) { return a->fragments > b->fragments; });
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        FragmentedFile copy;
        copy.path = list[i]->path;
        copy.size = list[i]->size;
        copy.clusters = list[i]->clusters;
        copy.fragments = list[i]->fragments;
        out.push_back(std::move(copy));
    }
}
