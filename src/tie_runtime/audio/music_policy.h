#ifndef TIE_MUSIC_POLICY_H
#define TIE_MUSIC_POLICY_H

#include "tie_runtime/audio/config.h"
#include "tie_runtime/diagnostics/diagnostics.h"
#include "tie_runtime/display/classic_display.h"
#include "tie_runtime/display/classic_framebuffer.h"
#include "tie_runtime/flight_assets/model_types.h"
#include "tie_runtime/input/input.h"
#include "tie_runtime/runtime/exports.h"
#include "tie_runtime/runtime/profile.h"
#include "tie_runtime/storage/storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

TieMusicSource TieMusicPolicy_Source(void);
bool TieMusicPolicy_UsesImuse(void);
bool TieMusicPolicy_UsesTie98(void);
/* TFTC's digital soundtrack, driven by TIE's adaptive music states. */
bool TieMusicPolicy_UsesTftc(void);
void TieMusicPolicy_ResetClock(void);
void TieMusicPolicy_AdvanceTime(int32_t delta_us);
uint32_t TieMusicPolicy_NowMs(void);
/* Per-frame flight music update for the selected soundtrack: adaptive iMUSE
 * (TIE_updatemusic), TIE98 digital CD music, or TFTC's FLAC soundtrack. */
void TieMusicPolicy_UpdateFlightMusic(void);
/* Set from mission start to mission end. */
void TieMusicPolicy_SetFlightActive(bool active);
/* One line naming the music source and, in flight, what it is playing. */
void TieMusicPolicy_Describe(char* out, size_t capacity);

#endif
