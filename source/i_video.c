// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 Id Software, Inc.
// Copyright (C) 2010 Ville Vuorinen
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
//	DOOM graphics stuff for PS3.
//
//      Note that user input is also handled here (for now), despite
//      the filename. This is how it was in the original source.
//
//-----------------------------------------------------------------------------

#include <unistd.h>
#include <assert.h>
#include <malloc.h>

#include <sysutil/video.h>
#include <psl1ght/lv2.h>
#include <rsx/gcm.h>
#include <rsx/reality.h>
#include <io/pad.h>
#include <rsx/nv40.h>
#include <rsx/commands.h>

#include "i_system.h"
#include "d_main.h"
#include "doomtype.h"
#include "doomdef.h"
#include "d_event.h"
#include "v_video.h"
#include "nv_shaders.h"
#include "gammatab.h"

// INPUT
PadInfo padinfo;
PadData paddata, lastpaddata;

// GRAPHICS
int usegamma;
int graphics_initialized = 0;

VideoResolution rsx_res;

static uint32_t *rsx_buffers[2];       // The buffer we will be drawing into.
static uint32_t rsx_buffer_offsets[2]; // The offset of the buffers in RSX memory
static uint32_t *rsx_depth_buffer;     // Depth buffer. We aren't using it but the PS3 crashes if we don't have it.
static uint32_t rsx_depthbuf_offset;

static int rsx_screen_pitch, rsx_depth_pitch;
static int rsx_current_buffer = 0;
static uint32_t rsx_screen_texture_offset;
gcmContextData *rsx_context; // Context to keep track of the RSX buffer.

uint32_t *sw_scaled_screen;
static uint32_t current_palette[256];

typedef struct
{
    int dest_width;
    int dest_height;
    int mul_width;
    int mul_height;
    void (*scalerfunc)();
} doomres_t;

static void Draw1080Screen (void);
static void Draw720Screen (void);
static void Draw576Screen (void);
static void Draw480Screen (void);

#define MAX_SUPPORTED_RESOLUTIONS 4
static doomres_t supported_resolutions[MAX_SUPPORTED_RESOLUTIONS] =
{
    { 1920, 1080,  1280, 1000, Draw1080Screen },
    { 1280, 720,   960,  600,  Draw720Screen  },
    { 720,  576,   640,  400,  Draw576Screen  },
    { 720,  480,   640,  400,  Draw480Screen  }
};

static doomres_t *current_resolution;


// Block the PPU thread untill the previous flip operation has finished.
static void RSX_WaitFlip(void)
{
    while(gcmGetFlipStatus() != 0) 
        usleep(200);
    
    gcmResetFlipStatus();

    return;
}

static void RSX_Flip (int bufnum)
{
    gcmSetFlip (rsx_context, bufnum);
    realityFlushBuffer (rsx_context);
    
    // Prevent the RSX from continuing until the flip has finished.
    gcmSetWaitFlip(rsx_context);
    
    return;
}

static realityTexture rsx_texinfo;

static void RSX_InitScreenTexture (void)
{
    sw_scaled_screen = rsxMemAlign(16, 4*current_resolution->mul_width*current_resolution->mul_height);
    realityAddressToOffset(sw_scaled_screen, &rsx_screen_texture_offset);

    memset (&rsx_texinfo, 0, sizeof(realityTexture));
    
    rsx_texinfo.swizzle = NV30_3D_TEX_SWIZZLE_S0_X_S1 | NV30_3D_TEX_SWIZZLE_S0_Y_S1 |
                          NV30_3D_TEX_SWIZZLE_S0_Z_S1 | NV30_3D_TEX_SWIZZLE_S0_W_S1 |
                           NV30_3D_TEX_SWIZZLE_S1_X_X | NV30_3D_TEX_SWIZZLE_S1_Y_Y |
                           NV30_3D_TEX_SWIZZLE_S1_Z_Z | NV30_3D_TEX_SWIZZLE_S1_W_W;
                  
    rsx_texinfo.offset = rsx_screen_texture_offset;
    
    rsx_texinfo.format = NV40_3D_TEX_FORMAT_FORMAT_A8R8G8B8 | NV40_3D_TEX_FORMAT_LINEAR |
                         NV30_3D_TEX_FORMAT_DIMS_2D | NV30_3D_TEX_FORMAT_DMA0 |
                         NV30_3D_TEX_FORMAT_NO_BORDER | (0x8000) |
                         (1 << NV40_3D_TEX_FORMAT_MIPMAP_COUNT__SHIFT);

    rsx_texinfo.wrap = NV30_3D_TEX_WRAP_S_CLAMP_TO_EDGE |
                       NV30_3D_TEX_WRAP_T_CLAMP_TO_EDGE |
                       NV30_3D_TEX_WRAP_R_CLAMP_TO_EDGE;

    rsx_texinfo.enable = NV40_3D_TEX_ENABLE_ENABLE;

    rsx_texinfo.filter = NV30_3D_TEX_FILTER_MIN_LINEAR | NV30_3D_TEX_FILTER_MAG_LINEAR | 0x3fd6;

    rsx_texinfo.width = current_resolution->mul_width;
    rsx_texinfo.height = current_resolution->mul_height;
    rsx_texinfo.stride = current_resolution->mul_width * 4;

    return;
}

