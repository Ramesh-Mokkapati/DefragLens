// SystemFilesDlg.cpp
// System Files dialog: fills the report list, shows the page file setting, and runs each fix
// only after an explicit Yes. Every fix is followed by a fresh report so the effect is visible.

#include "pch.h"
#include "SystemFilesDlg.h"

namespace
{
    // "1.5 GB" style sizes, using Windows' own formatting.
    CString FormatBytes(ULONGLONG bytes)
    {
        TCHAR buf[64] = {};
        StrFormatByteSize64((LONGLONG)bytes, buf, _countof(buf));
        return buf;
    }
}

BEGIN_MESSAGE_MAP(CSystemFilesDlg, CDialogEx)
    ON_BN_CLICKED(IDC_SYS_REFRESH, &CSystemFilesDlg::OnRefresh)
    ON_BN_CLICKED(IDC_SYS_FIX_PAGEFILE, &CSystemFilesDlg::OnFixPageFile)
    ON_BN_CLICKED(IDC_SYS_RESTORE_PAGEFILE, &CSystemFilesDlg::OnRestorePageFile)
    ON_BN_CLICKED(IDC_SYS_FIX_HIBER, &CSystemFilesDlg::OnFixHibernation)
    ON_BN_CLICKED(IDC_SYS_FIX_MFT, &CSystemFilesDlg::OnFixMft)
END_MESSAGE_MAP()

CSystemFilesDlg::CSystemFilesDlg(CDefragEngine& engine, CWnd* pParent)
    : CDialogEx(IDD_SYSTEM_FILES, pParent), m_engine(engine)
{
}

// Binds the report list control to m_list.
void CSystemFilesDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_SYS_LIST, m_list);
}

// Titles the dialog with the volume, sets up the columns (the last one takes the remaining
// width) and loads the report.
BOOL CSystemFilesDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    CString title;
    title.Format(_T("System Files on %s"), (LPCTSTR)m_engine.GetRoot());
    SetWindowText(title);

    int dpi = (int)GetDpiForWindow(m_hWnd);
    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_INFOTIP);
    m_list.InsertColumn(0, _T("File"), LVCFMT_LEFT, MulDiv(95, dpi, 96));
    m_list.InsertColumn(1, _T("Size"), LVCFMT_RIGHT, MulDiv(80, dpi, 96));
    m_list.InsertColumn(2, _T("Fragments"), LVCFMT_RIGHT, MulDiv(75, dpi, 96));
    CRect rc;
    m_list.GetClientRect(&rc);
    m_list.InsertColumn(3, _T("Assessment"), LVCFMT_LEFT, rc.Width() - MulDiv(95 + 80 + 75, dpi, 96));

    Refresh();
    return TRUE;
}

// Fragment counts marked "+" are minimums (part of the run list could not be read).
void CSystemFilesDlg::Refresh()
{
    CWaitCursor wait;
    CString error;
    bool ok = QuerySystemFiles(m_engine.GetRoot(), m_reports, error);

    m_list.DeleteAllItems();
    if (ok)
    {
        for (int i = 0; i < (int)m_reports.size(); ++i)
        {
            const SystemFileReport& r = m_reports[i];
            m_list.InsertItem(i, r.name);
            m_list.SetItemText(i, 1, r.present ? FormatBytes(r.size) : CString(_T("-")));
            CString fragments = _T("-");
            if (r.present && r.readable)
            {
                fragments.Format(r.approximate ? _T("%lu+") : _T("%lu"), r.fragments);
            }

            m_list.SetItemText(i, 2, fragments);
            m_list.SetItemText(i, 3, r.assessment);
        }
    }
    else
    {
        ShowResult(error);
    }

    // Page file configuration as Windows stores it
    // Shown raw except for "?:\pagefile.sys", which is easier to read as words.
    std::vector<CString> entries = GetPagingFilesSetting();
    CString setting;
    for (const CString& entry : entries)
    {
        CString text = entry;
        text.Trim();
        if (text.IsEmpty())
        {
            continue;
        }

        if (text[0] == _T('?'))
        {
            text = _T("System managed on all drives");
        }

        setting += (setting.IsEmpty() ? _T("") : _T(";  ")) + text;
    }

    if (setting.IsEmpty())
    {
        setting = _T("No paging file");
    }

    SetDlgItemText(IDC_SYS_PAGING, setting);

    // Offer only the fixes that apply: the file exists, a backup exists, or the MFT has movable pieces.
    auto present = [&](SystemFileId id) { return ok && m_reports[id].present; };
    GetDlgItem(IDC_SYS_FIX_PAGEFILE)->EnableWindow(present(SysPageFile));
    GetDlgItem(IDC_SYS_RESTORE_PAGEFILE)->EnableWindow(HasPageFileBackup());
    GetDlgItem(IDC_SYS_FIX_HIBER)->EnableWindow(present(SysHiberFile));
    GetDlgItem(IDC_SYS_FIX_MFT)->EnableWindow(ok && m_reports[SysMft].readable && m_reports[SysMft].fragments > 2);
}

