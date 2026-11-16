/*
 * 3D cockpit model drawn around the camera in place of the cockpit art when
 * the 3D cockpit option is on. Models are XWA-style cockpit OPTs loaded from
 * resources/cockpit3d/<Name>Cockpit.opt, chosen by the cockpit view's craft.
 * A view with no matching model falls back to the cockpit art.
 */

#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/time.h"
#include "aeron/scene/mesh.h"
#include "aeron/vfs.h"
#include "opt.h"
#include "tie_remaster/flight/cockpit/dc_effects.h"
#include "tie_remaster/flight/cockpit/dc_layout.h"
#include "tie_remaster/flight/render_math.h"
#include "tie_remaster/flight/renderer_internal.h"
#include "tie_remaster/flight/tftc_ships.h"
#include "tie_runtime/presentation/presentation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Horizontal field of view without a .dc xwahacker_fov: the XWAU cockpits' usual one. */
#define TIE_FLIGHT_COCKPIT3D_FOV_DEG 88.0f

/* TFTC's cockpits sit in dim light, their grey shell still showing its
 * texture as in XWA, and their self-lit texels glow without blooming. */
#define TIE_FLIGHT_COCKPIT3D_BASE_COLOR 0.6f
#define TIE_FLIGHT_COCKPIT3D_GLOW 0.5f
/* The cockpit takes no shadows (its own hull would black it out), so the
 * sun and sky reach it only weakly, as through the canopy frame. */
#define TIE_FLIGHT_COCKPIT3D_WORLD_LIGHT 0.4f

static float s_cockpit3d_fov_deg = TIE_FLIGHT_COCKPIT3D_FOV_DEG;

static void TieFlightCockpit3d_Transform(const float m[16], const float p[3], float w, float out[4]);

float TieFlightCockpit3d_FovDeg(void) { return s_cockpit3d_fov_deg; }

/* The pilot's eye in the cockpit model (metres): the craft's [CockpitPov]
 * from FlightModels/<Craft>.ini, copied beside the cockpit as <Name>.ini.
 * TFTC ships the section disabled as "[CockpitPov;]" to document XWA's
 * built-in values, which are the ones XWA uses, so it is read either way. */
static void TieFlightCockpit3d_LoadEye(TieFlightRenderer* g, const char* model) {
	g->cockpit3d_eye[0] = g->cockpit3d_eye[1] = g->cockpit3d_eye[2] = 0.0f;
	char path[96];
	snprintf(path, sizeof path, "cockpit3d/%s.ini", model);
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path, 1024u * 1024u, &bytes, &size))
		return;
	char* text = malloc(size + 1);
	if (!text) {
		free(bytes);
		return;
	}
	memcpy(text, bytes, size);
	text[size] = '\0';
	free(bytes);
	float pov[3] = { 0.0f, 0.0f, 0.0f };
	bool in_section = false;
	for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		while (*line == ' ' || *line == '\t')
			++line;
		if (line[0] == '[') {
			in_section = strncmp(line, "[CockpitPov", 11) == 0;
			continue;
		}
		static const char* const keys[3] = { "CockpitPovX", "CockpitPovY", "CockpitPovZ" };
		for (int k = 0; k < 3 && in_section; ++k) {
			const size_t length = strlen(keys[k]);
			if (strncmp(line, keys[k], length) == 0) {
				const char* equals = strchr(line + length, '=');
				if (equals)
					pov[k] = strtof(equals + 1, NULL);
			}
		}
	}
	free(text);
	/* XWA's POV Y counts forward, which is -Y in the OPT. */
	pov[1] = -pov[1];
	/* TFTC's assault gunboat and missile boat cockpits sit further from the
	 * eye than their .ini POV puts them (which would hang the missile boat's
	 * 3 m up): these eyes put their targeting screens where TFTC shows them.
	 * The TIE Avenger's eye sits 5 cm behind and 2 cm above its POV, which
	 * crowds its console a little and crops its readouts over the canopy. */
	static const struct {
		const char* model;
		float eye[3];
	} k_eyes[] = {
		{ "AssaultGunboatCockpit", { 0.0f, -41.2f, 15.8f } },
		{ "MissileBoatCockpit", { 0.0f, -131.0f, 19.5f } },
		{ "TieAvengerCockpit", { 0.0f, 5.0f, 4.0f } },
	};
	for (size_t i = 0; i < sizeof k_eyes / sizeof k_eyes[0]; ++i)
		if (strcmp(k_eyes[i].model, model) == 0)
			memcpy(pov, k_eyes[i].eye, sizeof pov);
	for (int k = 0; k < 3; ++k)
		g->cockpit3d_eye[k] = pov[k] * AERON_OPT_METERS_PER_UNIT;
	Aeron_LogInfo("tie.cockpit3d", "%s: eye at %.0f %.0f %.0f", path, pov[0], pov[1], pov[2]);
}