static void RSX_Init (void)
{
    int ret;
    VideoState state;
    void *host_addr;
    int32_t buffer_size, depth_buffer_size;
    uint32_t *frag_mem;
    int i;

    // Allocate a 1Mb buffer, aligned to a 1MB boundary to be our shared
    // IO memory with the RSX.

    host_addr = memalign(1024*1024, 1024*1024);
    if (!host_addr)
        I_Error ("RSX_Init: Can't allocate RSX shared I/O buffer.");

    // Initialize Reality, which sets up the command buffer and shared
    // IO memory
    rsx_context = realityInit(0x10000, 1024*1024, host_addr); 
    if (!rsx_context)
        I_Error ("RSX_Init: Call to realityInit failed (rsx_context==NULL).");

    // Get the state of the display
    ret = videoGetState(0, 0, &state);
    if (ret)
        I_Error ("RSX_Init: Call to videoGetState failed.");
        
    // Make sure display is enabled
    if (state.state)
        I_Error ("RSX_Init: VideoState.state != 0.");

    // Get the current resolution
    ret = videoGetResolution(state.displayMode.resolution, &rsx_res);
    if (ret)
        I_Error ("RSX_Init: Call to videoGetResolution failed.");
	
    rsx_screen_pitch = 4 * rsx_res.width; // each pixel is 4 bytes
    rsx_depth_pitch = 2 * rsx_res.width; // And each value in the depth buffer is a 16 bit float
    
    // PS3DOOM NOTE:
    // 720x576 & 720x480 will crash if this is not done.
    // Maybe because 720%128 != 0? (both 1280 and 1920 are evenly
    // divisible by 128)
    if (rsx_res.width == 720)
    {
        rsx_screen_pitch = 4*768;
        rsx_depth_pitch = 2*768;
    }

    // Configure the buffer format to xRGB
    VideoConfiguration vconfig;
    memset(&vconfig, 0, sizeof(VideoConfiguration));
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch = rsx_screen_pitch;
    
    ret = videoConfigure (0, &vconfig, NULL, 0);
    if (ret)
        I_Error ("RSX_Init: Call to videoConfigure failed.");
    
    ret = videoGetState(0, 0, &state);
    if (ret)
        I_Error ("RSX_Init: Call to videoGetState failed.");

    buffer_size = rsx_screen_pitch * rsx_res.height;
    depth_buffer_size = rsx_depth_pitch * rsx_res.height;
    
    // Wait for VSYNC to flip
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    // Allocate two buffers for the RSX to draw to the screen (double buffering)
    rsx_buffers[0] = rsxMemAlign(16, buffer_size);
    if (!rsx_buffers[0])
        I_Error ("RSX_Init: Couldn't allocate %i bytes for buffer 0.", buffer_size);

    rsx_buffers[1] = rsxMemAlign(16, buffer_size);
    if (!rsx_buffers[1])
        I_Error ("RSX_Init: Couldn't allocate %i bytes for buffer 1.", buffer_size);

    rsx_depth_buffer = rsxMemAlign(16, depth_buffer_size * 2);
    if (!rsx_depth_buffer)
        I_Error ("RSX_Init: Couldn't allocate %i bytes for depth buffer.", depth_buffer_size);
    
    assert(realityAddressToOffset(rsx_buffers[0], &rsx_buffer_offsets[0]) == 0);
    assert(realityAddressToOffset(rsx_buffers[1], &rsx_buffer_offsets[1]) == 0);

    // Setup the display buffers
    assert(gcmSetDisplayBuffer(0, rsx_buffer_offsets[0], rsx_screen_pitch, rsx_res.width, rsx_res.height) == 0);
    assert(gcmSetDisplayBuffer(1, rsx_buffer_offsets[1], rsx_screen_pitch, rsx_res.width, rsx_res.height) == 0);

    assert(realityAddressToOffset(rsx_depth_buffer, &rsx_depthbuf_offset) == 0); 

    gcmResetFlipStatus();
    RSX_Flip(1);

    frag_mem = rsxMemAlign(256, 256);
    if (!frag_mem)
        I_Error ("RSX_Init: Couldn't allocate 256 bytes for fragment program.");
    
    realityInstallFragmentProgram(rsx_context, &nv30_fp, frag_mem);
    
    current_resolution = 0;
    for (i=0; i<MAX_SUPPORTED_RESOLUTIONS; i++)
    {
        if (supported_resolutions[i].dest_width == rsx_res.width &&
            supported_resolutions[i].dest_height == rsx_res.height)
        {
            current_resolution = &supported_resolutions[i];
            break;
        }
    }
    
    if (!current_resolution)
    {
        I_Error ("I_InitGraphics: Current resolution is not supported. (%ix%i)\n",
            rsx_res.width, rsx_res.height);
    }

    RSX_InitScreenTexture();

    printf ("RSX_Init: Done, target resolution is %ix%i -> %ix%i.\n",
            current_resolution->mul_width, current_resolution->mul_height,
            rsx_res.width, rsx_res.height);

    return;
}

