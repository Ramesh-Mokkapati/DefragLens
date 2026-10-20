// BootTimeDlg.cpp
// Boot-Time Defrag dialog: choose what the pass should process, its safety options, then
// schedule or cancel it. Shows whether a pass is scheduled and how the last one went.

#include "pch.h"
#include "BootTimeDlg.h"
#include "BootTimeJob.h"

BEGIN_MESSAGE_MAP(CBootTimeDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BT_FAILED, &CBootTimeDlg::OnOptionChanged)
    ON_BN_CLICKED(IDC_BT_SYSFILES, &CBootTimeDlg::OnOptionChanged)
    ON_BN_CLICKED(IDC_BT_MFT, &CBootTimeDlg::OnOptionChanged)
    ON_BN_CLICKED(IDC_BT_SCHEDULE, &CBootTimeDlg::OnSchedule)
    ON_BN_CLICKED(IDC_BT_CANCELRUN, &CBootTimeDlg::OnCancelRun)
    ON_BN_CLICKED(IDC_BT_VIEWLOG, &CBootTimeDlg::OnViewLog)
END_MESSAGE_MAP()

// The engine supplies the volume and the files that could not be moved while Windows ran.
CBootTimeDlg::CBootTimeDlg(CDefragEngine& engine, CWnd* pParent)
    : CDialogEx(IDD_BOOT_TIME, pParent), m_engine(engine)
{
}

// Binds the time-limit spin control to m_minutesSpin.
void CBootTimeDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_BT_MINUTES_SPIN, m_minutesSpin);
}

// Defaults: retry in-use files (when there are any), include the system files, dry run on,
// 10-minute time limit.
BOOL CBootTimeDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    CString title;
    title.Format(_T("Boot-Time Defrag for %s"), (LPCTSTR)m_engine.GetRoot());
    SetWindowText(title);

    // Files the engine could not move this session; the checkbox shows how many.
    m_failed = m_engine.GetFailedPaths();
    CString failedText;
    failedText.Format(_T("&Files that could not be moved while Windows was running (%Iu)"), m_failed.size());
    SetDlgItemText(IDC_BT_FAILED, failedText);
    GetDlgItem(IDC_BT_FAILED)->EnableWindow(!m_failed.empty());
    CheckDlgButton(IDC_BT_FAILED, m_failed.empty() ? BST_UNCHECKED : BST_CHECKED);
    CheckDlgButton(IDC_BT_SYSFILES, BST_CHECKED);
    CheckDlgButton(IDC_BT_DRYRUN, BST_CHECKED);

    m_minutesSpin.SetRange32(1, 240);
    m_minutesSpin.SetPos32(10);

    UpdateState();
    return TRUE;
}

// Refreshes the "scheduled" and "last run" lines and the buttons. `message` (the result of
// Schedule or Cancel) replaces the scheduled-state text when given.
void CBootTimeDlg::UpdateState(const CString& message)
{
    bool scheduled = IsBootTimeScheduled();
    CString state = message;
    if (state.IsEmpty())
    {
        state = scheduled
            ? _T("A boot-time pass is scheduled for the next restart. Use Restart, not Shut down: with Fast Startup on, ")
              _T("Shut down skips this stage.")
            : _T("Nothing is scheduled.");
    }

    SetDlgItemText(IDC_BT_STATE, state);

    CTime when;
    CString summary;
    bool haveLog = GetLastBootLog(when, summary);
    SetDlgItemText(IDC_BT_LASTRUN, haveLog
        ? _T("Last run ") + when.Format(_T("%Y-%m-%d %H:%M")) + _T(": ") + summary
        : CString(_T("No boot-time pass has run yet.")));

    GetDlgItem(IDC_BT_CANCELRUN)->EnableWindow(scheduled);
    GetDlgItem(IDC_BT_VIEWLOG)->EnableWindow(haveLog);
    OnOptionChanged();
}

// Scheduling needs at least one thing to process.
void CBootTimeDlg::OnOptionChanged()
{
    bool anything = IsDlgButtonChecked(IDC_BT_FAILED) == BST_CHECKED ||
        IsDlgButtonChecked(IDC_BT_SYSFILES) == BST_CHECKED ||
        IsDlgButtonChecked(IDC_BT_MFT) == BST_CHECKED;
    GetDlgItem(IDC_BT_SCHEDULE)->EnableWindow(anything);
}

// Builds the job from the dialog, spells out exactly what will happen, and schedules it
// only on an explicit Yes (No is the default button).
void CBootTimeDlg::OnSchedule()
{
    BootTimeJob job;
    job.drive = m_engine.GetRoot()[0];
    job.dryRun = IsDlgButtonChecked(IDC_BT_DRYRUN) == BST_CHECKED;
    job.systemFiles = IsDlgButtonChecked(IDC_BT_SYSFILES) == BST_CHECKED;
    job.mft = IsDlgButtonChecked(IDC_BT_MFT) == BST_CHECKED;
    job.minutes = (DWORD)(std::min)((std::max)(m_minutesSpin.GetPos32(), 1), 240);
    if (IsDlgButtonChecked(IDC_BT_FAILED) == BST_CHECKED)
    {
        job.files = m_failed;
    }

    CString what;
    if (!job.files.empty())
    {
        what.AppendFormat(_T("  - %Iu file(s) that were in use\n"), job.files.size());
    }

    if (job.systemFiles)
    {
        what += _T("  - page file, swap file and hibernation file\n");
    }

    if (job.mft)
    {
        what += _T("  - MFT\n");
    }

    CString text;
    text.Format(_T("Schedule a boot-time pass on %c: for the next restart?\n\n%s\n%s")
        _T("Time limit: %lu minutes.\n\n")
        _T("This copies DefragLensBoot.exe to System32 and adds it to the BootExecute registry value, after ")
        _T("Windows' own disk check. It removes itself as soon as it starts, so it runs only once. ")
        _T("Its log is shown here afterwards.%s"),
        job.drive, (LPCTSTR)what,
        job.dryRun ? _T("DRY RUN: nothing will be moved; only a log is written.\n") : _T("Files WILL be moved.\n"),
        job.minutes,
        job.dryRun ? _T("") : _T("\n\nMake sure you have a recent backup before moving system files."));
    if (MessageBox(text, _T("Confirm boot-time pass"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    CWaitCursor wait;
    CString message;
    ScheduleBootTime(job, message);
    UpdateState(message);
}

// Removes the scheduled pass (registry entry and job file).
void CBootTimeDlg::OnCancelRun()
{
    CString message;
    CancelBootTime(message);
    UpdateState(message);
}

// Opens the full boot-time log in Notepad.
void CBootTimeDlg::OnViewLog()
{
    CString args = _T("\"") + GetBootLogPath() + _T("\"");
    ShellExecute(m_hWnd, nullptr, _T("notepad.exe"), args, nullptr, SW_SHOWNORMAL);
}
