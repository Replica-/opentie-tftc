#ifndef AERON_SCENE_MESH_COMMON_H
#define AERON_SCENE_MESH_COMMON_H

/*
 * Shared scene-mesh constants and articulation POD. AERON_MAX_MESH_SLOTS comes from
 * mesh_table_layout.hlsli, shared with the shaders. Mesh pipelines
 * share the same scene storage record, indexed by component ordinal. */

#include <stdint.h>

#include "../../../shaders/mesh_table_layout.hlsli"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-component articulation source. Indexed by model-local component ordinal
 * (0..AERON_MAX_MESH_SLOTS-1). The game-side table builder reads
 * axis/pivot/has_rotation here and composes the per-mesh affine.
 * `direction` is the component's rest-facing axis (a turret's barrels at
 * angle 0), zero when the source carries none.
 *
 * Unused slots stay zero (has_rotation == 0) — the slot still
 * contributes an identity affine to the storage record. */
typedef struct AeronMeshRot {
	float   axis[3];
	float   pivot[3];
	float   direction[3];
	uint8_t has_rotation;
	uint8_t mesh_type; /* OPT mesh-type enum (MainHull=1, Wing=2, …) */
	uint8_t _pad[2];
} AeronMeshRot;

#ifdef __cplusplus
}
#endif

#endif