static void RSX_MakeScreenQuad (float width, float height, float xoff)
{
    realityVertexBegin(rsx_context, REALITY_QUADS);
    {
        // upper left
        realityTexCoord2f(rsx_context, 0.0, 0.0);
        realityVertex4f(rsx_context, xoff, 0.0f, 0.0, 1.0); 

        // upper right
        realityTexCoord2f(rsx_context, 1.0, 0.0);
        realityVertex4f(rsx_context, xoff+width, 0.0f, 0.0, 1.0); 

        // lower right
        realityTexCoord2f(rsx_context, 1.0, 1.0);
        realityVertex4f(rsx_context, xoff+width, height, 0.0, 1.0); 

        // lower left
        realityTexCoord2f(rsx_context, 0.0, 1.0);
        realityVertex4f(rsx_context, xoff, height, 0.0, 1.0); 
    }
    realityVertexEnd(rsx_context);
    
    return;
}

static void RSX_BeginFrame (void)
{
    realityViewportTranslate(rsx_context, 0.0, 0.0, 0.0, 0.0);
    realityViewportScale(rsx_context, 1.0, 1.0, 1.0, 0.0); 

    realityZControl(rsx_context, 0, 1, 1); // disable viewport culling

    realityViewport(rsx_context, rsx_res.width, rsx_res.height);

    // Set the color0 target to point at the offset of our current surface
    realitySetRenderSurface(rsx_context, REALITY_SURFACE_COLOR0, REALITY_RSX_MEMORY, 
                            rsx_buffer_offsets[rsx_current_buffer], rsx_screen_pitch);

    // Setup depth buffer
    realitySetRenderSurface(rsx_context, REALITY_SURFACE_ZETA, REALITY_RSX_MEMORY, 
                            rsx_depthbuf_offset, rsx_depth_pitch);

    // Choose color0 as the render target and tell the rsx about the surface format.
    realitySelectRenderTarget(rsx_context, REALITY_TARGET_0, 
                              REALITY_TARGET_FORMAT_COLOR_X8R8G8B8 | 
                              REALITY_TARGET_FORMAT_ZETA_Z16 | 
                              REALITY_TARGET_FORMAT_TYPE_LINEAR,
                              rsx_res.width, rsx_res.height, 0, 0);

    realitySetClearColor(rsx_context, 0x00000000);
    realitySetClearDepthValue(rsx_context, 0xFFFF);

    realityClearBuffers(rsx_context, REALITY_CLEAR_BUFFERS_COLOR_R |
                                 REALITY_CLEAR_BUFFERS_COLOR_G |
                                 REALITY_CLEAR_BUFFERS_COLOR_B |
                                 NV30_3D_CLEAR_BUFFERS_COLOR_A |
                                 NV30_3D_CLEAR_BUFFERS_STENCIL |
                                 REALITY_CLEAR_BUFFERS_DEPTH);

    // Load shaders, because the RSX won't do anything without them.
    realityLoadVertexProgram(rsx_context, &nv40_vp);
    realityLoadFragmentProgram(rsx_context, &nv30_fp); 

    // Load texture
    realitySetTexture (rsx_context, 0, &rsx_texinfo);
}

