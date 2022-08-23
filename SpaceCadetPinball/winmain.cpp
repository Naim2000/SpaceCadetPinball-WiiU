#include "pch.h"
#include "winmain.h"

#include "control.h"
#include "fullscrn.h"
#include "midi.h"
#include "pinball.h"
#include "options.h"
#include "pb.h"
#include "render.h"
#include "Sound.h"

#include <whb/proc.h>
#include <romfs-wiiu.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <whb/log.h>

SDL_Window* winmain::MainWindow = nullptr;
SDL_Renderer* winmain::Renderer = nullptr;
ImGuiIO* winmain::ImIO = nullptr;

int winmain::return_value = 0;
bool winmain::bQuit = false;
bool winmain::activated = false;
int winmain::DispFrameRate = 0;
bool winmain::DispGRhistory = false;
bool winmain::single_step = false;
bool winmain::has_focus = true;
int winmain::last_mouse_x;
int winmain::last_mouse_y;
int winmain::mouse_down;
bool winmain::no_time_loss = false;

bool winmain::restart = false;

gdrv_bitmap8* winmain::gfr_display = nullptr;
bool winmain::ShowAboutDialog = false;
bool winmain::ShowImGuiDemo = false;
bool winmain::ShowSpriteViewer = false;
bool winmain::LaunchBallEnabled = true;
bool winmain::HighScoresEnabled = true;
bool winmain::DemoActive = false;
int winmain::MainMenuHeight = 0;
std::string winmain::FpsDetails;
double winmain::UpdateToFrameRatio;
winmain::DurationMs winmain::TargetFrameTime;
optionsStruct& winmain::Options = options::Options;
winmain::DurationMs winmain::SpinThreshold = DurationMs(0.005);
WelfordState winmain::SleepState{};

bool leftTrigger = false;
bool rightTrigger = false;

