// DefragLensBoot: native program run by smss.exe from the BootExecute registry value, before Windows
// starts services, opens the page file or lets any program lock files.
//
// Safety rules, in order:
//   1. Remove our BootExecute entry and delete the job file before doing anything else, so a crash or
//      hang can never repeat on the next boot.
//   2. Refuse to touch a volume that is marked dirty (Windows' disk check must repair it first).
//   3. Move data only with FSCTL_MOVE_FILE, the same journaled operation used while Windows runs:
//      NTFS copies the clusters and switches the file over atomically, so an interruption leaves each
//      file whole in either its old or new place. Nothing is ever written to the volume directly.
//   4. Stop at the job's time limit, and log every step to a file the main program shows afterwards.
//
// Only ntdll.dll is available at this stage: no kernel32, no C runtime, no static constructors.

#include "NtApi.h"

// The compiler emits calls to memset/memcpy for struct initialisation and copies. Without the C
// runtime they must be defined here; #pragma function stops the compiler expanding them inline.
#pragma function(memset, memcpy)
extern "C" void* __cdecl memset(void* dest, int value, size_t count)
{
    volatile unsigned char* p = static_cast<unsigned char*>(dest);   // volatile: keep the compiler from calling memset here
    while (count--)
    {
        *p++ = static_cast<unsigned char>(value);
    }

    return dest;
}

extern "C" void* __cdecl memcpy(void* dest, const void* src, size_t count)
{
    volatile unsigned char* d = static_cast<unsigned char*>(dest);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    while (count--)
    {
        *d++ = *s++;
    }

    return dest;
}

namespace
{
    // Names and paths. Native programs use NT paths: \Registry\Machine\... for HKLM, \SystemRoot\...
    // for the Windows folder, \??\C:\... for drive-letter paths.
    const WCHAR kEntryName[] = L"DefragLensBoot";
    const WCHAR kSessionManagerKey[] = L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    const WCHAR kJobPath[] = L"\\SystemRoot\\DefragLensBoot\\job.txt";
    const WCHAR kLogPath[] = L"\\SystemRoot\\DefragLensBoot\\boot.log";
    const LONGLONG kMoveChunk = 16384;                 // clusters per FSCTL_MOVE_FILE call
    // Upper limit on job.txt (thousands of file paths fit comfortably).
    const ULONG kMaxJobBytes = 4 * 1024 * 1024;
    const LONGLONG kTicksPerSecond = 10000000;         // NtQuerySystemTime units

    // ------------------------------------------------------------------------
    // Memory and strings
    // ------------------------------------------------------------------------

    // Private heap created in NtProcessStartup; all memory comes from here.
    PVOID g_heap;

    // Zero-filled allocation; returns nullptr when out of memory.
    void* Alloc(SIZE_T size)
    {
        return RtlAllocateHeap(g_heap, HEAP_ZERO_MEMORY, size);
    }

    void Free(void* p)
    {
        if (p)
        {
            RtlFreeHeap(g_heap, 0, p);
        }
    }

    // Minimal growable array for plain data (no constructors or destructors are run).
    // Call Init() before use and Release() when done.
    template <class T>
    struct Array
    {
        T* items;
        SIZE_T count;
        SIZE_T capacity;

        void Init()
        {
            items = nullptr;
            count = 0;
            capacity = 0;
        }

        bool Push(const T& value)
        {
            if (count == capacity)
            {
                SIZE_T newCapacity = capacity ? capacity * 2 : 64;
                T* grown = static_cast<T*>(Alloc(newCapacity * sizeof(T)));
                if (!grown)
                {
                    return false;
                }

                if (count)
                {
                    memcpy(grown, items, count * sizeof(T));
                }

                Free(items);
                items = grown;
                capacity = newCapacity;
            }

            items[count++] = value;
            return true;
        }

        void Release()
        {
            Free(items);
            Init();
        }
    };

    // Small string helpers (the C runtime's wcslen and friends are not available).
    SIZE_T StrLen(const WCHAR* s)
    {
        SIZE_T n = 0;
        while (s[n])
        {
            ++n;
        }

        return n;
    }

    // ASCII-only upper-casing: enough for comparing our own keywords and drive letters.
    WCHAR Upper(WCHAR c)
    {
        return (c >= L'a' && c <= L'z') ? static_cast<WCHAR>(c - 32) : c;
    }