typedef struct TieFlightCockpit3dModel {
	const char* view_prefix; /* PanelViewDef.name prefix, uppercase */
	const char* model;       /* basename under resources/cockpit3d/ */
} TieFlightCockpit3dModel;

static const TieFlightCockpit3dModel k_cockpit3d_models[] = {
	{ "TIEFTR", "TieFighterCockpit" },	  { "TIEINT", "TieInterceptorCockpit" },
	{ "TIEBMB", "TieBomberCockpit" },	  { "TIEADV", "TieAvengerCockpit" },
	{ "TIEDLX", "TieDefenderCockpit" },	  { "GUNBT", "AssaultGunboatCockpit" },
	{ "MISSBT", "MissileBoatCockpit" },	  { "MISSUP", "MissileBoatCockpit" },
};

/* How far the yoke tilts with the stick fully back or forward, and turns
 * with it fully over, in degrees. */
#define TIE_FLIGHT_COCKPIT3D_YOKE_PITCH_DEG 8.0f
#define TIE_FLIGHT_COCKPIT3D_YOKE_TURN_DEG 20.0f

#define TIE_FLIGHT_COCKPIT3D_YOKE_MAX_MESHES 16
#define TIE_FLIGHT_COCKPIT3D_YOKE_MAX_TEXTURES 32

/* A cockpit's yoke, as TFTC's effects .mat defines it: the faces of every
 * texture with a JoystickRoot turn about that point (metres, OPT axes, like
 * the [CockpitPov] eye: the TIE Bomber's sits at x = +52). Here the OPT
 * meshes carrying those textures turn: tilted fore and aft with pitch, and
 * turned like a wheel about the column, which runs from the root up and back
 * toward the grips, with yaw and roll. */
typedef struct TieFlightCockpit3dYoke {
	int meshes[TIE_FLIGHT_COCKPIT3D_YOKE_MAX_MESHES];
	int mesh_count;
	float pivot[3]; /* OPT units */
	float column[3];
} TieFlightCockpit3dYoke;

static TieFlightCockpit3dYoke s_cockpit3d_yoke;

static bool TieFlightCockpit3d_NameEqual(const char* left, const char* right) {
	for (; *left && *right; ++left, ++right) {
		const char a = (char)(*left >= 'a' && *left <= 'z' ? *left - 32 : *left);
		const char b = (char)(*right >= 'a' && *right <= 'z' ? *right - 32 : *right);
		if (a != b)
			return false;
	}
	return *left == *right;
}

/* Reads the joystick textures and root from `mat` (the .mat text, changed in
 * place). Section headers list texture names split on ','. */
static int TieFlightCockpit3d_ReadJoystick(char* mat, char names[][16], int capacity, float root[3]) {
	int count = 0, section_end = 0;
	for (char* line = strtok(mat, "\n"); line; line = strtok(NULL, "\n")) {
		while (*line == ' ' || *line == '\t')
			++line;
		if (line[0] == '[') {
			/* A section's names wait after the kept ones until it turns out
			 * to define the joystick. */
			section_end = count;
			for (char* p = line + 1; *p && *p != ']' && section_end < capacity;) {
				size_t n = strcspn(p, ",]\r");
				snprintf(names[section_end++], 16, "%.*s", (int)n, p);
				p += n;
				if (*p == ',')
					++p;
			}
			continue;
		}
		if (strncmp(line, "JoystickRoot", 12) != 0)
			continue;
		const char* equals = strchr(line, '=');
		float x, y, z;
		if (equals && sscanf(equals + 1, " %f , %f , %f", &x, &y, &z) == 3) {
			root[0] = x;
			root[1] = y;
			root[2] = z;
			count = section_end;
		}
	}
	return count;
}

/* The yoke of `model` from its cockpit3d/<model>.mat, or NULL without one:
 * the meshes whose first LOD shows a joystick texture. */
