// SystemFilesDlg.h
// The System Files dialog: shows the report from SystemFiles.h for the open volume and
// offers the page file, hibernation file and MFT fixes, each behind a confirmation.

#pragma once
#include "DefragEngine.h"
#include "SystemFiles.h"

// Read-only report on pagefile.sys, hiberfil.sys, swapfile.sys and $MFT for the open volume,
// with fixes that each run only after the user confirms them.
class CSystemFilesDlg : public CDialogEx
{
public:
    // The engine supplies the volume and performs the MFT consolidation.
    CSystemFilesDlg(CDefragEngine& engine, CWnd* pParent = nullptr);
    enum { IDD = IDD_SYSTEM_FILES };

    bool VolumeChanged() const { return m_volumeChanged; }   // clusters moved: caller refreshes its map

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;

    // Button handlers
    afx_msg void OnRefresh();
    afx_msg void OnFixPageFile();
    afx_msg void OnRestorePageFile();
    afx_msg void OnFixHibernation();
    afx_msg void OnFixMft();
    DECLARE_MESSAGE_MAP()

private:
    // Re-reads the report and page file setting, then enables only the fixes that apply.
    void Refresh();
    // Shows the outcome of the last fix (or an error) under the buttons.
    void ShowResult(const CString& message);
    // Yes/No warning box; No is the default button.
    bool Confirm(const CString& text);

    CDefragEngine& m_engine;
    CListCtrl m_list;
    std::vector<SystemFileReport> m_reports;   // indexed by SystemFileId
    bool m_volumeChanged = false;
};
