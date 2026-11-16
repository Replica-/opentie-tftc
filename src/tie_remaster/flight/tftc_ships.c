/*
 * TFTC ship models drawn in place of the original ones. tools/tftc/copy_tftc_models.sh
 * copies the XWA-format OPTs of a TFTC install, with their XWA effects .mat
 * files, to resources/tftc/ under the names of the TIE98 OPTs they replace.
 * Only the drawn mesh changes: collision, hardpoints and debris keep the
 * original model.
 */

#include "tie_remaster/flight/tftc_ships.h"

#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/vfs.h"
#include "tie_remaster/flight/renderer_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIE_FLIGHT_TFTC_MAX_BYTES (256u * 1024u * 1024u)

/* XWA effects defaults for keys a .mat file leaves out. */
#define TIE_FLIGHT_TFTC_DEFAULT_METALLIC 0.25f
#define TIE_FLIGHT_TFTC_DEFAULT_GLOSSINESS 0.128f

typedef struct TieFlightTftcMaterial {
	char name[64]; /* empty for [Default] */
	float metallic;
	float glossiness;
	bool shadeless;
} TieFlightTftcMaterial;

typedef struct TieFlightTftcMaterials {
	TieFlightTftcMaterial* items;
	size_t count;
	size_t capacity;
} TieFlightTftcMaterials;

/* The basename of the species' TIE98 OPT, without the extension. The
 * remastered catalog names GLBs, so its original fallback is used instead. */
static bool TieFlightTftcShips_ModelName(const TieFlightAssetSource* assets, uint16_t species_idx, char* out,
										 size_t capacity) {
	for (const TieFlightAssetSource* source = assets; source; source = source->fallback) {
		const TieFlightAssetEntry* entry = TieFlightAssets_Find(source->catalog, species_idx);
		if (!entry)
			continue;
		const char* name = entry->path;
		for (const char* cursor = entry->path; *cursor; ++cursor)
			if (*cursor == '/' || *cursor == '\\')
				name = cursor + 1;
		const char* dot = strrchr(name, '.');
		if (!dot || (strcmp(dot, ".OPT") != 0 && strcmp(dot, ".opt") != 0))
			continue;
		const size_t length = (size_t)(dot - name);
		if (length == 0 || length >= capacity)
			return false;
		memcpy(out, name, length);
		out[length] = '\0';
		return true;
	}
	return false;
}

static bool TieFlightTftcShips_NameEqual(const char* left, const char* right) {
	for (; *left && *right; ++left, ++right) {
		char a = *left, b = *right;
		if (a >= 'a' && a <= 'z')
			a = (char)(a - 'a' + 'A');
		if (b >= 'a' && b <= 'z')
			b = (char)(b - 'a' + 'A');
		if (a != b)
			return false;
	}
	return *left == *right;
}

static char* TieFlightTftcShips_Trim(char* text) {
	while (*text == ' ' || *text == '\t')
		++text;
	char* end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';
	return text;
}

static TieFlightTftcMaterial* TieFlightTftcShips_AddMaterial(TieFlightTftcMaterials* materials, const char* name,
															 const TieFlightTftcMaterial* defaults) {
	if (materials->count == materials->capacity) {
		const size_t capacity = materials->capacity ? materials->capacity * 2 : 16;
		TieFlightTftcMaterial* items = realloc(materials->items, capacity * sizeof *items);
		if (!items)
			return NULL;
		materials->items = items;
		materials->capacity = capacity;
	}
	TieFlightTftcMaterial* material = &materials->items[materials->count++];
	*material = *defaults;
	snprintf(material->name, sizeof material->name, "%s", name);
	return material;
}

/* Reads the [Default] and per-texture sections of an XWA effects .mat file.
 * A section header lists one or more texture names split on ','; each starts
 * from the [Default] values. Keys other than Metallic, Glossiness and
 * Shadeless, and the animation lines, are ignored. */
