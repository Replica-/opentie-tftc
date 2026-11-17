#include "tie_runtime/runtime/runtime.h"
#include "tie_runtime/display/classic_framebuffer.h"
#include <landru/host_timing.h>

#include "tie/dsound_wave_tie98.h"
#include "tie/create.h"
#include "tie/fediskio.h"
#include "tie/frontend_display_tie98.h"
#include "tie/fsfx.h"
#include "tie/gamesnd.h"
#include "tie/panel.h"
#include "tie/shell.h"
#include "tie/shellext.h"
#include "tie/shipext.h"
#include "tie/textext.h"
#include "tie/tie.h"
#include "tie/tielogo.h"
#include "tie/title.h"
#include "tie/user.h"
#include "tie/wavestream_tie98.h"
#include "tie_runtime/audio/direct_sound.h"
#include "tie_runtime/audio/imuse_session.h"
#include "tie_runtime/audio/music_policy.h"
#include "tie_runtime/audio/tftc_music.h"
#include "tie_runtime/diagnostics/flight_trace.h"
#include "tie_runtime/display/classic_display.h"
#include "tie_runtime/display/palette_cycle.h"
#include "tie_runtime/display/tie98_display.h"
#include "tie_runtime/flight_assets/service.h"
#include "tie_runtime/integration/landru_adapter.h"
#include "tie_runtime/runtime/flight_screen.h"
#include "tie_runtime/runtime/inflight_state.h"
#include "tie_runtime/runtime/profile.h"
#include "tie_runtime/runtime/shell_task.h"
#include "tie_runtime/snapshot/snapshot.h"
#include "tie_runtime/snapshot/snapshot_flight.h"
#include "tie_runtime/snapshot/snapshot_frontend.h"
#include "tie_runtime/snapshot/snapshot_hud.h"
#include "tie_runtime/snapshot/snapshot_internal.h"
#include "tie_runtime/storage/storage.h"
#include "tie_runtime/timing/replay_timing.h"
#include "tie_runtime/timing/sim_clock.h"

#include <landru/error.h>
#include <landru/pal.h>
#include <landru/surface.h>
#include <landru/task.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool s_quit_requested;
static bool s_tie98_display_initialized;
static bool s_flight_resource_release_requested;
static bool s_settings_menu_requested;
static bool s_frontend_task_waiting;
static bool s_frontend_present_pending;
static uint64_t s_frontend_present_deadline_us;
static uint64_t s_frontend_deferred_us;

enum {
	TIE_RUNTIME_MAX_ADVANCE_US = 64 * 1000,
	TIE_RUNTIME_FRONTEND_PRESENT_US = LANDRU_VGA_RETRACE_PERIOD_US,
};

#define TIE_RUNTIME_MAX_TEST_MISSIONS 32

static char s_test_mission_text[512];
static const char* s_test_missions[TIE_RUNTIME_MAX_TEST_MISSIONS];
static int s_test_mission_count;
static int s_test_mission_index;

static void TieRuntime_SetTestMissions(const char* list) {
	s_test_mission_count = 0;
	s_test_mission_index = 0;
	if (!list)
		return;
	snprintf(s_test_mission_text, sizeof s_test_mission_text, "%s", list);
	for (char* name = strtok(s_test_mission_text, ","); name && s_test_mission_count < TIE_RUNTIME_MAX_TEST_MISSIONS;
		 name = strtok(NULL, ",")) {
		/* shipext keeps mission names lowercase, without the extension. */
		for (char* cursor = name; *cursor; ++cursor) {
			*cursor = (char)tolower((unsigned char)*cursor);
			if (*cursor == '.')
				*cursor = '\0';
		}
		if (name[0])
			s_test_missions[s_test_mission_count++] = name;
	}
}

static void TieRuntime_SelectTestMission(void) {
	shipext_Set_Mission_Name(s_test_missions[s_test_mission_index]);
	shipext_Find_Mission_Ship();
	TieDiagnostics_Log(TIE_LOG_INFO, "[SKIP] test mission %d/%d: %s\n", s_test_mission_index + 1,
					   s_test_mission_count, s_test_missions[s_test_mission_index]);
}

const char* TieRuntime_TestMission(void) {
	return s_test_mission_index < s_test_mission_count ? s_test_missions[s_test_mission_index] : NULL;
}