static void Draw480Screen (void)
{
    int sx, sy, dx, dy;
    uint32_t pixel;
        
    //
    // 720x480 (4:3):
    //
    // Multiply 2x2 n software to 640x400, then scale to 720x480 with
    // a filter using RSX.
    //

    for (sy=0; sy<200; sy++)
    {
        dy = sy*2;
        
        for (sx=0; sx<320; sx++)
        {
            dx = sx*2;
            
            pixel = current_palette[screens[0][sy*320+sx]];
            
            sw_scaled_screen [dy*640+dx] = pixel; dx++;
            sw_scaled_screen [dy*640+dx] = pixel; dx--; dy++;
            
            sw_scaled_screen [dy*640+dx] = pixel; dx++;
            sw_scaled_screen [dy*640+dx] = pixel;
            
            dy--;
        }
    }
    
    RSX_BeginFrame();
    RSX_MakeScreenQuad (720, 480, 0);
    
    return;
}

static void Draw576Screen (void)
{
    int sx, sy, dx, dy;
    uint32_t pixel;
        
    //
    // 720x576 (4:3):
    //
    // Multiply 2x2 n software to 640x400, then scale to 720x576 with
    // a filter using RSX.
    //

    for (sy=0; sy<200; sy++)
    {
        dy = sy*2;
        
        for (sx=0; sx<320; sx++)
        {
            dx = sx*2;
            
            pixel = current_palette[screens[0][sy*320+sx]];
            
            sw_scaled_screen [dy*640+dx] = pixel; dx++;
            sw_scaled_screen [dy*640+dx] = pixel; dx--; dy++;
            
            sw_scaled_screen [dy*640+dx] = pixel; dx++;
            sw_scaled_screen [dy*640+dx] = pixel;
            
            dy--;
        }
    }
    
    RSX_BeginFrame();
    RSX_MakeScreenQuad (720, 576, 0);
    
    return;
}

static void Draw1080Screen (void)
{
    int sx, sy, dx, dy;
    uint32_t pixel;
        
    //
    // 1920x1080:
    //
    // Multiply 4x5 in software to 1280x1000, then scale to 1440x1080 with
    // a filter using RSX.
    //

    for (sy=0; sy<200; sy++)
    {
        dy = sy*5;
        
        for (sx=0; sx<320; sx++)
        {
            dx = sx*4;
            
            pixel = current_palette[screens[0][sy*320+sx]];
            
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx-=3; dy++;
            
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx-=3; dy++;
            
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx-=3; dy++;

            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx-=3; dy++;

            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel; dx++;
            sw_scaled_screen [dy*1280+dx] = pixel;
            
            dy -= 4;
        }
    }
    
    RSX_BeginFrame();
    RSX_MakeScreenQuad (1440, 1080, 240);
    
    return;
}

