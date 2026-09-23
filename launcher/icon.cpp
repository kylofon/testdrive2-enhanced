// icon.cpp -- the app icon: a road running to the horizon under a cyan sky, its
// surface and the fields beside it in lighter and darker bands (the enhanced
// road), in the game's EGA colours and two darker shades, drawn at 16x16 and
// scaled by whole pixels so every size shows the same picture. app.ico holds
// the same drawing for Explorer; make_icon.py writes it.
#include "icon.h"

#include <wx/image.h>

namespace {

wxImage RoadTile() {
    wxImage tile(16, 16);
    auto put = [&](int x, int y, unsigned long rgb) { tile.SetRGB(x, y, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF); };
    // The bands: two rows each near the bottom, one row towards the horizon.
    auto dark = [](int y) { return y >= 12 ? ((y - 12) / 2) % 2 == 1 : y % 2 == 1; };
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) put(x, y, y < 6 ? 0x55FFFF : dark(y) ? 0x007A00 : 0x00AA00);
    // The road: two pixels wide at the horizon (row 6), the whole width at the bottom.
    for (int y = 6; y < 16; ++y) {
        const int half = 1 + (y - 6) * 7 / 9;
        for (int x = 8 - half; x < 8 + half; ++x)
            put(x, y, x == 8 - half || x == 7 + half ? 0x555555 : dark(y) ? 0x8A8A8A : 0xAAAAAA);
    }
    for (int y : {8, 11, 12, 15})  // the centre line's dashes, longer nearer
        for (int x = 7; x <= 8; ++x) put(x, y, 0xFFFF55);
    return tile;
}

}  // namespace

wxBitmap AppBitmap(int size) { return wxBitmap(RoadTile().Scale(size, size, wxIMAGE_QUALITY_NEAREST)); }

wxIconBundle AppIcons() {
    wxIconBundle icons;
    for (int size : {16, 20, 24, 32, 48, 64, 256}) {
        wxIcon icon;
        icon.CopyFromBitmap(AppBitmap(size));
        icons.AddIcon(icon);
    }
    return icons;
}
