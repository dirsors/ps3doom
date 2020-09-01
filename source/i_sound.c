// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
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
//       PS3 interface for sound.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <malloc.h>

#include <audio/audio.h>
#include <psl1ght/lv2/timer.h>
#include <sys/thread.h>
#include <psl1ght/lv2/thread.h>

#include "z_zone.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "doomdef.h"
#include "m_swap.h"


#define SAMPLECOUNT		256
#define NUM_CHANNELS		32
#define BUFMUL                  4
#define MIXBUFFERSIZE		(SAMPLECOUNT*BUFMUL)

static sys_lwmutex_t chanmutex;
static sys_lwmutex_attribute_t MutexAttrs;

static sys_ppu_thread_t mixthread;
static char *mixthread_name = "PS3DOOM Sound FX mixer";

// The actual lengths of all sound effects.
int 		lengths[NUMSFX];

// The global mixing buffer.
// Basically, samples from all active internal channels
//  are modifed and added, and stored in the buffer
//  that is submitted to the audio device.
int16_t mixbuffer[MIXBUFFERSIZE];

typedef struct
{
    byte *snd_start_ptr, *snd_end_ptr;
    unsigned int starttic;
    int sfxid;
    int *leftvol, *rightvol;
    int handle;
} channel_t;

static channel_t channels[NUM_CHANNELS];

int		vol_lookup[128*256];

static AudioPortConfig ps3_audio_port_cfg;


static void I_SndMixResetChannel (int channum)
{
    memset (&channels[channum], 0, sizeof(channel_t));

    return;
}


//
// This function loads the sound data from the WAD lump
// for a single sound effect.
//
static void* I_SndLoadSample (char* sfxname, int* len)
{
    int i;
    int sfxlump_num, sfxlump_len;
    char sfxlump_name[20];
    byte *sfxlump_data, *sfxlump_sound;
    byte *padded_sfx_data;
    uint16_t orig_rate;
    int padded_sfx_len;
    float times;
    int x;
    
    sprintf (sfxlump_name, "DS%s", sfxname);
    
    // check if the sound lump exists
    if (W_CheckNumForName(sfxlump_name) == -1)
        return 0;
        
    sfxlump_num = W_GetNumForName (sfxlump_name);
    sfxlump_len = W_LumpLength (sfxlump_num);
    
    // if it's not at least 9 bytes (8 byte header + at least 1 sample), it's
    // not in the correct format
    if (sfxlump_len < 9)
        return 0;
    
    // load it
    sfxlump_data = W_CacheLumpNum (sfxlump_num, PU_STATIC);
    sfxlump_sound = sfxlump_data + 8;
    sfxlump_len -= 8;
    
    // get original sample rate from DMX header
    memcpy (&orig_rate, sfxlump_data+2, 2);
    orig_rate = SHORT (orig_rate);
    
    times = 48000.0f / (float)orig_rate;
    
    padded_sfx_len = ((sfxlump_len*ceil(times) + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;
    padded_sfx_data = (byte*)malloc(padded_sfx_len);
    
    for (i=0; i < padded_sfx_len; i++)
    {
        x = floor ((float)i/times);
        
        if (x < sfxlump_len) // 8 was already subtracted
            padded_sfx_data[i] = sfxlump_sound[x];
        else
            padded_sfx_data[i] = 128; // fill the rest with silence
    }
        
    Z_Free (sfxlump_data); //  free original lump

    *len = padded_sfx_len;
    return (void *)(padded_sfx_data);
}


//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//

void I_SetChannels()
{
    // Init internal lookups (raw data, mixing buffer, channels).
    // This function sets up internal lookups used during
    //  the mixing process. 
  
    int i, j;
  
    // Okay, reset internal mixing channels to zero.
    for (i=0; i<NUM_CHANNELS; i++)
        I_SndMixResetChannel(i);

    // Generates volume lookup tables which also turn the unsigned
    // samples into signed samples.
    for (i=0 ; i<128 ; i++)
    {
        for (j=0 ; j<256 ; j++)
            vol_lookup[i*256+j] = (i*(j-128)*256)/127;
    }
    
    return;
}	

 
void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
    return;
}

void I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
    return;
}


//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

