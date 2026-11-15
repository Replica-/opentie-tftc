#include "tie_remaster/flight/tftc_skybox.h"

#include "aeron/image.h"
#include "aeron/log.h"
#include "aeron/vfs.h"
#include "tie_remaster/flight/renderer_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Aeron's own stb_image decodes PNG only; this private copy adds JPEG. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* The originals are 4096 px; the copy script scales them to 2048. */
#define TIE_FLIGHT_TFTC_SKYBOX_MAX_FACE 2048
#define TIE_FLIGHT_TFTC_SKYBOX_MAX_BYTES (64u * 1024u * 1024u)

typedef struct TieFlightTftcSkyboxDef {
	char cube[64];
	float rotation_deg[3];
} TieFlightTftcSkyboxDef;

/* "b1m3fm" -> "b1m3": TFTC keys its missions by battle and mission number. */
static bool TieFlightTftcSkybox_MissionKey(const char* mission_name, char* out, size_t capacity) {
	int battle = 0, mission = 0, consumed = 0;
	if (sscanf(mission_name, "b%dm%d%n", &battle, &mission, &consumed) != 2 || consumed == 0)
		return false;
	return snprintf(out, capacity, "b%dm%d", battle, mission) < (int)capacity;
}

static char* TieFlightTftcSkybox_Trim(char* text) {
	while (*text == ' ' || *text == '\t')
		++text;
	char* end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return text;
}

/* The cube of AllRegions or, failing that, Region0 (TIE missions have one
 * region), and its rotation. */
static bool TieFlightTftcSkybox_Parse(char* text, TieFlightTftcSkyboxDef* out) {
	TieFlightTftcSkyboxDef all = { 0 }, region0 = { 0 };
	for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		char* equals = strchr(line, '=');
		if (!equals || line[0] == ';' || line[0] == '[')
			continue;
		*equals = '\0';
		const char* key = TieFlightTftcSkybox_Trim(line);
		const char* value = TieFlightTftcSkybox_Trim(equals + 1);
		TieFlightTftcSkyboxDef* def = strncmp(key, "AllRegions", 10) == 0 ? &all
									  : strncmp(key, "Region0", 7) == 0	  ? &region0
																		  : NULL;
		if (!def)
			continue;
		const char* suffix = key + (def == &all ? 10 : 7);
		if (!*suffix) {
			/* Effects\CubeMaps\<Name>: only the name is kept. */
			const char* name = value;
			for (const char* cursor = value; *cursor; ++cursor)
				if (*cursor == '\\' || *cursor == '/')
					name = cursor + 1;
			snprintf(def->cube, sizeof def->cube, "%s", name);
		} else if (strncmp(suffix, "Rotation", 8) == 0 && suffix[8] >= 'X' && suffix[8] <= 'Z' && !suffix[9]) {
			def->rotation_deg[suffix[8] - 'X'] = strtof(value, NULL);
		}
	}
	*out = all.cube[0] ? all : region0;
	return out->cube[0] != '\0';
}

static bool TieFlightTftcSkybox_ReadText(TieFlightRenderer* g, const char* path, char** out) {
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path, 64u * 1024u, &bytes, &size))
		return false;
	char* text = malloc(size + 1);
	if (text) {
		memcpy(text, bytes, size);
		text[size] = '\0';
	}
	free(bytes);
	*out = text;
	return text != NULL;
}

/* RGBA pixels of one face, scaled down to TIE_FLIGHT_TFTC_SKYBOX_MAX_FACE. */
static uint8_t* TieFlightTftcSkybox_LoadFace(TieFlightRenderer* g, const char* cube, int face, int* size) {
	static const char* const extensions[] = { "jpg", "png" };
	for (size_t e = 0; e < sizeof extensions / sizeof extensions[0]; ++e) {
		char path[160];
		snprintf(path, sizeof path, "tftc/skyboxes/cubemaps/%s/%s.%d.%s", cube, cube, face, extensions[e]);
		uint8_t* bytes = NULL;
		size_t length = 0;
		if (!AeronVfs_ReadAll(g->assets->vfs, AERON_VFS_ROOT_RESOURCE, path, TIE_FLIGHT_TFTC_SKYBOX_MAX_BYTES, &bytes,
							  &length))
			continue;
		int w = 0, h = 0, channels = 0;
		uint8_t* pixels = stbi_load_from_memory(bytes, (int)length, &w, &h, &channels, 4);
		free(bytes);
		if (!pixels || w != h) {
			Aeron_LogWarn("tie.tftc", "%s: %s", path, pixels ? "face is not square" : stbi_failure_reason());
			stbi_image_free(pixels);
			return NULL;
		}
		while (w > TIE_FLIGHT_TFTC_SKYBOX_MAX_FACE) {
			int half_w = 0, half_h = 0;
			uint8_t* half = Aeron_ImageDownsampleRgba8(pixels, w, h, &half_w, &half_h);
			stbi_image_free(pixels);
			if (!half)
				return NULL;
			/* Downsampled images are malloc-owned; stb's free is free. */
			pixels = half;
			w = half_w;
			h = half_h;
		}
		*size = w;
		return pixels;
	}
	Aeron_LogWarn("tie.tftc", "skybox %s: face %d not found", cube, face);
	return NULL;
}