bool TieRuntime_TestMissionHasNext(void) { return s_test_mission_index + 1 < s_test_mission_count; }

void TieRuntime_EndTestMission(void) {
	/* As the replay viewer's exit (15): a status of 10 or more skips the
	 * replay prompt and info room that wait for input. */
	mission.end_flag = 1;
	mission.player_status = 15;
}

bool TieRuntime_TestMissionAdvance(void) {
	if (!TieRuntime_TestMissionHasNext())
		return false;
	++s_test_mission_index;
	TieRuntime_SelectTestMission();
	return true;
}

bool TieRuntime_Init(const TieRuntimeConfig* config, char* error, size_t error_capacity) {
	if (!config) {
		TieRuntime_Shutdown();
		return false;
	}

	TieClassicFramebuffer_InvalidatePresentedVga();
	TieStorage_Init(&config->storage);
	if (!TieFlightAssets_Init(&config->flight_assets, error, error_capacity)) {
		TieStorage_Shutdown();
		return false;
	}
	TieInflightOptions_Load();
	TieAudio_Configure(&config->audio);
	TieProfile_SetFrontend(config->frontend_profile);
	TieProfile_SetFlight(&config->flight_profile);

	TieSimClock_Init();
	TieMusicPolicy_ResetClock();
	TieFlightScreen_Reset();
	s_settings_menu_requested = false;
	s_frontend_task_waiting = false;
	s_frontend_present_pending = false;
	s_frontend_present_deadline_us = 0;
	s_frontend_deferred_us = 0;
	g_quitRequested = 0;
	/* The recovered Win32 global is only an opaque platform token. */
	g_flightWindowHandle = (void*)(uintptr_t)1;
	if (TieStorage_HasInstallation(TIE_GAME_VERSION_TIE98) && !TieDirectSound_Init(g_flightWindowHandle)) {
		if (error && error_capacity)
			snprintf(error, error_capacity, "TIE98 DirectSound initialization failed");
		TieRuntime_Shutdown();
		return false;
	}

	const bool initialize_tie98_display = TieProfile_UsesDx5() || TieFlightAssets_Tie98Available();
	uint16_t initial_mode =
		TieProfile_UsesTie98Frontend() ? TieProfile_Frontend()->vesa_mode
		: config->flight_profile.version == TIE_GAME_VERSION_TIE98 &&
				config->flight_profile.tie98_original_renderer == TIE98_ORIGINAL_RENDERER_D3D
			? TIE98_DISPLAY_MODE_HARDWARE_FLIGHT
			: TIE98_DISPLAY_MODE_SOFTWARE_FLIGHT;
	if (initialize_tie98_display && !Tie98Display_Startup(initial_mode)) {
		TieDiagnostics_Log(TIE_LOG_ERROR, "TIE98 display initialization failed\n");
		TieRuntime_Shutdown();
		return false;
	}
	s_tie98_display_initialized = initialize_tie98_display;
	if (!TieLandruAdapter_Init()) {
		if (s_tie98_display_initialized)
			Tie98Display_Shutdown();
		s_tie98_display_initialized = false;
		TieRuntime_Shutdown();
		return false;
	}

	musicenabled = 1;
	sfxenabled = 1;
	voiceenabled = 1;
	g_playerEngineSoundUpdateEnabled = TieProfile_Flight()->player_engine_sound_enabled ? 1 : 0;
	strcpy(resourcedir, "RESOURCE/");
	shipext_unlock_all = config->unlock_all;
	create_player_craft = (uint8_t)config->player_craft;
	create_player_hyperspace_in = config->hyperspace_in ? 1u : 0u;

	if (config->skip_to_mission) {
		/* The shell loads the ship tables while opening, so the pilot's mission
		 * can only be selected once it has been started on the flight scene. */
		/* The launch film goes on into the flight (shellext transition_check). */
		TieShell_Begin(config->launch_film && !config->test_missions ? 270 : SCENE_FLIGHT_BATTLE, 1);
		if (config->skip_pilot && shipext_Load_Pilot(config->skip_pilot)) {
			if (config->skip_battle > 0 && config->skip_battle <= NUM_BATTLES && config->skip_mission > 0) {
				const int battle = config->skip_battle - 1;
				pilot_record.cur_battle = (uint8_t)battle;
				pilot_record.battle_status[battle] = 1;
				pilot_record.battle_cursor[battle] = (uint8_t)(config->skip_mission - 1);
			}
			TieRuntime_SetTestMissions(config->test_missions);
			if (s_test_mission_count) {
				TieRuntime_SelectTestMission();
			} else {
				shipext_Set_Tour_Battle();
				/* The launch film and loadout follow --player-craft's craft:
				 * its SHIPn.LFD (fighter, interceptor, bomber, advanced,
				 * gunboat, defender). */
				static const struct {
					int craft;
					int16_t ship;
				} ships[] = { { 5, 0 }, { 6, 1 }, { 7, 2 }, { 8, 3 }, { 16, 4 }, { 9, 5 } };
				for (size_t i = 0; i < sizeof ships / sizeof ships[0]; ++i)
					if (config->player_craft == ships[i].craft)
						shipext_Set_Mission_Ship(ships[i].ship);
				/* The flight reloads the pilot and names the mission's voice bank
				 * (fsfx_loadvoicelfd) after its record's combat chamber choice;
				 * name it after this mission, as the combat chamber does for a
				 * tour mission (12 + battle). */
				fsfx_skip_voice_id_a = (int16_t)(12 + pilot_record.cur_battle);
				fsfx_skip_voice_id_b = pilot_record.battle_cursor[pilot_record.cur_battle];
			}
			TieDiagnostics_Log(TIE_LOG_INFO, "[SKIP] pilot %s, mission %s\n", config->skip_pilot,
							   shipext_Get_Mission_Name());
		} else {
			TieDiagnostics_Log(TIE_LOG_ERROR, "[SKIP] could not load pilot %s\n",
							   config->skip_pilot ? config->skip_pilot : "(none)");
			TieRuntime_Shutdown();
			return false;
		}
		return true;
	}
	TieShell_Begin(6, 1);
	return true;
}