static const TieFlightCockpit3dYoke* TieFlightCockpit3d_YokeOf(TieFlightRenderer* g, const opt_file_t* opt,
															   const char* model) {
	char path[96];
	snprintf(path, sizeof path, "cockpit3d/%s.mat", model);
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path, 1024u * 1024u, &bytes, &size))
		return NULL;
	char* text = malloc(size + 1);
	if (!text) {
		free(bytes);
		return NULL;
	}
	memcpy(text, bytes, size);
	text[size] = '\0';
	free(bytes);
	char names[TIE_FLIGHT_COCKPIT3D_YOKE_MAX_TEXTURES][16];
	float root[3] = { 0.0f, 0.0f, 0.0f };
	const int name_count = TieFlightCockpit3d_ReadJoystick(text, names, TIE_FLIGHT_COCKPIT3D_YOKE_MAX_TEXTURES, root);
	free(text);
	TieFlightCockpit3dYoke* yoke = &s_cockpit3d_yoke;
	memset(yoke, 0, sizeof *yoke);
	for (int m = 0; m < opt->mesh_count && yoke->mesh_count < TIE_FLIGHT_COCKPIT3D_YOKE_MAX_MESHES; ++m) {
		const opt_mesh_t* mesh = &opt->meshes[m];
		bool joystick = false;
		for (int gr = 0; mesh->lod_count > 0 && gr < mesh->lods[0].group_count && !joystick; ++gr) {
			const int t = mesh->lods[0].groups[gr].texture_index;
			for (int n = 0; t >= 0 && t < opt->texture_count && n < name_count && !joystick; ++n)
				joystick = TieFlightCockpit3d_NameEqual(opt->textures[t].name, names[n]);
		}
		if (joystick)
			yoke->meshes[yoke->mesh_count++] = m;
	}
	if (!yoke->mesh_count)
		return NULL;
	for (int k = 0; k < 3; ++k)
		yoke->pivot[k] = root[k] * AERON_OPT_UNITS_PER_METER;
	/* From the root up and back toward the grips, as in TFTC's TIE cockpits. */
	yoke->column[0] = 0.0f;
	yoke->column[1] = 18.0f;
	yoke->column[2] = 19.0f;
	Aeron_LogInfo("tie.cockpit3d", "%s: yoke of %d meshes about %.1f %.1f %.1f", path, yoke->mesh_count,
				  yoke->pivot[0], yoke->pivot[1], yoke->pivot[2]);
	return yoke;
}

static bool TieFlightCockpit3d_OnYoke(const TieFlightCockpit3dYoke* yoke, int mesh) {
	for (int i = 0; yoke && i < yoke->mesh_count; ++i)
		if (yoke->meshes[i] == mesh)
			return true;
	return false;
}

static bool TieFlightCockpit3d_Listed(const int* list, int count, int mesh) {
	for (int i = 0; i < count; ++i)
		if (list[i] == mesh)
			return true;
	return false;
}

/* The cover_texture images of a .dc file as effects lines (dc_effects.h):
 * "[<Texture>]" and "cover_seq = Effects\<File>.dat, (<group>-<image>, 1, 1)"
 * for each, under the texture of its [<Cockpit>.opt,<Texture>] section. */
static char* TieFlightCockpit3d_CoverLines(const char* dc_text) {
	const size_t capacity = strlen(dc_text) + 1;
	char* out = malloc(capacity);
	char* text = malloc(capacity);
	if (!out || !text) {
		free(out);
		free(text);
		return NULL;
	}
	memcpy(text, dc_text, capacity);
	size_t used = 0;
	out[0] = '\0';
	char texture[32] = "";
	for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		while (*line == ' ' || *line == '\t')
			++line;
		if (line[0] == '[') {
			const char* comma = strchr(line, ',');
			const char* close = strchr(line, ']');
			texture[0] = '\0';
			if (comma && close && close > comma)
				snprintf(texture, sizeof texture, "%.*s", (int)(close - comma - 1), comma + 1);
			continue;
		}
		char file[96];
		int group, image;
		if (!texture[0] || strncmp(line, "cover_texture", 13) != 0 ||
			sscanf(line, "cover_texture = %95[^-\r]-%d-%d", file, &group, &image) != 3)
			continue;
		const int n = snprintf(out + used, capacity - used, "[%s]\ncover_seq = %s, (%d-%d, 1, 1)\n", texture, file,
							   group, image);
		if (n > 0 && (size_t)n < capacity - used)
			used += (size_t)n;
	}
	free(text);
	return out;
}

/* The missile boat's hologram text boxes crowd its console: each is made a
 * tenth wider and moved out from the middle by a tenth of its width. */
static void TieFlightCockpit3d_SpreadHolograms(TieDcLayout* dc) {
	for (int s = 0; s < dc->count; ++s) {
		TieDcScreen* screen = &dc->screens[s];
		if (!screen->hologram || screen->triangle_count <= 0)
			continue;
		float lo = 1e30f, hi = -1e30f;
		for (int t = 0; t < screen->triangle_count; ++t)
			for (int c = 0; c < 3; ++c) {
				lo = fminf(lo, screen->triangles[t].position[c][0]);
				hi = fmaxf(hi, screen->triangles[t].position[c][0]);
			}
		const float centre = (lo + hi) * 0.5f, shift = (centre < 0.0f ? -0.1f : 0.1f) * (hi - lo);
		for (int t = 0; t < screen->triangle_count; ++t)
			for (int c = 0; c < 3; ++c) {
				float* x = &screen->triangles[t].position[c][0];
				*x = centre + shift + (*x - centre) * 1.1f;
			}
	}
}

/* A screen's meshes, indexed in the OPT without its `hidden` meshes, by their
 * index in the whole OPT. */
