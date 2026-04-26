// PsionDoomApp.h
//
// Copyright 17/02/2019 
//

#ifndef HEADER_ISYSTEME32
#define HEADER_ISYSTEME32


#ifdef __cplusplus
extern "C" {
#endif


#include "doomtype.h"

void I_InitScreen_e32();

void I_CreateBackBuffer_e32();

int I_GetVideoWidth_e32();

int I_GetVideoHeight_e32();

void I_FinishUpdate_e32(const byte* srcBuffer, const byte* pallete, const unsigned int width, const unsigned int height);

void I_SetPallete_e32(const byte* pallete);

void I_ProcessKeyEvents();

void I_DebugCheckpoint_e32(const char* checkpoint);
void I_DebugLog_e32(const char* message);
void I_DebugPause_e32(const char* reason);
int I_IsTexturedPlanesEnabled_e32(void);
int I_IsVisualExtrasEnabled_e32(void);
int I_IsFilesystemEnabled_e32(void);
int I_IsDemoEnabled_e32(void);
int I_IsShutdownRequested_e32(void);
int I_IsStartupOptionsActive_e32(void);

int I_GetTime_e32(void);

void I_Error (const char *error, ...);

void I_Quit_e32();

byte* I_GetBackBuffer();

byte* I_GetFrontBuffer();

#ifdef __cplusplus
}
#endif


#endif
