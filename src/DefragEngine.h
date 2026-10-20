// DefragEngine.h
// The defragmentation engine: everything that reads or changes the volume layout.
//
// It talks to the file system only through the documented defragmentation IOCTLs:
//   FSCTL_GET_VOLUME_BITMAP       which clusters are in use
//   FSCTL_GET_RETRIEVAL_POINTERS  where each file's clusters are
//   FSCTL_MOVE_FILE               move a range of a file's clusters (journaled and atomic in NTFS)
// The UI calls the long-running operations on a worker thread and polls thread-safe
// snapshots for display; the engine posts WM_DEFRAG_PROGRESS to say when to poll.

#pragma once

// Messages posted by the engine / worker thread to the notify window.
#define WM_DEFRAG_PROGRESS (WM_APP + 1)   // wParam: percent complete (0-100)
#define WM_DEFRAG_DONE     (WM_APP + 2)   // wParam: CDefragEngine::Operation, lParam: TRUE on success

// State of one cell in the cluster map. When several states fall in one cell, later
// overlays in BuildCellMap win: fragmented, highly fragmented, moved, boot, active.
enum CellState : BYTE
{
    CellFree = 0,           // every cluster in the cell is free
    CellUsed,               // at least one cluster is in use
    CellFragmented,         // part of a fragmented file
    CellMoved,              // written by the current defragmentation pass
    CellActive,             // the file currently being moved
    CellHighlyFragmented,   // part of a file at or above the "highly fragmented" threshold
    CellBoot,               // part of a file listed in Layout.ini (read at startup)
};

// Default for what counts as "highly fragmented" (fragments per file).
const DWORD kDefaultHighFragmentThreshold = 10;

// One run of a file's clusters, as returned by FSCTL_GET_RETRIEVAL_POINTERS.
struct FileExtent
{
    LONGLONG vcn;       // virtual cluster number inside the file
    LONGLONG lcn;       // logical cluster number on the volume, -1 for sparse/compressed runs
    LONGLONG clusters;
};

// Number of physically discontiguous runs; sparse/compressed (virtual) runs are ignored.
DWORD CountFragments(const std::vector<FileExtent>& extents);

// A file found fragmented by Analyze(), kept so the map, list and defragment passes can use it.
struct FragmentedFile
{
    CString path;           // full path without the "\\?\" prefix
    ULONGLONG size = 0;     // bytes (allocated size for folders)
    LONGLONG clusters = 0;  // allocated clusters
    DWORD fragments = 0;
    bool done = false;      // defragmented since the last analysis
    std::vector<FileExtent> extents;
};

// Counters shown in the statistics panel. Read with GetStats(); updated under m_lock.
struct VolumeStats
{
    DWORD bytesPerCluster = 0;
    ULONGLONG totalClusters = 0;
    ULONGLONG freeClusters = 0;
    ULONGLONG filesScanned = 0;
    ULONGLONG dirsScanned = 0;
    ULONGLONG filesSkipped = 0;       // could not be opened or queried
    ULONGLONG fragmentedFiles = 0;
    ULONGLONG totalFragments = 0;     // fragments belonging to fragmented files
    ULONGLONG fragmentedClusters = 0; // clusters belonging to fragmented files
    ULONGLONG filesDefragged = 0;
    ULONGLONG filesNotMoved = 0;

    // Boot files listed in %SystemRoot%\Prefetch\Layout.ini
    bool bootLayoutFound = false;
    ULONGLONG bootFiles = 0;          // listed files present on this volume and movable
    ULONGLONG bootFragmented = 0;
    ULONGLONG bootPlaced = 0;         // last Boot Optimize: placed (moved or already in place)
    ULONGLONG bootNotMoved = 0;
    bool bootPacked = false;          // last Boot Optimize put every file together in boot order
};

// Analyzes and defragments a volume using the Windows defragmentation IOCTLs
// (FSCTL_GET_VOLUME_BITMAP, FSCTL_GET_RETRIEVAL_POINTERS, FSCTL_MOVE_FILE).
// Analyze() and Defragment() run on a worker thread; the Get*/Build* methods
// are safe to call from the UI thread at any time.
class CDefragEngine
{
public:
    // The long-running operations the UI can start; also sent back in WM_DEFRAG_DONE.
    enum Operation { OpAnalyze = 1, OpDefragment = 2, OpDefragmentHigh = 3, OpBootOptimize = 4 };

    CDefragEngine() = default;
    ~CDefragEngine();
    CDefragEngine(const CDefragEngine&) = delete;
    CDefragEngine& operator=(const CDefragEngine&) = delete;

    // Opens the volume for defragmentation (needs administrator rights) and reads its bitmap.
    bool Open(const CString& root, CString& error);   // root such as "C:\\"
    // Releases the volume handle and forgets all analysis results.
    void Close();
    bool IsOpen() const { return m_hVolume != INVALID_HANDLE_VALUE; }
    bool HasAnalysis() const { return m_analyzed; }
    CString GetRoot() const { return m_root; }
    CString GetFileSystem() const { return m_fileSystem; }
    int GetMediaType() const { return m_mediaType; }   // 1 = SSD, 0 = HDD, -1 = unknown

    // Window that receives WM_DEFRAG_PROGRESS while an operation runs.
    void SetNotifyWindow(HWND hWnd) { m_hNotify = hWnd; }
    // Asks the running operation to stop at the next safe point (between move chunks).
    void RequestCancel() { m_cancel = true; }
    void ResetCancel() { m_cancel = false; }