    // Case-insensitive "does s begin with prefix".
    bool StartsWithNoCase(const WCHAR* s, const WCHAR* prefix)
    {
        while (*prefix)
        {
            if (Upper(*s++) != Upper(*prefix++))
            {
                return false;
            }
        }

        return true;
    }

    // Heap copy of a + b, zero-terminated.
    WCHAR* Concat(const WCHAR* a, const WCHAR* b)
    {
        SIZE_T la = StrLen(a), lb = StrLen(b);
        WCHAR* out = static_cast<WCHAR*>(Alloc((la + lb + 1) * sizeof(WCHAR)));
        if (out)
        {
            memcpy(out, a, la * sizeof(WCHAR));
            memcpy(out + la, b, lb * sizeof(WCHAR));
        }

        return out;
    }

    // Points a UNICODE_STRING at an existing zero-terminated string (no copy).
    void InitString(UNICODE_STRING& us, const WCHAR* s)
    {
        us.Buffer = const_cast<WCHAR*>(s);
        us.Length = static_cast<USHORT>(StrLen(s) * sizeof(WCHAR));
        us.MaximumLength = static_cast<USHORT>(us.Length + sizeof(WCHAR));
    }

    // Fixed-size line builder (kept small: no large stack frames without the C runtime).
    struct Text
    {
        static const SIZE_T kMax = 400;
        WCHAR buf[kMax];
        SIZE_T len;

        Text& Clear()
        {
            len = 0;
            buf[0] = 0;
            return *this;
        }

        Text& Add(const WCHAR* s)
        {
            while (*s && len < kMax - 1)
            {
                buf[len++] = *s++;
            }

            buf[len] = 0;
            return *this;
        }

        Text& Num(ULONGLONG v)
        {
            WCHAR digits[24];
            int n = 0;
            do
            {
                digits[n++] = static_cast<WCHAR>(L'0' + v % 10);
                v /= 10;
            } while (v);

            while (n && len < kMax - 1)
            {
                buf[len++] = digits[--n];
            }

            buf[len] = 0;
            return *this;
        }

        Text& Hex(ULONG v)
        {
            Add(L"0x");
            for (int shift = 28; shift >= 0 && len < kMax - 1; shift -= 4)
            {
                ULONG nibble = (v >> shift) & 0xF;
                buf[len++] = static_cast<WCHAR>(nibble < 10 ? L'0' + nibble : L'A' + nibble - 10);
            }

            buf[len] = 0;
            return *this;
        }
    };

    // ------------------------------------------------------------------------
    // Files, log and boot screen
    // ------------------------------------------------------------------------

    // NtCreateFile wrapper for an NT path.
    NTSTATUS OpenPath(const WCHAR* ntPath, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options, HANDLE* handle)
    {
        UNICODE_STRING name;
        InitString(name, ntPath);
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        IO_STATUS_BLOCK iosb;
        return NtCreateFile(handle, access, &oa, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition, options, nullptr, 0);
    }

    // boot.log, open for the whole run; null if it could not be created.
    HANDLE g_log;

    // Writes a line on the boot screen (the same place Windows' disk check shows its progress).
    void Display(const WCHAR* s)
    {
        UNICODE_STRING us;
        InitString(us, s);
        NtDisplayString(&us);
        WCHAR newline[] = L"\n";
        InitString(us, newline);
        NtDisplayString(&us);
    }

    // Shows a line on screen and appends it to boot.log.
    void Log(const Text& line)
    {
        Display(line.buf);
        if (!g_log)
        {
            return;
        }

        IO_STATUS_BLOCK iosb;
        NtWriteFile(g_log, nullptr, nullptr, nullptr, &iosb, const_cast<WCHAR*>(line.buf),
            static_cast<ULONG>(line.len * sizeof(WCHAR)), nullptr, nullptr);
        WCHAR crlf[] = L"\r\n";
        NtWriteFile(g_log, nullptr, nullptr, nullptr, &iosb, crlf, 2 * sizeof(WCHAR), nullptr, nullptr);
        NtFlushBuffersFile(g_log, &iosb);   // keep the log useful even if the machine stops mid-run
    }

    // Convenience overload for fixed text.
    void Log(const WCHAR* s)
    {
        Text t;
        t.Clear().Add(s);
        Log(t);
    }

