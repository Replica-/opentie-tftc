#ifndef TIE_REMASTER_FLIGHT_TFTC_SKYBOX_H
#define TIE_REMASTER_FLIGHT_TFTC_SKYBOX_H

#include "aeron/render.h"

struct TieFlightRenderer;

/* TFTC's per-mission skybox: resources/tftc/skyboxes/b<B>m<M>.ini, the
 * mission's [SkyBoxes] section, names a cube in skyboxes/cubemaps/<Name>/ whose
 * six faces <Name>.0..5.jpg (or .png) are +X, -X, +Y, -Y, +Z, -Z. Loads the
 * cube for `mission_name` ("b1m3fm") when the mission changes; with no skybox
 * for the mission it releases the previous one. */
void TieFlightTftcSkybox_Refresh(struct TieFlightRenderer* g, AeronCommandBuffer* cmd, const char* mission_name);
void TieFlightTftcSkybox_Release(struct TieFlightRenderer* g);

#endif
