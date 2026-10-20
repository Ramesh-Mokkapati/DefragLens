// pch.h
// Precompiled header for the DefragLens application: Windows/MFC headers, the
// device-I/O definitions used by the defragmentation code, and the standard
// library containers shared across modules. Every .cpp file includes it first.

#pragma once

#include "targetver.h"

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN                        // leave out rarely used Windows headers
#endif
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS  // no silent narrow/wide CString conversions

#include <afxwin.h>        // MFC core and standard components
#include <afxext.h>        // MFC extensions
#include <afxdialogex.h>   // CDialogEx
#include <afxcmn.h>        // common controls: CListCtrl, CProgressCtrl, CSpinButtonCtrl
#include <winioctl.h>      // FSCTL_* codes and structures for volume and file I/O
#include <shlwapi.h>       // StrFormatByteSize64, PathRemoveFileSpec

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "resource.h"

#pragma comment(lib, "shlwapi.lib")

// Use comctl32 version 6 so controls get the current Windows visual style.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
