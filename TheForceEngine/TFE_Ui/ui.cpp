#include <TFE_Ui/ui.h>
#include <TFE_FileSystem/paths.h>
#include <TFE_FileSystem/fileutil.h>

#include "imGUI/imgui.h"
#include "imGUI/imgui_impl_sdl2.h"
#include "imGUI/imgui_impl_opengl3.h"
#ifndef TFE_IOS
#include "portable-file-dialogs.h"
#else
#include "imGUI/imgui_internal.h"	// touch drag scrolling, below
#endif
#include "markdown.h"
#include <SDL.h>
#include <TFE_Input/input.h>

namespace TFE_Ui
{
const char* glsl_version = nullptr;
static s32 s_uiScale = 100;
static bool s_guiFrameActive;

bool init(void* window, void* context, s32 uiScale)
{
#ifdef TFE_GLES
	glsl_version = "#version 300 es";	// OpenGL ES 3.0 (iOS, ANGLE)
#else
	glsl_version = strcmp(SDL_GetPlatform(), "Mac OS X") == 0 ? "#version 410" : "#version 130";
#endif
	
	s_uiScale = uiScale;

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
#ifdef TFE_IOS
	// A finger is less precise than a mouse: a little more room around each
	// control to hit it, without changing the layout.
	ImGui::GetStyle().TouchExtraPadding = ImVec2(4.0f, 4.0f);
#endif

	// Setup Platform/Renderer bindings
	ImGui_ImplSDL2_InitForOpenGL((SDL_Window *)window, context);
	ImGui_ImplOpenGL3_Init(glsl_version);

	// Set the default font (13 px)
	// TODO: Allow scaled UI, so loading a different font for larger scales.
	if (s_uiScale <= 100)
	{
		io.Fonts->AddFontDefault();
	}
	else
	{
		char fp[TFE_MAX_PATH];
		sprintf(fp, "Fonts/DroidSansMono.ttf");
		TFE_Paths::mapSystemPath(fp);
		io.Fonts->AddFontFromFileTTF(fp, f32(13 * s_uiScale / 100));
	}
	
	TFE_Markdown::init(f32(16 * s_uiScale / 100));

#ifndef TFE_IOS	// (no file dialogs on iOS)
	// Initialize file dialogs.
	if (!pfd::settings::available())
	{
		// TODO: Log error
		return false;
	}
#endif
	
	return true;
}

void shutdown()
{
	TFE_Markdown::shutdown();

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
}

void setUiScale(s32 scale)
{
	s_uiScale = scale;
}

s32 getUiScale()
{
	return s_uiScale;
}

void setUiInput(const void* inputEvent)
{
	const SDL_Event* sdlEvent = (SDL_Event*)inputEvent;
	ImGui_ImplSDL2_ProcessEvent(sdlEvent);
}

#ifdef TFE_IOS
// iOS: the on-screen keyboard while an ImGui text field or the game's own
// text box (the new agent's name) wants typing (SDL only starts text input by
// itself where there is no on-screen keyboard).
static void updateTextInput()
{
	// (whether the keyboard is on screen, not SDL_IsTextInputActive: that only
	// says SDL takes typed text, which on iOS it does all along -- the touch
	// overlay keeps it so for a hardware keyboard -- so the keyboard was never
	// asked for)
	static bool s_asked = false;
	static u32 s_askedAt = 0;
	SDL_Window* window = SDL_GetKeyboardFocus();
	const bool wantText = ImGui::GetIO().WantTextInput || TFE_Input::gameTextInput();
	const bool shown = window && SDL_IsScreenKeyboardShown(window) == SDL_TRUE;
	if (wantText)
	{
		// asked again at most twice a second, should iOS put it away
		const u32 now = SDL_GetTicks();
		if (!shown && (!s_asked || now - s_askedAt >= 500))
		{
			// the game's own box: where it ends, so SDL slides the view up
			// above the keyboard (ImGui's boxes set their own)
			const f32 bottom = TFE_Input::gameTextBottom();
			if (bottom > 0.0f && window)
			{
				s32 w, h;
				SDL_GetWindowSize(window, &w, &h);
				SDL_Rect rect = { 0, 0, w, s32(bottom * f32(h)) };
				SDL_SetTextInputRect(&rect);
			}
			SDL_StartTextInput();
			s_asked = true;
			s_askedAt = now;
		}
	}
	else if (s_asked)
	{
		s_asked = false;
		SDL_StopTextInput();
		// (stopping also stops SDL's typed text, which a hardware keyboard needs)
		SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
	}
}

// iOS: a finger dragged up or down over a window that scrolls (not starting
// on a control) scrolls it, as touch screens do; ImGui itself only scrolls
// with the scroll bar or a mouse wheel.
static void touchDragScroll()
{
	ImGuiContext& g = *GImGui;
	ImGuiIO& io = ImGui::GetIO();
	static ImGuiWindow* s_dragWindow = nullptr;
	static bool s_scrolling = false;

	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		s_dragWindow = g.HoveredWindow;
		s_scrolling = false;
	}
	// (a press on a window's empty space makes its "move" id active, even
	// for windows that can't move: that is not a control)
	const bool controlActive = ImGui::IsAnyItemActive() && !(g.ActiveIdWindow && g.ActiveId == g.ActiveIdWindow->MoveId);
	if (!io.MouseDown[ImGuiMouseButton_Left] || !s_dragWindow || controlActive)
	{
		if (!io.MouseDown[ImGuiMouseButton_Left]) { s_dragWindow = nullptr; }
		return;
	}
	if (!s_scrolling)
	{
		if (fabsf(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y) < io.MouseDragThreshold) { return; }
		s_scrolling = true;
	}
	// the window under the finger, or the nearest one around it that scrolls
	ImGuiWindow* window = s_dragWindow;
	while (window && window->ScrollMax.y <= 0.0f && (window->Flags & ImGuiWindowFlags_ChildWindow))
	{
		window = window->ParentWindow;
	}
	if (window && window->ScrollMax.y > 0.0f && io.MouseDelta.y != 0.0f)
	{
		ImGui::SetScrollY(window, window->Scroll.y - io.MouseDelta.y);
	}
}
#endif