    // Creates (or replaces) boot.log as UTF-16LE with a byte-order mark, so Notepad and the main
    // program read it correctly.
    void OpenLog()
    {
        HANDLE h = nullptr;
        if (NT_SUCCESS(OpenPath(kLogPath, GENERIC_WRITE | SYNCHRONIZE, FILE_SHARE_READ, kFileOverwriteIf,
            kFileSynchronousIo | kFileNonDirectoryFile, &h)))
        {
            g_log = h;
            WCHAR bom = 0xFEFF;
            IO_STATUS_BLOCK iosb;
            NtWriteFile(g_log, nullptr, nullptr, nullptr, &iosb, &bom, sizeof(bom), nullptr, nullptr);
        }
    }

    // ------------------------------------------------------------------------
    // BootExecute entry
    // ------------------------------------------------------------------------

    // Removes every BootExecute string that starts with our name; leaves all others untouched.
    NTSTATUS RemoveBootExecuteEntry(bool& removed)
    {
        removed = false;
        UNICODE_STRING keyName;
        InitString(keyName, kSessionManagerKey);
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &keyName, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        HANDLE key = nullptr;
        NTSTATUS status = NtOpenKey(&key, KEY_QUERY_VALUE | KEY_SET_VALUE, &oa);
        if (!NT_SUCCESS(status))
        {
            return status;
        }

        WCHAR valueText[] = L"BootExecute";
        UNICODE_STRING valueName;
        InitString(valueName, valueText);
        const ULONG infoSize = 64 * 1024;
        auto* info = static_cast<NtKeyValuePartialInformation*>(Alloc(infoSize));
        auto* rebuilt = static_cast<WCHAR*>(Alloc(infoSize));
        ULONG resultLength = 0;
        status = info && rebuilt
            ? NtQueryValueKey(key, &valueName, kKeyValuePartialInformation, info, infoSize, &resultLength)
            : STATUS_NO_MEMORY;
        if (NT_SUCCESS(status) && info->Type == REG_MULTI_SZ)
        {
            const WCHAR* src = reinterpret_cast<const WCHAR*>(info->Data);
            SIZE_T srcChars = info->DataLength / sizeof(WCHAR);
            SIZE_T out = 0;
            SIZE_T i = 0;
            while (i < srcChars && src[i])
            {
                const WCHAR* entry = src + i;
                SIZE_T entryLen = 0;
                while (i + entryLen < srcChars && entry[entryLen])
                {
                    ++entryLen;
                }

                SIZE_T nameLen = StrLen(kEntryName);
                bool ours = entryLen >= nameLen && StartsWithNoCase(entry, kEntryName) &&
                    (entryLen == nameLen || entry[nameLen] == L' ');
                if (ours)
                {
                    removed = true;
                }
                else
                {
                    memcpy(rebuilt + out, entry, entryLen * sizeof(WCHAR));
                    out += entryLen;
                    rebuilt[out++] = 0;
                }

                i += entryLen + 1;
            }

            rebuilt[out++] = 0;
            if (removed)
            {
                status = NtSetValueKey(key, &valueName, 0, REG_MULTI_SZ, rebuilt, static_cast<ULONG>(out * sizeof(WCHAR)));
                NtFlushKey(key);
            }
        }

        Free(info);
        Free(rebuilt);
        NtClose(key);
        return status;
    }

    // ------------------------------------------------------------------------
    // Job
    // ------------------------------------------------------------------------

    // The job read from job.txt (see BootTimeJob.cpp for the writer).
    struct Job
    {
        WCHAR volume[3];          // "C:"
        bool dryRun;
        bool systemFiles;
        bool mft;
        ULONG minutes;
        Array<WCHAR*> files;      // "C:\..." paths, heap strings
    };

    Job g_job;

    // Decimal digits at the start of s; stops at the first non-digit.
    ULONG ParseNumber(const WCHAR* s)
    {
        ULONG v = 0;
        while (*s >= L'0' && *s <= L'9')
        {
            v = v * 10 + (*s++ - L'0');
        }

        return v;
    }