void TieRuntime_Shutdown(void) {
	/* The application can close while the flight task is still active. Flush
	 * diagnostics while mission state and storage are still available. */
	TIE_FLIGHT_TRACE_SHUTDOWN();
	landru_task_clear_all();
	TieFlightScreen_Reset();
	FrontendWaveStream_Shutdown();
	TieTftcMusic_Shutdown();
	TieDirectSound_Shutdown();
	TieLandruAdapter_Shutdown();
	if (s_tie98_display_initialized)
		Tie98Display_Shutdown();
	s_tie98_display_initialized = false;
	TieAudio_Configure(NULL);
	TieFlightAssets_Shutdown();
	TieInflightOptions_Reset();
	TieStorage_Shutdown();
	s_quit_requested = false;
	s_flight_resource_release_requested = false;
	s_settings_menu_requested = false;
	s_frontend_task_waiting = false;
	s_frontend_present_pending = false;
	s_frontend_present_deadline_us = 0;
	s_frontend_deferred_us = 0;
	g_quitRequested = 0;
	g_flightWindowHandle = NULL;
	TieClassicDisplay_Reset();
}

bool TieRuntime_IsActive(void) { return !landru_task_stack_empty(); }

void TieRuntime_RequestExit(void) {
	s_quit_requested = true;
	g_quitRequested = 1;
	xerror_Set_Landru_Exit(0);
}

bool TieRuntime_ShouldExit(void) { return s_quit_requested; }

void TieRuntime_RequestSettingsMenu(void) { s_settings_menu_requested = true; }

bool TieRuntime_ConsumeSettingsMenuRequest(void) {
	const bool requested = s_settings_menu_requested;
	s_settings_menu_requested = false;
	return requested;
}

bool TieRuntime_SetMusicDuckingVolumePercent(int percent) {
	if (!TieImuseSession_SetMusicDuckingVolumePercent(percent))
		return false;
	return TieAudio_SetMusicDuckingVolumePercent(percent);
}

/* SDL focus events replace the recovered WM_ACTIVATEAPP delivery. */
void TieRuntime_SetWindowActive(bool active) {
	if (!TieProfile_UsesDx5())
		return;
	const int new_state = active ? 1 : 0;
	if (g_windowActive == new_state)
		return;
	g_windowActive = new_state;
	if (new_state) {
		g_windowReactivated = 1;
		if (g_frontendDisplayWndProcMode == 1)
			FrontendDisplay_RestoreBackBuffer();
	} else if (g_frontendDisplayWndProcMode == 1) {
		FrontendDisplay_SaveBackBuffer();
	}
}

