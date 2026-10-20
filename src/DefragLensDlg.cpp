// DefragLensDlg.cpp
// Main window behaviour. Long operations run on a worker thread (WorkerProc); the UI thread
// only reads the engine's thread-safe snapshots when WM_DEFRAG_PROGRESS arrives, so the
// window stays responsive and Stop works at any time.

#include "pch.h"
#include "DefragLens.h"
#include "DefragLensDlg.h"
#include "SystemFilesDlg.h"
#include "BootTimeDlg.h"
#include "BootTimeJob.h"

namespace
{
    // The file list shows at most this many of the most fragmented files.
    const size_t kMaxListedFiles = 2000;
    // Upper limit for the "highly fragmented" threshold box.
    const int kMaxHighThreshold = 100000;
    // Registry values under HKCU\Software\DefragLens\DefragLens\Settings.
    const LPCTSTR kSettingsSection = _T("Settings");
    const LPCTSTR kHighThresholdEntry = _T("HighFragmentThreshold");
    const LPCTSTR kBootLogSeenEntry = _T("BootLogSeen");   // time of the last boot-time log already reported
    const COLORREF kHighlyFragmentedText = RGB(111, 18, 98);   // matches the map colour

    // "1.5 GB" style sizes, using Windows' own formatting.
    CString FormatBytes(ULONGLONG bytes)
    {
        TCHAR buf[64] = {};
        StrFormatByteSize64((LONGLONG)bytes, buf, _countof(buf));
        return buf;
    }

    // Thousands separators for large counts: 1234567 -> "1,234,567".
    CString FormatNumber(ULONGLONG n)
    {
        CString digits;
        digits.Format(_T("%I64u"), n);
        CString out;
        int len = digits.GetLength();
        for (int i = 0; i < len; ++i)
        {
            if (i > 0 && (len - i) % 3 == 0)
            {
                out += _T(',');
            }

            out += digits[i];
        }

        return out;
    }
}

BEGIN_MESSAGE_MAP(CDefragLensDlg, CDialogEx)
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_CBN_SELCHANGE(IDC_COMBO_VOLUME, &CDefragLensDlg::OnVolumeChanged)
    ON_BN_CLICKED(IDC_BTN_ANALYZE, &CDefragLensDlg::OnAnalyze)
    ON_BN_CLICKED(IDC_BTN_DEFRAG, &CDefragLensDlg::OnDefragment)
    ON_BN_CLICKED(IDC_BTN_DEFRAG_HIGH, &CDefragLensDlg::OnDefragmentHigh)
    ON_BN_CLICKED(IDC_BTN_BOOT, &CDefragLensDlg::OnBootOptimize)
    ON_BN_CLICKED(IDC_BTN_SYSFILES, &CDefragLensDlg::OnSystemFiles)
    ON_BN_CLICKED(IDC_BTN_BOOTTIME, &CDefragLensDlg::OnBootTime)
    ON_BN_CLICKED(IDC_BTN_STOP, &CDefragLensDlg::OnStop)
    ON_EN_CHANGE(IDC_EDIT_HIGH, &CDefragLensDlg::OnThresholdChanged)
    ON_BN_CLICKED(IDC_CHECK_HIGH_ONLY, &CDefragLensDlg::OnHighOnlyClicked)
    ON_NOTIFY(NM_DBLCLK, IDC_LIST_FILES, &CDefragLensDlg::OnFileDblClk)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_LIST_FILES, &CDefragLensDlg::OnFileCustomDraw)
    ON_MESSAGE(WM_DEFRAG_PROGRESS, &CDefragLensDlg::OnDefragProgress)
    ON_MESSAGE(WM_DEFRAG_DONE, &CDefragLensDlg::OnDefragDone)
END_MESSAGE_MAP()

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