    // Reads and deletes the job file (FILE_DELETE_ON_CLOSE): a job runs at most once.
    bool ReadJob(NTSTATUS& status)
    {
        g_job.volume[0] = 0;
        g_job.dryRun = true;
        g_job.systemFiles = false;
        g_job.mft = false;
        g_job.minutes = 10;
        g_job.files.Init();

        HANDLE h = nullptr;
        status = OpenPath(kJobPath, GENERIC_READ | DELETE | SYNCHRONIZE, FILE_SHARE_READ, kFileOpen,
            kFileSynchronousIo | kFileNonDirectoryFile | kFileDeleteOnClose, &h);
        if (!NT_SUCCESS(status))
        {
            return false;
        }

        auto* data = static_cast<BYTE*>(Alloc(kMaxJobBytes + sizeof(WCHAR)));
        ULONG total = 0;
        while (data && total < kMaxJobBytes)
        {
            IO_STATUS_BLOCK iosb;
            NTSTATUS readStatus = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, data + total, kMaxJobBytes - total,
                nullptr, nullptr);
            if (!NT_SUCCESS(readStatus) || iosb.Information == 0)
            {
                break;
            }

            total += static_cast<ULONG>(iosb.Information);
        }

        NtClose(h);   // deletes the job file
        if (!data)
        {
            status = STATUS_NO_MEMORY;
            return false;
        }

        WCHAR* text = reinterpret_cast<WCHAR*>(data);
        SIZE_T chars = total / sizeof(WCHAR);
        text[chars] = 0;
        SIZE_T pos = (chars && text[0] == 0xFEFF) ? 1 : 0;
        bool versionOk = false;
        while (pos < chars)
        {
            WCHAR* line = text + pos;
            while (pos < chars && text[pos] != L'\r' && text[pos] != L'\n')
            {
                ++pos;
            }

            while (pos < chars && (text[pos] == L'\r' || text[pos] == L'\n'))
            {
                text[pos++] = 0;
            }

            if (StartsWithNoCase(line, L"version="))
            {
                versionOk = ParseNumber(line + 8) == 1;
            }
            else if (StartsWithNoCase(line, L"volume=") && line[7] && line[8] == L':')
            {
                g_job.volume[0] = Upper(line[7]);
                g_job.volume[1] = L':';
                g_job.volume[2] = 0;
            }
            else if (StartsWithNoCase(line, L"dryrun="))
            {
                g_job.dryRun = line[7] != L'0';
            }
            else if (StartsWithNoCase(line, L"systemfiles="))
            {
                g_job.systemFiles = line[12] == L'1';
            }
            else if (StartsWithNoCase(line, L"mft="))
            {
                g_job.mft = line[4] == L'1';
            }
            else if (StartsWithNoCase(line, L"minutes="))
            {
                ULONG minutes = ParseNumber(line + 8);
                g_job.minutes = minutes < 1 ? 1 : (minutes > 240 ? 240 : minutes);
            }
            else if (StartsWithNoCase(line, L"file=") && g_job.volume[0] && Upper(line[5]) == g_job.volume[0] &&
                line[6] == L':' && line[7] == L'\\')
            {
                WCHAR* copy = Concat(line + 5, L"");
                if (copy)
                {
                    g_job.files.Push(copy);
                }
            }
        }

        Free(data);
        if (!versionOk || !g_job.volume[0])
        {
            status = STATUS_INVALID_PARAMETER;
            return false;
        }