/* XWA's RotationX/Y/Z, applied X then Y then Z, as a row-major 3x3. */
static void TieFlightTftcSkybox_Rotation(const float deg[3], float out[9]) {
	const float k = 3.14159265f / 180.0f;
	const float cx = cosf(deg[0] * k), sx = sinf(deg[0] * k);
	const float cy = cosf(deg[1] * k), sy = sinf(deg[1] * k);
	const float cz = cosf(deg[2] * k), sz = sinf(deg[2] * k);
	const float rx[9] = { 1, 0, 0, 0, cx, -sx, 0, sx, cx };
	const float ry[9] = { cy, 0, sy, 0, 1, 0, -sy, 0, cy };
	const float rz[9] = { cz, -sz, 0, sz, cz, 0, 0, 0, 1 };
	float ryx[9];
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) {
			ryx[r * 3 + c] = ry[r * 3 + 0] * rx[0 * 3 + c] + ry[r * 3 + 1] * rx[1 * 3 + c] + ry[r * 3 + 2] * rx[2 * 3 + c];
		}
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			out[r * 3 + c] = rz[r * 3 + 0] * ryx[0 * 3 + c] + rz[r * 3 + 1] * ryx[1 * 3 + c] + rz[r * 3 + 2] * ryx[2 * 3 + c];
}

void TieFlightTftcSkybox_Release(TieFlightRenderer* g) {
	if (g->tftc_skybox) {
		Aeron_DestroyTexture(g->tftc_skybox);
		g->tftc_skybox = NULL;
	}
	g->tftc_skybox_mission[0] = '\0';
}

void TieFlightTftcSkybox_Refresh(TieFlightRenderer* g, AeronCommandBuffer* cmd, const char* mission_name) {
	if (!g || !cmd || !mission_name || strcmp(mission_name, g->tftc_skybox_mission) == 0)
		return;
	TieFlightTftcSkybox_Release(g);
	snprintf(g->tftc_skybox_mission, sizeof g->tftc_skybox_mission, "%s", mission_name);

	char key[16], path[64];
	if (!TieFlightTftcSkybox_MissionKey(mission_name, key, sizeof key))
		return;
	snprintf(path, sizeof path, "tftc/skyboxes/%s.ini", key);
	char* text = NULL;
	if (!TieFlightTftcSkybox_ReadText(g, path, &text))
		return;
	TieFlightTftcSkyboxDef def;
	const bool parsed = TieFlightTftcSkybox_Parse(text, &def);
	free(text);
	if (!parsed)
		return;

	uint8_t* faces[6] = { 0 };
	int size = 0;
	bool ok = true;
	for (int face = 0; face < 6 && ok; ++face) {
		int face_size = 0;
		faces[face] = TieFlightTftcSkybox_LoadFace(g, def.cube, face, &face_size);
		ok = faces[face] && (face == 0 || face_size == size);
		size = face_size;
	}
	if (ok) {
		g->tftc_skybox = Aeron_CreateTexture(&(AeronTextureDesc) {
			.width = size,
			.height = size,
			.mip_count = 1,
			.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
			.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
			.cube = 1,
			.debug_name = "TFTC skybox",
		});
		for (int face = 0; face < 6 && g->tftc_skybox; ++face) {
			if (!Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
													 .texture = g->tftc_skybox,
													 .width = size,
													 .height = size,
													 .raw_data = faces[face],
													 .raw_size = (uint32_t)size * (uint32_t)size * 4u,
													 .layer = face,
												 })) {
				Aeron_DestroyTexture(g->tftc_skybox);
				g->tftc_skybox = NULL;
			}
		}
	}
	for (int face = 0; face < 6; ++face)
		free(faces[face]);
	if (!g->tftc_skybox) {
		Aeron_LogWarn("tie.tftc", "skybox %s for %s could not be loaded", def.cube, key);
		return;
	}
	TieFlightTftcSkybox_Rotation(def.rotation_deg, g->tftc_skybox_rotation);
	Aeron_LogInfo("tie.tftc", "%s uses skybox %s (%d px, rotation %.0f %.0f %.0f)", key, def.cube, size,
				  def.rotation_deg[0], def.rotation_deg[1], def.rotation_deg[2]);
}
