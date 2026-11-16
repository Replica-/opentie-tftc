#include "tie_remaster/flight/cockpit/dc_effects.h"

#include "aeron/log.h"
#include "xwau_dat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIE_DC_EFFECTS_MAX_NAMES 64
#define TIE_DC_EFFECTS_MAX_FRAMES 64
#define TIE_DC_EFFECTS_MAX_DATS 4

/* A decoded DAT image, uploaded once and shared by every sequence showing it. */
typedef struct TieDcEffectsImage {
	int dat, group, image;
	AeronTexture* texture;
} TieDcEffectsImage;

typedef struct TieDcEffectsBuild {
	TieDcEffects* out;
	AeronCommandBuffer* cmd;
	AeronVfs* vfs;
	const opt_file_t* opt;
	char dat_names[TIE_DC_EFFECTS_MAX_DATS][64];
	XwauDat* dats[TIE_DC_EFFECTS_MAX_DATS];
	int dat_count;
	TieDcEffectsImage* images;
	int image_count, image_capacity;
} TieDcEffectsBuild;

static char* TieDcEffects_Trim(char* text) {
	while (*text == ' ' || *text == '\t')
		++text;
	char* end = text + strlen(text);
	while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		*--end = '\0';
	return text;
}

/* The open DAT that "Effects\<File>.dat" names, read from
 * cockpit3d/Effects/<File>.dat; -1 when it cannot be read. */
static int TieDcEffects_Dat(TieDcEffectsBuild* build, const char* reference) {
	const char* file = strrchr(reference, '\\');
	file = file ? file + 1 : reference;
	for (int i = 0; i < build->dat_count; ++i)
		if (strcmp(build->dat_names[i], file) == 0)
			return build->dats[i] ? i : -1;
	if (build->dat_count >= TIE_DC_EFFECTS_MAX_DATS)
		return -1;
	const int index = build->dat_count++;
	snprintf(build->dat_names[index], sizeof build->dat_names[index], "%s", file);
	char path[128];
	snprintf(path, sizeof path, "cockpit3d/Effects/%s", file);
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!AeronVfs_ReadAll(build->vfs, AERON_VFS_ROOT_RESOURCE, path, 256u * 1024u * 1024u, &bytes, &size)) {
		Aeron_LogWarn("tie.cockpit3d", "%s not found; its effects are not shown", path);
		return -1;
	}
	char error[256];
	build->dats[index] = XwauDat_OpenMemory(bytes, size, path, error, sizeof error);
	if (!build->dats[index]) {
		Aeron_LogWarn("tie.cockpit3d", "%s", error);
		return -1;
	}
	return index;
}

/* Image (group, image) of DAT `dat` as a premultiplied texture, decoded and
 * uploaded the first time it is asked for. */
static AeronTexture* TieDcEffects_Image(TieDcEffectsBuild* build, int dat, int group, int image) {
	for (int i = 0; i < build->image_count; ++i)
		if (build->images[i].dat == dat && build->images[i].group == group && build->images[i].image == image)
			return build->images[i].texture;
	XwauDatImage decoded;
	char error[256];
	if (!XwauDat_LoadImageFrom(build->dats[dat], group, image, &decoded, error, sizeof error)) {
		Aeron_LogWarn("tie.cockpit3d", "%s", error);
		return NULL;
	}
	const size_t texels = (size_t)decoded.width * decoded.height;
	for (size_t i = 0; i < texels; ++i) {
		uint8_t* p = decoded.rgba + i * 4;
		for (int k = 0; k < 3; ++k)
			p[k] = (uint8_t)((p[k] * p[3] + 127) / 255);
	}
	AeronTexture* texture = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = (int)decoded.width,
		.height = (int)decoded.height,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "TFTC cockpit effect",
	});
	if (texture && !Aeron_UploadTextureDataCmd(build->cmd, &(AeronTextureUploadDesc) {
															   .texture = texture,
															   .width = (int)decoded.width,
															   .height = (int)decoded.height,
															   .raw_data = decoded.rgba,
															   .raw_size = (uint32_t)(texels * 4u),
														   })) {
		Aeron_DestroyTexture(texture);
		texture = NULL;
	}
	XwauDat_FreeImage(&decoded);
	if (!texture)
		return NULL;
	if (build->image_count == build->image_capacity) {
		const int capacity = build->image_capacity ? build->image_capacity * 2 : 32;
		TieDcEffectsImage* grown = realloc(build->images, (size_t)capacity * sizeof *grown);
		AeronTexture** textures = realloc(build->out->textures, (size_t)capacity * sizeof *textures);
		if (textures)
			build->out->textures = textures;
		if (!grown || !textures) {
			if (grown)
				build->images = grown;
			Aeron_DestroyTexture(texture);
			return NULL;
		}
		build->images = grown;
		build->image_capacity = capacity;
	}
	build->images[build->image_count++] = (TieDcEffectsImage) { dat, group, image, texture };
	build->out->textures[build->out->texture_count++] = texture;
	return texture;
}