static void TieFlightTftcShips_ParseMaterials(char* text, TieFlightTftcMaterials* out) {
	TieFlightTftcMaterial defaults = {
		.metallic = TIE_FLIGHT_TFTC_DEFAULT_METALLIC,
		.glossiness = TIE_FLIGHT_TFTC_DEFAULT_GLOSSINESS,
	};
	if (!TieFlightTftcShips_AddMaterial(out, "", &defaults))
		return;
	/* The section's materials, by index: their storage may move as it grows. */
	size_t first = 0, end = 1;
	for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		char* comment = strchr(line, ';');
		if (comment)
			*comment = '\0';
		line = TieFlightTftcShips_Trim(line);
		if (line[0] == '[') {
			char* close = strchr(line, ']');
			if (!close)
				continue;
			*close = '\0';
			first = out->count;
			for (char* name = line + 1; *name;) {
				char* comma = strchr(name, ',');
				if (comma)
					*comma = '\0';
				const char* section = TieFlightTftcShips_Trim(name);
				if (TieFlightTftcShips_NameEqual(section, "Default")) {
					first = 0;
					break;
				}
				if (section[0] && !TieFlightTftcShips_AddMaterial(out, section, &out->items[0]))
					return;
				if (!comma)
					break;
				name = comma + 1;
			}
			end = first == 0 ? 1 : out->count;
			continue;
		}
		char* equals = strchr(line, '=');
		if (!equals)
			continue;
		*equals = '\0';
		const char* key = TieFlightTftcShips_Trim(line);
		const float value = strtof(TieFlightTftcShips_Trim(equals + 1), NULL);
		if (!isfinite(value))
			continue;
		for (size_t i = first; i < end; ++i) {
			TieFlightTftcMaterial* current = &out->items[i];
			if (TieFlightTftcShips_NameEqual(key, "Metallic"))
				current->metallic = value;
			else if (TieFlightTftcShips_NameEqual(key, "Glossiness"))
				current->glossiness = value;
			else if (TieFlightTftcShips_NameEqual(key, "Shadeless"))
				current->shadeless = value != 0.0f;
		}
	}
}

/* XWA effects raise the specular term to 128 * Glossiness. Map that
 * Blinn-Phong exponent to the matching GGX roughness. */
static float TieFlightTftcShips_Roughness(float glossiness) {
	const float exponent = 128.0f * (glossiness > 0.0f ? glossiness : 0.0f);
	const float roughness = sqrtf(2.0f / (exponent + 2.0f));
	return roughness < 0.04f ? 0.04f : roughness > 1.0f ? 1.0f : roughness;
}

static float TieFlightTftcShips_Clamp01(float value) { return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value; }

bool TieFlightTftc_LoadMaterials(AeronVfs* vfs, const char* mat_path, TieFlightTftcMaterialSet* out) {
	memset(out, 0, sizeof *out);
	TieFlightTftcMaterials materials = { 0 };
	uint8_t* mat_bytes = NULL;
	size_t mat_size = 0;
	if (AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_RESOURCE, mat_path, 1024u * 1024u, &mat_bytes, &mat_size)) {
		char* text = malloc(mat_size + 1);
		if (text) {
			memcpy(text, mat_bytes, mat_size);
			text[mat_size] = '\0';
			TieFlightTftcShips_ParseMaterials(text, &materials);
			free(text);
		}
		free(mat_bytes);
	} else {
		TieFlightTftcMaterial defaults = {
			.metallic = TIE_FLIGHT_TFTC_DEFAULT_METALLIC,
			.glossiness = TIE_FLIGHT_TFTC_DEFAULT_GLOSSINESS,
		};
		TieFlightTftcShips_AddMaterial(&materials, "", &defaults);
	}
	AeronOptMaterialOverride* overrides = calloc(materials.count ? materials.count : 1, sizeof *overrides);
	if (!overrides || !materials.count) {
		free(overrides);
		free(materials.items);
		return false;
	}
	for (size_t i = 0; i < materials.count; ++i) {
		const TieFlightTftcMaterial* material = &materials.items[i];
		overrides[i] = (AeronOptMaterialOverride) {
			.texture_name = material->name[0] ? material->name : NULL,
			.metallic = TieFlightTftcShips_Clamp01(material->metallic),
			.roughness = TieFlightTftcShips_Roughness(material->glossiness),
			.shadeless = material->shadeless,
		};
	}
	out->overrides = overrides;
	out->count = materials.count;
	out->storage = materials.items;
	return true;
}

void TieFlightTftc_FreeMaterials(TieFlightTftcMaterialSet* set) {
	free(set->overrides);
	free(set->storage);
	memset(set, 0, sizeof *set);
}

