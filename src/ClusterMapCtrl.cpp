// ClusterMapCtrl.cpp
// Paints the cluster map and its legend. Painting is double-buffered (drawn into a memory
// bitmap, then copied in one step) so frequent progress updates do not flicker.

#include "pch.h"
#include "ClusterMapCtrl.h"
#include "DefragEngine.h"

namespace
{
    const int kCellSize = 8;        // pixels at 96 DPI, including a 1px gap
    const int kLegendHeight = 24;   // pixels at 96 DPI
    // Colour of the 1-pixel gaps between cells.
    const COLORREF kGridColor = RGB(214, 214, 214);
}

BEGIN_MESSAGE_MAP(CClusterMapCtrl, CStatic)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

// Sizes are designed at 96 DPI and scaled to the monitor's DPI.
int CClusterMapCtrl::Scale(int px) const
{
    return MulDiv(px, (int)GetDpiForWindow(m_hWnd), 96);
}

// The cells use the whole control except a strip at the bottom for the legend.
CRect CClusterMapCtrl::MapRect() const
{
    CRect rc;
    GetClientRect(&rc);
    rc.bottom = (std::max)(rc.top, rc.bottom - Scale(kLegendHeight));
    return rc;
}

// Whole cells only: columns that fit across times rows that fit down.
size_t CClusterMapCtrl::GetCellCapacity() const
{
    if (!GetSafeHwnd())
    {
        return 0;
    }

    CRect rc = MapRect();
    int cell = Scale(kCellSize);
    return (size_t)(rc.Width() / cell) * (size_t)(rc.Height() / cell);
}

// Repaint without erasing; OnPaint covers the whole client area.
void CClusterMapCtrl::SetCells(std::vector<BYTE>&& cells)
{
    m_cells = std::move(cells);
    Invalidate(FALSE);
}

// One colour per CellState; free space is white.
COLORREF CClusterMapCtrl::StateColor(BYTE state)
{
    switch (state)
    {
    case CellUsed:
        return RGB(46, 117, 214);

    case CellFragmented:
        return RGB(220, 53, 69);

    case CellHighlyFragmented:
        return RGB(111, 18, 98);

    case CellBoot:
        return RGB(0, 170, 170);

    case CellMoved:
        return RGB(40, 167, 69);

    case CellActive:
        return RGB(255, 193, 7);

    default:
        return RGB(255, 255, 255);
    }
}

// Skips the default background erase; OnPaint paints every pixel itself, which avoids flicker.
BOOL CClusterMapCtrl::OnEraseBkgnd(CDC*)
{
    return TRUE;   // everything is painted in OnPaint
}

// Draws the grid, then the legend, into a memory DC and copies it to the screen.
void CClusterMapCtrl::OnPaint()
{
    CPaintDC dc(this);
    CRect client;
    GetClientRect(&client);

    // Off-screen bitmap the size of the control; everything is drawn here first.
    CDC mem;
    mem.CreateCompatibleDC(&dc);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(&dc, client.Width(), client.Height());
    CBitmap* oldBmp = mem.SelectObject(&bmp);

    mem.FillSolidRect(client, GetSysColor(COLOR_BTNFACE));

    // Cell grid
    CRect map = MapRect();
    int cell = Scale(kCellSize);
    int cols = (std::max)(1, map.Width() / cell);
    int rows = map.Height() / cell;
    // Fill the grid area with the gap colour; each cell is then drawn 1 pixel smaller than its slot.
    mem.FillSolidRect(map.left, map.top, cols * cell + 1, rows * cell + 1, kGridColor);
    // No volume open yet: show an empty pale grid rather than nothing.
    if (m_cells.empty())
    {
        for (int i = 0; i < cols * rows; ++i)
        {
            mem.FillSolidRect(map.left + 1 + (i % cols) * cell, map.top + 1 + (i / cols) * cell,
                cell - 1, cell - 1, RGB(245, 245, 245));
        }
    }

    for (size_t i = 0; i < m_cells.size() && i < (size_t)cols * rows; ++i)
    {
        int x = map.left + 1 + (int)(i % cols) * cell;
        int y = map.top + 1 + (int)(i / cols) * cell;
        mem.FillSolidRect(x, y, cell - 1, cell - 1, StateColor(m_cells[i]));
    }

    // Legend
    // One swatch and label per state, laid out left to right under the grid.
    struct LegendItem { BYTE state; LPCTSTR text; };
    const LegendItem legend[] = {
        { CellFree,       _T("Free space") },
        { CellUsed,       _T("Used (contiguous)") },
        { CellFragmented, _T("Fragmented files") },
        { CellHighlyFragmented, _T("Highly fragmented") },
        { CellMoved,      _T("Defragmented") },
        { CellBoot,       _T("Boot files") },
        { CellActive,     _T("Being moved") },
    };

    CFont* oldFont = mem.SelectObject(GetParent()->GetFont());
    mem.SetBkMode(TRANSPARENT);
    mem.SetTextColor(GetSysColor(COLOR_BTNTEXT));
    int box = Scale(11);
    int x = client.left;
    int yText = map.bottom + Scale(6);
    TEXTMETRIC tm;
    mem.GetTextMetrics(&tm);
    int yBox = yText + (tm.tmHeight - box) / 2;

    // Each legend entry: a swatch with a grey border, then its label, then a gap.
    for (const LegendItem& item : legend)
    {
        mem.FillSolidRect(x, yBox, box, box, RGB(128, 128, 128));
        mem.FillSolidRect(x + 1, yBox + 1, box - 2, box - 2, StateColor(item.state));
        x += box + Scale(5);
        mem.TextOut(x, yText, item.text);
        x += mem.GetTextExtent(item.text).cx + Scale(18);
    }

    mem.SelectObject(oldFont);

    dc.BitBlt(0, 0, client.Width(), client.Height(), &mem, 0, 0, SRCCOPY);
    mem.SelectObject(oldBmp);
}