static bool TieDcEffects_AddFrame(TieDcEffects* out, AeronTexture* texture, float seconds) {
	if (!texture || seconds <= 0.0f)
		return false;
	TieDcEffectFrame* grown = realloc(out->frames, (size_t)(out->frame_count + 1) * sizeof *grown);
	if (!grown)
		return false;
	out->frames = grown;
	out->frames[out->frame_count++] = (TieDcEffectFrame) { texture, seconds };
	return true;
}

/* One sequence line (`key` = `value`) of a section naming `names`: adds its
 * frames and one effect for each of the names that has faces. */
static void TieDcEffects_Line(TieDcEffectsBuild* build, const char* key, char* value, char names[][32],
							  int name_count) {
	const bool frames = strncmp(key, "frame_", 6) == 0;
	const bool glow = strncmp(key, "lightmap_", 9) == 0;
	const bool over = strcmp(key, "cover_seq") == 0;
	if (!frames && !glow && !over && strncmp(key, "anim_", 5) != 0)
		return;
	const bool random = strstr(key, "_rand") != NULL;
	if (strstr(key, "_once"))
		return; /* plays on an event */
	/* XWA's parser drops the parentheses, then reads comma-separated fields. */
	char* w = value;
	for (const char* r = value; *r; ++r)
		if (*r != '(' && *r != ')')
			*w++ = *r;
	*w = '\0';
	char* fields[3 + 3 * TIE_DC_EFFECTS_MAX_FRAMES];
	int field_count = 0;
	for (char* p = value; p && field_count < (int)(sizeof fields / sizeof fields[0]);) {
		char* comma = strchr(p, ',');
		if (comma)
			*comma = '\0';
		fields[field_count++] = TieDcEffects_Trim(p);
		p = comma ? comma + 1 : NULL;
	}
	int at = 0;
	if (at < field_count && (strncmp(fields[at], "EVT_", 4) == 0 || strncmp(fields[at], "IEVT_", 5) == 0)) {
		if (strcmp(fields[at], "EVT_NONE") != 0)
			return;
		++at;
	}
	if (at >= field_count)
		return;
	TieDcEffects* out = build->out;
	const int first = out->frame_count;
	if (frames) {
		/* Effects\<File>.dat-<group>, fps: every image of the group in turn. */
		char* dash = strrchr(fields[at], '-');
		if (!dash || at + 1 >= field_count)
			return;
		*dash = '\0';
		const int group = atoi(dash + 1);
		const float fps = strtof(fields[at + 1], NULL);
		const int dat = TieDcEffects_Dat(build, fields[at]);
		if (dat < 0 || fps <= 0.0f)
			return;
		const int* images = NULL;
		const size_t count = XwauDat_ListGroup(build->dats[dat], group, &images);
		int ids[TIE_DC_EFFECTS_MAX_FRAMES];
		const int n = count < TIE_DC_EFFECTS_MAX_FRAMES ? (int)count : TIE_DC_EFFECTS_MAX_FRAMES;
		memcpy(ids, images, (size_t)n * sizeof ids[0]);
		for (int i = 0; i < n; ++i)
			TieDcEffects_AddFrame(out, TieDcEffects_Image(build, dat, group, ids[i]), 1.0f / fps);
	} else {
		/* Effects\<File>.dat, then <group>-<image>, seconds, intensity per frame. */
		const int dat = TieDcEffects_Dat(build, fields[at]);
		if (dat < 0)
			return;
		for (int f = at + 1; f + 1 < field_count; f += 3) {
			int group, image;
			if (sscanf(fields[f], "%d-%d", &group, &image) != 2)
				break;
			TieDcEffects_AddFrame(out, TieDcEffects_Image(build, dat, group, image), strtof(fields[f + 1], NULL));
		}
	}
	const int frame_count = out->frame_count - first;
	if (frame_count <= 0)
		return;
	float seconds = 0.0f;
	for (int i = first; i < out->frame_count; ++i)
		seconds += out->frames[i].seconds;
	for (int n = 0; n < name_count; ++n) {
		TieDcScreen faces;
		if (!TieDcLayout_PlaceTexture(build->opt, names[n], &faces))
			continue;
		TieDcEffect* grown = realloc(out->items, (size_t)(out->count + 1) * sizeof *grown);
		if (!grown) {
			free(faces.triangles);
			return;
		}
		out->items = grown;
		out->items[out->count++] = (TieDcEffect) {
			.faces = faces,
			.glow = glow,
			.over = over,
			.random = random,
			.frame_first = first,
			.frame_count = frame_count,
			.seconds = seconds,
		};
	}
}

