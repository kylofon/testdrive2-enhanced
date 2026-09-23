// icon.h -- the app icon, a banded road running to the horizon.
#pragma once

#include <wx/bitmap.h>
#include <wx/iconbndl.h>

// The icon at `size` pixels square.
wxBitmap AppBitmap(int size);

// Every size a window needs, for SetIcons.
wxIconBundle AppIcons();
