#ifndef TIE_APP_APPLICATION_H
#define TIE_APP_APPLICATION_H

#include <stdbool.h>

typedef struct TieLaunchOptions {
	const char* tie95_data;
	const char* tie98_data;
	const char* resource_root;
	bool headless; /* hidden window, always focused */
	const char* capture_path; /* save the flight view to this PNG, then quit */
	float capture_after_seconds;
	bool skip_to_mission;
	const char* skip_pilot; /* pilot file name without .tfr */
	int skip_battle;        /* 1-based tour battle, or 0 for the pilot's own */
	int skip_mission;       /* 1-based mission in that battle */
	const char* missions;   /* "B6M1AW,HA1W": fly these mission files in turn (testing) */
	const char* music;      /* "imuse", "tie98" or "tftc": music source for this run only; NULL = config */
} TieLaunchOptions;

int TieApplication_Run(const TieLaunchOptions* launch);

#endif
