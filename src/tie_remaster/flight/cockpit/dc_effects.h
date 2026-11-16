#ifndef TIE_REMASTER_FLIGHT_COCKPIT_DC_EFFECTS_H
#define TIE_REMASTER_FLIGHT_COCKPIT_DC_EFFECTS_H

/*
 * The image sequences that XWA's effects renderer plays on a TFTC cockpit's
 * textures, from its effects .mat (Materials/<Name>.mat): a frame_* line
 * replaces the texture with frames of an Effects DAT group, and a lightmap_*
 * line lights it with frames added over it. Only the sequences that always
 * play (EVT_NONE, or no event) are read. The frames are drawn over the
 * texture's faces, the way the DC screens are (dc_layout.h). A cover_seq line,
 * which the caller makes from a .dc cover_texture ("cover_seq =
 * Effects\<File>.dat, (<group>-<image>, 1, 1)"), is a cover: XWA draws it
 * over the screen's live content, which shows through its transparent parts.
 */

#include <stdbool.h>

#include "aeron/render.h"
#include "aeron/vfs.h"
#include "opt.h"
#include "tie_remaster/flight/cockpit/dc_layout.h"

typedef struct TieDcEffectFrame {
	AeronTexture* texture; /* premultiplied, owned by the effects' texture list */
	float seconds;
} TieDcEffectFrame;

typedef struct TieDcEffect {
	TieDcScreen faces; /* the texture's faces; display = texture coordinates */
	bool glow;         /* lightmap: added over the face rather than covering it */
	bool over;         /* a DC cover_texture: drawn over the DC screens */
	bool random;       /* *_rand: a random frame each time one ends */
	int frame_first, frame_count;
	float seconds; /* the frames' total */
} TieDcEffect;

typedef struct TieDcEffects {
	TieDcEffect* items;
	int count;
	TieDcEffectFrame* frames;
	int frame_count;
	AeronTexture** textures;
	int texture_count;
} TieDcEffects;

/* Reads `mat_text` (changed in place) and builds the sequences of the
 * textures of `opt` it names, uploading their frames through `cmd`. A DAT
 * named "Effects\<File>.dat" is read from cockpit3d/Effects/<File>.dat.
 * Returns the number of sequences built. */
int TieDcEffects_Build(TieDcEffects* out, AeronCommandBuffer* cmd, AeronVfs* vfs, char* mat_text,
					   const opt_file_t* opt);
void TieDcEffects_Free(TieDcEffects* effects);

/* The frame of `effect` to show `seconds` after the cockpit loaded. */
AeronTexture* TieDcEffects_Frame(const TieDcEffects* effects, const TieDcEffect* effect, double seconds);

#endif