void I_StopSound (int handle)
{
    int i;
    
    sys_lwmutex_lock (&chanmutex, 0);

    for (i=0; i<NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
        {
            I_SndMixResetChannel(i);
            sys_lwmutex_unlock (&chanmutex);
            return;
        }
    }
    
    sys_lwmutex_unlock (&chanmutex);
    return;
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
static int currenthandle = 0;
int I_StartSound (int id, int vol, int sep, int pitch, int priority)
{
    int	i;
    
    int	oldestslot, oldesttics;
    int	slot;

    int	rightvol;
    int	leftvol;

    // this effect was not loaded.
    if (!S_sfx[id].data)
        return -1;

    sys_lwmutex_lock (&chanmutex, 0);

    // Loop all channels to find a free slot.
    slot = -1;
    oldesttics = gametic;
    oldestslot = 0;

    for (i=0; i<NUM_CHANNELS; i++)
    {
	if (!channels[i].snd_start_ptr)  // not playing
        {
            slot = i;
            break;
        }
        
        if (channels[i].starttic < oldesttics)
        {
            oldesttics = channels[i].starttic;
            oldestslot = i;
        }
    }
    
    // No free slots, so replace the oldest sound still playing.
    if (slot == -1)
        slot = oldestslot;
    
    channels[slot].handle = ++currenthandle;
    
    // Set pointers to raw sound data start & end.
    channels[slot].snd_start_ptr = (byte *)S_sfx[id].data;
    channels[slot].snd_end_ptr = channels[slot].snd_start_ptr + lengths[id];

    // Save starting gametic.
    channels[slot].starttic = gametic;

    sep += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol = vol - ((vol*sep*sep) >> 16); ///(256*256);
    sep -= 257;
    rightvol = vol - ((vol*sep*sep) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("addsfx: rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("addsfx: leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level???
    channels[slot].leftvol = &vol_lookup[leftvol*256];
    channels[slot].rightvol = &vol_lookup[rightvol*256];

    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channels[slot].sfxid = id;

    sys_lwmutex_unlock (&chanmutex);

    return currenthandle;
}


int I_SoundIsPlaying (int handle)
{
    int i;
    
    sys_lwmutex_lock (&chanmutex, 0);

    for (i=0; i<NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
        {
            sys_lwmutex_unlock (&chanmutex);
            return 1;
        }
    }

    sys_lwmutex_unlock (&chanmutex);
    return 0;
}



//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//

void I_UpdateSound(void)
{
    // Mix current sound data. Data, from raw sound, for right and left.
    byte sample;
    int dl, dr;

    // Pointers in global mixbuffer, left, right, end.
    int16_t *leftout, *rightout, *leftend;

    // Step in mixbuffer, left and right, thus two.
    int step;

    // Mixing channel index.
    int chan;

    // Left and right channel are in global mixbuffer, alternating.
    leftout = mixbuffer;
    rightout = mixbuffer+1;
    step = 2;

    // Determine end, for left channel only (right channel is implicit).
    leftend = mixbuffer + SAMPLECOUNT*step;

    // Mix sounds into the mixing buffer.
    // Loop over step*SAMPLECOUNT, that is 512 values for two channels.

    while (leftout <= leftend)
    {
        // Reset left/right value.
	dl = 0;
	dr = 0;


	for (chan=0; chan<NUM_CHANNELS; chan++)
	{
            // Check channel, if active.
            if (channels[chan].snd_start_ptr)
            {
                // Get the raw data from the channel. 
                sample = *channels[chan].snd_start_ptr;
                
                // Add left and right part for this channel (sound) to the
                // current data. Adjust volume accordingly.
                dl += channels[chan].leftvol[sample];
                dr += channels[chan].rightvol[sample];

                channels[chan].snd_start_ptr++;

                if (!(channels[chan].snd_start_ptr < channels[chan].snd_end_ptr))
                    I_SndMixResetChannel (chan);
	    }
	}
	
	// Clamp to range. Left hardware channel.
	// Has been char instead of short.
	// if (dl > 127) *leftout = 127;
	// else if (dl < -128) *leftout = -128;
	// else *leftout = dl;

	if (dl > 0x7fff)
	    *leftout = 0x7fff;
	else if (dl < -0x8000)
	    *leftout = -0x8000;
	else
	    *leftout = dl;

	// Same for right hardware channel.
	if (dr > 0x7fff)
	    *rightout = 0x7fff;
	else if (dr < -0x8000)
	    *rightout = -0x8000;
	else
	    *rightout = dr;

	// Increment current pointers in mixbuffer.
	leftout += step;
	rightout += step;
    }

    return;
}

static uint32_t playOneBlock(u64 *readIndex, float *audioDataStart)
{
    static uint64_t audio_block_index=1;
    uint64_t current_block = *readIndex;
    float *buf;

    if (audio_block_index == current_block)
        return 0;

    buf = audioDataStart + 2 /*channelcount*/ * AUDIO_BLOCK_SAMPLES * audio_block_index;

    I_UpdateSound();

    for (int i = 0; i < SAMPLECOUNT*2; i++)
        buf[i] = (float)mixbuffer[i]/32767.0f;

    audio_block_index = (audio_block_index + 1) % AUDIO_BLOCK_8;

    return 1;
}

static void mix_thread_func (uint64_t arg)
{
    for (;;)
    {
        sys_ppu_thread_yield();
        usleep (20);

        sys_lwmutex_lock (&chanmutex, 0);

        playOneBlock((u64*)(u64)ps3_audio_port_cfg.readIndex,
                     (float*)(u64)ps3_audio_port_cfg.audioDataStart);

        sys_lwmutex_unlock (&chanmutex);
    }
 
    sys_ppu_thread_exit(0);
    return;
}

void I_UpdateSoundParams (int handle, int vol, int sep, int pitch)
{
    int rightvol;
    int	leftvol;
    int i;

    // sys_lwmutex_lock (&chanmutex, 0);
    
    for (i=0; i<NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
        {
            sep += 1;

            leftvol = vol - ((vol*sep*sep) >> 16); ///(256*256);
            sep -= 257;
            rightvol = vol - ((vol*sep*sep) >> 16);	

            if (rightvol < 0 || rightvol > 127)
        	I_Error("I_UpdateSoundParams: rightvol out of bounds.");
    
            if (leftvol < 0 || leftvol > 127)
	        I_Error("I_UpdateSoundParams: leftvol out of bounds.");

            channels[i].leftvol = &vol_lookup[leftvol*256];
            channels[i].rightvol = &vol_lookup[rightvol*256];

            //sys_lwmutex_unlock (&chanmutex);
            return;
        }
    }
 
    //sys_lwmutex_unlock (&chanmutex);
    return;
}


void I_ShutdownSound(void)
{    
    return;
}


void I_InitSound(void)
{
    int i;
    u64 thread_arg = 0x666;
    u64 priority = 1500;
    size_t stack_size = 0x10000;

    AudioPortParam params;
    uint32_t portNum;
    int ret;
    
    // init PSL1GHT audio
    ret = audioInit();
    printf ("I_InitSound: audioInit returns %d.\n", ret);
    
    params.numChannels = AUDIO_PORT_2CH;        // stereo
    params.numBlocks = AUDIO_BLOCK_8;
    params.attr = 0;
    params.level = 1.0f;
    
    ret = audioPortOpen (&params, &portNum);
    printf ("I_InitSound: audioPortOpen returns %d.\n"\
            "             Port number is %d.\n", ret, portNum);
    
    ret = audioGetPortConfig (portNum, &ps3_audio_port_cfg);
    printf ("I_InitSound: audioGetPortConfig returns %d.\n"\
            "              readIndex     : 0x%08x\n"\
            "              status        : %d\n"\
            "              channelCount  : %d\n"\
            "              numBlocks     : %d\n"\
            "              portSize      : %d\n"\
            "              audioDataStart: 0x%08x\n",
        ret,
        ps3_audio_port_cfg.readIndex,
        ps3_audio_port_cfg.status,
        ps3_audio_port_cfg.channelCount,
        ps3_audio_port_cfg.numBlocks,
        ps3_audio_port_cfg.portSize,
        ps3_audio_port_cfg.audioDataStart);
    
    ret = audioPortStart (portNum);
    printf ("I_InitSound: audioPortStart returns %d.\n", ret);

    memset (&lengths, 0, sizeof(int)*NUMSFX);
    for (i=1 ; i<NUMSFX ; i++)
    { 
        // Alias? Example is the chaingun sound linked to pistol.
        if (!S_sfx[i].link)
        {
            // Load data from WAD file.
            S_sfx[i].data = I_SndLoadSample( S_sfx[i].name, &lengths[i] );
        }	
        else
        {
            // Previously loaded already?
            S_sfx[i].data = S_sfx[i].link->data;
            lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
        }
    }

    I_SetChannels();
  
    memset (&MutexAttrs, 0, sizeof(sys_lwmutex_attribute_t));
    MutexAttrs.attr_protocol = 2;          // PRIORITY
    MutexAttrs.attr_recursive = 0x20;      // NOT RECURSIVE
    if (sys_lwmutex_create (&chanmutex, &MutexAttrs) != 0)
        I_Error ("I_InitSound: sys_lwmutex_create failed.\n");

    ret = sys_ppu_thread_create(&mixthread, mix_thread_func, thread_arg, priority,
           stack_size, THREAD_JOINABLE, mixthread_name);
    
    printf ("I_InitSound: sys_ppu_thread_create returned %d.\n", ret);

    return;
}




//
// MUSIC API.
// Still no music done.
// Remains. Dummies.
//
void I_InitMusic(void)		{ }
void I_ShutdownMusic(void)	{ }

static int	looping=0;
static int	musicdies=-1;

void I_PlaySong(int handle, int looping)
{
  // UNUSED.
  handle = looping = 0;
  musicdies = gametic + TICRATE*30;
}

void I_PauseSong (int handle)
{
  // UNUSED.
  handle = 0;
}

void I_ResumeSong (int handle)
{
  // UNUSED.
  handle = 0;
}

void I_StopSong(int handle)
{
  // UNUSED.
  handle = 0;
  
  looping = 0;
  musicdies = 0;
}

void I_UnRegisterSong(int handle)
{
  // UNUSED.
  handle = 0;
}

int I_RegisterSong(void* data)
{
  // UNUSED.
  data = NULL;
  
  return 1;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
  // UNUSED.
  handle = 0;
  return looping || musicdies > gametic;
}