int TieFlightTftc_HiddenMeshes(AeronVfs* vfs, const char* ini_path, int* out, int capacity) {
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_RESOURCE, ini_path, 1024u * 1024u, &bytes, &size))
		return 0;
	char* text = malloc(size + 1);
	if (!text) {
		free(bytes);
		return 0;
	}
	memcpy(text, bytes, size);
	text[size] = '\0';
	free(bytes);
	/* Two passes over the section: the default's name, then its line. */
	char profile[64] = "Default";
	int count = 0;
	for (int pass = 0; pass < 2; ++pass) {
		bool in_section = false;
		for (char* line = text; line && *line;) {
			char* next = strchr(line, '\n');
			if (next)
				*next = '\0';
			char copy[512];
			snprintf(copy, sizeof copy, "%s", line);
			if (next)
				*next = '\n';
			line = next ? next + 1 : NULL;
			char* comment = strchr(copy, ';');
			if (comment)
				*comment = '\0';
			char* trimmed = TieFlightTftcShips_Trim(copy);
			if (trimmed[0] == '[') {
				in_section = TieFlightTftcShips_NameEqual(trimmed, "[ObjectProfiles]");
				continue;
			}
			char* equals = strchr(trimmed, '=');
			if (!in_section || !equals)
				continue;
			*equals = '\0';
			const char* key = TieFlightTftcShips_Trim(trimmed);
			char* value = TieFlightTftcShips_Trim(equals + 1);
			if (pass == 0) {
				if (TieFlightTftcShips_NameEqual(key, "Default") && value[0])
					snprintf(profile, sizeof profile, "%s", value);
				continue;
			}
			if (!TieFlightTftcShips_NameEqual(key, profile))
				continue;
			for (char* cursor = value; *cursor && count < capacity;) {
				char* end = NULL;
				const long index = strtol(cursor, &end, 10);
				if (end == cursor) {
					++cursor;
					continue;
				}
				if (index >= 0)
					out[count++] = (int)index;
				cursor = end;
			}
		}
	}
	free(text);
	return count;
}

AeronSceneMesh* TieFlightTftcShips_Load(TieFlightRenderer* g, AeronCommandBuffer* cmd, uint16_t species_idx) {
	char name[64];
	if (!TieFlightTftcShips_ModelName(g->assets, species_idx, name, sizeof name))
		return NULL;
	char path[96];
	snprintf(path, sizeof path, "tftc/%s.opt", name);
	if (!AeronVfs_Exists(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path))
		return NULL;
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path, TIE_FLIGHT_TFTC_MAX_BYTES, &bytes, &size)) {
		Aeron_LogWarn("tie.tftc", "%s could not be read; using the original model", path);
		return NULL;
	}

	char mat_path[96];
	snprintf(mat_path, sizeof mat_path, "tftc/%s.mat", name);
	TieFlightTftcMaterialSet materials;
	const bool have_materials = TieFlightTftc_LoadMaterials(g->assets->vfs, mat_path, &materials);
	char ini_path[96];
	snprintf(ini_path, sizeof ini_path, "tftc/%s.ini", name);
	int hidden[TIE_FLIGHT_TFTC_MAX_HIDDEN];
	const int hidden_count = TieFlightTftc_HiddenMeshes(g->assets->vfs, ini_path, hidden, TIE_FLIGHT_TFTC_MAX_HIDDEN);

	AeronFlightModel built = { 0 };
	AeronOptModelError error = { 0 };
	const bool converted = have_materials && Aeron_OptModelBuildMemory(
												 bytes, size, path,
												 &(AeronOptModelBuildOptions) {
													 .smooth_angle_degrees = g->assets->smooth_angle_degrees,
													 .emissive_strength = 1.0f,
													 .emissive = true,
													 .material_overrides = materials.overrides,
													 .material_override_count = materials.count,
													 /* TFTC ships carry far more texture than TIE98's. */
													 .max_atlas_size = 8192,
													 .mesh_skip = hidden,
													 .mesh_skip_count = (size_t)hidden_count,
												 },
												 &built, &error);
	TieFlightTftc_FreeMaterials(&materials);
	free(bytes);
	if (!converted) {
		Aeron_LogWarn("tie.tftc", "%s: %s; using the original model", path,
					  error.message[0] ? error.message : "out of memory");
		return NULL;
	}
	char label[96];
	snprintf(label, sizeof label, "flight.tftc[%s]", name);
	AeronSceneMeshCreateStatus status = AERON_SCENE_MESH_CREATE_SUCCESS;
	AeronSceneMesh* mesh = AeronScene_MeshCreate(cmd, &built, label, &status);
	Aeron_FlightModelFree(&built);
	if (!mesh) {
		Aeron_LogWarn("tie.tftc", "%s: scene mesh creation failed (%s); using the original model", path,
					  status == AERON_SCENE_MESH_CREATE_RESOURCE_FAILURE ? "GPU resource failure"
																		: "invalid converted model");
		return NULL;
	}
	Aeron_LogInfo("tie.tftc", "species %u uses %s: %u vertices, %u materials, %u engine glows", (unsigned)species_idx, path,
				  mesh->vertex_count, mesh->material_count, mesh->engine_glow_count);
	return mesh;
}