    // Files with at least this many fragments are "highly fragmented" (map colour, counts).
    void SetHighFragmentThreshold(DWORD fragments) { m_highThreshold = (std::max)(2UL, fragments); }
    DWORD GetHighFragmentThreshold() const { return m_highThreshold; }

    // Worker-thread entry points. Defragment() only moves files with at least
    // minFragments fragments; the default covers every fragmented file.
    bool Analyze();
    bool Defragment(DWORD minFragments = 2);

    // Defragments the files Windows reads at startup (Layout.ini) and packs them
    // together in boot order, as close to the start of the volume as possible.
    bool BootOptimize();

    // Moves every MFT extent except the first (which NTFS pins) into one free run.
    // Runs on the calling thread; NTFS only.
    bool ConsolidateMft(CString& message);

    // Thread-safe snapshots for the UI.
    // Fills cells[0..cellCount) with CellState values, each cell covering an equal share of the volume.
    void BuildCellMap(std::vector<BYTE>& cells, size_t cellCount) const;
    VolumeStats GetStats() const;
    CString GetStatusText() const;
    // Most fragmented files first, at most maxCount, without their extent lists.
    void GetFragmentedFiles(std::vector<FragmentedFile>& out, size_t maxCount, DWORD minFragments = 2) const;
    // Count and total size of files at or above the highly fragmented threshold.
    void GetHighlyFragmented(ULONGLONG& files, ULONGLONG& bytes) const;
    // Files a Defragment or Boot Optimize pass could not move (typically in use); candidates for boot time.
    std::vector<CString> GetFailedPaths() const;

private:
    struct Range { LONGLONG lcn; LONGLONG clusters; };   // a run of clusters on the volume
    struct BootFile { CString path; std::vector<FileExtent> extents; LONGLONG clusters = 0; };
    enum MoveResult { MoveDone, MoveSkipped, MoveFailed, MoveCancelled };

    // Re-reads the allocation bitmap into m_bitmap and refreshes the cluster counters.
    bool ReadVolumeBitmap();
    // Asks the storage stack whether the volume's disk has a seek penalty (HDD) or not (SSD).
    int QueryMediaType() const;
    // Reads every extent of an open file; true for resident (MFT-only) files with no extents.
    static bool GetExtents(HANDLE hFile, std::vector<FileExtent>& extents);
    // Records one file or folder found during Analyze() (counts it, keeps it if fragmented).
    void AnalyzeEntry(const CString& path, bool isDir, ULONGLONG size, ULONGLONG& scannedClusters);
    // Defragments m_files[index] into the first suitable free run.
    MoveResult DefragFile(size_t index);
    // target < 0: find a free run and skip files that are already contiguous.
    // target >= 0: place the file exactly there (slot of plannedClusters, already reserved).
    MoveResult RelocateFile(const CString& path, LONGLONG target, LONGLONG plannedClusters,
        std::vector<FileExtent>& after, bool& afterValid);
    // Moves extents[first..] one after another to clusters starting at dest, in chunks.
    MoveResult MoveExtents(HANDLE hFile, const std::vector<FileExtent>& extents, size_t first, LONGLONG dest);
    // Opens each Layout.ini path on this volume and keeps the ones that can be moved.
    void CollectBootFiles(const std::vector<CString>& paths, std::vector<BootFile>& out) const;
    // Publishes boot file counts and map ranges for the UI.
    void UpdateBootState(const std::vector<BootFile>& files);
    // Keeps m_failedPaths in step with the latest attempt for a file.
    void RecordMoveResult(const CString& path, MoveResult result);   // caller must hold m_lock
    // Next-fit search for `clusters` contiguous free clusters; returns the first LCN or -1.
    LONGLONG FindFreeRun(LONGLONG clusters);
    // Recounts fragmented files, fragments and clusters from m_files.
    void RecomputeFragmentStats();

    // Bitmap helpers; caller must hold m_lock.
    bool TestBit(ULONGLONG lcn) const { return ((m_bitmap[(size_t)(lcn >> 3)] >> (lcn & 7)) & 1) != 0; }
    void SetBits(ULONGLONG lcn, ULONGLONG count);
    bool AnyBitSet(ULONGLONG from, ULONGLONG to) const;

    void SetStatus(const CString& text);
    // Posts WM_DEFRAG_PROGRESS, at most every kNotifyIntervalMs unless forced.
    void Notify(int percent, bool force = false);

    HANDLE m_hVolume = INVALID_HANDLE_VALUE;   // \\.\X: opened for read/write
    HWND m_hNotify = nullptr;
    CString m_root;                    // "C:\"
    CString m_fileSystem;              // "NTFS", "FAT32", ...
    int m_mediaType = -1;
    std::atomic<bool> m_cancel{ false };
    std::atomic<bool> m_analyzed{ false };
    std::atomic<DWORD> m_highThreshold{ kDefaultHighFragmentThreshold };
    ULONGLONG m_lastNotify = 0;
    int m_percent = 0;                 // progress of the current defrag pass
    ULONGLONG m_cursor = 0;            // next-fit position for free-space search
    DWORD m_lastMoveError = 0;         // GetLastError() of the last failed FSCTL_MOVE_FILE

    mutable std::mutex m_lock;         // guards everything below
    std::vector<BYTE> m_bitmap;        // 1 bit per cluster, 1 = in use
    VolumeStats m_stats;
    std::vector<FragmentedFile> m_files;
    std::vector<Range> m_moved;        // clusters written by the current pass (green on the map)
    std::vector<Range> m_bootRanges;   // where the Layout.ini boot files currently live
    std::vector<CString> m_failedPaths;
    Range m_active = { -1, 0 };        // destination of the move in progress (amber on the map)
    CString m_status;
};
