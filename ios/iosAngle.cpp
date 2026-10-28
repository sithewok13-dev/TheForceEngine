// ANGLE (OpenGL ES 3.0 on Metal) for iOS: see iosAngle.h

#include <SDL.h>
#include <SDL_metal.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

#include <string.h>
#include <stdint.h>

#include <TFE_System/system.h>
#include "iosAngle.h"

// (the real SDL functions are needed here, not the macros)
#undef SDL_WINDOW_OPENGL
#undef SDL_GL_CreateContext
#undef SDL_GL_DeleteContext
#undef SDL_GL_SwapWindow
#undef SDL_GL_GetProcAddress
#undef SDL_GL_SetSwapInterval
#undef SDL_GL_GetSwapInterval
#undef SDL_GL_SetAttribute
#undef SDL_GL_ExtensionSupported

static SDL_MetalView s_metalView = nullptr;
static EGLDisplay    s_display = EGL_NO_DISPLAY;
static EGLSurface    s_surface = EGL_NO_SURFACE;
static EGLContext    s_context = EGL_NO_CONTEXT;
static int           s_swapInterval = 1;

static void fail(const char* what)
{
	TFE_System::logWrite(LOG_ERROR, "ANGLE", "%s (EGL error 0x%04x)", what, (unsigned)eglGetError());
	SDL_SetError("ANGLE: %s", what);
}

// ANGLE picks a backend per display, so ask for Metal by name rather than
// relying on it being the default on Apple platforms
static EGLDisplay getMetalDisplay()
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplayEXT =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (getPlatformDisplayEXT)
	{
		const EGLint attribs[] =
		{
			EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
			EGL_NONE
		};
		EGLDisplay metal = getPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, (void*)(uintptr_t)EGL_DEFAULT_DISPLAY, attribs);
		if (metal != EGL_NO_DISPLAY) { return metal; }
		TFE_System::logWrite(LOG_WARNING, "ANGLE", "No Metal display, using the default one.");
	}
	return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

SDL_GLContext IOS_GL_CreateContext(SDL_Window* window)
{
	if (s_context != EGL_NO_CONTEXT)
	{
		IOS_GL_DeleteContext((SDL_GLContext)s_context);
	}

	// SDL's Metal view is backed by a CAMetalLayer, sized and scaled for the
	// window; ANGLE draws straight into that layer
	s_metalView = SDL_Metal_CreateView(window);
	if (!s_metalView)
	{
		TFE_System::logWrite(LOG_ERROR, "ANGLE", "SDL_Metal_CreateView failed: %s", SDL_GetError());
		return nullptr;
	}
	void* layer = SDL_Metal_GetLayer(s_metalView);
	if (!layer)
	{
		TFE_System::logWrite(LOG_ERROR, "ANGLE", "SDL_Metal_GetLayer failed: %s", SDL_GetError());
		IOS_GL_DeleteContext(nullptr);
		return nullptr;
	}

	s_display = getMetalDisplay();
	EGLint major = 0, minor = 0;
	if (s_display == EGL_NO_DISPLAY || !eglInitialize(s_display, &major, &minor))
	{
		fail("eglInitialize failed");
		IOS_GL_DeleteContext(nullptr);
		return nullptr;
	}
	TFE_System::logWrite(LOG_MSG, "ANGLE", "EGL %d.%d, %s", major, minor, eglQueryString(s_display, EGL_VENDOR));

	const EGLint configAttribs[] =
	{
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RED_SIZE,        8,
		EGL_GREEN_SIZE,      8,
		EGL_BLUE_SIZE,       8,
		EGL_ALPHA_SIZE,      8,
		EGL_DEPTH_SIZE,      24,
		EGL_STENCIL_SIZE,    8,
		EGL_NONE
	};
	EGLConfig config = nullptr;
	EGLint numConfigs = 0;
	if (!eglChooseConfig(s_display, configAttribs, &config, 1, &numConfigs) || numConfigs < 1)
	{
		fail("eglChooseConfig found no OpenGL ES 3 config");
		IOS_GL_DeleteContext(nullptr);
		return nullptr;
	}

	s_surface = eglCreateWindowSurface(s_display, config, (EGLNativeWindowType)layer, nullptr);
	if (s_surface == EGL_NO_SURFACE)
	{
		fail("eglCreateWindowSurface failed");
		IOS_GL_DeleteContext(nullptr);
		return nullptr;
	}

	const EGLint contextAttribs[] =
	{
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_CONTEXT_MINOR_VERSION, 0,
		EGL_NONE
	};
	s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttribs);
	if (s_context == EGL_NO_CONTEXT || !eglMakeCurrent(s_display, s_surface, s_surface, s_context))
	{
		fail("eglCreateContext or eglMakeCurrent failed");
		IOS_GL_DeleteContext(nullptr);
		return nullptr;
	}
	eglSwapInterval(s_display, s_swapInterval);

	TFE_System::logWrite(LOG_MSG, "ANGLE", "%s / %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
	return (SDL_GLContext)s_context;
}

void IOS_GL_DeleteContext(SDL_GLContext)
{
	if (s_display != EGL_NO_DISPLAY)
	{
		eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (s_context != EGL_NO_CONTEXT) { eglDestroyContext(s_display, s_context); }
		if (s_surface != EGL_NO_SURFACE) { eglDestroySurface(s_display, s_surface); }
		eglTerminate(s_display);
	}
	s_context = EGL_NO_CONTEXT;
	s_surface = EGL_NO_SURFACE;
	s_display = EGL_NO_DISPLAY;

	// the view must outlive the surface drawing into its layer
	if (s_metalView)
	{
		SDL_Metal_DestroyView(s_metalView);
		s_metalView = nullptr;
	}
}

void IOS_GL_SwapWindow(SDL_Window*)
{
	if (s_display != EGL_NO_DISPLAY && s_surface != EGL_NO_SURFACE)
	{
		eglSwapBuffers(s_display, s_surface);
	}
}

void* IOS_GL_GetProcAddress(const char* proc)
{
	return (void*)eglGetProcAddress(proc);
}

int IOS_GL_SetSwapInterval(int interval)
{
	// iOS always composites on vsync; 0 only means "don't wait"
	s_swapInterval = interval ? 1 : 0;
	if (s_display == EGL_NO_DISPLAY) { return 0; }
	return eglSwapInterval(s_display, s_swapInterval) == EGL_TRUE ? 0 : -1;
}

int IOS_GL_GetSwapInterval(void)
{
	return s_swapInterval;
}

int IOS_GL_SetAttribute(SDL_GLattr, int)
{
	return 0;
}

SDL_bool IOS_GL_ExtensionSupported(const char* extension)
{
	if (s_context == EGL_NO_CONTEXT || !extension) { return SDL_FALSE; }
	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (GLint i = 0; i < count; i++)
	{
		const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (ext && strcmp(ext, extension) == 0) { return SDL_TRUE; }
	}
	return SDL_FALSE;
}