int winmain::WinMain(LPCSTR lpCmdLine)
{
	WHBProcInit();
	WHBLogCafeInit();
	WHBLogUdpInit();
	WHBLogPrintf("passed init");
	romfsInit();
	restart = false;
	bQuit = false;

	std::set_new_handler(memalloc_failure);

	// SDL init
	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO |
		SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Could not initialize SDL2", SDL_GetError(), nullptr);
		return 1;
	}

	pinball::quickFlag = strstr(lpCmdLine, "-quick") != nullptr;

	// SDL window
	SDL_Window* window = SDL_CreateWindow
	(
		pinball::get_rc_string(38, 0),
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		1280, 720,
		SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE
	);
	MainWindow = window;
	if (!window)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Could not create window", SDL_GetError(), nullptr);
		return 1;
	}

	// If HW fails, fallback to SW SDL renderer.
	SDL_Renderer* renderer = nullptr;
	auto swOffset = strstr(lpCmdLine, "-sw") != nullptr ? 1 : 0;
	for (int i = swOffset; i < 2 && !renderer; i++)
	{
		Renderer = renderer = SDL_CreateRenderer
		(
			window,
			-1,
			i == 0 ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE
		);
	}
	if (!renderer)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Could not create renderer", SDL_GetError(), window);
		return 1;
	}
	SDL_RendererInfo rendererInfo{};
	if (!SDL_GetRendererInfo(renderer, &rendererInfo))
		printf("Using SDL renderer: %s\n", rendererInfo.name);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	// ImGui init
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiSDL::Initialize(renderer, 0, 0);
	ImGui::StyleColorsDark();
	ImGuiIO& io = ImGui::GetIO();
	ImIO = &io;
	// ImGui_ImplSDL2_Init is private, we are not actually using ImGui OpenGl backend
	ImGui_ImplSDL2_InitForOpenGL(window, nullptr);
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	io.IniFilename = "fs:/vol/external01/wiiu/apps/spacecadetpinball/imgui_pb.ini";

	// First step: just load the options
	options::InitPrimary();

	// Data search order: WD, executable path, user pref path, platform specific paths.
	auto basePath = (char*)"romfs:/";
	std::vector<const char*> searchPaths
	{
		{
			"",
			basePath,
			prefPath
		}
	};
	searchPaths.insert(searchPaths.end(), std::begin(PlatformDataPaths), std::end(PlatformDataPaths));
	pb::SelectDatFile(searchPaths);

	// Second step: run updates depending on FullTiltMode
	options::InitSecondary();

	if (!Sound::Init(Options.SoundChannels, Options.Sounds, Options.SoundVolume))
		Options.Sounds = false;

	if (!pinball::quickFlag && !midi::music_init(Options.MusicVolume))
		Options.Music = false;

	if (pb::init())
	{
		std::string message = "The .dat file is missing.\n"
			"Make sure that the game data is present in any of the following locations:\n";
		for (auto path : searchPaths)
		{
			if (path)
			{
				message = message + (path[0] ? path : "working directory") + "\n";
			}
		}
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Could not load game data",
		                         message.c_str(), window);
		return 1;
	}

	fullscrn::init();

	pb::reset_table();
	pb::firsttime_setup();

	if (strstr(lpCmdLine, "-fullscreen"))
	{
		Options.FullScreen = true;
	}

	SDL_ShowWindow(window);
	fullscrn::set_screen_mode(Options.FullScreen);

	if (strstr(lpCmdLine, "-demo"))
		pb::toggle_demo();
	else
		pb::replay_level(false);

	unsigned updateCounter = 0, frameCounter = 0;

	auto frameStart = Clock::now();
	double UpdateToFrameCounter = 0;
	DurationMs sleepRemainder(0), frameDuration(TargetFrameTime);
	auto prevTime = frameStart;
	while (true)
	{
		if (DispFrameRate)
		{
			auto curTime = Clock::now();
			if (curTime - prevTime > DurationMs(1000))
			{
				char buf[60];
				auto elapsedSec = DurationMs(curTime - prevTime).count() * 0.001;
				snprintf(buf, sizeof buf, "Updates/sec = %02.02f Frames/sec = %02.02f ",
				         updateCounter / elapsedSec, frameCounter / elapsedSec);
				SDL_SetWindowTitle(window, buf);
				FpsDetails = buf;
				frameCounter = updateCounter = 0;
				prevTime = curTime;
			}
		}

		if (!ProcessWindowMessages() || bQuit || !WHBProcIsRunning())
			break;

		if (has_focus)
		{
			if (mouse_down)
			{
				int x, y, w, h;
				SDL_GetMouseState(&x, &y);
				SDL_GetWindowSize(window, &w, &h);
				float dx = static_cast<float>(last_mouse_x - x) / static_cast<float>(w);
				float dy = static_cast<float>(y - last_mouse_y) / static_cast<float>(h);
				pb::ballset(dx, dy);

				// Original creates continuous mouse movement with mouse capture.
				// Alternative solution: mouse warp at window edges.
				int xMod = 0, yMod = 0;
				if (x == 0 || x >= w - 1)
					xMod = w - 2;
				if (y == 0 || y >= h - 1)
					yMod = h - 2;
				if (xMod != 0 || yMod != 0)
				{
					// Mouse warp does not work over remote desktop or in some VMs
					x = abs(x - xMod); y = abs(y - yMod);
					SDL_WarpMouseInWindow(window, x, y);
				}

				last_mouse_x = x;
				last_mouse_y = y;
			}
			if (!single_step && !no_time_loss)
			{
				auto dt = static_cast<float>(frameDuration.count());
				pb::frame(dt);
				if (DispGRhistory)
				{
					auto width = 300;
					auto height = 64, halfHeight = height / 2;
					if (!gfr_display)
					{
						gfr_display = new gdrv_bitmap8(width, height, false);
						gfr_display->CreateTexture("nearest", SDL_TEXTUREACCESS_STREAMING);
					}

					gdrv::ScrollBitmapHorizontal(gfr_display, -1);
					gdrv::fill_bitmap(gfr_display, 1, halfHeight, width - 1, 0, ColorRgba::Black()); // Background
					// Target	
					gdrv::fill_bitmap(gfr_display, 1, halfHeight, width - 1, halfHeight, ColorRgba::White());

					auto target = static_cast<float>(TargetFrameTime.count());
					auto scale = halfHeight / target;
					auto diffHeight = std::min(static_cast<int>(std::round(std::abs(target - dt) * scale)), halfHeight);
					auto yOffset = dt < target ? halfHeight : halfHeight - diffHeight;
					gdrv::fill_bitmap(gfr_display, 1, diffHeight, width - 1, yOffset, ColorRgba::Red()); // Target diff
				}
				updateCounter++;
			}
			no_time_loss = false;

			if (UpdateToFrameCounter >= UpdateToFrameRatio)
			{
				ImGui_ImplSDL2_NewFrame();
				ImGui::NewFrame();
				RenderUi();

				SDL_RenderClear(renderer);
				// Alternative clear hack, clear might fail on some systems
				// Todo: remove original clear, if save for all platforms
				SDL_RenderFillRect(renderer, nullptr);
				render::PresentVScreen();

				ImGui::Render();
				ImGuiSDL::Render(ImGui::GetDrawData());

				SDL_RenderPresent(renderer);
				frameCounter++;
				UpdateToFrameCounter -= UpdateToFrameRatio;
			}

			auto sdlError = SDL_GetError();
			if (sdlError[0])
			{
				SDL_ClearError();
				printf("SDL Error: %s\n", sdlError);
			}

			auto updateEnd = Clock::now();
			auto targetTimeDelta = TargetFrameTime - DurationMs(updateEnd - frameStart) - sleepRemainder;

			TimePoint frameEnd;
			if (targetTimeDelta > DurationMs::zero() && !Options.UncappedUpdatesPerSecond)
			{
				if (Options.HybridSleep)
					HybridSleep(targetTimeDelta);
				else
					std::this_thread::sleep_for(targetTimeDelta);
				frameEnd = Clock::now();
			}
			else
			{
				frameEnd = updateEnd;
			}

			// Limit duration to 2 * target time
			sleepRemainder = Clamp(DurationMs(frameEnd - updateEnd) - targetTimeDelta, -TargetFrameTime, TargetFrameTime);
			frameDuration = std::min<DurationMs>(DurationMs(frameEnd - frameStart), 2 * TargetFrameTime);
			frameStart = frameEnd;
			UpdateToFrameCounter++;
		}
	}

	delete gfr_display;
	gfr_display = nullptr;
	options::uninit();
	midi::music_shutdown();
	pb::uninit();
	Sound::Close();
	ImGuiSDL::Deinitialize();
	ImGui_ImplSDL2_Shutdown();
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	ImGui::DestroyContext();
	SDL_Quit();
	romfsExit();
	WHBProcShutdown();

	return return_value;
}