static void TieFlightCockpit3d_RemapMeshes(TieDcScreen* screen, const int* hidden, int hidden_count, int total) {
	for (int t = 0; t < screen->triangle_count; ++t) {
		int* mesh = &screen->triangles[t].mesh;
		for (int m = 0, n = 0; m < total; ++m)
			if (!TieFlightCockpit3d_Listed(hidden, hidden_count, m) && n++ == *mesh) {
				*mesh = m;
				break;
			}
	}
}

static const char* TieFlightCockpit3d_ModelForView(const char* view_name) {
	for (size_t i = 0; i < sizeof k_cockpit3d_models / sizeof k_cockpit3d_models[0]; ++i) {
		const char* prefix = k_cockpit3d_models[i].view_prefix;
		size_t n = strlen(prefix);
		size_t j = 0;
		for (; j < n && view_name[j]; ++j) {
			char c = view_name[j];
			if (c >= 'a' && c <= 'z')
				c = (char)(c - 32);
			if (c != prefix[j])
				break;
		}
		if (j == n)
			return k_cockpit3d_models[i].model;
	}
	return NULL;
}

/* The camera rides the craft whose own mesh SceneSubmit hides. The first
 * flight frame still carries an unset (identity) camera orientation. */
static const TieFlightObjectState* TieFlightCockpit3d_RiddenCraft(const TieSnapshot* curr) {
	if (curr->camera.zoom_active != 0 || curr->replay_mode == 2 || !curr->cockpit.panel_loaded ||
		curr->flight_frame == 0)
		return NULL;
	for (uint16_t i = 0; i < curr->flight_count; ++i) {
		if (curr->flights[i].slot == curr->camera.target_obj_slot)
			return &curr->flights[i];
	}
	return NULL;
}

void TieFlightCockpit3d_Release(TieFlightRenderer* g) {
	for (uint32_t i = 0; i < g->cockpit3d_mesh_count; ++i)
		AeronScene_MeshDestroy(g->cockpit3d_meshes[i]);
	g->cockpit3d_mesh_count = 0;
	g->cockpit3d_model = NULL;
	g->cockpit3d_yoke = NULL;
	g->cockpit3d_yoke_first = 0;
	g->cockpit3d_active = false;
	TieDcLayout_Free(&g->cockpit3d_dc);
	g->cockpit3d_dc_loaded = false;
	TieDcEffects_Free(&g->cockpit3d_effects);
}

static bool TieFlightCockpit3d_BuildChunk(TieFlightRenderer* g, AeronCommandBuffer* cmd, const uint8_t* bytes,
										  size_t size, const char* path, const TieFlightTftcMaterialSet* materials,
										  const int* skip, int skip_count, int first, int count) {
	/* A run of meshes left out entirely has nothing to build. */
	int shown = count;
	for (int i = first; i < first + count; ++i)
		if (TieFlightCockpit3d_Listed(skip, skip_count, i))
			--shown;
	if (shown <= 0)
		return true;
	AeronFlightModel built = { 0 };
	AeronOptModelError error = { 0 };
	if (!Aeron_OptModelBuildMemory(bytes, size, path,
								   &(AeronOptModelBuildOptions) {
									   .smooth_angle_degrees = g->assets->smooth_angle_degrees,
									   .emissive_strength = TIE_FLIGHT_COCKPIT3D_GLOW,
									   .emissive = true,
									   .material_overrides = materials->overrides,
									   .material_override_count = materials->count,
									   /* XWA cockpits carry far more texture than a ship. */
									   .max_atlas_size = 8192,
									   .mesh_first = first,
									   .mesh_count = count,
									   .mesh_skip = skip,
									   .mesh_skip_count = (size_t)skip_count,
									   .base_color_scale = TIE_FLIGHT_COCKPIT3D_BASE_COLOR,
								   },
								   &built, &error))
		return false;
	AeronSceneMeshCreateStatus status = AERON_SCENE_MESH_CREATE_SUCCESS;
	AeronSceneMesh* mesh = AeronScene_MeshCreate(cmd, &built, path, &status);
	Aeron_FlightModelFree(&built);
	if (!mesh)
		return false;
	g->cockpit3d_meshes[g->cockpit3d_mesh_count++] = mesh;
	return true;
}

/* Scene meshes use 16-bit indices and an XWA cockpit converts to far more
 * vertices than that, so it is built as runs of its OPT meshes, halving a
 * run until it fits. */
static void TieFlightCockpit3d_BuildRuns(TieFlightRenderer* g, AeronCommandBuffer* cmd, const uint8_t* bytes,
										 size_t size, const char* path, const TieFlightTftcMaterialSet* materials,
										 const int* skip, int skip_count, int first, int end) {
	while (first < end && g->cockpit3d_mesh_count < TIE_FLIGHT_COCKPIT3D_MAX_CHUNKS) {
		int count = end - first;
		while (!TieFlightCockpit3d_BuildChunk(g, cmd, bytes, size, path, materials, skip, skip_count, first, count) &&
			   count > 1)
			count = (count + 1) / 2;
		if (count == 1 && g->cockpit3d_mesh_count == 0)
			Aeron_LogWarn("tie.cockpit3d", "%s: mesh %d could not be built", path, first);
		first += count;
	}
}

