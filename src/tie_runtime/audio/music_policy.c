#include "tie_runtime/audio/music_policy.h"
#include "tie_runtime/audio/imuse_session.h"
#include "tie_runtime/audio/tftc_music.h"

#include "tie/tie.h"

#include <stdio.h>

static uint64_t music_clock_us;
static bool music_flight_active;

TieMusicSource TieMusicPolicy_Source(void) { return TieAudio_Config()->music_source; }

bool TieMusicPolicy_UsesImuse(void) { return TieMusicPolicy_Source() == TIE_MUSIC_IMUSE; }
bool TieMusicPolicy_UsesTie98(void) { return TieMusicPolicy_Source() == TIE_MUSIC_TIE98; }
bool TieMusicPolicy_UsesTftc(void) { return TieMusicPolicy_Source() == TIE_MUSIC_TFTC; }
void TieMusicPolicy_ResetClock(void) { music_clock_us = 0; }

void TieMusicPolicy_AdvanceTime(int32_t delta_us) {
	if (delta_us > 0)
		music_clock_us += (uint32_t)delta_us;
}

uint32_t TieMusicPolicy_NowMs(void) { return (uint32_t)(music_clock_us / 1000u); }

void TieMusicPolicy_UpdateFlightMusic(void) {
	if (TieMusicPolicy_UsesTie98())
		tie_updatemusic_tie98();
	else
		tie_updatemusic();
}

void TieMusicPolicy_SetFlightActive(bool active) { music_flight_active = active; }

static const char* TieMusicPolicy_SynthName(void) {
	switch (TieAudio_Config()->midi_backend.kind) {
		case TIE_MIDI_BACKEND_FLUIDSYNTH:
			return "FluidSynth";
		case TIE_MIDI_BACKEND_FM4_OPL3:
			return "OPL3";
		case TIE_MIDI_BACKEND_SC55:
			return "SC-55";
		default:
			return "no synthesizer";
	}
}

/* tie_updatemusic's states; see TieTftcMusic_SetTieState. */
static const char* TieMusicPolicy_StateName(int state) {
	static const char* const names[] = {
		"none",	   "intro",	 "waiting",	"rebellion", "police",	"intrigue",
		"challenge", "confident", "panic", "climax",	   "failure", "success",
	};
	return state >= 0 && state < (int)(sizeof names / sizeof names[0]) ? names[state] : "unknown";
}

void TieMusicPolicy_Describe(char* out, size_t capacity) {
	if (!out || !capacity)
		return;
	const char* off = musicenabled ? "" : " (music off)";
	switch (TieMusicPolicy_Source()) {
		case TIE_MUSIC_TIE98: {
			static const char* const tracks[] = { "", "", "battle", "success", "failure" };
			const int track = tie_cdmusictrack();
			if (music_flight_active && track >= 2 && track <= 4)
				snprintf(out, capacity, "Music: CD Music - Track%02d.ogg (%s)%s", track, tracks[track], off);
			else
				snprintf(out, capacity, "Music: CD Music%s", off);
			break;
		}
		case TIE_MUSIC_TFTC: {
			const char* tune = music_flight_active ? TieTftcMusic_CurrentTune() : TieTftcMusic_CurrentRoom();
			const char* stings = music_flight_active ? TieTftcMusic_CurrentStings() : NULL;
			if (tune && stings)
				snprintf(out, capacity, "Music: TFTC - %s | sting: %s%s", tune, stings, off);
			else if (tune)
				snprintf(out, capacity, "Music: TFTC - %s%s", tune, off);
			else
				snprintf(out, capacity, "Music: TFTC%s", off);
			break;
		}
		default:
			if (music_flight_active && lastmusicstate)
				snprintf(out, capacity, "Music: iMUSE MIDI (%s) - state %u (%s)%s", TieMusicPolicy_SynthName(),
						 (unsigned)lastmusicstate, TieMusicPolicy_StateName(lastmusicstate), off);
			else
				snprintf(out, capacity, "Music: iMUSE MIDI (%s)%s", TieMusicPolicy_SynthName(), off);
			break;
	}
}
