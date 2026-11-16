#ifndef TIE_RUNTIME_RUNTIME_H
#define TIE_RUNTIME_RUNTIME_H

#include "tie_runtime/audio/imuse_session.h"
#include "tie_runtime/flight_assets/service.h"
#include "tie_runtime/runtime/profile_types.h"
#include "tie_runtime/storage/storage.h"

typedef struct TieRuntimeConfig {
	TieStorageConfig storage;
	TieFlightAssetsConfig flight_assets;
	TieFrontendProfileId frontend_profile;
	TieFlightProfile flight_profile;
	TieAudioConfig audio;
	/* Developer shortcut: load this pilot and start its current tour battle mission. */
	bool skip_to_mission;
	const char* skip_pilot;
	/* Optional 1-based tour battle and mission to start instead; 0 keeps the pilot's. */
	int skip_battle;
	int skip_mission;
	/* Comma-separated mission files ("B6M1AW,HA1W") flown in turn after
	 * skip_pilot loads, without the screens between them (testing). */
	const char* test_missions;
	/* Developer shortcut: every pilot loaded has all tour battles completed,
	 * so the combat chamber offers every tour mission. */
	bool unlock_all;
} TieRuntimeConfig;

bool TieRuntime_Init(const TieRuntimeConfig* config, char* error, size_t error_capacity);
void TieRuntime_Shutdown(void);
bool TieRuntime_IsActive(void);

/* Requests cooperative shutdown of both frontend and flight tasks. */
void TieRuntime_RequestExit(void);
bool TieRuntime_ShouldExit(void);
void TieRuntime_SetWindowActive(bool active);

/* One-shot request from recovered UI tasks to the application overlay. */
void TieRuntime_RequestSettingsMenu(void);
bool TieRuntime_ConsumeSettingsMenuRequest(void);
bool TieRuntime_SetMusicDuckingVolumePercent(int percent);

/* The --missions test sequence: the mission being flown (NULL outside one),
 * whether another follows, ending the current one, and moving to the next. */
const char* TieRuntime_TestMission(void);
bool TieRuntime_TestMissionHasNext(void);
void TieRuntime_EndTestMission(void);
bool TieRuntime_TestMissionAdvance(void);

/* Advances one host-visible tick using the supplied synthetic-clock delta. */
void TieRuntime_Tick(int32_t delta_us);

#ifdef TIE_MODERN
/* UINT64_MAX means no task deadline precedes the presentation cadence. */
uint64_t TieRuntime_NextWakeDelayUs(void);
#endif
void TieRuntime_RequestFlightResourceRelease(void);
bool TieRuntime_FlightResourceReleaseRequested(void);
void TieRuntime_CompleteFlightResourceRelease(void);

#endif
