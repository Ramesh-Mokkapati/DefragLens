// DefragLens.cpp
// Application entry point: initializes common controls, points MFC's profile
// functions at the registry, and runs the main dialog.

#include "pch.h"
#include "DefragLens.h"
#include "DefragLensDlg.h"

BEGIN_MESSAGE_MAP(CDefragLensApp, CWinApp)
END_MESSAGE_MAP()

CDefragLensApp theApp;

// Application start-up: initialise common controls and the settings location, show the
// main window modally, then return FALSE so MFC ends the program when the window closes.
BOOL CDefragLensApp::InitInstance()
{
    // List views, progress bars and up-down controls come from comctl32 (version 6 via the manifest in pch.h).
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    CWinApp::InitInstance();

    // Settings go to HKEY_CURRENT_USER\Software\DefragLens\DefragLens (GetProfileInt / WriteProfileInt).
    SetRegistryKey(_T("DefragLens"));

    CDefragLensDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;   // dialog closed: exit instead of starting a message pump
}
