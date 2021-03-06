// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 2013 James Haley et al.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
// Additional terms and conditions compatible with the GPLv3 apply. See the
// file COPYING-EE for details.
//
//--------------------------------------------------------------------------
//
// DESCRIPTION:  
//    EDF Font Definitions
//
//-----------------------------------------------------------------------------

#ifndef E_FONTS_H__
#define E_FONTS_H__

struct vfont_t;

#ifdef NEED_EDF_DEFINITIONS

#define EDF_SEC_FONT      "font"
#define EDF_SEC_FNTDELTA  "fontdelta"

#define ITEM_FONT_HUD     "hu_font"
#define ITEM_FONT_HUDO    "hu_overlayfont"
#define ITEM_FONT_MENU    "mn_font"
#define ITEM_FONT_BMENU   "mn_font_big"
#define ITEM_FONT_NMENU   "mn_font_normal"
#define ITEM_FONT_FINAL   "f_font"
#define ITEM_FONT_FTITLE  "f_titlefont"
#define ITEM_FONT_INTR    "in_font"
#define ITEM_FONT_INTRB   "in_font_big"
#define ITEM_FONT_INTRBN  "in_font_bignum"
#define ITEM_FONT_CONS    "c_font"

extern cfg_opt_t edf_font_opts[];
extern cfg_opt_t edf_fntdelta_opts[];
void E_ProcessFonts(cfg_t *);
void E_ProcessFontDeltas(cfg_t *);

#endif

void     E_ReloadFonts();

vfont_t *E_FontForName(const char *);
vfont_t *E_FontForNum(int);
int      E_FontNumForName(const char *);

#endif

// EOF

