#include "tie_app/application.h"

#include "aeron/main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int TieMain_Usage(const char* message, const char* argument) {
	if (message && argument)
		fprintf(stderr, "[tie-aeron] "), fprintf(stderr, message, argument), fputc('\n', stderr);
	else if (message)
		fprintf(stderr, "[tie-aeron] %s\n", message);
	fprintf(stderr, "Usage: tie [--tie95-data <dir>] [--tie98-data <dir>] [--resource-root <dir>] [--skip-to-mission <pilot> [battle mission]] [--missions <file,file...>] [--headless] [--capture <file.png> [seconds]] [--press <keys>] [--music imuse|tie98|tftc] [--unlock-all] [--player-craft <craft>] [--hyperspace-in] [--launch-film]\n");
	return 2;
}

static int TieMain_ParseOptions(int argc, char* argv[], TieLaunchOptions* out) {
	static const char* const names[] = { "--tie95-data", "--tie98-data", "--resource-root" };
	const char** destinations[] = { &out->tie95_data, &out->tie98_data, &out->resource_root };
	memset(out, 0, sizeof *out);
	for (int index = 1; index < argc; ++index) {
		const char* argument = argv[index];
		if (strcmp(argument, "--headless") == 0) {
			out->headless = true;
			continue;
		}
		if (strcmp(argument, "--unlock-all") == 0) {
			/* --unlock-all: every pilot loaded this run has all tour battles completed. */
			out->unlock_all = true;
			continue;
		}
		if (strcmp(argument, "--launch-film") == 0) {
			/* --launch-film: with --skip-to-mission, the craft's launch film plays first. */
			out->launch_film = true;
			continue;
		}
		if (strcmp(argument, "--hyperspace-in") == 0) {
			/* --hyperspace-in: every mission opens with the player's hyperspace arrival. */
			out->hyperspace_in = true;
			continue;
		}
		if (strcmp(argument, "--player-craft") == 0) {
			/* --player-craft <craft>: the player flies this craft in every mission:
			 * fighter, interceptor, bomber, advanced, defender, gunboat or a mission craft type. */
			static const struct {
				const char* name;
				int type;
			} crafts[] = { { "fighter", 5 }, { "interceptor", 6 }, { "bomber", 7 },
						   { "advanced", 8 }, { "defender", 9 },	  { "gunboat", 16 } };
			const char* craft = index + 1 < argc ? argv[++index] : NULL;
			out->player_craft = 0;
			for (size_t c = 0; craft && c < sizeof crafts / sizeof crafts[0]; ++c)
				if (strcmp(craft, crafts[c].name) == 0)
					out->player_craft = crafts[c].type;
			if (craft && !out->player_craft && craft[0] >= '1' && craft[0] <= '9')
				out->player_craft = atoi(craft);
			if (out->player_craft <= 0 || out->player_craft > 88)
				return TieMain_Usage("--player-craft takes fighter, interceptor, bomber, advanced, defender, "
									 "gunboat or a craft type: %s",
									 craft ? craft : "");
			continue;
		}
		if (strcmp(argument, "--music") == 0) {
			/* --music <imuse|tie98|tftc>: music source for this run, not saved. */
			const char* source = index + 1 < argc ? argv[++index] : NULL;
			if (!source || (strcmp(source, "imuse") != 0 && strcmp(source, "tie98") != 0 &&
							strcmp(source, "tftc") != 0))
				return TieMain_Usage("--music takes imuse, tie98 or tftc: %s", source ? source : "");
			out->music = source;
			continue;
		}
		if (strcmp(argument, "--missions") == 0) {
			/* --missions <file,file...>: with --skip-to-mission, fly these missions in turn. */
			if (index + 1 >= argc || argv[index + 1][0] == '-')
				return TieMain_Usage("option requires mission names: %s", "--missions");
			out->missions = argv[++index];
			continue;
		}
		if (strcmp(argument, "--capture") == 0) {
			/* --capture <file.png> [seconds]: save the flight view after that many seconds, then quit. */
			if (index + 1 >= argc || argv[index + 1][0] == '-')
				return TieMain_Usage("option requires a file name: %s", "--capture");
			out->capture_path = argv[++index];
			out->capture_after_seconds = 6.0f;
			if (index + 1 < argc && argv[index + 1][0] >= '0' && argv[index + 1][0] <= '9')
				out->capture_after_seconds = (float)atof(argv[++index]);
			continue;
		}
		if (strcmp(argument, "--press") == 0) {
			/* --press <keys>: with --capture, type these keys into flight first (t selects a target). */
			if (index + 1 >= argc || argv[index + 1][0] == '-')
				return TieMain_Usage("option requires keys: %s", "--press");
			out->press_keys = argv[++index];
			continue;
		}
		if (strcmp(argument, "--skip-to-mission") == 0 ||
			strncmp(argument, "--skip-to-mission=", 18) == 0) {
			const char* pilot =
				argument[17] == '=' ? argument + 18 : (index + 1 < argc ? argv[++index] : NULL);
			if (out->skip_to_mission)
				return TieMain_Usage("duplicate option: %s", "--skip-to-mission");
			if (!pilot || !pilot[0])
				return TieMain_Usage("option requires a pilot name: %s", "--skip-to-mission");
			out->skip_to_mission = true;
			out->skip_pilot = pilot;
			/* Optional tour battle and mission numbers, as in B1M3. */
			if (index + 2 < argc && argv[index + 1][0] >= '1' && argv[index + 1][0] <= '9' &&
				argv[index + 2][0] >= '1' && argv[index + 2][0] <= '9') {
				out->skip_battle = atoi(argv[++index]);
				out->skip_mission = atoi(argv[++index]);
			}
			continue;
		}
		int option = -1;
		const char* value = NULL;
		for (int candidate = 0; candidate < 3; ++candidate) {
			const size_t length = strlen(names[candidate]);
			if (strcmp(argument, names[candidate]) == 0 ||
				(strncmp(argument, names[candidate], length) == 0 && argument[length] == '=')) {
				option = candidate;
				value = argument[length] == '=' ? argument + length + 1 : NULL;
				break;
			}
		}
		if (option < 0)
			return TieMain_Usage(argument[0] == '-' ? "unknown option: %s"
													: "positional argument is not accepted: %s",
								 argument);
		if (*destinations[option])
			return TieMain_Usage("duplicate option: %s", names[option]);
		if (!value && ++index < argc && strncmp(argv[index], "--", 2) != 0)
			value = argv[index];
		if (!value || !value[0])
			return TieMain_Usage("option requires a directory: %s", names[option]);
		*destinations[option] = value;
	}
	return 0;
}

int main(int argc, char* argv[]) {
	TieLaunchOptions launch;
	const int status = TieMain_ParseOptions(argc, argv, &launch);
	return status ? status : TieApplication_Run(&launch);
}
