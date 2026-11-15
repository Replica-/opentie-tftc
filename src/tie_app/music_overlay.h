#ifndef TIE_APP_MUSIC_OVERLAY_H
#define TIE_APP_MUSIC_OVERLAY_H

#include <stdbool.h>

#include "aeron/input.h"
#include "aeron/scene/font_atlas.h"

/* A line in the top-left corner naming the music source and, in flight,
 * the tune or music state playing. It has its own UI context, whose input
 * capture is ignored, so it never takes input from the game. */
bool TieMusicOverlay_Init(const AeronFontAtlas* font);
void TieMusicOverlay_Shutdown(void);
void TieMusicOverlay_Frame(const AeronInputSnapshot* input, float dt_seconds);

#endif