        return true;
    }

    // ------------------------------------------------------------------------
    // Volume
    // ------------------------------------------------------------------------

    // Volume state for the run: handle, allocation bitmap (as returned by FSCTL_GET_VOLUME_BITMAP,
    // header included), next-fit cursor and the deadline from the job's time limit.
    HANDLE g_volume;
    BYTE* g_bitmap;
    ULONGLONG g_totalClusters;
    ULONGLONG g_cursor;
    LARGE_INTEGER g_deadline;
    bool g_timeUp;

    // Counters for the summary line.
    ULONG g_examined, g_alreadyOk, g_moved, g_wouldMove, g_notMoved, g_skipped;

    // True once the time limit has passed; checked before each file and between move chunks.
    bool TimeUp()
    {
        if (!g_timeUp)
        {
            LARGE_INTEGER now;
            NtQuerySystemTime(&now);
            g_timeUp = now.QuadPart >= g_deadline.QuadPart;
        }

        return g_timeUp;
    }

    // Synchronous file-system control call (handles here are opened for synchronous I/O).
    NTSTATUS Fsctl(HANDLE h, ULONG code, PVOID in, ULONG inSize, PVOID out, ULONG outSize)
    {
        IO_STATUS_BLOCK iosb;
        return NtFsControlFile(h, nullptr, nullptr, nullptr, &iosb, code, in, inSize, out, outSize);
    }

    // Reads the whole allocation bitmap: a small probe returns the volume size
    // (STATUS_BUFFER_OVERFLOW), then one call with a big enough buffer fetches everything.
    NTSTATUS ReadBitmap()
    {
        STARTING_LCN_INPUT_BUFFER in = {};
        ULONGLONG probe[8] = {};
        NTSTATUS status = Fsctl(g_volume, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in), probe, sizeof(probe));
        if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW)
        {
            return status;
        }

        ULONGLONG total = static_cast<ULONGLONG>(reinterpret_cast<VOLUME_BITMAP_BUFFER*>(probe)->BitmapSize.QuadPart);
        SIZE_T bytes = static_cast<SIZE_T>((total + 7) / 8);
        SIZE_T size = FIELD_OFFSET(VOLUME_BITMAP_BUFFER, Buffer) + bytes + 16;
        auto* buffer = static_cast<BYTE*>(Alloc(size));
        if (!buffer)
        {
            return STATUS_NO_MEMORY;
        }

        status = Fsctl(g_volume, FSCTL_GET_VOLUME_BITMAP, &in, sizeof(in), buffer, static_cast<ULONG>(size));
        if (!NT_SUCCESS(status))
        {
            Free(buffer);
            return status;
        }

        Free(g_bitmap);
        g_bitmap = buffer;
        g_totalClusters = total;
        return STATUS_SUCCESS;
    }

    // The bitmap bytes after the VOLUME_BITMAP_BUFFER header; one bit per cluster, 1 = in use.
    BYTE* BitmapBits()
    {
        return reinterpret_cast<VOLUME_BITMAP_BUFFER*>(g_bitmap)->Buffer;
    }

    // True if cluster `lcn` is in use.
    bool TestBit(ULONGLONG lcn)
    {
        return ((BitmapBits()[lcn >> 3] >> (lcn & 7)) & 1) != 0;
    }

    // Reserves clusters in our copy of the bitmap so a later search does not pick them again.
    void SetBits(ULONGLONG lcn, ULONGLONG count)
    {
        for (ULONGLONG i = lcn; i < lcn + count && i < g_totalClusters; ++i)
        {
            BitmapBits()[i >> 3] |= static_cast<BYTE>(1u << (i & 7));
        }
    }

    // Looks for `need` free clusters in a row within [from, to); skips whole bitmap bytes that are
    // entirely used or free. Returns the first cluster of the run or -1.
    LONGLONG SearchRun(ULONGLONG from, ULONGLONG to, ULONGLONG need)
    {
        BYTE* bits = BitmapBits();
        ULONGLONG runStart = 0, runLen = 0;
        for (ULONGLONG lcn = from; lcn < to;)
        {
            if ((lcn & 7) == 0 && lcn + 8 <= to)
            {
                BYTE b = bits[lcn >> 3];
                if (b == 0xFF)
                {
                    runLen = 0;
                    lcn += 8;
                    continue;
                }

                if (b == 0)
                {
                    if (runLen == 0)
                    {
                        runStart = lcn;
                    }

                    runLen += 8;
                    lcn += 8;
                    if (runLen >= need)
                    {
                        return static_cast<LONGLONG>(runStart);
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
                    return static_cast<LONGLONG>(runStart);
                }
            }

            ++lcn;
        }

        return -1;
    }

    // Next fit: search from the cursor to the end, then wrap around once. Same approach as the
    // main program's engine.
    LONGLONG FindFreeRun(ULONGLONG need)
    {
        if (need == 0 || need > g_totalClusters)
        {
            return -1;
        }

        ULONGLONG cursor = g_cursor < g_totalClusters ? g_cursor : 0;
        LONGLONG found = SearchRun(cursor, g_totalClusters, need);
        if (found < 0 && cursor > 0)
        {
            ULONGLONG end = cursor + need < g_totalClusters ? cursor + need : g_totalClusters;
            found = SearchRun(0, end, need);
        }

        if (found >= 0)
        {
            g_cursor = static_cast<ULONGLONG>(found) + need;
        }

        return found;
    }

    // ------------------------------------------------------------------------
    // Files
    // ------------------------------------------------------------------------

    // One run of a file's clusters: file position (VCN), volume position (LCN, -1 for sparse
    // runs) and length.
    struct Extent
    {
        LONGLONG vcn;
        LONGLONG lcn;
        LONGLONG clusters;
    };

    // Reads a file's extent list in batches; STATUS_BUFFER_OVERFLOW means "more to come".
    NTSTATUS GetExtents(HANDLE file, Array<Extent>& extents)
    {
        extents.count = 0;
        STARTING_VCN_INPUT_BUFFER in = {};
        const ULONG outSize = 16 * 1024;
        auto* out = static_cast<RETRIEVAL_POINTERS_BUFFER*>(Alloc(outSize));
        if (!out)
        {
            return STATUS_NO_MEMORY;
        }

        NTSTATUS status;
        for (;;)
        {
            status = Fsctl(file, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof(in), out, outSize);
            if (status == STATUS_END_OF_FILE)
            {
                status = STATUS_SUCCESS;   // resident in the MFT: no clusters
                break;
            }

            if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW)
            {
                break;
            }

            LONGLONG vcn = out->StartingVcn.QuadPart;
            for (DWORD i = 0; i < out->ExtentCount; ++i)
            {
                LONGLONG next = out->Extents[i].NextVcn.QuadPart;
                extents.Push({ vcn, out->Extents[i].Lcn.QuadPart, next - vcn });
                vcn = next;
            }

            if (status != STATUS_BUFFER_OVERFLOW || out->ExtentCount == 0)
            {
                status = STATUS_SUCCESS;
                break;
            }

            in.StartingVcn.QuadPart = vcn;
        }

        Free(out);
        return status;
    }

    // Physically separate pieces; extents that continue exactly where the previous one ended
    // count as the same piece.
    ULONG CountFragments(const Array<Extent>& extents)
    {
        ULONG fragments = 0;
        LONGLONG nextLcn = -2;
        for (SIZE_T i = 0; i < extents.count; ++i)
        {
            if (extents.items[i].lcn < 0)
            {
                continue;
            }

            if (extents.items[i].lcn != nextLcn)
            {
                ++fragments;
            }

            nextLcn = extents.items[i].lcn + extents.items[i].clusters;
        }

        return fragments;
    }

    // Moves extents[first..] one after another to clusters starting at dest, in chunks of
    // kMoveChunk so the time limit is checked regularly. Each FSCTL_MOVE_FILE call is atomic.
    NTSTATUS MoveExtents(HANDLE file, const Array<Extent>& extents, SIZE_T first, LONGLONG dest)
    {
        for (SIZE_T i = first; i < extents.count; ++i)
        {
            const Extent& e = extents.items[i];
            if (e.lcn < 0)
            {
                continue;
            }

            for (LONGLONG done = 0; done < e.clusters;)
            {
                if (TimeUp())
                {
                    return STATUS_CANCELLED;   // not STATUS_TIMEOUT: that one counts as success under NT_SUCCESS
                }

                LONGLONG count = e.clusters - done < kMoveChunk ? e.clusters - done : kMoveChunk;
                MOVE_FILE_DATA mfd = {};
                mfd.FileHandle = file;
                mfd.StartingVcn.QuadPart = e.vcn + done;
                mfd.StartingLcn.QuadPart = dest;
                mfd.ClusterCount = static_cast<DWORD>(count);
                NTSTATUS status = Fsctl(g_volume, FSCTL_MOVE_FILE, &mfd, sizeof(mfd), nullptr, 0);
                if (!NT_SUCCESS(status))
                {
                    return status;
                }

                dest += count;
                done += count;
            }
        }

        return STATUS_SUCCESS;
    }

    // Defragments one file. keepFirst leaves the first allocated extent in place ($MFT).
    void ProcessFile(const WCHAR* dosPath, bool keepFirst)
    {
        if (TimeUp())
        {
            return;
        }

        ++g_examined;
        Text t;
        WCHAR* ntPath = Concat(L"\\??\\", dosPath);
        if (!ntPath)
        {
            Log(t.Clear().Add(dosPath).Add(L": out of memory, skipped."));
            ++g_skipped;
            return;
        }

        HANDLE file = nullptr;
        NTSTATUS status = OpenPath(ntPath, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, kFileOpen,
            kFileSynchronousIo | kFileOpenForBackup | kFileOpenReparsePoint, &file);
        Free(ntPath);
        if (!NT_SUCCESS(status))
        {
            if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND)
            {
                Log(t.Clear().Add(dosPath).Add(L": not present, skipped."));
            }
            else
            {
                Log(t.Clear().Add(dosPath).Add(L": cannot be opened (").Hex(static_cast<ULONG>(status)).Add(L"), skipped."));
            }

            ++g_skipped;
            return;
        }

        Array<Extent> extents;
        extents.Init();
        status = GetExtents(file, extents);
        if (!NT_SUCCESS(status))
        {
            Log(t.Clear().Add(dosPath).Add(L": location unreadable (").Hex(static_cast<ULONG>(status)).Add(L"), skipped."));
            ++g_skipped;
            extents.Release();
            NtClose(file);
            return;
        }

        // First allocated extent; everything after it is what moves when keepFirst is set.
        SIZE_T firstAllocated = 0;
        while (firstAllocated < extents.count && extents.items[firstAllocated].lcn < 0)
        {
            ++firstAllocated;
        }

        bool hasVirtualRuns = false;
        ULONGLONG allocated = 0, toMove = 0;
        for (SIZE_T i = 0; i < extents.count; ++i)
        {
            if (extents.items[i].lcn < 0)
            {
                hasVirtualRuns = true;
                continue;
            }

            allocated += static_cast<ULONGLONG>(extents.items[i].clusters);
            if (!keepFirst || i > firstAllocated)
            {
                toMove += static_cast<ULONGLONG>(extents.items[i].clusters);
            }
        }

        ULONG before = CountFragments(extents);
        t.Clear().Add(dosPath).Add(L": ").Num(before).Add(L" fragment(s), ").Num(allocated).Add(L" clusters");
        Log(t);

        SIZE_T first = keepFirst ? firstAllocated + 1 : 0;
        if (before <= (keepFirst ? 2u : 1u) || toMove == 0)
        {
            Log(L"  already as contiguous as possible; nothing to do.");
            ++g_alreadyOk;
        }
        else if (hasVirtualRuns && !keepFirst)
        {
            Log(L"  compressed or sparse; left as is.");
            ++g_skipped;
        }
        else if (g_job.dryRun)
        {
            Log(t.Clear().Add(L"  dry run: would move ").Num(toMove).Add(L" clusters into one free area."));
            ++g_wouldMove;
        }
        else
        {
            LONGLONG target = FindFreeRun(toMove);
            if (target < 0)
            {
                Log(L"  no free area is large enough; left as is.");
                ++g_notMoved;
            }
            else
            {
                SetBits(static_cast<ULONGLONG>(target), toMove);
                status = MoveExtents(file, extents, first, target);
                Array<Extent> after;
                after.Init();
                ULONG afterCount = NT_SUCCESS(GetExtents(file, after)) ? CountFragments(after) : before;
                after.Release();
                if (NT_SUCCESS(status))
                {
                    Log(t.Clear().Add(L"  moved: now ").Num(afterCount).Add(L" fragment(s)."));
                    ++g_moved;
                }
                else
                {
                    // Every completed FSCTL_MOVE_FILE is atomic, so the file is intact; it may just be
                    // in more (or fewer) pieces than before.
                    Log(t.Clear().Add(L"  stopped (").Hex(static_cast<ULONG>(status)).Add(L"); file intact, now ")
                        .Num(afterCount).Add(L" fragment(s)."));
                    ++g_notMoved;
                    ReadBitmap();
                }
            }
        }

        extents.Release();
        NtClose(file);
    }

    // The pass itself: open the volume, refuse it if marked dirty, read the bitmap, then process
    // the system files, the listed files and the MFT, and log a summary.
    void Run()
    {
        Text t;
        Log(t.Clear().Add(L"Volume ").Add(g_job.volume).Add(g_job.dryRun ? L", DRY RUN (nothing will be moved)" : L"")
            .Add(L", time limit ").Num(g_job.minutes).Add(L" min, ").Num(g_job.files.count).Add(L" listed file(s)")
            .Add(g_job.systemFiles ? L", system files" : L"").Add(g_job.mft ? L", MFT" : L""));

        // Volume device opened read/write (what FSCTL_MOVE_FILE needs) without locking it.
        WCHAR volumePath[] = L"\\??\\X:";
        volumePath[4] = g_job.volume[0];
        NTSTATUS status = OpenPath(volumePath, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, kFileOpen, kFileSynchronousIo, &g_volume);
        if (!NT_SUCCESS(status))
        {
            Log(t.Clear().Add(L"Cannot open the volume (").Hex(static_cast<ULONG>(status)).Add(L"). Nothing was moved."));
            return;
        }

        ULONG dirty = 0;
        status = Fsctl(g_volume, FSCTL_IS_VOLUME_DIRTY, nullptr, 0, &dirty, sizeof(dirty));
        if (!NT_SUCCESS(status) || (dirty & VOLUME_IS_DIRTY))
        {
            Log(L"The volume is marked dirty (or its state is unknown). Windows' disk check must repair it first. "
                L"Nothing was moved.");
            NtClose(g_volume);
            return;
        }

        status = ReadBitmap();
        if (!NT_SUCCESS(status))
        {
            Log(t.Clear().Add(L"Cannot read the volume bitmap (").Hex(static_cast<ULONG>(status)).Add(L"). Nothing was moved."));
            NtClose(g_volume);
            return;
        }

        LARGE_INTEGER start;
        NtQuerySystemTime(&start);
        g_deadline.QuadPart = start.QuadPart + static_cast<LONGLONG>(g_job.minutes) * 60 * kTicksPerSecond;

        WCHAR root[] = L"X:\\";
        root[0] = g_job.volume[0];
        if (g_job.systemFiles)
        {
            // Not yet opened by Windows at this stage of startup.
            const WCHAR* names[] = { L"pagefile.sys", L"swapfile.sys", L"hiberfil.sys" };
            for (const WCHAR* name : names)
            {
                WCHAR* path = Concat(root, name);
                if (path)
                {
                    ProcessFile(path, false);
                    Free(path);
                }
            }
        }

        for (SIZE_T i = 0; i < g_job.files.count; ++i)
        {
            ProcessFile(g_job.files.items[i], false);
        }

        if (g_job.mft)
        {
            WCHAR* path = Concat(root, L"$MFT");
            if (path)
            {
                ProcessFile(path, true);
                Free(path);
            }
        }

        if (g_timeUp)
        {
            Log(L"Time limit reached; the remaining items were not processed.");
        }

        LARGE_INTEGER end;
        NtQuerySystemTime(&end);
        t.Clear().Add(L"Summary: ").Num(g_examined).Add(L" examined, ").Num(g_moved).Add(L" moved, ")
            .Num(g_alreadyOk).Add(L" already contiguous, ");
        if (g_job.dryRun)
        {
            t.Num(g_wouldMove).Add(L" would be moved, ");
        }

        t.Num(g_notMoved).Add(L" not moved, ").Num(g_skipped).Add(L" skipped; ")
            .Num(static_cast<ULONGLONG>((end.QuadPart - start.QuadPart) / kTicksPerSecond)).Add(L" s.");
        Log(t);

        Free(g_bitmap);
        NtClose(g_volume);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// Called by smss.exe (the image's entry point; there is no main or C runtime start-up).
// Must always end with NtTerminateProcess: smss waits for this process before Windows continues.
extern "C" void NTAPI NtProcessStartup(PVOID)
{
    g_heap = RtlCreateHeap(HEAP_GROWABLE, nullptr, 0, 0, nullptr, nullptr);
    BOOLEAN previous = FALSE;
    // The process runs as SYSTEM; enable the privileges needed to open any file and move clusters.
    RtlAdjustPrivilege(kSeBackupPrivilege, TRUE, FALSE, &previous);
    RtlAdjustPrivilege(kSeRestorePrivilege, TRUE, FALSE, &previous);
    RtlAdjustPrivilege(kSeManageVolumePrivilege, TRUE, FALSE, &previous);

    Display(L"DefragLens boot-time pass");

    // Rule 1: never run twice. Unregister first, then consume the job.
    bool removed = false;
    NTSTATUS removeStatus = RemoveBootExecuteEntry(removed);
    NTSTATUS jobStatus = STATUS_SUCCESS;
    bool haveJob = g_heap && ReadJob(jobStatus);

    OpenLog();
    Text t;
    Log(L"DefragLens boot-time pass");
    Log(t.Clear().Add(L"BootExecute entry: ").Add(removed ? L"removed" : L"not found")
        .Add(L" (").Hex(static_cast<ULONG>(removeStatus)).Add(L")"));

    if (!haveJob)
    {
        Log(t.Clear().Add(L"No valid job (").Hex(static_cast<ULONG>(jobStatus)).Add(L"); nothing to do."));
    }
    else
    {
        Run();
    }

    Log(L"Finished; Windows will now continue starting.");
    if (g_log)
    {
        NtClose(g_log);
    }

    NtTerminateProcess(NtCurrentProcess(), STATUS_SUCCESS);
}