void winmain::RenderUi()
{
	// A minimal window with a button to prevent menu lockout.
	if (!Options.ShowMenu)
	{
		ImGui::SetNextWindowPos(ImVec2{});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10, 0});
		if (ImGui::Begin("main", nullptr,
		                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
		                 ImGuiWindowFlags_AlwaysAutoResize |
		                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing))
		{
			ImGui::PushID(1);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{});
			if (ImGui::Button("Menu"))
			{
				options::toggle(Menu1::Show_Menu);
			}
			ImGui::PopStyleColor(1);
			ImGui::PopID();
		}
		ImGui::End();
		ImGui::PopStyleVar();

		// This window can not loose nav focus for some reason, clear it manually.
		if (ImGui::IsNavInputDown(ImGuiNavInput_Cancel))
			ImGui::FocusWindow(nullptr);
	}

	// No demo window in release to save space
#ifndef NDEBUG
	if (ShowImGuiDemo)
		ImGui::ShowDemoWindow(&ShowImGuiDemo);
#endif

	a_dialog();
	high_score::RenderHighScoreDialog();
	if (ShowSpriteViewer)
		render::SpriteViewer(&ShowSpriteViewer);
	options::RenderControlDialog();
	if (DispGRhistory)
		RenderFrameTimeDialog();
}