#define TIE_FLIGHT_TFTC_GLOW_SIZE 64
/* HDR boost of the glow colours (OpenXWA models.engine_emissive_strength). */
#define TIE_FLIGHT_TFTC_GLOW_STRENGTH 10.0f

void TieFlightTftcShips_EnsureGlowTexture(TieFlightRenderer* g, AeronCommandBuffer* cmd) {
	if (g->tftc_glow_texture || !cmd)
		return;
	/* A premultiplied white disc fading to its rim, standing in for XWA's
	 * glow sprite; the glow colours tint it. */
	static uint8_t pixels[TIE_FLIGHT_TFTC_GLOW_SIZE * TIE_FLIGHT_TFTC_GLOW_SIZE * 4];
	const float half = TIE_FLIGHT_TFTC_GLOW_SIZE * 0.5f;
	for (int y = 0; y < TIE_FLIGHT_TFTC_GLOW_SIZE; ++y)
		for (int x = 0; x < TIE_FLIGHT_TFTC_GLOW_SIZE; ++x) {
			const float dx = ((float)x + 0.5f - half) / half, dy = ((float)y + 0.5f - half) / half;
			float a = 1.0f - sqrtf(dx * dx + dy * dy);
			a = a < 0.0f ? 0.0f : a * a;
			uint8_t* p = &pixels[(y * TIE_FLIGHT_TFTC_GLOW_SIZE + x) * 4];
			p[0] = p[1] = p[2] = p[3] = (uint8_t)(a * 255.0f + 0.5f);
		}
	g->tftc_glow_texture = TieFlightRenderer_CreateDataTex(AERON_TEXTURE_FORMAT_RGBA8_UNORM,
														   TIE_FLIGHT_TFTC_GLOW_SIZE, TIE_FLIGHT_TFTC_GLOW_SIZE, 4);
	if (g->tftc_glow_texture && !TieFlightRenderer_UploadToTexture(cmd, g->tftc_glow_texture, TIE_FLIGHT_TFTC_GLOW_SIZE,
																   TIE_FLIGHT_TFTC_GLOW_SIZE, 4, pixels)) {
		Aeron_DestroyTexture(g->tftc_glow_texture);
		g->tftc_glow_texture = NULL;
	}
}

static float TieFlightTftcShips_SrgbToLinear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

float TieFlightTftcShips_EngineGlowScale(int current_speed) {
	/* XWA's throttle term with its ~6% flicker and 0.35 floor; TIE has no
	 * power redirect margin, so the margin is full. */
	const float s = (float)current_speed / 3600.0f * (1.0f - (float)(rand() & 0xf) * 0.004f);
	return s < 0.35f ? 0.35f : s;
}

