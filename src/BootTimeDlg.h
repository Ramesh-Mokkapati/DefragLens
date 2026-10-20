// BootTimeDlg.h
// The Boot-Time Defrag dialog: options for the one-time pass that runs at the next restart
// (what to process, dry run, time limit), plus schedule, cancel and view-log buttons.

#pragma once
#include "DefragEngine.h"

// Chooses what the boot-time pass should do and schedules or cancels it.
class CBootTimeDlg : public CDialogEx
{
public:
    // The engine supplies the open volume and the list of files it could not move.
    CBootTimeDlg(CDefragEngine& engine, CWnd* pParent = nullptr);
    enum { IDD = IDD_BOOT_TIME };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;

    afx_msg void OnOptionChanged();   // a "what to process" box was ticked or cleared
    afx_msg void OnSchedule();
    afx_msg void OnCancelRun();
    afx_msg void OnViewLog();
    DECLARE_MESSAGE_MAP()

private:
    // Refreshes the status lines and buttons; `message` overrides the scheduled-state text.
    void UpdateState(const CString& message = CString());

    CDefragEngine& m_engine;
    CSpinButtonCtrl m_minutesSpin;
    std::vector<CString> m_failed;   // in-use files from this session's Defragment/Boot Optimize passes
};