static void Draw720Screen (void)
{
    int sx, sy, dx, dy;
    uint32_t pixel;
        
    //
    // 1280x720:
    //
    // Multiply 3x3 in software to 960x600, then scale to 960x720 with
    // a filter using RSX.
    //

    for (sy=0; sy<200; sy++)
    {
        dy = sy*3;
        
        for (sx=0; sx<320; sx++)
        {
            dx = sx*3;
            
            pixel = current_palette[screens[0][sy*320+sx]];
            
            sw_scaled_screen [dy*960+dx] = pixel; dx++;
            sw_scaled_screen [dy*960+dx] = pixel; dx++;
            sw_scaled_screen [dy*960+dx] = pixel; dx-=2; dy++;
            
            sw_scaled_screen [dy*960+dx] = pixel; dx++;
            sw_scaled_screen [dy*960+dx] = pixel; dx++;
            sw_scaled_screen [dy*960+dx] = pixel; dx-=2; dy++;
            
            sw_scaled_screen [dy*960+dx] = pixel; dx++;
            sw_scaled_screen [dy*960+dx] = pixel; dx++;
            sw_scaled_screen [dy*960+dx] = pixel;
            
            dy -= 2;
        }
    }
    
    RSX_BeginFrame();
    RSX_MakeScreenQuad (960, 720, 160);
    
    return;
}

void I_ShutdownGraphics (void)
{
    return;
}

// PS3 DOOM NOTE: lol.
extern boolean menuactive;
static void I_GetEvent(void)
{
    event_t event;
    
    ioPadGetInfo(&padinfo);
    memset (&event, 0, sizeof(event));
    
    if (padinfo.status[0])
    {
        lastpaddata = paddata;
        ioPadGetData (0, &paddata);
        
        // d-pad up = UP ARROW KEY
        if (paddata.BTN_UP && !lastpaddata.BTN_UP)
        {
            event.type = ev_keydown;
            event.data1 = KEY_UPARROW;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_UP && lastpaddata.BTN_UP)
        {
            event.type = ev_keyup;
            event.data1 = KEY_UPARROW;
            D_PostEvent (&event);
        }

        // d-pad down = DOWN ARROW KEY
        if (paddata.BTN_DOWN && !lastpaddata.BTN_DOWN)
        {
            event.type = ev_keydown;
            event.data1 = KEY_DOWNARROW;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_DOWN && lastpaddata.BTN_DOWN)
        {
            event.type = ev_keyup;
            event.data1 = KEY_DOWNARROW;
            D_PostEvent (&event);
        }

        // d-pad left = LEFT ARROW KEY
        if (paddata.BTN_LEFT && !lastpaddata.BTN_LEFT)
        {
            event.type = ev_keydown;
            event.data1 = KEY_LEFTARROW;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_LEFT && lastpaddata.BTN_LEFT)
        {
            event.type = ev_keyup;
            event.data1 = KEY_LEFTARROW;
            D_PostEvent (&event);
        }

        // d-pad right = RIGHT ARROW KEY
        if (paddata.BTN_RIGHT && !lastpaddata.BTN_RIGHT)
        {
            event.type = ev_keydown;
            event.data1 = KEY_RIGHTARROW;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_RIGHT && lastpaddata.BTN_RIGHT)
        {
            event.type = ev_keyup;
            event.data1 = KEY_RIGHTARROW;
            D_PostEvent (&event);
        }

        // d-pad X = ENTER
        if (paddata.BTN_CROSS && !lastpaddata.BTN_CROSS)
        {
            event.type = ev_keydown;
            event.data1 = KEY_ENTER;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_CROSS && lastpaddata.BTN_CROSS)
        {
            event.type = ev_keyup;
            event.data1 = KEY_ENTER;
            D_PostEvent (&event);
        }

        // d-pad O = BACKSPACE or F11
        if (paddata.BTN_CIRCLE && !lastpaddata.BTN_CIRCLE)
        {
            event.type = ev_keydown;
            event.data1 = menuactive ? KEY_BACKSPACE : KEY_F11;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_CIRCLE && lastpaddata.BTN_CIRCLE)
        {
            event.type = ev_keyup;
            event.data1 = menuactive ? KEY_BACKSPACE : KEY_F11;
            D_PostEvent (&event);
        }

        // d-pad [] = 'y'
        if (paddata.BTN_SQUARE && !lastpaddata.BTN_SQUARE)
        {
            event.type = ev_keydown;
            event.data1 = 'y';
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_SQUARE && lastpaddata.BTN_SQUARE)
        {
            event.type = ev_keyup;
            event.data1 = 'y';
            D_PostEvent (&event);
        }

        // d-pad /\ = 'n'
        if (paddata.BTN_TRIANGLE && !lastpaddata.BTN_TRIANGLE)
        {
            event.type = ev_keydown;
            event.data1 = 'n';
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_TRIANGLE && lastpaddata.BTN_TRIANGLE)
        {
            event.type = ev_keyup;
            event.data1 = 'n';
            D_PostEvent (&event);
        }
        
        // d-pad R2 = CTRL
        if (paddata.BTN_R2 && !lastpaddata.BTN_R2)
        {
            event.type = ev_keydown;
            event.data1 = KEY_RCTRL;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_R2 && lastpaddata.BTN_R2)
        {
            event.type = ev_keyup;
            event.data1 = KEY_RCTRL;
            D_PostEvent (&event);
        }
        
        // d-pad L2 = SPACE
        if (paddata.BTN_L2 && !lastpaddata.BTN_L2)
        {
            event.type = ev_keydown;
            event.data1 = ' ';
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_L2 && lastpaddata.BTN_L2)
        {
            event.type = ev_keyup;
            event.data1 = ' ';
            D_PostEvent (&event);
        }

        // d-pad L1 = ,
        if (paddata.BTN_L1 && !lastpaddata.BTN_L1)
        {
            event.type = ev_keydown;
            event.data1 = ',';
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_L1 && lastpaddata.BTN_L1)
        {
            event.type = ev_keyup;
            event.data1 = ',';
            D_PostEvent (&event);
        }

        // d-pad R1 = .
        if (paddata.BTN_R1 && !lastpaddata.BTN_R1)
        {
            event.type = ev_keydown;
            event.data1 = '.';
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_R1 && lastpaddata.BTN_R1)
        {
            event.type = ev_keyup;
            event.data1 = '.';
            D_PostEvent (&event);
        }

        // start = ESC
        if (paddata.BTN_START && !lastpaddata.BTN_START)
        {
            event.type = ev_keydown;
            event.data1 = KEY_ESCAPE;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_START && lastpaddata.BTN_START)
        {
            event.type = ev_keyup;
            event.data1 = KEY_ESCAPE;
            D_PostEvent (&event);
        }

        // select = TAB
        if (paddata.BTN_SELECT && !lastpaddata.BTN_SELECT)
        {
            event.type = ev_keydown;
            event.data1 = KEY_TAB;
            D_PostEvent (&event);
        }
        else if (!paddata.BTN_SELECT && lastpaddata.BTN_SELECT)
        {
            event.type = ev_keyup;
            event.data1 = KEY_TAB;
            D_PostEvent (&event);
        }
    }

    return;
}

