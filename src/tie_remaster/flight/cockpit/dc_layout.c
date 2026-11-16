#include "tie_remaster/flight/cockpit/dc_layout.h"

#include "tie_remaster/flight/cockpit/editor.h"
#include "tie_runtime/snapshot/hud_types.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIE_DC_METERS_PER_UNIT (1600.0f / 65536.0f)

static const struct {
	const char* name;
	TieDcSource source;
} k_sources[] = {
	{ "TARGETING_COMPUTER_SRC", TIE_DC_TARGETING },
	{ "LEFT_TEXT_BOX_SRC", TIE_DC_LEFT_TEXT },
	{ "RIGHT_TEXT_BOX_SRC", TIE_DC_RIGHT_TEXT },
	{ "LEFT_SENSOR_SRC", TIE_DC_LEFT_SENSOR },
	{ "RIGHT_SENSOR_SRC", TIE_DC_RIGHT_SENSOR },
	{ "SHIELDS_SRC", TIE_DC_SHIELDS },
	{ "DUAL_LASERS_L_SRC", TIE_DC_LASERS },
	{ "DUAL_LASERS_R_SRC", TIE_DC_LASERS },
	{ "QUAD_LASERS_LEFT_SRC", TIE_DC_LASERS },
	{ "QUAD_LASERS_RIGHT_SRC", TIE_DC_LASERS },
	{ "MISSILES_SRC", TIE_DC_MISSILES },
	{ "AUTOSIZE_MISSILES_L_SRC", TIE_DC_MISSILES },
	{ "AUTOSIZE_MISSILES_R_SRC", TIE_DC_MISSILES },
	{ "SPEED_THROTTLE_SRC", TIE_DC_SPEED },
	{ "AUTOSIZE_SPEED_SRC", TIE_DC_SPEED },
	{ "BEAM_LEVEL_SRC", TIE_DC_BEAM },
	{ "LASER_RECHARGE_SRC", TIE_DC_LASER_POWER },
	{ "SHIELD_RECHARGE_SRC", TIE_DC_SHIELD_POWER },
	{ "BEAM_RECHARGE_SRC", TIE_DC_BEAM_POWER },
	{ "ENGINE_POWER_SRC", TIE_DC_ENGINE_POWER },
	{ "NAME_TIME_SRC", TIE_DC_CLOCK },
	{ "AUTOSIZE_TIME_SRC", TIE_DC_CLOCK },
	{ "THROTTLE_BAR_SRC", TIE_DC_THROTTLE },
	{ "SHIELDS_FRONT_SRC", TIE_DC_SHIELD_FRONT_TEXT },
	{ "SHIELDS_BACK_SRC", TIE_DC_SHIELD_BACK_TEXT },
	{ "AUTOSIZE_CHAFF_SRC", TIE_DC_CHAFF },
};

TieDcSource TieDcLayout_SourceOf(int id) {
	if (id == TIE_HUDI_CMD_3D_CRT || (id >= TIE_HUDI_TARGET_SUBSYSTEM_PCT && id <= TIE_HUDI_TARGET_SUBSYSTEM_FOCUS) ||
		(id >= 79 && id <= 90))
		return TIE_DC_TARGETING;
	if (id == TIE_HUDI_RADAR_LEFT)
		return TIE_DC_LEFT_SENSOR;
	if (id == TIE_HUDI_RADAR_RIGHT)
		return TIE_DC_RIGHT_SENSOR;
	if (id >= TIE_HUDI_LASER_LED_FIRST && id <= TIE_HUDI_LASER_LED_LAST)
		return TIE_DC_LASERS;
	if ((id >= TIE_HUDI_MISSILE_HP_FIRST && id <= TIE_HUDI_MISSILE_AMMO_LAST) ||
		(id >= TIE_HUDI_WEAPON_FIRE_FIRST && id <= TIE_HUDI_WEAPON_FIRE_LAST))
		return TIE_DC_MISSILES;
	if (id >= TIE_HUDI_SHIELD_FWD_NORMAL && id <= TIE_HUDI_SHIELD_REAR_OVER)
		return TIE_DC_SHIELD_BARS;
	if (id == TIE_HUDI_HULL_DAMAGE_LEVER)
		return TIE_DC_SHIELDS;
	if (id == TIE_HUDI_SPEED_DIGITS)
		return TIE_DC_SPEED;
	if (id == TIE_HUDI_THROTTLE_DIGITS)
		return TIE_DC_THROTTLE;
	if (id == TIE_HUDI_BEAM_ARC || id == TIE_HUDI_BEAM_FIRE)
		return TIE_DC_BEAM;
	switch (id) {
		case TIE_HUDI_POWER_LASERS: return TIE_DC_LASER_POWER;
		case TIE_HUDI_POWER_SHIELDS: return TIE_DC_SHIELD_POWER;
		case TIE_HUDI_POWER_BEAM: return TIE_DC_BEAM_POWER;
		case TIE_HUDI_POWER_BALANCE: return TIE_DC_ENGINE_POWER;
		case TIE_HUDI_CLOCK_DIGITS: return TIE_DC_CLOCK;
		case TIE_COCKPIT_EDITOR_REPAIRS_ID: return TIE_DC_LEFT_TEXT;
		case TIE_COCKPIT_EDITOR_OBJECTIVES_ID: return TIE_DC_RIGHT_TEXT;
		default: return TIE_DC_NONE;
	}
}