void begin()
{
#ifdef TFE_IOS
	updateTextInput();
#endif
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
#ifdef TFE_IOS
	touchDragScroll();
#endif
	s_guiFrameActive = true; // No way to check if we're inside a frame using ImGui's APIs?
}

void render()
{
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	s_guiFrameActive = false;
}

bool isGuiFrameActive() { return s_guiFrameActive; }

void invalidateFontAtlas()
{
	ImGui_ImplOpenGL3_DestroyFontsTexture();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
// General TFE tries to keep paths consistently using forward slashes for readability, consistency and
// generally they work equally well on Linux, Mac and Windows.
/////////////////////////////////////////////////////////////////////////////////////////////////////////
// *However* some specific APIs (usually of the Win32 variety) require backslashes.
// So FileUtil::convertToOSPath() must be used with initial paths to convert to the correct slash type.
// This can be a bit of a waste but I'd rather have consistent paths through most of the application
// and restrict the ugliness to as small an area as possible.
/////////////////////////////////////////////////////////////////////////////////////////////////////////
#ifdef TFE_IOS
// No file dialogs on iOS: the game's files are found in the app's Documents
// folder, and the callers treat an empty result as "cancelled".
FileResult openFileDialog(const char*, const char*, std::vector<std::string> const&, bool)
{
	return FileResult();
}

FileResult directorySelectDialog(const char*, const char*, bool)
{
	return FileResult();
}

FileResult saveFileDialog(const char*, const char*, std::vector<std::string>, bool)
{
	return FileResult();
}
#else
FileResult openFileDialog(const char* title, const char* initPath, std::vector<std::string> const &filters/* = { "All Files", "*" }*/, bool multiSelect/* = false*/)
{
	char initPathOS[TFE_MAX_PATH] = "";
	if (initPath)
	{
		FileUtil::convertToOSPath(initPath, initPathOS);
	}

	return pfd::open_file(title, initPathOS, filters, multiSelect ? pfd::opt::multiselect : pfd::opt::none).result();
}

FileResult directorySelectDialog(const char* title, const char* initPath, bool forceInitPath/* = false*/)
{
	char initPathOS[TFE_MAX_PATH] = "";
	if (initPath)
	{
		FileUtil::convertToOSPath(initPath, initPathOS);
	}

	FileResult result;
	std::string res = pfd::select_folder(title, initPathOS).result();
	result.push_back(res);

	return result;
}

FileResult saveFileDialog(const char* title, const char* initPath, std::vector<std::string> filters/* = { "All Files", "*" }*/, bool forceOverwrite/* = false*/)
{
	char initPathOS[TFE_MAX_PATH] = "";
	if (initPath)
	{
		FileUtil::convertToOSPath(initPath, initPathOS);
	}

	FileResult result;
	std::string res = pfd::save_file(title, initPathOS, filters, forceOverwrite ? pfd::opt::force_overwrite : pfd::opt::none).result();
	result.push_back(res);

	return result;
}

#endif

}