static void TieFlightCockpit3d_Load(TieFlightRenderer* g, AeronCommandBuffer* cmd, const char* model) {
	TieFlightCockpit3d_Release(g);
	g->cockpit3d_model = model;

	char path[96];
	snprintf(path, sizeof path, "cockpit3d/%s.opt", model);
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path, 256u * 1024u * 1024u, &bytes, &size)) {
		Aeron_LogWarn("tie.cockpit3d", "%s not found; using the cockpit art", path);
		return;
	}
	opt_error_t parse_error = { { 0 } };
	opt_file_t* opt = opt_load_memory(bytes, size, &parse_error);
	if (!opt) {
		Aeron_LogWarn("tie.cockpit3d", "%s: %s", path, parse_error.msg);
		free(bytes);
		return;
	}
	const int total = opt->mesh_count;
	g->cockpit3d_yoke = TieFlightCockpit3d_YokeOf(g, opt, model);

	/* The craft's object profile, from <Name>.ini, hides cockpit meshes too:
	 * they are left out of the model and their displays. */
	char ini_path[96];
	snprintf(ini_path, sizeof ini_path, "cockpit3d/%s.ini", model);
	int hidden[TIE_FLIGHT_TFTC_MAX_HIDDEN];
	const int hidden_count = TieFlightTftc_HiddenMeshes(g->assets->vfs, ini_path, hidden, TIE_FLIGHT_TFTC_MAX_HIDDEN);
	opt_file_t shown = *opt;
	opt_mesh_t* shown_meshes = hidden_count ? malloc((size_t)(total > 0 ? total : 1) * sizeof *shown_meshes) : NULL;
	if (shown_meshes) {
		shown.meshes = shown_meshes;
		shown.mesh_count = 0;
		for (int m = 0; m < total; ++m) {
			bool skip = false;
			for (int h = 0; h < hidden_count && !skip; ++h)
				skip = hidden[h] == m;
			if (!skip)
				shown_meshes[shown.mesh_count++] = opt->meshes[m];
		}
	}

	/* Where the cockpit shows each HUD display, from DynamicCockpit/<Name>.dc,
	 * and the cover images over them. */
	char* cover_text = NULL;
	char dc_path[96];
	snprintf(dc_path, sizeof dc_path, "cockpit3d/%s.dc", model);
	uint8_t* dc_bytes = NULL;
	size_t dc_size = 0;
	if (AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, dc_path, 1024u * 1024u, &dc_bytes, &dc_size)) {
		char* dc_text = malloc(dc_size + 1);
		if (dc_text) {
			memcpy(dc_text, dc_bytes, dc_size);
			dc_text[dc_size] = '\0';
			g->cockpit3d_dc_loaded = TieDcLayout_Build(dc_text, &shown, &g->cockpit3d_dc);
			/* The TIE cockpits' targeting glass reaches below the .dc's display. */
			for (int s = 0; strncmp(model, "Tie", 3) == 0 && s < g->cockpit3d_dc.count; ++s)
				if (g->cockpit3d_dc.screens[s].source == TIE_DC_TARGETING)
					TieDcLayout_ExtendDown(&g->cockpit3d_dc.screens[s], &shown, 0.15f);
			cover_text = TieFlightCockpit3d_CoverLines(dc_text);
			free(dc_text);
			for (int s = 0; shown_meshes && s < g->cockpit3d_dc.count; ++s)
				TieFlightCockpit3d_RemapMeshes(&g->cockpit3d_dc.screens[s], hidden, hidden_count, total);
			if (strcmp(model, "MissileBoatCockpit") == 0)
				TieFlightCockpit3d_SpreadHolograms(&g->cockpit3d_dc);
		}
		free(dc_bytes);
		Aeron_LogInfo("tie.cockpit3d", "%s: %d HUD screens", dc_path,
					  g->cockpit3d_dc_loaded ? g->cockpit3d_dc.count : 0);
	} else {
		Aeron_LogWarn("tie.cockpit3d", "%s not found; the HUD stays flat", dc_path);
	}
	s_cockpit3d_fov_deg = g->cockpit3d_dc_loaded && g->cockpit3d_dc.fov_deg > 0.0f ? g->cockpit3d_dc.fov_deg
																				   : TIE_FLIGHT_COCKPIT3D_FOV_DEG;

	/* The image sequences the cockpit's effects .mat plays on its textures. */
	char effects_path[96];
	snprintf(effects_path, sizeof effects_path, "cockpit3d/%s.mat", model);
	uint8_t* effects_bytes = NULL;
	size_t effects_size = 0;
	if (AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, effects_path, 1024u * 1024u, &effects_bytes,
						 &effects_size)) {
		const size_t cover_size = cover_text ? strlen(cover_text) : 0;
		char* effects_text = malloc(effects_size + 1 + cover_size + 1);
		if (effects_text) {
			memcpy(effects_text, effects_bytes, effects_size);
			effects_text[effects_size] = '\n';
			memcpy(effects_text + effects_size + 1, cover_text ? cover_text : "", cover_size + 1);
			TieDcEffects_Build(&g->cockpit3d_effects, cmd, g->assets->vfs, effects_text, &shown);
			free(effects_text);
			for (int e = 0; shown_meshes && e < g->cockpit3d_effects.count; ++e)
				TieFlightCockpit3d_RemapMeshes(&g->cockpit3d_effects.items[e].faces, hidden, hidden_count, total);
			g->cockpit3d_effects_start_us = Aeron_NowUs();
			Aeron_LogInfo("tie.cockpit3d", "%s: %d image sequences, %d frames", effects_path,
						  g->cockpit3d_effects.count, g->cockpit3d_effects.texture_count);
		}
		free(effects_bytes);
	}
	free(cover_text);
	TieFlightCockpit3d_LoadEye(g, model);
	free(shown_meshes);
	opt_free(opt);

	/* The cockpit's XWA effects materials, from <Name>.mat beside it. */
	char mat_path[96];
	snprintf(mat_path, sizeof mat_path, "cockpit3d/%s.mat", model);
	TieFlightTftcMaterialSet materials;
	if (!TieFlightTftc_LoadMaterials(g->assets->vfs, mat_path, &materials)) {
		free(bytes);
		return;
	}

	/* The yoke is left out of the shell and built after it, as scene meshes
	 * of its own that turn with the stick. */
	const TieFlightCockpit3dYoke* yoke = g->cockpit3d_yoke;
	int* skip = malloc((size_t)(total > 0 ? total : 1) * sizeof *skip);
	if (!skip) {
		TieFlightTftc_FreeMaterials(&materials);
		free(bytes);
		return;
	}
	int skip_count = 0;
	for (int m = 0; m < total; ++m)
		if (TieFlightCockpit3d_Listed(hidden, hidden_count, m) || TieFlightCockpit3d_OnYoke(yoke, m))
			skip[skip_count++] = m;
	TieFlightCockpit3d_BuildRuns(g, cmd, bytes, size, path, &materials, skip, skip_count, 0, total);
	g->cockpit3d_yoke_first = g->cockpit3d_mesh_count;
	int yoke_first = total, yoke_end = 0;
	skip_count = 0;
	for (int m = 0; m < total; ++m) {
		if (TieFlightCockpit3d_OnYoke(yoke, m) && !TieFlightCockpit3d_Listed(hidden, hidden_count, m)) {
			yoke_first = m < yoke_first ? m : yoke_first;
			yoke_end = m + 1;
		} else {
			skip[skip_count++] = m;
		}
	}
	TieFlightCockpit3d_BuildRuns(g, cmd, bytes, size, path, &materials, skip, skip_count, yoke_first, yoke_end);
	free(skip);
	TieFlightTftc_FreeMaterials(&materials);
	free(bytes);
	uint32_t vertices = 0;
	for (uint32_t i = 0; i < g->cockpit3d_mesh_count; ++i)
		vertices += g->cockpit3d_meshes[i]->vertex_count;
	Aeron_LogInfo("tie.cockpit3d", "loaded %s: %d OPT meshes in %u scene meshes, %u vertices", path, total,
				  g->cockpit3d_mesh_count, vertices);
}

