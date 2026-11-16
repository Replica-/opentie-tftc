#ifndef TIE_REMASTER_FLIGHT_TFTC_SHIPS_H
#define TIE_REMASTER_FLIGHT_TFTC_SHIPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aeron/asset/opt_model.h"
#include "aeron/render.h"
#include "aeron/scene/mesh.h"
#include "aeron/scene/scene3d.h"
#include "aeron/vfs.h"

struct TieFlightRenderer;
struct TieFlightSpeciesSceneShip;
struct TieFlightObjectState;
struct TieSnapshot;

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

#define TIE_FLIGHT_TFTC_MAX_HIDDEN 64

/* The meshes the craft's default object profile hides. Its .ini names the
 * profile under [ObjectProfiles] ("Default = TA-Mk2") and lists each
 * profile's hidden mesh indices ("TA-Mk2 = 13, 15, 23, 25"). The same list
 * applies to the craft's OPT and its cockpit OPT: TFTC's TieAvenger holds
 * both marks of the craft in each, and drawing every mesh shows the two at
 * once. Returns the number of indices written to `out`. */
int TieFlightTftc_HiddenMeshes(AeronVfs* vfs, const char* ini_path, int* out, int capacity);

/* Builds the TFTC replacement for a species' render mesh from
 * resources/tftc/<NAME>.opt and its optional <NAME>.mat, where NAME is the
 * basename of the species' TIE98 OPT (TIEFTR for IVFILES/TIEFTR.OPT). NULL
 * when there is no replacement or it cannot be built; the original model is
 * then drawn. */
AeronSceneMesh* TieFlightTftcShips_Load(struct TieFlightRenderer* g, AeronCommandBuffer* cmd,
										uint16_t species_idx);

/* Pairs each rotary gun turret of the species' TFTC model with the original
 * model's turret nearest its pivot, on the same face of the hull where one
 * exists. The pairing is cosmetic: the TFTC turret takes its pair's aim. */
void TieFlightTftcShips_PairTurrets(struct TieFlightSpeciesSceneShip* s);
/* True when the craft's TFTC model has turrets to turn this frame. */
bool TieFlightTftcShips_NeedsMeshTable(const struct TieFlightSpeciesSceneShip* s,
									   const struct TieFlightObjectState* fl, const struct TieSnapshot* curr);
/* Mesh table for the TFTC model: each paired turret turned about its own
 * pivot to face where its original turret faces, every other mesh at rest. */
void TieFlightTftcShips_BuildMeshTable(const struct TieFlightSpeciesSceneShip* s,
									   const struct TieFlightObjectState* fl, const struct TieSnapshot* curr,
									   AeronSceneMeshTable* out);

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
