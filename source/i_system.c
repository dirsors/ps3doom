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
//
//-----------------------------------------------------------------------------


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>

#include <psl1ght/lv2/timer.h>
#include <psl1ght/lv2.h>
#include <psl1ght/lv2/thread.h>

#include <io/pad.h>

#include "doomdef.h"
#include "m_misc.h"
#include "v_video.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

#include "ps3_data.h"
#include "ps3_screens.h"


int	mb_used = 32; // was 6


void
I_Tactile
( int	on,
  int	off,
  int	total )
{
  // UNUSED.
  on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}


int  I_GetHeapSize (void)
{
    return mb_used*1024*1024;
}

byte* I_ZoneBase (int*	size)
{
    *size = mb_used*1024*1024;
    return (byte *) malloc (*size);
}



//
// I_GetTime
// returns time in 1/70th second tics
//

// PS3 DOOM TODO: PSL1GHT does not have any support the Cell OS Lv-2 timer
// syscalls yet (except for usleep and sleep), so here's a little kludge
// which implements a 35Hz using a usleeping loop running in another thread.
// lol.

static int ticker = 0;
static sys_ppu_thread_t ticker_thread_id;

static void ticker_thread_func (uint64_t arg)
{
    //sys_ppu_thread_t id;
    //sys_ppu_thread_stack_t stackinfo;
    //sys_ppu_thread_get_id(&id);
    //sys_ppu_thread_get_stack_information(&stackinfo);
    //printf("stack\naddr: %p\nsize: %08X\n", stackinfo.addr, stackinfo.size);
    
    for(;;)
    {
        sys_ppu_thread_yield();
        usleep(1000000/TICRATE);
        ticker++;
    }

    sys_ppu_thread_exit(0);
}

static void I_StartupTimer(void)
{
    uint64_t thread_arg = 0;
    uint64_t priority = 1500;
    size_t stack_size = 0x1000;
    char *thread_name = "PS3DOOM 35Hz timer";
    int s;
    
    s = sys_ppu_thread_create (&ticker_thread_id,
                               ticker_thread_func,
                               thread_arg,
                               priority,
                               stack_size,
                               THREAD_JOINABLE,
                               thread_name);
                               
    printf ("I_StartupTimer: sys_ppu_thread_create returned %d.\n", s);
    return;
}

int  I_GetTime (void)
{
    return ticker;
}



//
// I_Init
//
void I_Init (void)
{
    I_InitSound();
    //  I_InitGraphics();
    I_StartupTimer();
}

//
// I_Quit
//
void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults ();
    I_ShutdownGraphics();
    exit(0);
}

void I_WaitVBL(int count)
{
#ifdef SGI
    sginap(1);                                           
#else
#ifdef SUN
    sleep(0);
#else
    usleep (count * (1000000/70) );                                
#endif
#endif
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*	I_AllocLow(int length)
{
    byte*	mem;
        
    mem = (byte *)malloc (length);
    memset (mem,0,length);
    return mem;
}


//
// I_Error
//
extern boolean demorecording;
extern int graphics_initialized;

static PadData paddata;
static PadInfo padinfo;

void I_Error (char *error, ...)
{
    va_list	argptr;
    char errmsg[2048];
    
    // Message first.
    va_start (argptr,error);
    vsprintf (errmsg,error,argptr);
    va_end (argptr);

    fprintf(stderr, "---! I_Error !---\n");
    fprintf(stderr, "%s\n", errmsg);
    fflush(stderr);

    // Shutdown. Here might be other errors.
    if (demorecording)
	G_CheckDemoStatus();

    D_QuitNetGame ();
    
    if (graphics_initialized)
    {
        I_SetPalette (errorscreen_pal);
        memcpy (screens[0], errorscreen_pic, 64000);
        
        DrawString ("Error:", 8, 8, 190, 213);
        DrawString (errmsg, 8, 40, 190, 213);
        DrawString ("-- HOLD START & SELECT TO EXIT --", 28, 172, 190, 213);
        
        memset (&paddata, 0, sizeof(paddata));
        
        while (!paddata.BTN_START || !paddata.BTN_SELECT)
        {
            ioPadGetInfo(&padinfo);
            if (padinfo.status[0])
                ioPadGetData (0, &paddata);
                
            usleep(1000);
            I_FinishUpdate();
        }

        I_ShutdownGraphics();
    }
    
    exit(-1);
}
