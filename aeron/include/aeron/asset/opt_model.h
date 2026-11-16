#ifndef AERON_ASSET_OPT_MODEL_H
#define AERON_ASSET_OPT_MODEL_H

#include <stdbool.h>
#include <stddef.h>

#include "aeron/asset/flight_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native LucasArts OPT coordinates use 65536 units for 1600 metres. */
#define AERON_OPT_METERS_PER_UNIT (1600.0f / 65536.0f)
#define AERON_OPT_UNITS_PER_METER (65536.0f / 1600.0f)

typedef struct AeronOptModelBuildOptions {
	float                               smooth_angle_degrees;
	float                               emissive_strength;
	bool                                emissive;
	const struct AeronOptAlphaOverride* alpha_overrides;
	size_t                              alpha_override_count;
	/* PBR values by texture name; an entry with a NULL name applies to
	 * every texture without its own entry. */
	const struct AeronOptMaterialOverride* material_overrides;
	size_t                                 material_override_count;
	int                                 max_atlas_size; /* 0 = cook default */
	/* Builds only meshes [mesh_first, mesh_first + mesh_count); 0 = all. */
	int                                 mesh_first;
	int                                 mesh_count;
	/* Mesh indices (into the whole OPT) left out of the model, as an XWA
	 * object profile hides them. */
	const int*                          mesh_skip;
	size_t                              mesh_skip_count;
	/* Multiplies every material's base color (not its emission); 0 = 1. */
	float                               base_color_scale;
} AeronOptModelBuildOptions;

typedef struct AeronOptAlphaOverride {
	const char*        texture_name;
	AeronGltfAlphaMode alpha_mode;
	float              alpha_cutoff;
} AeronOptAlphaOverride;

typedef struct AeronOptMaterialOverride {
	const char* texture_name;
	float       metallic;  /* 0..1 */
	float       roughness; /* 0..1 */
	bool        shadeless; /* drawn at full brightness, unaffected by lighting */
} AeronOptMaterialOverride;

typedef struct AeronOptModelError {
	int  code;
	char message[256];
} AeronOptModelError;

bool Aeron_OptModelBuildMemory(const void* bytes, size_t size, const char* label,
							   const AeronOptModelBuildOptions* options, AeronFlightModel* out,
							   AeronOptModelError* error);

#ifdef __cplusplus
}
#endif

#endif
