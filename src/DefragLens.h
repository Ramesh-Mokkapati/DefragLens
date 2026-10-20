// DefragLens.h
// Application object for DefragLens, a dialog-based MFC disk defragmenter.
// The whole user interface lives in CDefragLensDlg; the application only sets up
// common controls and the settings location, then shows that dialog.

#pragma once

class CDefragLensApp : public CWinApp
{
public:
    // Runs the main dialog modally and returns FALSE so MFC exits when it closes.
    BOOL InitInstance() override;
    DECLARE_MESSAGE_MAP()
};

// The single application instance MFC requires.
extern CDefragLensApp theApp;