// Shows the outcome of the last action (or an error) in the line under the fix buttons.
void CSystemFilesDlg::ShowResult(const CString& message)
{
    SetDlgItemText(IDC_SYS_RESULT, message);
}

// Yes/No warning before any system change; No is the default button, so Enter cancels.
bool CSystemFilesDlg::Confirm(const CString& text)
{
    return MessageBox(text, _T("Confirm system change"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

// Clears the last result and reads the report again.
void CSystemFilesDlg::OnRefresh()
{
    ShowResult(_T(""));
    Refresh();
}

// Fixes the page file at its current size, rounded up to whole megabytes (at least 16 MB).
void CSystemFilesDlg::OnFixPageFile()
{
    const SystemFileReport& page = m_reports[SysPageFile];
    ULONGLONG sizeMB = (std::max)(16ULL, (page.size + (1ULL << 20) - 1) >> 20);
    TCHAR drive = m_engine.GetRoot()[0];

    CString text;
    text.Format(_T("Set the page file on %c: to a fixed size of %I64u MB (its current size)?\n\n")
        _T("This changes the PagingFiles registry setting and turns off \"Automatically manage paging file size\". ")
        _T("The current setting is saved so you can restore it.\n\n")
        _T("It takes effect after a restart. A fixed size stops the page file growing in new pieces; ")
        _T("it does not rearrange the pieces it already has."), drive, sizeMB);
    if (!Confirm(text))
    {
        return;
    }

    CString message;
    SetFixedPageFile(drive, sizeMB, message);
    ShowResult(message);
    Refresh();
}

// Restores the page file setting saved before the first change.
void CSystemFilesDlg::OnRestorePageFile()
{
    if (!Confirm(_T("Restore the page file setting that was in place before this program changed it?\n\n")
        _T("It takes effect after a restart.")))
    {
        return;
    }

    CString message;
    RestorePageFileSetting(message);
    ShowResult(message);
    Refresh();
}

// Deletes and recreates hiberfil.sys through powercfg. Takes a few seconds.
void CSystemFilesDlg::OnFixHibernation()
{
    if (!Confirm(_T("Windows will delete hiberfil.sys and create it again ")
        _T("(\"powercfg /hibernate off\", then \"powercfg /hibernate on\").\n\n")
        _T("Hibernation and Fast Startup are unavailable for the few seconds this takes. ")
        _T("The new file is in one piece only if enough contiguous free space exists; running Defragment first helps.\n\n")
        _T("Continue?")))
    {
        return;
    }

    CWaitCursor wait;
    CString message;
    RecreateHibernationFile(message);
    m_volumeChanged = true;
    ShowResult(message);
    Refresh();
}

// Moves the movable part of the MFT into one free run (synchronous; up to a minute).
void CSystemFilesDlg::OnFixMft()
{
    if (!Confirm(_T("Move every part of the MFT except its first piece (which NTFS pins in place) into one free area?\n\n")
        _T("This uses the same file-system move operation as defragmentation, but the MFT is the volume's most ")
        _T("critical structure: make sure you have a recent backup. It can take a minute.\n\n")
        _T("Continue?")))
    {
        return;
    }

    CWaitCursor wait;
    CString message;
    m_engine.ConsolidateMft(message);
    m_volumeChanged = true;
    ShowResult(message);
    Refresh();
}
