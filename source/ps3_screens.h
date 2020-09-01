#ifndef __PS3_SCREENS_H__
#define __PS3_SCREENS_H__

#include "doomtype.h"

extern void DrawLetter (byte c, int x, int y, byte color);
extern void DrawString (char *str, int x, int y, byte color, byte shadow);
extern void DrawBackTile (byte *tile);

#endif   // __PS3_SCREENS_H__