void TieFlightCockpit3d_Prepare(TieFlightRenderer* g, AeronCommandBuffer* cmd, const TieSnapshot* curr) {
	g->cockpit3d_active = false;
	if (!TiePresentation_Cockpit3d() || !g->scene_model_backend || !TieFlightCockpit3d_RiddenCraft(curr))
		return;
	const char* model = TieFlightCockpit3d_ModelForView(curr->cockpit.view_name);
	if (!model)
		return;
	if (model != g->cockpit3d_model)
		TieFlightCockpit3d_Load(g, cmd, model);
}

/* The cockpit's transform with the yoke tilted and turned by the stick about
 * where its column meets the floor: turned about the column, then the column
 * tilted, the top coming back with the stick pulled back and the right grip
 * dropping with it pushed right. Without a yoke, the cockpit's own. */
static void TieFlightCockpit3d_YokeTransform(const TieFlightCockpit3dYoke* yoke, const float stick[3],
											 const float cockpit[16], float out[16]) {
	memcpy(out, cockpit, sizeof(float[16]));
	if (!yoke)
		return;
	const float pivot[3] = { yoke->pivot[0] * AERON_OPT_METERS_PER_UNIT, yoke->pivot[1] * AERON_OPT_METERS_PER_UNIT,
							 yoke->pivot[2] * AERON_OPT_METERS_PER_UNIT };
	const float degrees = 3.14159265f / 180.0f;
	const float turn = fmaxf(-1.0f, fminf(1.0f, stick[0] + stick[2]));
	float turn_3x4[3][4], tilt_3x4[3][4];
	TieRenderMath_Mat3x4RotationAboutPivot(turn_3x4, yoke->column, pivot,
										   turn * TIE_FLIGHT_COCKPIT3D_YOKE_TURN_DEG * degrees);
	TieRenderMath_Mat3x4RotationAboutPivot(tilt_3x4, (const float[3]) { 1.0f, 0.0f, 0.0f }, pivot,
										   -stick[1] * TIE_FLIGHT_COCKPIT3D_YOKE_PITCH_DEG * degrees);
	float turn_m[16], tilt_m[16], local[16];
	TieRenderMath_Mat4Identity(turn_m);
	TieRenderMath_Mat4Identity(tilt_m);
	memcpy(turn_m, turn_3x4, sizeof turn_3x4);
	memcpy(tilt_m, tilt_3x4, sizeof tilt_3x4);
	TieRenderMath_Mat4Multiply(local, tilt_m, turn_m);
	TieRenderMath_Mat4Multiply(out, cockpit, local);
}

