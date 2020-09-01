#include "doomtype.h"
#include "ps3_data.h"
#include "v_video.h"

void DrawLetter (byte c, int x, int y, byte color)
{
    int dy;
    byte *cdata;
    
    if (x+8 > 320) return;
    if (y+16 > 200) return;

    cdata = font_8x16 + c*16;
    for (dy=y; dy < y+16; dy++)
    {
        if (*cdata & 0x80)  screens[0][dy*320 + x] = color; x++;
        if (*cdata & 0x40)  screens[0][dy*320 + x] = color; x++;
        if (*cdata & 0x20)  screens[0][dy*320 + x] = color; x++;
        if (*cdata & 0x10)  screens[0][dy*320 + x] = color; x++;
        if (*cdata & 0x08)  screens[0][dy*320 + x] = color; x++;
        if (*cdata & 0x04)  screens[0][dy*320 + x] = color; x++;
        if (*cdata & 0x02)  screens[0][dy*320 + x] = color; x++;
        if (*cdata & 0x01)  screens[0][dy*320 + x] = color; x++;
        cdata++;
        x -= 8;
    }
}

void DrawString (char *str, int x, int y, byte color, byte shadow)
{
    int i, l, startx;

    l = strlen(str); startx = x;
    for (i=0; i<l; i++)
    {
        if (str[i]=='\n')
        {
            y += 16;
            x = startx;
        }
        else if (str[i]=='\r')
            x = startx;
        else if (str[i]==' ' || str[i]==0 || str[i]==255) 
            x += 8;
        else
        {
            if (x+8 > 310)
            {
               x = startx;
               y += 16;
            }
            DrawLetter (str[i], x+1, y+1, shadow);
            DrawLetter (str[i], x, y, color);
            x += 8;
        }
    }
}

void DrawBackTile (byte *tile)
{
    int x, y;
    
    for (y=0; y < 200; y++)
    {
       for (x=0; x < 320; x++)
           screens[0][x+y*320] = tile[(x%64)+((y%64)*64)];
    }
    
    return;
}
