#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <malloc.h>

#include <io/pad.h>

#include "doomtype.h"
#include "i_system.h"
#include "v_video.h"
#include "i_video.h"
#include "doomstat.h"
#include "d_main.h"

#include "ps3_data.h"
#include "ps3_screens.h"

typedef enum
{
    SOURCE_USB,
    SOURCE_CFLASH,
    SOURCE_SD,
    SOURCE_MSTICK,
    SOURCE_HDD,
    SOURCE_NONE,
    NUMSOURCES
} source_t;

static const char *sourcenames[NUMSOURCES] =
{
    "USB",
    "CF",
    "SD/MMC",
    "MS",
    "HDD",
    "none"
};

typedef struct
{
    char *path;
    source_t source;
} searchdir_t;

#define NUM_WAD_SEARCH_DIRS 12
static const searchdir_t wad_search_dirs[NUM_WAD_SEARCH_DIRS] =
{
    { "/dev_usb000/ps3doom/", SOURCE_USB },
    { "/dev_usb001/ps3doom/", SOURCE_USB },
    { "/dev_usb002/ps3doom/", SOURCE_USB },
    { "/dev_usb003/ps3doom/", SOURCE_USB },
    { "/dev_usb004/ps3doom/", SOURCE_USB },
    { "/dev_usb005/ps3doom/", SOURCE_USB },
    { "/dev_usb006/ps3doom/", SOURCE_USB },
    { "/dev_usb007/ps3doom/", SOURCE_USB },

    { "/dev_cf/ps3doom/", SOURCE_CFLASH },
    { "/dev_sd/ps3doom/", SOURCE_SD },
    { "/dev_ms/ps3doom/", SOURCE_MSTICK },

    { "/dev_hdd0/game/DOOM00666/USRDIR/", SOURCE_HDD },
};

typedef struct
{
   char *name;
   GameMode_t mode;
   GameMission_t mission;
   source_t source;
} iwad_t;

#define NUM_SUPPORTED_IWADS 5
static const iwad_t supported_iwads[NUM_SUPPORTED_IWADS] =
{
    { "doom2.wad",    commercial, doom2,     SOURCE_NONE },
    { "plutonia.wad", commercial, pack_plut, SOURCE_NONE },
    { "tnt.wad",      commercial, pack_tnt,  SOURCE_NONE },
    { "doom.wad",     retail,     doom,      SOURCE_NONE },
    { "doom1.wad",    shareware,  doom,      SOURCE_NONE }
};

#define MAX_IWADS 10
static iwad_t iwadlist[MAX_IWADS];
static int available_iwads;

static void SearchIWADs (void)
{
    char pathbuf[2048];
    int current_dir, current_wad;
    struct stat tmp;
    
    available_iwads = 0;
    
    for (current_dir=0; current_dir<NUM_WAD_SEARCH_DIRS; current_dir++)
    {
        for (current_wad=0; current_wad<NUM_SUPPORTED_IWADS; current_wad++)
        {
            sprintf (pathbuf, "%s%s", wad_search_dirs[current_dir].path,
                                      supported_iwads[current_wad].name);

            if (!stat(pathbuf,&tmp)) // this file does exist
            {
                iwadlist[available_iwads].name = (char*)malloc(2048);
                strcpy (iwadlist[available_iwads].name, pathbuf);
                iwadlist[available_iwads].mode = supported_iwads[current_wad].mode;
                iwadlist[available_iwads].mission = supported_iwads[current_wad].mission;
                iwadlist[available_iwads].source = wad_search_dirs[current_dir].source;
                
                available_iwads++;
                
                if (!(available_iwads < MAX_IWADS))
                    return;
            }
        }
    }
    
    if (!available_iwads)
        I_Error ("No supported IWADs found, so bye.");
        
    return;
}

static PadInfo padinfo;
static PadData paddata, lastpaddata;

static void DrawIWAD (iwad_t iwad, int x, int y, byte color, byte shadow)
{
    int i;
    DrawString (sourcenames[iwad.source], x, y, color, shadow);
    
    for (i=strlen(iwad.name); i>0; i--)
    {
        if (iwad.name[i-1]=='/')
            break;
    }
    
    DrawString (iwad.name+i, x + 7*8, y, color, shadow);
    return;
}

void PS3_LaunchScreen (void)
{
    int launch;
    int i,selected_iwad;
    
    I_SetPalette (launcher_palette);
    
    SearchIWADs();
    launch = 0; selected_iwad = 0;
    
    memset (&paddata, 0, sizeof(paddata));
    memset (&lastpaddata, 0, sizeof(lastpaddata));

    while (!launch)
    {
        ioPadGetInfo(&padinfo);
        
        if (padinfo.status[0])
            ioPadGetData (0, &paddata);

        DrawBackTile (launcher_backtile);
        
        if (paddata.BTN_DOWN && !lastpaddata.BTN_DOWN)
            selected_iwad = (selected_iwad + 1) % available_iwads;
        else if (paddata.BTN_UP && !lastpaddata.BTN_UP)
        {
            if (selected_iwad > 0)
                selected_iwad = (selected_iwad - 1);
            else
                selected_iwad = available_iwads-1;
        }
            
        for (i=0; i<available_iwads; i++)
        {
            if (selected_iwad == i)
                DrawIWAD (iwadlist[i], 16, 16+16*i, 112, 127); // green
            else
                DrawIWAD (iwadlist[i], 16, 16+16*i, 80,  111); // gray
        }

        if (paddata.BTN_CROSS)
            launch = 1;
            
        lastpaddata = paddata;
        I_FinishUpdate ();
    }
    
    D_AddFile (iwadlist[selected_iwad].name);
    gamemode = iwadlist[selected_iwad].mode;
    gamemission = iwadlist[selected_iwad].mission;
    
    for (i=0; i<available_iwads; i++)
       free (iwadlist[i].name);
    
    DrawBackTile (launcher_backtile);
    DrawString ("Loading...", 120, 92, 4, 111); // white
    I_FinishUpdate ();

    return;
}