void TieFlightCockpit3d_Submit(TieFlightRenderer* g, const TieSnapshot* curr, const float cam_pos[3]) {
	const TieFlightObjectState* craft = TieFlightCockpit3d_RiddenCraft(curr);
	if (!TiePresentation_Cockpit3d() || g->cockpit3d_mesh_count == 0 || !craft ||
		TieFlightCockpit3d_ModelForView(curr->cockpit.view_name) != g->cockpit3d_model)
		return;

	/* The model turns with the craft, not the camera, so side and rear views
	 * look at the matching part of it. The eye sits at the craft's cockpit POV
	 * in the model, as in XWA, so the model moves back by it. */
	float transform[16];
	float craft_transform[16];
	TieRenderMath_Mat4FromQuaternionTranslation(craft_transform, craft->ori, cam_pos, AERON_OPT_UNITS_PER_METER);
	const float* eye = g->cockpit3d_eye;
	const float to_eye[16] = {
		1.0f, 0.0f, 0.0f, -eye[0], 0.0f, 1.0f, 0.0f, -eye[1], 0.0f, 0.0f, 1.0f, -eye[2], 0.0f, 0.0f, 0.0f, 1.0f,
	};
	TieRenderMath_Mat4Multiply(transform, craft_transform, to_eye);
	TieFlightCockpit3d_YokeTransform(g->cockpit3d_yoke, curr->cockpit.stick, transform, g->cockpit3d_yoke_transform);
	for (uint32_t i = 0; i < g->cockpit3d_mesh_count; ++i) {
		AeronSceneMeshInstance in;
		memset(&in, 0, sizeof in);
		in.mesh = g->cockpit3d_meshes[i];
		if (in.mesh->all_materials_single_sided)
			in.cull_mode = AERON_CULL_BACK;
		const float* placed = i >= g->cockpit3d_yoke_first ? g->cockpit3d_yoke_transform : transform;
		memcpy(in.transform, placed, sizeof in.transform);
		memcpy(in.prev_transform, placed, sizeof in.prev_transform);
		in.zero_velocity = 1;
		in.world_light_scale = TIE_FLIGHT_COCKPIT3D_WORLD_LIGHT;
		/* The hidden exterior hull still casts shadows and would black out
		 * the interior, and the cockpit must not shadow the world. */
		in.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW;
		AeronScene_AddMeshInstance(g->scene, &in);
	}
	memcpy(g->cockpit3d_transform, transform, sizeof g->cockpit3d_transform);
	g->cockpit3d_active = true;
}

bool TieFlightRenderer_Cockpit3dActive(const TieFlightRenderer* g) { return g && g->cockpit3d_active; }

const char* TieFlightRenderer_Cockpit3dModel(const TieFlightRenderer* g) {
	return g && g->cockpit3d_active ? g->cockpit3d_model : NULL;
}

const TieDcEffects* TieFlightRenderer_Cockpit3dEffects(const TieFlightRenderer* g, double* seconds) {
	if (!g || !g->cockpit3d_active || !g->cockpit3d_effects.count)
		return NULL;
	*seconds = (double)(Aeron_NowUs() - g->cockpit3d_effects_start_us) / 1e6;
	return &g->cockpit3d_effects;
}

const TieDcLayout* TieFlightRenderer_Cockpit3dLayout(const TieFlightRenderer* g) {
	return g && g->cockpit3d_active && g->cockpit3d_dc_loaded ? &g->cockpit3d_dc : NULL;
}