static int32_t TieRuntime_ClampHostDeltaUs(int32_t delta_us) {
	if (delta_us <= 0)
		return 0;
	return delta_us > TIE_RUNTIME_MAX_ADVANCE_US ? TIE_RUNTIME_MAX_ADVANCE_US : delta_us;
}

static void TieRuntime_AdvanceEngineTime(uint64_t delta_us) {
	/* Deferred frontend time may span more than one catch-up quantum. Split it
	 * so every clock receives the same accepted duration. */
	while (delta_us > 0) {
		const int32_t step_us =
			(int32_t)(delta_us > TIE_RUNTIME_MAX_ADVANCE_US ? TIE_RUNTIME_MAX_ADVANCE_US : delta_us);
		TieSimClock_Advance(step_us);
		TieMusicPolicy_AdvanceTime(step_us);

		TieImuseSession_Advance(step_us);
		TiePaletteCycle_Tick();
		delta_us -= (uint32_t)step_us;
	}
}

/* The original cannot continue its view loop until the completed frame has
 * crossed the display boundary. TIE98 explicitly waits in its page flip;
 * TIE95 spends the corresponding interval transferring the frame to VGA.
 * Keep this on the VGA cadence used by both frontend timing profiles. */
static uint64_t TieRuntime_FrontendPresentDelayUs(void) {
	if (!s_frontend_present_pending)
		return UINT64_MAX;

	/* A palette update waits for its retrace before the subsequent frame
	 * transfer. Preserve that ordering instead of overlapping both waits. */
	uint64_t palette_delay_us = xpal_Next_VGA_Delay_Us();
	if (palette_delay_us != UINT64_MAX)
		return palette_delay_us;

	uint64_t now_us = TieSimClock_NowUs();
	if (!s_frontend_present_deadline_us)
		s_frontend_present_deadline_us = now_us + TIE_RUNTIME_FRONTEND_PRESENT_US;
	return s_frontend_present_deadline_us > now_us ? s_frontend_present_deadline_us - now_us : 0;
}

static void TieRuntime_ClearFrontendTiming(void) {
	s_frontend_task_waiting = false;
	s_frontend_present_pending = false;
	s_frontend_present_deadline_us = 0;
	s_frontend_deferred_us = 0;
}

/* Run a yielded frontend task at its synthetic deadline rather than at the
 * next host presentation slot. CONTINUE and DONE remain immediate, matching
 * the original synchronous call chain, while palette writes can insert their
 * mandatory VGA-retrace wait between task phases. */
static void TieRuntime_RunFrontendTasks(int32_t delta_us) {
	uint64_t remaining_us = s_frontend_deferred_us + (uint32_t)delta_us;
	bool runnable = !s_frontend_task_waiting;
	int budget = 64;

	s_frontend_deferred_us = 0;
	if (runnable) {
		TieRuntime_AdvanceEngineTime(remaining_us);
		remaining_us = 0;
	}

	while (budget-- > 0 && !landru_task_stack_empty()) {
		if (!runnable) {
			uint64_t delay_us = s_frontend_present_pending ? TieRuntime_FrontendPresentDelayUs()
														   : landru_task_next_wake_delay_us();
			if (delay_us == UINT64_MAX) {
				/* An untimed YIELD only defers work to the next host tick. Its
				 * elapsed interval has now arrived, so resume at the interval's
				 * end just as the unsplit runtime loop did. */
				TieRuntime_AdvanceEngineTime(remaining_us);
				remaining_us = 0;
				runnable = true;
				continue;
			}
			if (delay_us > remaining_us) {
				TieRuntime_AdvanceEngineTime(remaining_us);
				/* View and dialog waits historically serviced fast input on each
				 * host invocation even when their frame budget had not elapsed. */
				landru_task_service_wait();
				return;
			}
			TieRuntime_AdvanceEngineTime(delay_us);
			remaining_us -= delay_us;
			if (s_frontend_present_pending) {
				/* Completing a preceding palette wait starts the presentation
				 * deadline; completing that deadline makes the task runnable. */
				if (TieRuntime_FrontendPresentDelayUs() != 0)
					continue;
				s_frontend_present_pending = false;
				s_frontend_present_deadline_us = 0;
			}
			runnable = true;
		}

		/* Advancing time can itself update a cycling palette. Its retrace
		 * wait takes precedence over the task that was otherwise due. */
		if (xpal_Next_VGA_Delay_Us() != UINT64_MAX) {
			s_frontend_task_waiting = true;
			runnable = false;
			continue;
		}

		LandruTaskStepResult result = landru_task_step_once();
		if (!TieClassicDisplay_FrontendActive()) {
			TieRuntime_ClearFrontendTiming();
			TieRuntime_AdvanceEngineTime(remaining_us);
			landru_task_run_frame();
			return;
		}
		if (result == LANDRU_TASK_STEP_FRAME_COMPLETE) {
			s_frontend_task_waiting = true;
			s_frontend_present_pending = true;
			s_frontend_present_deadline_us = 0;
			s_frontend_deferred_us = remaining_us;
			return;
		}
		if (result == LANDRU_TASK_STEP_YIELD) {
			s_frontend_task_waiting = true;
			/* Timed yields remain on the synthetic timeline. Only input waits
			 * and other untimed yields require another host invocation. */
			if (landru_task_next_wake_delay_us() != UINT64_MAX) {
				runnable = false;
				continue;
			}
			TieRuntime_AdvanceEngineTime(remaining_us);
			return;
		}

		s_frontend_task_waiting = false;
		if (xpal_Next_VGA_Delay_Us() != UINT64_MAX) {
			s_frontend_task_waiting = true;
			runnable = false;
		}
	}

	TieRuntime_AdvanceEngineTime(remaining_us);
}

