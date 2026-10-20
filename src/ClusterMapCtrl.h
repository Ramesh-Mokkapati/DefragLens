// ClusterMapCtrl.h
// The cluster map: a grid of small squares, each standing for an equal share of the
// volume's clusters and coloured by CellState, with a legend along the bottom.

#pragma once

// Draws the volume as a grid of cells (each cell covers a range of clusters)
// with a color legend underneath. Subclasses a static control on the dialog.
class CClusterMapCtrl : public CStatic
{
public:
    // How many cells fit in the control at its current size; the dialog asks the engine for
    // exactly this many so each cell maps to one square.
    size_t GetCellCapacity() const;
    // Takes a new set of CellState values and repaints.
    void SetCells(std::vector<BYTE>&& cells);
    // Fill colour for a CellState (shared with the legend and the file list highlighting).
    static COLORREF StateColor(BYTE state);

protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    DECLARE_MESSAGE_MAP()

private:
    // Converts 96-DPI pixels to this window's DPI.
    int Scale(int px) const;
    // Client area minus the legend strip: where the cells are drawn.
    CRect MapRect() const;

    std::vector<BYTE> m_cells;   // CellState per cell, row by row
};