CDefragLensDlg::CDefragLensDlg(CWnd* pParent)
    : CDialogEx(IDD_DEFRAGLENS_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

// Binds the dialog template's controls to their member objects.
void CDefragLensDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_COMBO_VOLUME, m_volumes);
    DDX_Control(pDX, IDC_CLUSTER_MAP, m_map);
    DDX_Control(pDX, IDC_PROGRESS, m_progress);
    DDX_Control(pDX, IDC_LIST_STATS, m_stats);
    DDX_Control(pDX, IDC_LIST_FILES, m_files);
    DDX_Control(pDX, IDC_SPIN_HIGH, m_highSpin);
}

// Sets up columns, fonts and saved settings, fills the volume list, sizes the window and
// opens the Windows volume so the map shows something straight away.
BOOL CDefragLensDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    m_dpi = (int)GetDpiForWindow(m_hWnd);
    // The engine posts its progress messages to this window.
    m_engine.SetNotifyWindow(m_hWnd);
    m_progress.SetRange(0, 100);

    m_stats.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    m_stats.InsertColumn(0, _T("Property"), LVCFMT_LEFT, Scale(140));
    m_stats.InsertColumn(1, _T("Value"), LVCFMT_LEFT, Scale(170));

    m_files.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    m_files.InsertColumn(0, _T("Fragments"), LVCFMT_RIGHT, Scale(75));
    m_files.InsertColumn(1, _T("Size"), LVCFMT_RIGHT, Scale(85));
    m_files.InsertColumn(2, _T("File"), LVCFMT_LEFT, Scale(400));

    // Bold version of the list font, for highly fragmented rows (see OnFileCustomDraw).
    LOGFONT lf = {};
    m_files.GetFont()->GetLogFont(&lf);
    lf.lfWeight = FW_BOLD;
    m_boldFont.CreateFontIndirect(&lf);

    // Restore the threshold saved by the previous run, clamped in case the registry value was edited.
    int threshold = (int)AfxGetApp()->GetProfileInt(kSettingsSection, kHighThresholdEntry, kDefaultHighFragmentThreshold);
    threshold = (std::min)((std::max)(threshold, 2), kMaxHighThreshold);
    m_engine.SetHighFragmentThreshold((DWORD)threshold);

    m_highSpin.SetRange32(2, kMaxHighThreshold);
    m_highSpin.SetPos32(threshold);

    PopulateVolumes();
    SetBusy(false);

    SetWindowPos(nullptr, 0, 0, Scale(1120), Scale(780), SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow();
    OpenSelectedVolume();
    ShowBootTimeStatus();
    return TRUE;
}

// Called at startup and after the Boot-Time Defrag dialog: removes a leftover BootExecute entry,
// then says whether a pass is scheduled or shows the result of one that has run since.
void CDefragLensDlg::ShowBootTimeStatus()
{
    CleanUpStaleBootEntry();
    if (IsBootTimeScheduled())
    {
        SetDlgItemText(IDC_STATIC_STATUS, _T("A boot-time pass is scheduled for the next restart (see Boot-Time Defrag)."));
        return;
    }

    // Report a boot-time pass that ran since the program was last opened, once.
    CTime when;
    CString summary;
    if (GetLastBootLog(when, summary))
    {
        int seen = AfxGetApp()->GetProfileInt(kSettingsSection, kBootLogSeenEntry, 0);
        if ((int)when.GetTime() != seen)
        {
            AfxGetApp()->WriteProfileInt(kSettingsSection, kBootLogSeenEntry, (int)when.GetTime());
            SetDlgItemText(IDC_STATIC_STATUS, _T("Boot-time pass on ") + when.Format(_T("%Y-%m-%d %H:%M")) + _T(": ") + summary);
        }
    }
}

// Opens the Boot-Time Defrag dialog for the selected volume.
void CDefragLensDlg::OnBootTime()
{
    if (m_busy)
    {
        return;
    }

    if (!m_engine.IsOpen() && !OpenSelectedVolume())
    {
        return;
    }

    CBootTimeDlg dlg(m_engine, this);
    dlg.DoModal();
    ShowBootTimeStatus();
}