void TieRuntime_Tick(int32_t delta_us) {
	delta_us = TieRuntime_ClampHostDeltaUs(TieReplayTiming_SelectEngineDeltaUs(delta_us));

	/* Imperative draw hooks require a writable snapshot before tasks run. */
	TieSnapshotBuilder_BeginTick();
	if (TieClassicDisplay_FrontendActive()) {
		TieRuntime_RunFrontendTasks(delta_us);
	} else {
		TieRuntime_ClearFrontendTiming();
		TieRuntime_AdvanceEngineTime(delta_us);
		landru_task_run_frame();
	}

	const TieSceneKind settled_scene = TieSnapshotBuilder_GetSceneKind();
	const bool emit_flight = settled_scene == TIE_SCENE_FLIGHT_LOADING || settled_scene == TIE_SCENE_FLIGHT;
	if (emit_flight)
		TieFlightSnapshot_Capture();

	if (TieClassicDisplay_FrontendActive()) {
		LandruVideoTarget target;
		if (xsurface_Get_Active_Video_Target(&target)) {
			TieSnapshotBuilder_SetClassicDims((uint16_t)target.width, (uint16_t)target.height);
			TieSnapshotBuilder_SetLandruPresentation((uint16_t)target.width, (uint16_t)target.height,
													 target.width == 320 ? TIE_SOURCE_PIXEL_ASPECT_VGA_4_3
																		 : TIE_SOURCE_PIXEL_ASPECT_SQUARE,
													 (uint8_t)TieProfile_FrontendId(), target.generation);
		}
	}
	if (emit_flight) {
		TieSnapshotBuilder_SetFlightScreen(TieFlightScreen_Active());
		TieHudSnapshot_Capture();
	}
	TieLandruAdapter_EmitRenderState();
	TieFrontendSnapshot_CaptureText();
	TieFrontendSnapshot_CaptureTitle();
	TieFrontendSnapshot_CaptureLogo();
	TiePaletteSnapshot_Capture();
	TieSnapshotBuilder_FinalizeTick();
}

uint64_t TieRuntime_NextWakeDelayUs(void) {
	return s_frontend_present_pending ? TieRuntime_FrontendPresentDelayUs()
									  : landru_task_next_wake_delay_us();
}

void TieRuntime_RequestFlightResourceRelease(void) { s_flight_resource_release_requested = true; }

bool TieRuntime_FlightResourceReleaseRequested(void) { return s_flight_resource_release_requested; }

void TieRuntime_CompleteFlightResourceRelease(void) {
	if (!s_flight_resource_release_requested)
		return;
	TieFlightAssets_ClearRuntimeCaches();
	s_flight_resource_release_requested = false;
}
