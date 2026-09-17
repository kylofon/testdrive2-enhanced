#pragma once
/* Build variant, set by CMake:
 *   default      port of TD2EGA.EXE (EGA, 16 colours)
 *   TD_CGA=1     port of TD2CGA.EXE (CGA, 4 colours)                       -- not implemented yet
 *   TD_HERC=1    TD2CGA.EXE started as "td2cga herc" (Hercules); implies TD_CGA -- not implemented yet
 * Where TD2CGA differs, code will use EGA_CGA(ega, cga) for values and #if TD_CGA for statements. */
#ifndef TD_HERC
#define TD_HERC 0
#endif
#ifndef TD_CGA
#define TD_CGA TD_HERC
#endif
#if TD_HERC && !TD_CGA
#error "TD_HERC requires TD_CGA"
#endif
#if TD_CGA
#error "The TD2CGA.EXE build is not ported yet"
#endif

#define EGA_CGA(ega, cga) (TD_CGA ? (cga) : (ega))

#if TD_HERC
#define TD_VARIANT_NAME "Hercules"
#elif TD_CGA
#define TD_VARIANT_NAME "CGA"
#else
#define TD_VARIANT_NAME "EGA"
#endif
