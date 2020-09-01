// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
// 02111-1307, USA.
//
// DESCRIPTION:
//	Endianess handling, swapping 16bit and 32bit.
//
//-----------------------------------------------------------------------------


#ifndef __M_SWAP__
#define __M_SWAP__

#include <stdint.h>


// Endianess handling.
// WAD files are stored little endian.

#ifdef __BIG_ENDIAN__
    uint16_t SwapSHORT (uint16_t);
    uint32_t SwapLONG  (uint32_t);
    #define SHORT(x)  (SwapSHORT((uint16_t)(x)))
    #define LONG(x)   (SwapLONG((uint32_t)(x)))
#else
    #define SHORT(x)	(x)
    #define LONG(x)     (x)
#endif


#endif
