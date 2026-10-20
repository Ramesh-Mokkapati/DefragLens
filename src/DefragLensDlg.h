// DefragLensDlg.h
// The main window: volume picker, operation buttons, cluster map, progress, statistics and
// the list of fragmented files. It owns the CDefragEngine and the worker thread that runs
// the engine's long operations, and opens the System Files and Boot-Time Defrag dialogs.

#pragma once
#include "DefragEngine.h"
#include "ClusterMapCtrl.h"

class CDefragLensDlg : public CDialogEx
{
public:
    explicit CDefragLensDlg(CWnd* pParent = nullptr);
    enum { IDD = IDD_DEFRAGLENS_DIALOG };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override {}   // Enter must not close the dialog
    // Close button / Esc / window close: stops a running operation first (after asking).
    void OnCancel() override;

    // Window sizing
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);

    // Toolbar and buttons
    afx_msg void OnVolumeChanged();
    afx_msg void OnAnalyze();
    afx_msg void OnDefragment();
    afx_msg void OnDefragmentHigh();
    afx_msg void OnBootOptimize();
    afx_msg void OnSystemFiles();
    afx_msg void OnBootTime();
    afx_msg void OnStop();

    // "Highly fragmented" threshold row
    afx_msg void OnThresholdChanged();
    afx_msg void OnHighOnlyClicked();

    // File list
    afx_msg void OnFileDblClk(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnFileCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);

    // Messages from the engine / worker thread (see DefragEngine.h)
    afx_msg LRESULT OnDefragProgress(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnDefragDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    // Worker thread body: runs m_op on the engine and posts WM_DEFRAG_DONE when finished.
    static UINT AFX_CDECL WorkerProc(LPVOID param);

    void PopulateVolumes();          // fills the volume combo with fixed and removable drives
    bool OpenSelectedVolume();       // opens the selected volume in the engine and refreshes everything
    void StartOperation(CDefragEngine::Operation op);   // checks, confirms and starts the worker
    void WaitForWorker();            // joins and deletes the worker thread
    void SetBusy(bool busy);         // enables/disables controls while an operation runs
    void Layout(int cx, int cy);     // positions every control for the current client size
    void RefreshMap();
    void RefreshStats();
    void RefreshFileList();
    void UpdateHighButton();         // label (with count) and enabled state of the highly fragmented button
    void ShowBootTimeStatus();       // reports a scheduled or just-finished boot-time pass
    int Scale(int px) const { return MulDiv(px, m_dpi, 96); }   // 96-DPI pixels to window DPI

    HICON m_hIcon;
    CComboBox m_volumes;
    CClusterMapCtrl m_map;
    CProgressCtrl m_progress;
    CListCtrl m_stats;
    CListCtrl m_files;
    CSpinButtonCtrl m_highSpin;
    CFont m_boldFont;               // highly fragmented rows in the file list
    CDefragEngine m_engine;
    CWinThread* m_worker = nullptr; // running operation; null when idle
    CDefragEngine::Operation m_op = CDefragEngine::OpAnalyze;
    DWORD m_defragMin = 2;          // minimum fragments for the running defrag pass
    bool m_busy = false;
    int m_dpi = 96;
};
