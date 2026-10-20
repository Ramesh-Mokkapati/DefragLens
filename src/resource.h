//{{NO_DEPENDENCIES}}
// resource.h
// Resource identifiers shared by DefragLens.rc and the C++ code.

// Icon and dialog templates
#define IDR_MAINFRAME           128
#define IDD_DEFRAGLENS_DIALOG   102
#define IDD_SYSTEM_FILES        103
#define IDD_BOOT_TIME           104

// Main window (CDefragLensDlg): toolbar, cluster map, progress, statistics and file list
#define IDC_STATIC_VOLUME       1000
#define IDC_COMBO_VOLUME        1001
#define IDC_BTN_ANALYZE         1002
#define IDC_BTN_DEFRAG          1003
#define IDC_BTN_STOP            1004
#define IDC_STATIC_MEDIA        1005
#define IDC_CLUSTER_MAP         1006
#define IDC_PROGRESS            1007
#define IDC_STATIC_STATUS       1008
#define IDC_LIST_STATS          1009
#define IDC_LIST_FILES          1010

// Main window: "highly fragmented" row and the extra toolbar buttons
#define IDC_STATIC_HIGH         1011
#define IDC_EDIT_HIGH           1012
#define IDC_SPIN_HIGH           1013
#define IDC_STATIC_HIGH2        1014
#define IDC_BTN_DEFRAG_HIGH     1015
#define IDC_CHECK_HIGH_ONLY     1016
#define IDC_BTN_BOOT            1017
#define IDC_BTN_SYSFILES        1018
#define IDC_BTN_BOOTTIME        1019

// System Files dialog (CSystemFilesDlg)
#define IDC_SYS_LIST            1020
#define IDC_SYS_PAGING          1021
#define IDC_SYS_FIX_PAGEFILE    1022
#define IDC_SYS_RESTORE_PAGEFILE 1023
#define IDC_SYS_FIX_HIBER       1024
#define IDC_SYS_FIX_MFT         1025
#define IDC_SYS_RESULT          1026
#define IDC_SYS_REFRESH         1027

// Boot-Time Defrag dialog (CBootTimeDlg)
#define IDC_BT_FAILED           1030
#define IDC_BT_SYSFILES         1031
#define IDC_BT_MFT              1032
#define IDC_BT_DRYRUN           1033
#define IDC_BT_MINUTES          1034
#define IDC_BT_MINUTES_SPIN     1035
#define IDC_BT_STATE            1036
#define IDC_BT_SCHEDULE         1037
#define IDC_BT_CANCELRUN        1038
#define IDC_BT_VIEWLOG          1039
#define IDC_BT_LASTRUN          1040

// Next free values for the Visual Studio resource editor
#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE    130
#define _APS_NEXT_COMMAND_VALUE     32771
#define _APS_NEXT_CONTROL_VALUE     1041
#define _APS_NEXT_SYMED_VALUE       101
#endif
#endif