static bool TieDcLayout_NameEqual(const char* left, const char* right, size_t right_length) {
	size_t i = 0;
	for (; left[i] && i < right_length; ++i) {
		char a = left[i], b = right[i];
		if (a >= 'a' && a <= 'z')
			a = (char)(a - 'a' + 'A');
		if (b >= 'a' && b <= 'z')
			b = (char)(b - 'a' + 'A');
		if (a != b)
			return false;
	}
	return !left[i] && i == right_length;
}

static char* TieDcLayout_Trim(char* text) {
	while (*text == ' ' || *text == '\t')
		++text;
	char* end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		*--end = '\0';
	return text;
}

/* The scene mesh's model space: OPT axes in metres. */
static void TieDcLayout_ModelPoint(const opt_vec3_t* p, float out[3]) {
	out[0] = p->x * TIE_DC_METERS_PER_UNIT;
	out[1] = p->y * TIE_DC_METERS_PER_UNIT;
	out[2] = p->z * TIE_DC_METERS_PER_UNIT;
}

static float TieDcLayout_Length(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* Collects every LOD 0 triangle textured with `texture` that shows part of
 * the display whose top-left and bottom-right fall on texture coordinates
 * (u0, v0) and (u1, v1), and the display's width / height on the largest. */
static bool TieDcLayout_Place(const opt_file_t* opt, int texture, float u0, float v0, float u1, float v1,
							  TieDcScreen* screen) {
	if (u1 == u0 || v1 == v0)
		return false;
	int capacity = 0;
	float largest = 0.0f;
	for (int m = 0; m < opt->mesh_count; ++m) {
		const opt_mesh_t* mesh = &opt->meshes[m];
		if (mesh->lod_count <= 0)
			continue;
		const opt_lod_t* lod = &mesh->lods[0];
		for (int g = 0; g < lod->group_count; ++g) {
			if (lod->groups[g].texture_index != texture)
				continue;
			for (int f = 0; f < lod->groups[g].face_count; ++f) {
				const opt_face_t* face = &lod->groups[g].faces[f];
				const int corners_in_face = face->verts[3] < 0 ? 3 : 4;
				for (int t = 0; t + 2 < corners_in_face; ++t) {
					const int k[3] = { 0, t + 1, t + 2 };
					TieDcTriangle tri;
					tri.mesh = m;
					float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
					bool valid = true;
					for (int i = 0; i < 3 && valid; ++i) {
						const int vi = face->verts[k[i]], ui = face->uvs[k[i]];
						valid = vi >= 0 && vi < mesh->vertex_count && ui >= 0 && ui < mesh->uv_count;
						if (!valid)
							break;
						TieDcLayout_ModelPoint(&mesh->vertices[vi], tri.position[i]);
						tri.display[i][0] = (mesh->uvs[ui].u - u0) / (u1 - u0);
						tri.display[i][1] = (mesh->uvs[ui].v - v0) / (v1 - v0);
						for (int a = 0; a < 2; ++a) {
							lo[a] = fminf(lo[a], tri.display[i][a]);
							hi[a] = fmaxf(hi[a], tri.display[i][a]);
						}
					}
					/* Skip faces that show none of the display. */
					if (!valid || hi[0] <= 0.0f || lo[0] >= 1.0f || hi[1] <= 0.0f || lo[1] >= 1.0f)
						continue;
					/* Model distance per display unit along each axis, from the
					 * triangle's display-to-model map. */
					const float ds1 = tri.display[1][0] - tri.display[0][0], dt1 = tri.display[1][1] - tri.display[0][1];
					const float ds2 = tri.display[2][0] - tri.display[0][0], dt2 = tri.display[2][1] - tri.display[0][1];
					const float det = ds1 * dt2 - ds2 * dt1;
					if (fabsf(det) < 1e-12f)
						continue;
					float along_s[3], along_t[3];
					for (int a = 0; a < 3; ++a) {
						const float e1 = tri.position[1][a] - tri.position[0][a], e2 = tri.position[2][a] - tri.position[0][a];
						along_s[a] = (e1 * dt2 - e2 * dt1) / det;
						along_t[a] = (e2 * ds1 - e1 * ds2) / det;
					}
					const float area = fabsf(det);
					if (area > largest && TieDcLayout_Length(along_t) > 0.0f) {
						largest = area;
						screen->aspect = TieDcLayout_Length(along_s) / TieDcLayout_Length(along_t);
					}
					if (screen->triangle_count == capacity) {
						capacity = capacity ? capacity * 2 : 16;
						TieDcTriangle* grown = realloc(screen->triangles, (size_t)capacity * sizeof *grown);
						if (!grown)
							return false;
						screen->triangles = grown;
					}
					screen->triangles[screen->triangle_count++] = tri;
				}
			}
		}
	}
	return screen->triangle_count > 0;
}

void TieDcLayout_Free(TieDcLayout* layout) {
	for (int i = 0; i < layout->count; ++i)
		free(layout->screens[i].triangles);
	memset(layout, 0, sizeof *layout);
}

static int TieDcLayout_FindTexture(const opt_file_t* opt, const char* name, size_t length) {
	for (int i = 0; i < opt->texture_count; ++i)
		if (TieDcLayout_NameEqual(opt->textures[i].name, name, length))
			return i;
	return -1;
}

bool TieDcLayout_Build(const char* source_text, const opt_file_t* opt, TieDcLayout* out) {
	memset(out, 0, sizeof *out);
	if (!source_text || !opt)
		return false;
	char* text = malloc(strlen(source_text) + 1);
	if (!text)
		return false;
	strcpy(text, source_text);

	int texture = -1;
	float cover_w = 0.0f, cover_h = 0.0f;
	for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		char* comment = strchr(line, ';');
		if (comment)
			*comment = '\0';
		line = TieDcLayout_Trim(line);
		if (line[0] == '[') {
			/* [Cockpit.opt,TextureName] */
			char* comma = strchr(line, ',');
			char* close = strchr(line, ']');
			texture = comma && close && close > comma
						  ? TieDcLayout_FindTexture(opt, comma + 1, (size_t)(close - comma - 1))
						  : -1;
			continue;
		}
		char* equals = strchr(line, '=');
		if (!equals)
			continue;
		*equals = '\0';
		const char* key = TieDcLayout_Trim(line);
		char* value = TieDcLayout_Trim(equals + 1);
		if (strcmp(key, "xwahacker_fov") == 0) {
			out->fov_deg = strtof(value, NULL);
		} else if (strcmp(key, "cover_texture_size") == 0) {
			sscanf(value, "%f , %f", &cover_w, &cover_h);
		} else if (strcmp(key, "uv_coords") == 0 && texture >= 0 && cover_w > 0.0f && cover_h > 0.0f) {
			char* comma = strchr(value, ',');
			if (!comma)
				continue;
			size_t name_length = (size_t)(comma - value);
			while (name_length && (value[name_length - 1] == ' ' || value[name_length - 1] == '\t'))
				--name_length;
			TieDcSource source = TIE_DC_NONE;
			for (size_t i = 0; i < sizeof k_sources / sizeof k_sources[0]; ++i)
				if (TieDcLayout_NameEqual(k_sources[i].name, value, name_length))
					source = k_sources[i].source;
			float x0, y0, x1, y1;
			if (source == TIE_DC_NONE || out->count >= TIE_DC_MAX_SCREENS ||
				sscanf(comma + 1, " %f , %f , %f , %f", &x0, &y0, &x1, &y1) != 4)
				continue;
			/* The source's top-left lands on (x0, y0) of the texture. */
			TieDcScreen* screen = &out->screens[out->count];
			memset(screen, 0, sizeof *screen);
			if (TieDcLayout_Place(opt, texture, x0 / cover_w, y0 / cover_h, x1 / cover_w, y1 / cover_h, screen)) {
				screen->source = source;
				++out->count;
			} else {
				free(screen->triangles);
				memset(screen, 0, sizeof *screen);
			}
		}
	}
	free(text);
	return out->count > 0;
}
