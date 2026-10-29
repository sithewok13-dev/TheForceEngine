#pragma once
/*
 * iOS renders through ANGLE's Metal backend instead of Apple's deprecated
 * OpenGL ES. SDL2's UIKit backend only knows Apple's OpenGL ES (EAGL), with
 * no EGL, so SDL is built without OpenGL support and the context comes from
 * here: an EGL surface on SDL's Metal view (OpenGL ES 3.0). SDL still owns
 * the window, events, audio and controllers.
 *
 * Included by the render backend after <SDL.h>: the SDL_GL_* calls there
 * become these, so TFE's own window code stays as it is. The same approach
 * as the owner's DOOM 3 and Jedi Knight ports (there with SDL3).
 */
#ifdef TFE_IOS

#include <SDL.h>

SDL_GLContext IOS_GL_CreateContext(SDL_Window* window);
void          IOS_GL_DeleteContext(SDL_GLContext context);
void          IOS_GL_SwapWindow(SDL_Window* window);
void*         IOS_GL_GetProcAddress(const char* proc);
int           IOS_GL_SetSwapInterval(int interval);
int           IOS_GL_GetSwapInterval(void);
int           IOS_GL_SetAttribute(SDL_GLattr attr, int value);	// the config is fixed: ignored
SDL_bool      IOS_GL_ExtensionSupported(const char* extension);
// The surface's size in pixels (0 x 0 without one): what glReadPixels reads
void          IOS_GL_GetSurfaceSize(int* width, int* height);

// the window is drawn by Metal (ANGLE), not by SDL's OpenGL ES
#define SDL_WINDOW_OPENGL           SDL_WINDOW_METAL

#define SDL_GL_CreateContext        IOS_GL_CreateContext
#define SDL_GL_DeleteContext        IOS_GL_DeleteContext
#define SDL_GL_SwapWindow           IOS_GL_SwapWindow
#define SDL_GL_GetProcAddress       IOS_GL_GetProcAddress
#define SDL_GL_SetSwapInterval      IOS_GL_SetSwapInterval
#define SDL_GL_GetSwapInterval      IOS_GL_GetSwapInterval
#define SDL_GL_SetAttribute         IOS_GL_SetAttribute
#define SDL_GL_ExtensionSupported   IOS_GL_ExtensionSupported

#endif // TFE_IOS