int TieDcEffects_Build(TieDcEffects* out, AeronCommandBuffer* cmd, AeronVfs* vfs, char* mat_text,
					   const opt_file_t* opt) {
	memset(out, 0, sizeof *out);
	TieDcEffectsBuild build = { .out = out, .cmd = cmd, .vfs = vfs, .opt = opt };
	char names[TIE_DC_EFFECTS_MAX_NAMES][32];
	int name_count = 0;
	for (char* line = strtok(mat_text, "\n"); line; line = strtok(NULL, "\n")) {
		/* As in XWA's parser, a comment starts in the first column only. */
		if (line[0] == ';' || line[0] == '#')
			continue;
		line = TieDcEffects_Trim(line);
		if (line[0] == '[') {
			name_count = 0;
			for (char* p = line + 1; *p && *p != ']' && name_count < TIE_DC_EFFECTS_MAX_NAMES;) {
				const size_t n = strcspn(p, ",]");
				snprintf(names[name_count++], sizeof names[0], "%.*s", (int)n, p);
				p += n;
				if (*p == ',')
					++p;
			}
			continue;
		}
		char* equals = strchr(line, '=');
		if (!equals || !name_count)
			continue;
		*equals = '\0';
		const char* key = TieDcEffects_Trim(line);
		char* value = TieDcEffects_Trim(equals + 1);
		char* comment = strchr(value, ';');
		if (comment)
			*comment = '\0';
		TieDcEffects_Line(&build, key, value, names, name_count);
	}
	for (int i = 0; i < build.dat_count; ++i)
		XwauDat_Close(build.dats[i]);
	free(build.images);
	return out->count;
}

void TieDcEffects_Free(TieDcEffects* effects) {
	for (int i = 0; i < effects->count; ++i)
		free(effects->items[i].faces.triangles);
	for (int i = 0; i < effects->texture_count; ++i)
		Aeron_DestroyTexture(effects->textures[i]);
	free(effects->items);
	free(effects->frames);
	free(effects->textures);
	memset(effects, 0, sizeof *effects);
}

AeronTexture* TieDcEffects_Frame(const TieDcEffects* effects, const TieDcEffect* effect, double seconds) {
	if (effect->frame_count <= 0 || effect->seconds <= 0.0f)
		return NULL;
	const TieDcEffectFrame* frames = &effects->frames[effect->frame_first];
	if (effect->random) {
		/* A frame picked afresh each time the average frame's time passes. */
		const double slot = seconds / ((double)effect->seconds / (double)effect->frame_count);
		uint32_t hash = (uint32_t)(int64_t)slot * 2654435761u + (uint32_t)effect->frame_first * 40503u;
		hash ^= hash >> 15;
		return frames[hash % (uint32_t)effect->frame_count].texture;
	}
	double at = fmod(seconds, (double)effect->seconds);
	for (int i = 0; i < effect->frame_count; ++i) {
		if (at < (double)frames[i].seconds)
			return frames[i].texture;
		at -= (double)frames[i].seconds;
	}
	return frames[effect->frame_count - 1].texture;
}
