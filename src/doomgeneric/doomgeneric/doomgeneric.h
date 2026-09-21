#ifndef DOOM_GENERIC
#define DOOM_GENERIC

#include <stdlib.h>
#include <stdint.h>

#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 640
#endif  // DOOMGENERIC_RESX

#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 400
#endif  // DOOMGENERIC_RESY


#ifdef CMAP256

typedef uint8_t pixel_t;

#else  // CMAP256

typedef uint32_t pixel_t;

#endif  // CMAP256


extern pixel_t* DG_ScreenBuffer;

#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick();


//Implement below functions for your platform
void DG_Init();
void DG_DrawFrame();
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs();
int DG_GetKey(int* pressed, unsigned char* key);
void DG_SetWindowTitle(const char * title);

// MorphOS extension (v10.10 Fase C): relative mouse. Returns 1 on
// success; *dx/*dy are PS/2 counts since the previous poll, *buttons
// is the kernel button bitmask (bit0 left, bit1 right, bit2 middle).
// DG_MouseButtons() converts button edges into synthetic DOOM keys
// (left = fire, right = next weapon) on platforms that want it.
int DG_GetMouse(int* dx, int* dy, int* buttons);
void DG_MouseButtons(int buttons);

#ifdef __cplusplus
}
#endif

#endif //DOOM_GENERIC