int winmain::event_handler(const SDL_Event* event)
{
	ImGui_ImplSDL2_ProcessEvent(event);

	if (ImIO->WantCaptureMouse && !options::WaitingForInput())
	{
		if (mouse_down)
		{
			mouse_down = 0;
			SDL_SetWindowGrab(MainWindow, SDL_FALSE);
		}
		switch (event->type)
		{
		case SDL_MOUSEMOTION:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEWHEEL:
			return 1;
		default: ;
		}
	}
	if (ImIO->WantCaptureKeyboard && !options::WaitingForInput())
	{
		switch (event->type)
		{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			return 1;
		default: ;
		}
	}

	switch (event->type)
	{
	case SDL_QUIT:
		end_pause();
		bQuit = true;
		fullscrn::shutdown();
		return_value = 0;
		return 0;
	case SDL_KEYUP:
		pb::InputUp({InputTypes::Keyboard, event->key.keysym.sym});
		break;
	case SDL_KEYDOWN:
		if (!event->key.repeat)
			pb::InputDown({InputTypes::Keyboard, event->key.keysym.sym});
		switch (event->key.keysym.sym)
		{
		case SDLK_ESCAPE:
			if (Options.FullScreen)
				options::toggle(Menu1::Full_Screen);
			SDL_MinimizeWindow(MainWindow);
			break;
		case SDLK_F2:
			new_game();
			break;
		case SDLK_F3:
			pause();
			break;
		case SDLK_F4:
			options::toggle(Menu1::Full_Screen);
			break;
		case SDLK_F5:
			options::toggle(Menu1::Sounds);
			break;
		case SDLK_F6:
			options::toggle(Menu1::Music);
			break;
		case SDLK_F8:
			pause(false);
			options::ShowControlDialog();
			break;
		case SDLK_F9:
			options::toggle(Menu1::Show_Menu);
			break;
		default:
			break;
		}

		if (!pb::cheat_mode)
			break;

		switch (event->key.keysym.sym)
		{
		case SDLK_g:
			DispGRhistory ^= true;
			break;
		case SDLK_o:
			{
				auto plt = new ColorRgba[4 * 256];
				auto pltPtr = &plt[10]; // first 10 entries are system colors hardcoded in display_palette()
				for (int i1 = 0, i2 = 0; i1 < 256 - 10; ++i1, i2 += 8)
				{
					unsigned char blue = i2, redGreen = i2;
					if (i2 > 255)
					{
						blue = 255;
						redGreen = i1;
					}

					*pltPtr++ = ColorRgba{blue, redGreen, redGreen, 0};
				}
				gdrv::display_palette(plt);
				delete[] plt;
			}
			break;
		case SDLK_y:
			SDL_SetWindowTitle(MainWindow, "Pinball");
			DispFrameRate = DispFrameRate == 0;
			break;
		case SDLK_F1:
			pb::frame(10);
			break;
		case SDLK_F10:
			single_step ^= true;
			if (!single_step)
				no_time_loss = true;
			break;
		default:
			break;
		}
		break;
	case SDL_MOUSEBUTTONDOWN:
		{
			bool noInput = false;
			switch (event->button.button)
			{
			case SDL_BUTTON_LEFT:
				if (pb::cheat_mode)
				{
					mouse_down = 1;
					last_mouse_x = event->button.x;
					last_mouse_y = event->button.y;
					SDL_SetWindowGrab(MainWindow, SDL_TRUE);
					noInput = true;
				}
				break;
			default:
				break;
			}

			if (!noInput)
				pb::InputDown({InputTypes::Mouse, event->button.button});
		}
		break;
	case SDL_MOUSEBUTTONUP:
		{
			bool noInput = false;
			switch (event->button.button)
			{
			case SDL_BUTTON_LEFT:
				if (mouse_down)
				{
					mouse_down = 0;
					SDL_SetWindowGrab(MainWindow, SDL_FALSE);
					noInput = true;
				}
				break;
			default:
				break;
			}

			if (!noInput)
				pb::InputUp({InputTypes::Mouse, event->button.button});
		}
		break;
	case SDL_WINDOWEVENT:
		switch (event->window.event)
		{
		case SDL_WINDOWEVENT_FOCUS_GAINED:
		case SDL_WINDOWEVENT_TAKE_FOCUS:
		case SDL_WINDOWEVENT_SHOWN:
			activated = true;
			Sound::Activate();
			if (Options.Music && !single_step)
				midi::music_play();
			no_time_loss = true;
			has_focus = true;
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
		case SDL_WINDOWEVENT_HIDDEN:
			activated = false;
			fullscrn::activate(0);
			Options.FullScreen = false;
			Sound::Deactivate();
			midi::music_stop();
			has_focus = false;
			pb::loose_focus();
			break;
		case SDL_WINDOWEVENT_SIZE_CHANGED:
		case SDL_WINDOWEVENT_RESIZED:
			fullscrn::window_size_changed();
			break;
		default: ;
		}
		break;
	case SDL_JOYDEVICEADDED:
		SDL_JoystickOpen(event->jdevice.which);
		break;
	case SDL_JOYDEVICEREMOVED:
		{
			SDL_Joystick* joystick = SDL_JoystickFromInstanceID(event->jdevice.which);
			if (joystick)
			{
				SDL_JoystickClose(joystick);
			}
		}
		break;
	case SDL_JOYBUTTONDOWN:
		// this sucks
		switch (event->jbutton.button) {
			case 0:
				pb::InputDown({InputTypes::GameController, SDL_CONTROLLER_BUTTON_B});
				break;
			case 5:
				options::toggle(Menu1::Music);
				break;
			case 10:
				pause();
				break;
			case 11:
				winmain::new_game();
				break;
			case 6:
			case 8:
				pb::InputDown({InputTypes::GameController, SDL_CONTROLLER_BUTTON_LEFTSHOULDER});
				break;
			case 7:
			case 9:
				pb::InputDown({InputTypes::GameController, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER});
				break;
			case 13:
				pb::InputDown({InputTypes::GameController, SDL_CONTROLLER_BUTTON_DPAD_UP});
				break;
			case 12:
				pb::InputDown({InputTypes::GameController, SDL_CONTROLLER_BUTTON_DPAD_LEFT});
				break;
			case 14:
				pb::InputDown({InputTypes::GameController, SDL_CONTROLLER_BUTTON_DPAD_RIGHT});
				break;
		}
		break;
	case SDL_JOYBUTTONUP:
		switch (event->jbutton.button) {
			case 0:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_B});
				break;
			case 10:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_START});
				break;
			case 11:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_BACK});
				break;
			case 6:
			case 8:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_LEFTSHOULDER});
				break;
			case 7:
			case 9:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER});
				break;
			case 13:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_DPAD_UP});
				break;
			case 12:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_DPAD_LEFT});
				break;
			case 14:
				pb::InputUp({InputTypes::GameController, SDL_CONTROLLER_BUTTON_DPAD_RIGHT});
				break;
		}
		break;
	default: ;
	}

	return 1;
}

