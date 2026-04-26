/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Startup and quit functions. Handles signals, inits the
 *      memory management, then calls D_DoomMain. Also contains
 *      I_Init which does other system-related startup stuff.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "doomdef.h"
#include "d_main.h"
#include "m_fixed.h"
#include "i_system.h"
#include "i_video.h"
#include "z_zone.h"
#include "lprintf.h"
#include "m_random.h"
#include "doomstat.h"
#include "g_game.h"
#include "m_misc.h"
#include "i_sound.h"
#include "i_main.h"
#include "lprintf.h"
#include "global_data.h"

#ifdef NUMWORKS
#include "i_system_e32.h"
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef NUMWORKS
const char eadk_app_name[]
#if PLATFORM_DEVICE
    __attribute__((section(".rodata.eadk_app_name")))
#endif
    = "Doom";

const uint32_t eadk_api_level
#if PLATFORM_DEVICE
    __attribute__((section(".rodata.eadk_api_level")))
#endif
    = 0;

#if PLATFORM_DEVICE
void _exit(int status)
{
    (void)status;
    while (1)
    {
    }
}
#endif
#endif

/* Most of the following has been rewritten by Lee Killough
 *
 * I_GetTime
 * killough 4/13/98: Make clock rate adjustable by scale factor
 * cphipps - much made static
 */

void I_Init(void)
{
#ifdef NUMWORKS
    lprintf(LO_INFO, "I_Init: audio disabled for NumWorks phase-1 bring-up");
#else
    if (!(nomusicparm && nosfxparm))
        I_InitSound();
#endif
}

static void PrintVer(void)
{
    char vbuf[24];
    lprintf(LO_INFO,"%s",I_GetVersionString(vbuf,200));
}

int main(int argc, const char * const * argv)
{
    /* cphipps - call to video specific startup code */
    I_PreInitGraphics();

#ifdef NUMWORKS
    I_DebugCheckpoint_e32("After I_PreInitGraphics");
#endif

    PrintVer();

#ifdef NUMWORKS
    I_DebugCheckpoint_e32("After PrintVer");
#endif

    //Call this before Z_Init as maxmod uses malloc.
    I_Init();

#ifdef NUMWORKS
    I_DebugCheckpoint_e32("After I_Init");
#endif

    Z_Init();                  /* 1/18/98 killough: start up memory stuff first */

#ifdef NUMWORKS
    {
        char heapMsg[64];
        snprintf(heapMsg, sizeof(heapMsg), "After Z_Init (zone free RAM: %u)", Z_GetHeapSize());
        I_DebugCheckpoint_e32(heapMsg);
    }
#endif

    InitGlobals();

#ifdef NUMWORKS
    I_DebugCheckpoint_e32("After InitGlobals");
    if (I_IsDemoEnabled_e32())
    {
        D_SetDemoMode("demo1");
        I_DebugLog_e32("Demo mode enabled");
    }
    I_DebugCheckpoint_e32("Before D_DoomMain");
#endif

    D_DoomMain ();
    return 0;
}