// Lists fixed and removable drives with label, file system and size, preselecting the
// drive Windows is installed on. Network, optical and RAM drives are left out.
void CDefragLensDlg::PopulateVolumes()
{
    TCHAR windowsDir[MAX_PATH] = {};
    GetWindowsDirectory(windowsDir, MAX_PATH);

    TCHAR drives[512] = {};
    GetLogicalDriveStrings(_countof(drives) - 1, drives);
    int select = 0;
    for (LPCTSTR root = drives; *root; root += _tcslen(root) + 1)
    {
        UINT type = GetDriveType(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
        {
            continue;
        }

        TCHAR label[MAX_PATH] = {}, fs[MAX_PATH] = {};
        if (!GetVolumeInformation(root, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH))
        {
            continue;
        }

        ULARGE_INTEGER total = {};
        GetDiskFreeSpaceEx(root, nullptr, &total, nullptr);

        CString text;
        text.Format(_T("%c:  %s  [%s, %s]"), root[0], label[0] ? label : _T("Local Disk"), fs,
            (LPCTSTR)FormatBytes(total.QuadPart));
        int index = m_volumes.AddString(text);
        m_volumes.SetItemData(index, (DWORD_PTR)root[0]);
        if (_totupper(root[0]) == _totupper(windowsDir[0]))
        {
            select = index;
        }
    }

    if (m_volumes.GetCount() > 0)
    {
        m_volumes.SetCurSel(select);
    }
}

// Opens the selected drive in the engine (reading its bitmap) and shows its file system
// and media type. On failure the error goes to the status line.
bool CDefragLensDlg::OpenSelectedVolume()
{
    int sel = m_volumes.GetCurSel();
    if (sel < 0)
    {
        return false;
    }

    CString root;
    root.Format(_T("%c:\\"), (TCHAR)m_volumes.GetItemData(sel));
    CString error;
    bool ok = m_engine.Open(root, error);
    if (ok)
    {
        int media = m_engine.GetMediaType();
        CString info;
        info.Format(_T("%s  |  %s"), (LPCTSTR)m_engine.GetFileSystem(),
            media == 1 ? _T("Solid-state drive (SSD)") : media == 0 ? _T("Hard disk drive") : _T("Media type unknown"));
        SetDlgItemText(IDC_STATIC_MEDIA, info);
        SetDlgItemText(IDC_STATIC_STATUS, m_engine.GetStatusText());
    }
    else
    {
        SetDlgItemText(IDC_STATIC_MEDIA, _T(""));
        SetDlgItemText(IDC_STATIC_STATUS, error);
    }

    m_progress.SetPos(0);
    RefreshMap();
    RefreshStats();
    RefreshFileList();
    return ok;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Switching drives while an operation runs is blocked (the combo is disabled), but be safe.
void CDefragLensDlg::OnVolumeChanged()
{
    if (!m_busy)
    {
        OpenSelectedVolume();
    }
}

void CDefragLensDlg::OnAnalyze()
{
    StartOperation(CDefragEngine::OpAnalyze);
}

void CDefragLensDlg::OnDefragment()
{
    StartOperation(CDefragEngine::OpDefragment);
}

void CDefragLensDlg::OnDefragmentHigh()
{
    StartOperation(CDefragEngine::OpDefragmentHigh);
}

void CDefragLensDlg::OnBootOptimize()
{
    StartOperation(CDefragEngine::OpBootOptimize);
}

// Opens the System Files report (NTFS only). If a fix moved clusters, refresh the map.
void CDefragLensDlg::OnSystemFiles()
{
    if (m_busy)
    {
        return;
    }

    if (!m_engine.IsOpen() && !OpenSelectedVolume())
    {
        return;
    }

    if (m_engine.GetFileSystem() != _T("NTFS"))
    {
        AfxMessageBox(_T("The system file report is only available for NTFS volumes."), MB_ICONINFORMATION);
        return;
    }

    CSystemFilesDlg dlg(m_engine, this);
    dlg.DoModal();
    if (dlg.VolumeChanged())
    {
        SetDlgItemText(IDC_STATIC_STATUS, m_engine.GetStatusText());
        RefreshMap();
        RefreshStats();
    }
}

// The threshold box changed: apply and save a valid value, then recolour the map and list.
void CDefragLensDlg::OnThresholdChanged()
{
    if (!m_files.GetSafeHwnd())
    {
        return;   // fired while the dialog is still being created
    }

    BOOL ok = FALSE;
    UINT value = GetDlgItemInt(IDC_EDIT_HIGH, &ok, FALSE);
    if (!ok || value < 2 || value > (UINT)kMaxHighThreshold || value == m_engine.GetHighFragmentThreshold())
    {
        return;   // ignore partial input such as an empty box while typing
    }

    m_engine.SetHighFragmentThreshold(value);
    AfxGetApp()->WriteProfileInt(kSettingsSection, kHighThresholdEntry, (int)value);
    RefreshMap();
    RefreshStats();
    RefreshFileList();
}

// "Show only highly fragmented files" toggled.
void CDefragLensDlg::OnHighOnlyClicked()
{
    RefreshFileList();
}

void CDefragLensDlg::UpdateHighButton()
{
    ULONGLONG files = 0, bytes = 0;
    m_engine.GetHighlyFragmented(files, bytes);

    CString text = _T("Defragment Highl&y Fragmented");
    if (m_engine.HasAnalysis())
    {
        text.AppendFormat(_T(" (%s)"), (LPCTSTR)FormatNumber(files));
    }

    SetDlgItemText(IDC_BTN_DEFRAG_HIGH, text);

    // Before analysis the button stays usable: it analyzes first, then defragments.
    GetDlgItem(IDC_BTN_DEFRAG_HIGH)->EnableWindow(!m_busy && (!m_engine.HasAnalysis() || files > 0));
}

// Asks the engine to stop; the worker finishes its current chunk and posts WM_DEFRAG_DONE.
void CDefragLensDlg::OnStop()
{
    m_engine.RequestCancel();
    SetDlgItemText(IDC_STATIC_STATUS, _T("Stopping..."));
    GetDlgItem(IDC_BTN_STOP)->EnableWindow(FALSE);
}

// Common start-up for every long operation: make sure a volume is open, apply the checks and
// warnings for this operation, then start the worker thread.
void CDefragLensDlg::StartOperation(CDefragEngine::Operation op)
{
    if (m_busy)
    {
        return;
    }

    if (!m_engine.IsOpen() && !OpenSelectedVolume())
    {
        AfxMessageBox(_T("The selected volume could not be opened.\n\n") + m_engine.GetStatusText(), MB_ICONERROR);
        return;
    }

    // Layout.ini lists files on the Windows drive, so boot optimization only makes sense there.
    if (op == CDefragEngine::OpBootOptimize)
    {
        TCHAR windowsDir[MAX_PATH] = {};
        GetWindowsDirectory(windowsDir, MAX_PATH);
        if (_totupper(windowsDir[0]) != _totupper(m_engine.GetRoot()[0]))
        {
            CString text;
            text.Format(_T("Boot optimization applies to the Windows volume (%c:). Select it first."), windowsDir[0]);
            AfxMessageBox(text, MB_ICONINFORMATION);
            return;
        }
    }

    // Anything that moves data on an SSD needs a deliberate yes.
    if (op != CDefragEngine::OpAnalyze && m_engine.GetMediaType() == 1)
    {
        if (AfxMessageBox(_T("This volume is on a solid-state drive.\n\n")
            _T("SSDs gain little from defragmentation and every move adds write wear. ")
            _T("Windows already optimizes SSDs with TRIM.\n\nDefragment anyway?"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        {
            return;
        }
    }

    m_op = op;
    m_defragMin = op == CDefragEngine::OpDefragmentHigh ? m_engine.GetHighFragmentThreshold() : 2;
    m_engine.ResetCancel();
    m_progress.SetPos(0);
    SetBusy(true);

    // Below-normal priority keeps the PC usable. Suspended + no auto-delete so the CWinThread
    // object stays valid until WaitForWorker has joined it.
    m_worker = AfxBeginThread(WorkerProc, this, THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
    if (!m_worker)
    {
        SetBusy(false);
        AfxMessageBox(_T("Could not start the worker thread."), MB_ICONERROR);
        return;
    }

    m_worker->m_bAutoDelete = FALSE;
    m_worker->ResumeThread();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

UINT AFX_CDECL CDefragLensDlg::WorkerProc(LPVOID param)
{
    auto* dlg = static_cast<CDefragLensDlg*>(param);
    HWND hWnd = dlg->m_hWnd;
    CDefragEngine::Operation op = dlg->m_op;
    bool ok = false;
    switch (op)
    {
    case CDefragEngine::OpAnalyze:
        ok = dlg->m_engine.Analyze();
        break;

    case CDefragEngine::OpBootOptimize:
        ok = dlg->m_engine.BootOptimize();
        break;

    default:
        ok = dlg->m_engine.Defragment(dlg->m_defragMin);
        break;
    }

    // Posted (not sent) so the worker never waits on the UI thread; it may close right after.
    ::PostMessage(hWnd, WM_DEFRAG_DONE, (WPARAM)op, ok ? TRUE : FALSE);
    return 0;
}

// Waits for the worker thread to end and frees it. The thread has already posted its
// final message or been asked to stop, so this returns quickly.
void CDefragLensDlg::WaitForWorker()
{
    if (!m_worker)
    {
        return;
    }

    WaitForSingleObject(m_worker->m_hThread, INFINITE);
    delete m_worker;
    m_worker = nullptr;
}

// Progress tick from the engine: update the bar, status, map and statistics.
LRESULT CDefragLensDlg::OnDefragProgress(WPARAM wParam, LPARAM)
{
    if (!m_busy)
    {
        return 0;
    }

    m_progress.SetPos((int)wParam);
    SetDlgItemText(IDC_STATIC_STATUS, m_engine.GetStatusText());
    RefreshMap();
    RefreshStats();
    return 0;
}

// The operation finished (or was stopped): join the thread, re-enable the controls and show
// the final results. Mentions Boot-Time Defrag when some files were in use.
LRESULT CDefragLensDlg::OnDefragDone(WPARAM, LPARAM lParam)
{
    WaitForWorker();
    SetBusy(false);
    if (lParam)
    {
        m_progress.SetPos(100);
    }

    CString status = m_engine.GetStatusText();
    size_t failed = m_engine.GetFailedPaths().size();
    if (failed > 0 && m_op != CDefragEngine::OpAnalyze)
    {
        status.AppendFormat(_T(" %Iu file(s) were in use: Boot-Time Defrag can retry them at the next restart."), failed);
    }

    SetDlgItemText(IDC_STATIC_STATUS, status);
    RefreshMap();
    RefreshStats();
    RefreshFileList();
    return 0;
}

// Only Stop is available while an operation runs.
void CDefragLensDlg::SetBusy(bool busy)
{
    m_busy = busy;
    m_volumes.EnableWindow(!busy);
    GetDlgItem(IDC_BTN_ANALYZE)->EnableWindow(!busy);
    GetDlgItem(IDC_BTN_DEFRAG)->EnableWindow(!busy);
    GetDlgItem(IDC_BTN_STOP)->EnableWindow(busy);
    GetDlgItem(IDC_BTN_BOOT)->EnableWindow(!busy);
    GetDlgItem(IDC_BTN_SYSFILES)->EnableWindow(!busy);
    GetDlgItem(IDC_BTN_BOOTTIME)->EnableWindow(!busy);
    GetDlgItem(IDC_EDIT_HIGH)->EnableWindow(!busy);
    m_highSpin.EnableWindow(!busy);
    UpdateHighButton();
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// Asks the engine for exactly as many cells as the map control can show.
void CDefragLensDlg::RefreshMap()
{
    std::vector<BYTE> cells;
    m_engine.BuildCellMap(cells, m_map.GetCellCapacity());
    m_map.SetCells(std::move(cells));
}

// Rebuilds the statistics panel. Rows appear as information becomes available: volume figures
// always, analysis figures after Analyze, boot and defragmentation figures after those passes.
void CDefragLensDlg::RefreshStats()
{
    std::vector<std::pair<CString, CString>> rows;
    if (m_engine.IsOpen())
    {
        VolumeStats s = m_engine.GetStats();
        ULONGLONG used = s.totalClusters - s.freeClusters;
        CString v;

        rows.emplace_back(_T("Volume"), m_engine.GetRoot() + _T("  (") + m_engine.GetFileSystem() + _T(")"));
        rows.emplace_back(_T("Capacity"), FormatBytes(s.totalClusters * s.bytesPerCluster));
        v.Format(_T("%s  (%.1f%%)"), (LPCTSTR)FormatBytes(s.freeClusters * s.bytesPerCluster),
            s.totalClusters ? 100.0 * s.freeClusters / s.totalClusters : 0.0);
        rows.emplace_back(_T("Free space"), v);
        rows.emplace_back(_T("Cluster size"), FormatBytes(s.bytesPerCluster));
        rows.emplace_back(_T("Total clusters"), FormatNumber(s.totalClusters));

        if (m_engine.HasAnalysis() || s.filesScanned > 0)
        {
            rows.emplace_back(_T("Files analyzed"), FormatNumber(s.filesScanned));
            rows.emplace_back(_T("Folders analyzed"), FormatNumber(s.dirsScanned));
            rows.emplace_back(_T("Inaccessible / skipped"), FormatNumber(s.filesSkipped));
            v.Format(_T("%s  (%.1f%%)"), (LPCTSTR)FormatNumber(s.fragmentedFiles),
                s.filesScanned ? 100.0 * s.fragmentedFiles / s.filesScanned : 0.0);
            rows.emplace_back(_T("Fragmented files"), v);
            v.Format(_T("%s  (avg %.1f per file)"), (LPCTSTR)FormatNumber(s.totalFragments),
                s.fragmentedFiles ? (double)s.totalFragments / s.fragmentedFiles : 0.0);
            rows.emplace_back(_T("Fragments"), v);
            v.Format(_T("%s  (%.1f%% of used)"), (LPCTSTR)FormatBytes(s.fragmentedClusters * s.bytesPerCluster),
                used ? 100.0 * s.fragmentedClusters / used : 0.0);
            rows.emplace_back(_T("Fragmented data"), v);

            ULONGLONG highFiles = 0, highBytes = 0;
            m_engine.GetHighlyFragmented(highFiles, highBytes);
            CString label;
            label.Format(_T("Highly fragmented (%lu+)"), m_engine.GetHighFragmentThreshold());
            v.Format(_T("%s files  (%s)"), (LPCTSTR)FormatNumber(highFiles), (LPCTSTR)FormatBytes(highBytes));
            rows.emplace_back(label, v);
        }

        if (s.bootLayoutFound)
        {
            v.Format(_T("%s files, %s fragmented"), (LPCTSTR)FormatNumber(s.bootFiles), (LPCTSTR)FormatNumber(s.bootFragmented));
            rows.emplace_back(_T("Boot files (Layout.ini)"), v);
        }

        if (s.bootPlaced || s.bootNotMoved)
        {
            v.Format(_T("%s placed, %s not moved%s"), (LPCTSTR)FormatNumber(s.bootPlaced),
                (LPCTSTR)FormatNumber(s.bootNotMoved), s.bootPacked ? _T(" (packed in boot order)") : _T(""));
            rows.emplace_back(_T("Boot optimization"), v);
        }

        if (s.filesDefragged || s.filesNotMoved)
        {
            rows.emplace_back(_T("Files defragmented"), FormatNumber(s.filesDefragged));
            rows.emplace_back(_T("Could not be moved"), FormatNumber(s.filesNotMoved));
        }
    }

    // Update in place when the row count is unchanged to avoid flicker during progress.
    if (m_stats.GetItemCount() != (int)rows.size())
    {
        m_stats.DeleteAllItems();
        for (int i = 0; i < (int)rows.size(); ++i)
        {
            m_stats.InsertItem(i, rows[i].first);
        }
    }

    for (int i = 0; i < (int)rows.size(); ++i)
    {
        if (m_stats.GetItemText(i, 0) != rows[i].first)
        {
            m_stats.SetItemText(i, 0, rows[i].first);
        }

        if (m_stats.GetItemText(i, 1) != rows[i].second)
        {
            m_stats.SetItemText(i, 1, rows[i].second);
        }
    }
}

// Shows the most fragmented files (or only the highly fragmented ones when the box is ticked).
void CDefragLensDlg::RefreshFileList()
{
    bool highOnly = IsDlgButtonChecked(IDC_CHECK_HIGH_ONLY) == BST_CHECKED;
    std::vector<FragmentedFile> files;
    m_engine.GetFragmentedFiles(files, kMaxListedFiles, highOnly ? m_engine.GetHighFragmentThreshold() : 2);

    m_files.SetRedraw(FALSE);
    m_files.DeleteAllItems();
    for (int i = 0; i < (int)files.size(); ++i)
    {
        CString frags;
        frags.Format(_T("%lu"), files[i].fragments);
        m_files.InsertItem(i, frags);
        m_files.SetItemText(i, 1, FormatBytes(files[i].size));
        m_files.SetItemText(i, 2, files[i].path);
        m_files.SetItemData(i, files[i].fragments);   // read back by OnFileCustomDraw
    }

    m_files.SetRedraw(TRUE);
    m_files.Invalidate();
    UpdateHighButton();
}

// Custom draw lets individual rows change colour and font without owner-drawing the list.
void CDefragLensDlg::OnFileCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
    *pResult = CDRF_DODEFAULT;
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        *pResult = CDRF_NOTIFYITEMDRAW;
    }
    else if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
    {
        // Highly fragmented files are shown bold, in the same colour as on the map.
        if ((DWORD)draw->nmcd.lItemlParam >= m_engine.GetHighFragmentThreshold())
        {
            draw->clrText = kHighlyFragmentedText;
            ::SelectObject(draw->nmcd.hdc, m_boldFont.GetSafeHandle());
            *pResult = CDRF_NEWFONT;
        }
    }
}

// Double-click a file to open its folder in Explorer with the file selected.
void CDefragLensDlg::OnFileDblClk(NMHDR* pNMHDR, LRESULT* pResult)
{
    auto* item = reinterpret_cast<NMITEMACTIVATE*>(pNMHDR);
    if (item->iItem >= 0)
    {
        CString args = _T("/select,\"") + m_files.GetItemText(item->iItem, 2) + _T("\"");
        ShellExecute(m_hWnd, nullptr, _T("explorer.exe"), args, nullptr, SW_SHOWNORMAL);
    }

    *pResult = 0;
}

// ---------------------------------------------------------------------------
// Closing and layout
// ---------------------------------------------------------------------------

void CDefragLensDlg::OnCancel()
{
    if (m_busy)
    {
        if (AfxMessageBox(_T("An operation is still running. Stop it and exit?"), MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            return;
        }

        CWaitCursor wait;
        m_engine.RequestCancel();
        WaitForWorker();
    }

    m_engine.Close();
    CDialogEx::OnCancel();
}

// Smallest size at which every toolbar control still fits.
void CDefragLensDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
    lpMMI->ptMinTrackSize.x = Scale(1020);
    lpMMI->ptMinTrackSize.y = Scale(600);
    CDialogEx::OnGetMinMaxInfo(lpMMI);
}

void CDefragLensDlg::OnSize(UINT nType, int cx, int cy)
{
    CDialogEx::OnSize(nType, cx, cy);
    if (nType != SIZE_MINIMIZED)
    {
        Layout(cx, cy);
    }
}

// Lays out the window top to bottom: two toolbar rows, the cluster map (takes the spare
// height), progress bar, status line, statistics and file list side by side, Close button.
// All sizes are 96-DPI pixels scaled to the window's DPI.
void CDefragLensDlg::Layout(int cx, int cy)
{
    if (!m_map.GetSafeHwnd())
    {
        return;
    }

    const int margin = Scale(10), gap = Scale(6), btnH = Scale(26), textH = Scale(18);
    auto place = [this](int id, int x, int y, int w, int h)
    {
        if (CWnd* wnd = GetDlgItem(id))
        {
            wnd->MoveWindow(x, y, (std::max)(0, w), (std::max)(0, h), TRUE);
        }
    };

    // Toolbar row
    int x = margin, y = margin;
    place(IDC_STATIC_VOLUME, x, y + Scale(5), Scale(50), textH);
    x += Scale(52);
    place(IDC_COMBO_VOLUME, x, y + Scale(1), Scale(240), Scale(300));
    x += Scale(240) + gap;
    place(IDC_BTN_ANALYZE, x, y, Scale(90), btnH);
    x += Scale(90) + gap;
    place(IDC_BTN_DEFRAG, x, y, Scale(100), btnH);
    x += Scale(100) + gap;
    place(IDC_BTN_STOP, x, y, Scale(70), btnH);
    x += Scale(70) + gap * 2;
    place(IDC_BTN_BOOT, x, y, Scale(110), btnH);
    x += Scale(110) + gap;
    place(IDC_BTN_SYSFILES, x, y, Scale(110), btnH);
    x += Scale(110) + gap * 2;
    place(IDC_STATIC_MEDIA, x, y + Scale(5), cx - margin - x, textH);
    y += btnH + gap;

    // Highly fragmented row
    x = margin;
    place(IDC_STATIC_HIGH, x, y + Scale(5), Scale(118), textH);
    x += Scale(120);
    place(IDC_EDIT_HIGH, x, y + Scale(2), Scale(70), Scale(22));
    m_highSpin.SetBuddy(GetDlgItem(IDC_EDIT_HIGH));   // re-attach so the arrows follow the edit box
    x += Scale(70) + gap;
    place(IDC_STATIC_HIGH2, x, y + Scale(5), Scale(110), textH);
    x += Scale(110) + gap;
    place(IDC_BTN_DEFRAG_HIGH, x, y, Scale(230), btnH);
    x += Scale(230) + gap * 2;
    int bootTimeW = Scale(150);
    place(IDC_CHECK_HIGH_ONLY, x, y + Scale(4), cx - margin - x - bootTimeW - gap, textH);
    place(IDC_BTN_BOOTTIME, cx - margin - bootTimeW, y, bootTimeW, btnH);
    y += btnH + gap * 2;

    // Bottom area: stats + file list, then the Close button row
    // The statistics/file list area gets two fifths of the remaining height; the map gets the rest.
    int bottomH = (std::max)(Scale(170), (cy - y) * 2 / 5);
    int progressH = Scale(16);
    int mapH = cy - y - progressH - textH - bottomH - btnH - margin - gap * 4;
    mapH = (std::max)(Scale(80), mapH);

    place(IDC_CLUSTER_MAP, margin, y, cx - 2 * margin, mapH);
    y += mapH + gap;
    place(IDC_PROGRESS, margin, y, cx - 2 * margin, progressH);
    y += progressH + gap;
    place(IDC_STATIC_STATUS, margin, y, cx - 2 * margin, textH);
    y += textH + gap;

    int statsW = Scale(330);
    place(IDC_LIST_STATS, margin, y, statsW, bottomH);
    int filesX = margin + statsW + gap;
    int filesW = cx - margin - filesX;
    place(IDC_LIST_FILES, filesX, y, filesW, bottomH);
    m_files.SetColumnWidth(2, (std::max)(Scale(150), filesW - Scale(75 + 85) - GetSystemMetrics(SM_CXVSCROLL) - Scale(4)));
    y += bottomH + gap;

    place(IDCANCEL, cx - margin - Scale(90), y, Scale(90), btnH);

    RefreshMap();
}
