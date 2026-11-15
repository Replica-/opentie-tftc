#ifndef TIE_RUNTIME_AUDIO_TFTC_MUSIC_H
#define TIE_RUNTIME_AUDIO_TFTC_MUSIC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TFTC's digital soundtrack. TIE's own adaptive music logic picks the state;
 * each state plays its FLAC recording from resources/tftc/music/, entered
 * through the recorded bridge (resources/tftc/signals/) that TIE's MIDI
 * script plays between those states. TIE's music sequences play their
 * recorded stings from resources/tftc/signals/ over the state music. */

/* Always true: the recordings need no engine. */
bool TieTftcMusic_Start(void);
void TieTftcMusic_Shutdown(void);
/* `tie_state` is the state tie_updatemusic hands the MIDI music script
 * (1..11). */
void TieTftcMusic_SetTieState(int tie_state);
void TieTftcMusic_SetFailed(void);
/* Plays the recorded sting for TIE music sequence `tie_sequence`
 * (fscript_MsSetSequence ids: arrivals, kills, losses, goals, eject, hyperspace). */
void TieTftcMusic_PlayTieSequence(int tie_sequence);
void TieTftcMusic_Stop(void);
/* `volume` 0..16, TIE's in-flight music volume. */
void TieTftcMusic_SetVolume(int volume);
void TieTftcMusic_Update(void);
/* The FLAC file of the current state, or NULL when none plays. */
const char* TieTftcMusic_CurrentTune(void);

/* Front end music: plays `file`, relative to resources/tftc/, looping a room's,
 * crossfading from the room before; NULL fades it out. */
void TieTftcMusic_PlayRoom(const char* file, bool loop);
void TieTftcMusic_PauseRoom(bool paused);
/* The room's FLAC file, or NULL when none plays. */
const char* TieTftcMusic_CurrentRoom(void);
/* The stings sounding in flight, joined by " + ": the bridge into the
 * current track and a sting. NULL when none plays. */
const char* TieTftcMusic_CurrentStings(void);

#ifdef __cplusplus
}
#endif

#endif