int winmain::ProcessWindowMessages()
{
	static auto idleWait = 0;
	SDL_Event event;
	if (has_focus)
	{
		idleWait = static_cast<int>(TargetFrameTime.count());
		while (SDL_PollEvent(&event))
		{
			if (!event_handler(&event))
				return 0;
		}

		return 1;
	}

	// Progressively wait longer when transitioning to idle
	idleWait = std::min(idleWait + static_cast<int>(TargetFrameTime.count()), 500);
	if (SDL_WaitEventTimeout(&event, idleWait))
	{
		idleWait = static_cast<int>(TargetFrameTime.count());
		return event_handler(&event);
	}
	return 1;
}

void winmain::memalloc_failure()
{
	midi::music_stop();
	Sound::Close();
	char* caption = pinball::get_rc_string(170, 0);
	char* text = pinball::get_rc_string(179, 0);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, caption, text, MainWindow);
	std::exit(1);
}

void winmain::a_dialog()
{
	if (ShowAboutDialog == true)
	{
		ShowAboutDialog = false;
		ImGui::OpenPopup("About");
	}

	bool unused_open = true;
	if (ImGui::BeginPopupModal("About", &unused_open, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("3D Pinball for Windows - Space Cadet");
		ImGui::TextUnformatted("Original game by Cinematronics, Microsoft");
		ImGui::Separator();

		ImGui::TextUnformatted("Decompiled -> Ported to SDL");
		ImGui::TextUnformatted("Version 2.0.1");
		if (ImGui::SmallButton("Project home: https://github.com/k4zmu2a/SpaceCadetPinball"))
		{
#if SDL_VERSION_ATLEAST(2, 0, 14)
			// Relatively new feature, skip with older SDL
			SDL_OpenURL("https://github.com/k4zmu2a/SpaceCadetPinball");
#endif
		}
		ImGui::Separator();

		if (ImGui::Button("Ok"))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void winmain::end_pause()
{
	if (single_step)
	{
		pb::pause_continue();
		no_time_loss = true;
	}
}

void winmain::new_game()
{
	end_pause();
	pb::replay_level(false);
}

void winmain::pause(bool toggle)
{
	if (toggle || !single_step)
	{
		pb::pause_continue();
		no_time_loss = true;
	}
}

void winmain::Restart()
{
	restart = true;
	SDL_Event event{SDL_QUIT};
	SDL_PushEvent(&event);
}

void winmain::UpdateFrameRate()
{
	// UPS >= FPS
	auto fps = Options.FramesPerSecond, ups = Options.UpdatesPerSecond;
	UpdateToFrameRatio = static_cast<double>(ups) / fps;
	TargetFrameTime = DurationMs(1000.0 / ups);
}

void winmain::RenderFrameTimeDialog()
{
	if (!gfr_display)
		return;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{300, 70});
	if (ImGui::Begin("Frame Times", &DispGRhistory, ImGuiWindowFlags_NoScrollbar))
	{
		auto target = static_cast<float>(TargetFrameTime.count());
		auto scale = 1 / (gfr_display->Height / 2 / target);

		auto spin = Options.HybridSleep ? static_cast<float>(SpinThreshold.count()) : 0;
		ImGui::Text("Target frame time:%03.04fms, 1px:%03.04fms, SpinThreshold:%03.04fms",
		            target, scale, spin);
		gfr_display->BlitToTexture();
		auto region = ImGui::GetContentRegionAvail();
		ImGui::Image(gfr_display->Texture, region);
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void winmain::HybridSleep(DurationMs sleepTarget)
{
	static constexpr double StdDevFactor = 0.5;

	// This nice concept is from https://blat-blatnik.github.io/computerBear/making-accurate-sleep-function/
	// Sacrifices some CPU time for smaller frame time jitter
	while (sleepTarget > SpinThreshold)
	{
		auto start = Clock::now();
		std::this_thread::sleep_for(DurationMs(1));
		auto end = Clock::now();

		auto actualDuration = DurationMs(end - start);
		sleepTarget -= actualDuration;

		// Update expected sleep duration using Welford's online algorithm
		// With bad timer, this will run away to 100% spin
		SleepState.Advance(actualDuration.count());
		SpinThreshold = DurationMs(SleepState.mean + SleepState.GetStdDev() * StdDevFactor);
	}

	// spin lock
	for (auto start = Clock::now(); DurationMs(Clock::now() - start) < sleepTarget;);
}