void I_StartFrame (void)
{
    I_GetEvent();
    return;
}

void I_StartTic (void)
{
    return;
}

void I_UpdateNoBlit (void)
{
    return;
}

void I_FinishUpdate (void)
{
    //printf ("I_FinishUpdate\n");

    RSX_WaitFlip();
    current_resolution->scalerfunc();
    RSX_Flip(rsx_current_buffer);
    rsx_current_buffer = !rsx_current_buffer;

    return;
}

void I_ReadScreen (byte* scr)
{
    memcpy (scr, screens[0], SCREENWIDTH*SCREENHEIGHT);
    return;
}

void I_SetPalette (byte* palette)
{
    int i;
    uint8_t r, g, b;
    
    for (i=0; i<256; i++)
    {
        r = gammatable[usegamma][*palette++];
        g = gammatable[usegamma][*palette++];
        b = gammatable[usegamma][*palette++];
        
        current_palette[i] = (b | (g<<8) | (r<<16));
    }

    return;
}

void I_InitPad (void)
{
    int ret;
    
    // init the pad library (max 1 pad)
    ret = ioPadInit(1);
    if (ret != 0)
        I_Error ("I_InitPad: ioPadInit failed. (error code: %i)\n", ret);

    memset (&paddata, 0, sizeof(lastpaddata));
    memset (&lastpaddata, 0, sizeof(lastpaddata));
   
    return;
}

void I_InitGraphics(void)
{
    screens[0] = (byte *)malloc(SCREENWIDTH*SCREENHEIGHT);
    
    I_InitPad();
 
    RSX_Init();

    graphics_initialized = 1;
   
    return;
}