#ifndef TIE_REMASTER_FLIGHT_COCKPIT_DC_LAYOUT_H
#define TIE_REMASTER_FLIGHT_COCKPIT_DC_LAYOUT_H

/* TFTC Dynamic Cockpit layout: where an XWA cockpit OPT shows each HUD
 * display. A DynamicCockpit/<Cockpit>.dc section names a texture of the
 * cockpit and the rectangle of it, in that texture's pixels, that shows a
 * HUD source. The TIE HUD instruments matching the source are drawn through
 * every face using the texture, so they land where XWA would draw them.
 * Parsing follows OpenXWA's importer (aeron
 * tools/opt2gltf/xwau_dynamic_cockpit.c), including that
 * cover_texture_size carries over to later uv_coords lines. */

#include <stdbool.h>

#include "opt.h"

typedef enum TieDcSource {
	TIE_DC_NONE = -1,
	TIE_DC_TARGETING,     /* TARGETING_COMPUTER_SRC: targeting computer and target readouts */
	TIE_DC_LEFT_TEXT,     /* LEFT_TEXT_BOX_SRC: the repairs panel */
	TIE_DC_RIGHT_TEXT,    /* RIGHT_TEXT_BOX_SRC: the objectives panel */
	TIE_DC_LEFT_SENSOR,   /* LEFT_SENSOR_SRC: front radar */
	TIE_DC_RIGHT_SENSOR,  /* RIGHT_SENSOR_SRC: rear radar */
	TIE_DC_SHIELDS,       /* SHIELDS_SRC: shields and hull */
	TIE_DC_LASERS,        /* DUAL_LASERS_*_SRC: laser charge */
	TIE_DC_MISSILES,      /* MISSILES_SRC, AUTOSIZE_MISSILES_*_SRC: warheads */
	TIE_DC_SPEED,         /* SPEED_THROTTLE_SRC, AUTOSIZE_SPEED_SRC: speed and throttle */
	TIE_DC_BEAM,          /* BEAM_LEVEL_SRC: beam weapon */
	TIE_DC_LASER_POWER,   /* LASER_RECHARGE_SRC */
	TIE_DC_SHIELD_POWER,  /* SHIELD_RECHARGE_SRC */
	TIE_DC_BEAM_POWER,    /* BEAM_RECHARGE_SRC */
	TIE_DC_ENGINE_POWER,  /* ENGINE_POWER_SRC */
	TIE_DC_CLOCK,         /* NAME_TIME_SRC, AUTOSIZE_TIME_SRC: mission clock */
	TIE_DC_THROTTLE,      /* THROTTLE_BAR_SRC: throttle setting */
	TIE_DC_SHIELD_FRONT_TEXT, /* SHIELDS_FRONT_SRC: front shield percentage */
	TIE_DC_SHIELD_BACK_TEXT,  /* SHIELDS_BACK_SRC: rear shield percentage */
	TIE_DC_SHIELD_BARS,       /* TIE's shield LED bars: XWA shows a ring instead, so never on a screen */
	TIE_DC_CHAFF,             /* AUTOSIZE_CHAFF_SRC: countermeasures, which TIE craft never carry */
	TIE_DC_SOURCE_COUNT,
} TieDcSource;

/* One triangle of a face showing a display: its corners in cockpit model
 * space (metres) and where each falls in the display, 0..1 from its top-left
 * (outside 0..1 the face shows none of it). Drawing the display through every
 * triangle paints it on the cockpit like XWA's Dynamic Cockpit, bent strips
 * included. */
typedef struct TieDcTriangle {
	float position[3][3];
	float display[3][2];
} TieDcTriangle;

typedef struct TieDcScreen {
	TieDcSource source;
	TieDcTriangle* triangles;
	int triangle_count;
	float aspect; /* width / height of the display on the model */
} TieDcScreen;

#define TIE_DC_MAX_SCREENS 32

typedef struct TieDcLayout {
	/* Every screen of the file, in order; a source can show on several (XWA's
	 * left and right laser bars). */
	TieDcScreen screens[TIE_DC_MAX_SCREENS];
	int count;
	float fov_deg; /* xwahacker_fov, 0 when absent */
} TieDcLayout;

/* Builds the layout of `opt` from the .dc text. False when no screen of the
 * file is found on the model. */
bool TieDcLayout_Build(const char* text, const opt_file_t* opt, TieDcLayout* out);
void TieDcLayout_Free(TieDcLayout* layout);

/* The DC source a TIE HUD instrument (or cockpit editor panel id) belongs to. */
TieDcSource TieDcLayout_SourceOf(int instrument_id);

#endif
