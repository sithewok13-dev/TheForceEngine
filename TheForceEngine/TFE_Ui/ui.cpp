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
	const bool wantText = ImGui::GetIO().WantTextInput || TFE_Input::gameTextInput();
	if (wantText != (SDL_IsTextInputActive() == SDL_TRUE))
	{
		if (wantText) { SDL_StartTextInput(); }
		else { SDL_StopTextInput(); }
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
#ifdef TFE_IOS
	// iOS touch test (temporary): where taps get to, for a screenshot
	{
		const s32* c = TFE_Input::touchTestCounts();
		char text[128];
		snprintf(text, sizeof(text), "touch test  finger %d  touch-mouse %d  mouse %d  game %d", c[0], c[1], c[2], c[3]);
		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		const ImVec2 size = ImGui::CalcTextSize(text);
		const ImVec2 pos(ImGui::GetIO().DisplaySize.x * 0.5f - size.x * 0.5f, 8.0f);
		drawList->AddRectFilled(ImVec2(pos.x - 6.0f, pos.y - 3.0f), ImVec2(pos.x + size.x + 6.0f, pos.y + size.y + 3.0f), IM_COL32(0, 0, 0, 200));
		drawList->AddText(pos, IM_COL32(255, 255, 0, 255), text);
	}
#endif
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
