#ifndef TIE_REMASTER_FLIGHT_TFTC_SHIPS_H
#define TIE_REMASTER_FLIGHT_TFTC_SHIPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/asset/opt_model.h"
#include "aeron/render.h"
#include "aeron/scene/mesh.h"
#include "aeron/vfs.h"

struct TieFlightRenderer;

/* Material values of an XWA effects .mat file, ready for
 * AeronOptModelBuildOptions. Overrides name strings live in `storage`. */
typedef struct TieFlightTftcMaterialSet {
	AeronOptMaterialOverride* overrides;
	size_t count;
	void* storage;
} TieFlightTftcMaterialSet;

/* Reads `mat_path` from the resource root; without the file every texture
 * gets the XWA effects defaults. False only when out of memory. */
bool TieFlightTftc_LoadMaterials(AeronVfs* vfs, const char* mat_path, TieFlightTftcMaterialSet* out);
void TieFlightTftc_FreeMaterials(TieFlightTftcMaterialSet* set);

/* Builds the TFTC replacement for a species' render mesh from
 * resources/tftc/<NAME>.opt and its optional <NAME>.mat, where NAME is the
 * basename of the species' TIE98 OPT (TIEFTR for IVFILES/TIEFTR.OPT). NULL
 * when there is no replacement or it cannot be built; the original model is
 * then drawn. */
AeronSceneMesh* TieFlightTftcShips_Load(struct TieFlightRenderer* g, AeronCommandBuffer* cmd,
										uint16_t species_idx);

/* Engine glows of a TFTC model, from its OPT's EngineGlow nodes, drawn the
 * way OpenXWA draws them (xwa_remaster/ship.c XwaRemasterShip_SubmitEngineGlows):
 * a rect or diamond per glow across its right and up axes, extruded along its
 * look axis, its core colour at the centre and its outer colour at the rim.
 * `transform` is the instance's model to camera-local matrix and `cam_pos` the
 * eye in that space. The glow texture lives until the mission's release. */
void TieFlightTftcShips_EnsureGlowTexture(struct TieFlightRenderer* g, AeronCommandBuffer* cmd);
/* XWA's glow scale for a craft at `current_speed` (0..3600). */
float TieFlightTftcShips_EngineGlowScale(int current_speed);
void TieFlightTftcShips_SubmitEngineGlows(struct TieFlightRenderer* g, const AeronSceneMesh* mesh,
										  const float transform[16], const float cam_pos[3], float scale);

#endif