bool TieFlightRenderer_Cockpit3dProject(const TieFlightRenderer* g, int mesh, const float model[3], float clip[4]) {
	if (!g || !g->cockpit3d_active)
		return false;
	float local[4];
	TieFlightCockpit3d_Transform(TieFlightCockpit3d_OnYoke(g->cockpit3d_yoke, mesh) ? g->cockpit3d_yoke_transform
																					: g->cockpit3d_transform,
								 model, 1.0f, local);
	TieFlightRenderer_ProjectLocal(g, local, clip);
	return clip[3] > 1e-4f;
}

/* General 4x4 inverse (row-major); false when singular. */
static bool TieFlightCockpit3d_Invert(const float m[16], float out[16]) {
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
			 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
			 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
			 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
			  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
			 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
			 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
			 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
			  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
			 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
			 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
			  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
			  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
			 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
			 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
			  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
			  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0.0f)
		return false;
	for (int i = 0; i < 16; ++i)
		out[i] = inv[i] / det;
	return true;
}

static void TieFlightCockpit3d_Transform(const float m[16], const float p[3], float w, float out[4]) {
	for (int r = 0; r < 4; ++r)
		out[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3] * w;
}

void TieFlightRenderer_ProjectLocal(const TieFlightRenderer* g, const float point[3], float clip[4]) {
	TieFlightCockpit3d_Transform(g->frame.fcam.view_proj, point, 1.0f, clip);
}

bool TieFlightRenderer_ScreenRay(const TieFlightRenderer* g, float x, float y, float origin[3], float dir[3]) {
	if (!g || g->rt_w <= 0 || g->rt_h <= 0)
		return false;
	float inv[16];
	if (!TieFlightCockpit3d_Invert(g->frame.fcam.view_proj, inv))
		return false;
	/* NDC y is up; reverse-Z puts the near plane at 1. */
	const float ndc[2] = { 2.0f * x / (float)g->rt_w - 1.0f, 1.0f - 2.0f * y / (float)g->rt_h };
	float near_point[4], mid_point[4];
	TieFlightCockpit3d_Transform(inv, (const float[3]) { ndc[0], ndc[1], 1.0f }, 1.0f, near_point);
	TieFlightCockpit3d_Transform(inv, (const float[3]) { ndc[0], ndc[1], 0.5f }, 1.0f, mid_point);
	if (near_point[3] == 0.0f || mid_point[3] == 0.0f)
		return false;
	float length = 0.0f;
	for (int i = 0; i < 3; ++i) {
		origin[i] = near_point[i] / near_point[3];
		dir[i] = mid_point[i] / mid_point[3] - origin[i];
		length += dir[i] * dir[i];
	}
	length = sqrtf(length);
	if (length <= 0.0f)
		return false;
	for (int i = 0; i < 3; ++i)
		dir[i] /= length;
	return true;
}

bool TieFlightRenderer_Cockpit3dSurfaceAt(const TieFlightRenderer* g, float x, float y, float point[3],
										  float normal[3]) {
	float origin[3], dir[3];
	if (!g || !g->cockpit3d_active || !TieFlightRenderer_ScreenRay(g, x, y, origin, dir))
		return false;
	float best = 1.0e30f;
	bool hit = false;
	for (uint32_t c = 0; c < g->cockpit3d_mesh_count; ++c) {
		const float* m = c >= g->cockpit3d_yoke_first ? g->cockpit3d_yoke_transform : g->cockpit3d_transform;
		const AeronSceneMesh* mesh = g->cockpit3d_meshes[c];
		if (!mesh->cpu_vertices || !mesh->cpu_indices)
			continue;
		for (uint32_t i = 0; i + 2 < mesh->index_count; i += 3) {
			float v[3][4];
			for (int k = 0; k < 3; ++k)
				TieFlightCockpit3d_Transform(m, mesh->cpu_vertices[mesh->cpu_indices[i + k]].pos, 1.0f, v[k]);
			/* Moller-Trumbore. */
			const float e1[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
			const float e2[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
			const float p[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
								 dir[0] * e2[1] - dir[1] * e2[0] };
			const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
			if (fabsf(det) < 1.0e-9f)
				continue;
			const float inv_det = 1.0f / det;
			const float s[3] = { origin[0] - v[0][0], origin[1] - v[0][1], origin[2] - v[0][2] };
			const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv_det;
			if (u < 0.0f || u > 1.0f)
				continue;
			const float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
			const float w = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv_det;
			if (w < 0.0f || u + w > 1.0f)
				continue;
			const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv_det;
			if (t <= 0.0f || t >= best)
				continue;
			best = t;
			hit = true;
			float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			const float n_length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			const float facing = n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2] > 0.0f ? -1.0f : 1.0f;
			for (int k = 0; k < 3; ++k) {
				normal[k] = n_length > 0.0f ? n[k] / n_length * facing : -dir[k];
				point[k] = origin[k] + dir[k] * t;
			}
		}
	}
	return hit;
}