void TieFlightTftcShips_SubmitEngineGlows(TieFlightRenderer* g, const AeronSceneMesh* mesh,
										  const float transform[16], const float cam_pos[3], float scale) {
	if (!g || !g->scene || !mesh || !mesh->engine_glow_count || !g->tftc_glow_texture || !transform ||
		!cam_pos || scale <= 0.0f)
		return;
	const float k = AERON_OPT_UNITS_PER_METER; /* the transform's model scale */
	static const float k_rim_uv[4][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
	for (uint32_t gi = 0; gi < mesh->engine_glow_count; ++gi) {
		const AeronFlightEngineGlow* glow = &mesh->engine_glows[gi];
		if (!glow->enabled)
			continue;
		const float p[3] = { glow->position.x, glow->position.y, glow->position.z };
		const AeronFlightVec3* axes[3] = { &glow->look, &glow->right, &glow->up };
		/* Model -> camera-relative space; axes divide the model scale back out. */
		float c[3], ax[3][3]; /* ax: look, right, up */
		for (int r = 0; r < 3; ++r) {
			c[r] = transform[r * 4 + 0] * p[0] + transform[r * 4 + 1] * p[1] + transform[r * 4 + 2] * p[2] +
				   transform[r * 4 + 3] - cam_pos[r];
			for (int a = 0; a < 3; ++a)
				ax[a][r] = (transform[r * 4 + 0] * axes[a]->x + transform[r * 4 + 1] * axes[a]->y +
							transform[r * 4 + 2] * axes[a]->z) /
						   k;
		}
		/* Classic culls: at the eye, or under a pixel (max dim x 256 / depth).
		 * The direction to the glow stands in for the view axis. */
		const float depth = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		const float dim_x = k * glow->dimensions.x, dim_y = k * glow->dimensions.y, dim_z = k * glow->dimensions.z;
		if (depth < 1.0f || fmaxf(fmaxf(dim_x, dim_y), dim_z) * 256.0f / depth < 1.0f)
			continue;
		const float view[3] = { c[0] / depth, c[1] / depth, c[2] / depth };

		/* Rect for elongated glows, a diamond turned toward the eye for round
		 * ones (ratio in (0.85, 1.2)); the geometric scale clamps at 0.8. */
		const float clamped = scale >= 0.80000001f ? 0.80000001f : scale;
		float corners[4][3];
		const float ratio = glow->dimensions.y != 0.0f ? glow->dimensions.x / glow->dimensions.y : 1.0f;
		if (ratio <= 0.85000002f || ratio >= 1.2f) {
			const float as = dim_x * clamped * 0.5f, bs = dim_y * clamped * 0.5f;
			for (int r = 0; r < 3; ++r) {
				const float a = ax[1][r] * as, b = ax[2][r] * bs;
				corners[0][r] = c[r] - a + b;
				corners[1][r] = c[r] + a + b;
				corners[2][r] = c[r] + a - b;
				corners[3][r] = c[r] - a - b;
			}
		} else {
			const float radius = clamped * dim_x * 1.415f * 0.5f;
			const float dot_f = ax[1][0] * c[0] + ax[1][1] * c[1] + ax[1][2] * c[2];
			const float dot_s = ax[2][0] * c[0] + ax[2][1] * c[1] + ax[2][2] * c[2];
			const float len = sqrtf(dot_f * dot_f + dot_s * dot_s);
			float major[3], minor[3];
			for (int r = 0; r < 3; ++r) {
				if (len == 0.0f) {
					major[r] = ax[1][r];
					minor[r] = ax[2][r];
				} else {
					major[r] = ax[2][r] * (dot_s / len) + ax[1][r] * (dot_f / len);
					minor[r] = ax[1][r] * (dot_s / len) - ax[2][r] * (dot_f / len);
				}
			}
			for (int r = 0; r < 3; ++r) {
				corners[0][r] = c[r] + minor[r] * radius;
				corners[1][r] = c[r] + major[r] * radius;
				corners[2][r] = c[r] - minor[r] * radius;
				corners[3][r] = c[r] - major[r] * radius;
			}
		}

		/* Look-axis extrusion by each corner's depth from the centre, with the
		 * classic facing-dependent sign and asymmetric gains (10000/32768 on
		 * the negative side, scale x dim.z on the positive). */
		const float centre_dot = ax[0][0] * c[0] + ax[0][1] * c[1] + ax[0][2] * c[2];
		for (int ci = 0; ci < 4; ++ci) {
			const float corner_depth = view[0] * corners[ci][0] + view[1] * corners[ci][1] + view[2] * corners[ci][2];
			const float z_delta = centre_dot < 0.0f ? corner_depth - depth : depth - corner_depth;
			const float gain = z_delta < 0.0f ? (10000.0f / 32768.0f) : scale * dim_z;
			for (int r = 0; r < 3; ++r)
				corners[ci][r] += ax[0][r] * z_delta * gain;
		}

		/* sRGB glow colours, linearised and premultiplied for the PMA blend. */
		float core[4], outer[4];
		core[3] = glow->core_rgba[3];
		outer[3] = glow->outer_rgba[3];
		for (int ch = 0; ch < 3; ++ch) {
			core[ch] = TieFlightTftcShips_SrgbToLinear(glow->core_rgba[ch]) * core[3] * TIE_FLIGHT_TFTC_GLOW_STRENGTH;
			outer[ch] = TieFlightTftcShips_SrgbToLinear(glow->outer_rgba[ch]) * outer[3] * TIE_FLIGHT_TFTC_GLOW_STRENGTH;
		}
		AeronSceneBillboardDesc d;
		memset(&d, 0, sizeof d);
		d.texture = g->tftc_glow_texture;
		d.blend = AERON_SCENE_BILLBOARD_BLEND_PMA;
		d.stage = AERON_SCENE_BILLBOARD_STAGE_OVERLAY;
		d.center_color = core;
		for (int v = 0; v < 4; ++v) {
			for (int r = 0; r < 3; ++r)
				d.corners[v][r] = cam_pos[r] + corners[v][r];
			memcpy(d.uv[v], k_rim_uv[v], sizeof d.uv[v]);
			memcpy(d.colors[v], outer, sizeof outer);
		}
		AeronScene_AddBillboard(g->scene, &d);
	}
}
