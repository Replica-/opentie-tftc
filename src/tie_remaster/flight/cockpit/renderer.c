/* 2D cockpit + HUD overlay. GLB mode loads catalog assets; original modes
 * decode the selected installation's cockpit files into the same cache. */

#include "tie_remaster/flight/cockpit/renderer.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/text2d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aeron/scene/image_cache.h"
#include "aeron/scene/ktx2_reader.h"
#include "aeron/scene/runtime_atlas.h"
#include "aeron/scene/world.h"
#include "tie_formats/cockpit.h"
#include "tie_formats/cockpit_font.h"
#include "tie_formats/cockpit_masks.h"
#include "tie_formats/panel.h"
#include "tie_remaster/flight/cockpit/common.h"
#include "tie_remaster/flight/cockpit/dc_effects.h"
#include "tie_remaster/flight/cockpit/editor.h"
#include "tie_remaster/flight/cockpit/layout.h"
#include "tie_remaster/flight/cockpit/text.h"
#include "tie_remaster/flight/hud_geometry.h"
#include "tie_remaster/flight/mesh_draw.h"
#include "tie_remaster/flight/render_math.h"
#include "tie_remaster/flight/renderer.h"
#include "tie_remaster/gpu_debug.h"
#include "tie_remaster/scene2d/srgb_math.h"
#include "tie_remaster/scene2d/text.h"
#include "tie_runtime/flight_assets/assets.h"
#include "tie_runtime/flight_assets/source.h"
#include "tie_runtime/presentation/presentation.h"
#include "tie_runtime/snapshot/snapshot.h"

/* Repairs HUD default place and layout, cockpit px (right side, below the rear radar). */
#define TIE_COCKPIT_REPAIRS_X 582.0f
#define TIE_COCKPIT_REPAIRS_Y 150.0f
#define TIE_COCKPIT_REPAIRS_W 160.0f
#define TIE_COCKPIT_REPAIRS_LINE_H 11.0f
#define TIE_COCKPIT_REPAIRS_VALUE_X 136.0f
/* Objectives HUD default place and layout (left side, below the front radar). */
#define TIE_COCKPIT_OBJECTIVES_X -94.0f
#define TIE_COCKPIT_OBJECTIVES_Y 130.0f
#define TIE_COCKPIT_OBJECTIVES_W 300.0f
#define TIE_COCKPIT_OBJECTIVES_LINE_H 11.0f
#define TIE_COCKPIT_OBJECTIVES_INDENT 8.0f
#define TIE_COCKPIT_OBJECTIVE_ROWS 64
#define TIE_COCKPIT_MAX_SURFACES 32
/* Draw records per cockpit screen: a text screen (objectives, system status)
 * takes one per glyph. */
#define TIE_COCKPIT_SURFACE_RECORDS 8192

#define COCKPIT_MAX_CACHED 8
#define COCKPIT_VIEW_NAME_MAX 16

/* The chrome cache RT keeps a real alpha channel regardless of the scene
 * rt_format: the canopy cutout is per-texel alpha within the (overflowing)
 * coverage rects, not geometry. The scene chain (color/bloom/mb/PIP) carries
 * no alpha consumer and runs at the packed HDR format for bandwidth, so the
 * two formats are deliberately decoupled. */
#define COCKPIT_CHROME_FORMAT AERON_TEXTURE_FORMAT_RGBA16_FLOAT

/* CMD CRT mask cache — 7 SVGA variants + 4 VGA variants = 11 max. */
#define COCKPIT_CRT_MASK_MAX 11

#ifndef TIE_SHADER_DIR
#define TIE_SHADER_DIR "shaders"
#endif

/* TieCockpitText_RemapColor + the COCKPIT_BG_* defines live in cockpit_text.h;
 * TieCockpitCommon_InstrumentActive + TieCockpitCommon_IsSvga live in cockpit_common.h. */

/* Atlas-cel defaults; overridden by hud_layout.yaml when present. */
#define COCKPIT_CEL_W 32
#define COCKPIT_CEL_H 32
#define COCKPIT_CELS_PER_ROW 16

/* One opaque-region rectangle in base-image pixel coordinates. */
typedef TieCockpitCoverageRect TieCockpitRendererCoverRect;

typedef struct TieCockpitRendererEntry {
	char view_name[COCKPIT_VIEW_NAME_MAX];
	char parts_basename[10];
	uint16_t parts_shape_count;
	uint16_t classic_w;
	uint16_t classic_h;
	AeronTexture* base_tex; /* canopy bitmap (full screen) */
	int base_w, base_h;
	/* Opaque-region rects avoid rasterizing the transparent canopy opening. */
	TieCockpitRendererCoverRect* cover_rects;
	int cover_count;
	int cover_img_w, cover_img_h;
	bool tried_cover;
	AeronTexture* parts_tex; /* HUD shape atlas */
	int parts_w, parts_h;
	AeronTexture* damage_tex; /* additive crack overlay */
	int damage_w, damage_h;
	TieCockpitLayout layout; /* per-craft HUD parts layout */
	bool tried_layout;
	bool tried_base;
	bool tried_parts;
	bool tried_damage;
	AeronRuntimeAtlas original_parts;
	bool original_assets;
} TieCockpitRendererEntry;

/* One cached PIP CRT mask: the (variant, classic_w) selector matches
 * the snapshot's TieCockpitState.mask_variant + classic_w fields; the
 * texture is the 8-bit alpha cutout cockpit_extract baked into
 * <remaster_dir>/flight/cockpits/crt_mask_<v>_<res>.ktx2. `tried`
 * memoises a failed load so the per-frame draw doesn't repeat file I/O. */
typedef struct TieCockpitRendererCrtMaskEntry {
	uint8_t variant;
	uint16_t classic_w;
	AeronTexture* tex;
	int tex_w, tex_h;
	bool tried;
} TieCockpitRendererCrtMaskEntry;

typedef struct TieCockpitRendererDrawSpace {
	int target_w, target_h;
	int coord_w, coord_h;
	float fit_x, fit_y, fit_w, fit_h;
	float scale_x, scale_y;
} TieCockpitRendererDrawSpace;

struct TieCockpitRenderer {
	const TieFlightAssetSource* assets;
	/* The flight scene RT format (flight_gpu's rt_format, R11G11B10_UFLOAT):
	 * the per-frame color RT the cockpit composites into, and the format of
	 * the PIP RT. Cockpit composite/widget/text pipelines target it so they
	 * validate against the flight scene pass. The chrome cache does NOT use
	 * this — it needs alpha for the canopy cutout and carries its own
	 * COCKPIT_CHROME_FORMAT (RGBA16F). Cockpit pixels sit in linear HDR
	 * alongside the 3D scene; bloom samples them and present tonemaps them. */
	AeronTextureFormat rt_format;
	/* Depth-stencil format of the render pass cockpit drawing happens
	 * inside. Set at init to D32_FLOAT — cockpit is always called
	 * within the flight-main pass, which has flight's depth attachment
	 * bound (even though cockpit pipelines have depth test/write off).
	 * Threaded through to gpu_pipeline_get_with_depth so the pipeline's
	 * target_info attachment count matches the actual pass. */
	AeronTextureFormat depth_format;
	/* Cached state from the last successful TieCockpitRenderer_Prepare,
	 * consumed by the matching TieCockpitRenderer_DrawInPass. Stays valid
	 * until the next prepare() call. */
	const TieCockpitRendererEntry* pending_entry;
	bool pending_ready;
	struct TieScene2dTextRenderer* text_renderer;
	/* The proportional font of the TFTC cockpit screens, standing in for
	 * XWA's; unloaded leaves those screens without text. */
	AeronFontAtlas dc_font;
	bool dc_hologram; /* the DC text box being drawn is a hologram: larger lines fill its panel */
	struct TieFlightRenderer* flight_gpu; /* borrowed; for PIP pre-pass */
	AeronDrawList2D* chrome_bake_list;
	AeronDrawList2D* before_pip_list;
	AeronDrawList2D* after_pip_list;
	AeronDrawList2D* record_list;
	TieCockpitRendererDrawSpace pending_space;
	AeronShader* pip_vs;
	AeronShader* pip_fs;
	AeronGraphicsPipeline* pip_pipeline;
	AeronSampler* pip_color_sampler;
	AeronSampler* pip_mask_sampler; /* NEAREST — hard scanline edges */
	/* Asset reads belong to preparation. This counter makes accidental
	 * draw-phase I/O visible without changing the rendering path. */
	uint32_t draw_phase_asset_io_count;
	bool draw_phase_active;
	/* Chrome cache: the static cockpit layer (base bitmap + damage cracks
	 * + subsystem covers) pre-rendered into one alpha-bearing RT, so the
	 * per-frame path replaces 1 base blit + N crack/cover blits with a
	 * single composite. Baked in TieCockpitRenderer_Prepare (its own render pass,
	 * outside the flight-main pass) only when chrome_key changes — view /
	 * craft switch, mirror toggle, or a subsystem knockout/repair. The RT
	 * is screen-aligned (full rt_w×rt_h, fit viewport applied at bake) so
	 * the composite is a 1:1 sample, restricted to the base coverage rects
	 * to keep the canopy opening from being rasterised. rt_format carries
	 * a real alpha channel (RGBA16F) — the canopy cutout lives in the
	 * per-texel alpha within the (overflowing) coverage rects, not in the
	 * coarse rect geometry. */
	AeronRenderTarget* chrome_rt;
	int chrome_w, chrome_h;
	bool chrome_valid;
	struct {
		const TieCockpitRendererEntry* entry;
		uint16_t view_idx;
		uint16_t mirrored_view;
		uint16_t installed_subsystems;
		uint16_t working_subsystems;
		uint16_t subsystem_active;
		uint8_t covers_allowed;
		int coord_w, coord_h;
		int rt_w, rt_h;
	} chrome_key;
	/* Cockpit-editor layout of this frame's instruments, offsets included. */
	float editor_anchor_x[TIE_COCKPIT_EDITOR_ID_COUNT], editor_anchor_y[TIE_COCKPIT_EDITOR_ID_COUNT];
	float editor_extent_w[TIE_COCKPIT_EDITOR_ID_COUNT], editor_extent_h[TIE_COCKPIT_EDITOR_ID_COUNT];
	float editor_pivot_x[TIE_COCKPIT_EDITOR_ID_COUNT], editor_pivot_y[TIE_COCKPIT_EDITOR_ID_COUNT];
	bool editor_active[TIE_COCKPIT_EDITOR_ID_COUNT];
	/* Editor groups drawn onto the cockpit surface: each group renders into
	 * its own target, then a perspective quad lays it on the surface plane. */
	struct TieCockpitRendererSurface {
		int group; /* -1 = free */
		AeronRenderTarget* rt;
		int w, h;
		AeronDrawList2D* list;
		float x0, y0, x1, y1; /* group box, cockpit px */
		int screen; /* TFTC DC screen it is drawn on, -1 = an editor group */
		float brightness; /* of the DC screen laid on the cockpit, 0..1 */
		TieCockpitRendererDrawSpace space;
		bool live; /* recorded this frame */
	} surfaces[TIE_COCKPIT_MAX_SURFACES];
	/* record_list / pending_space saved while drawing into a surface. */
	AeronDrawList2D* surface_saved_list;
	TieCockpitRendererDrawSpace surface_saved_space;
	/* Objectives HUD laid out for this frame (TieCockpitRenderer_LayoutObjectives). */
	TieUIText objective_recs[TIE_COCKPIT_OBJECTIVE_ROWS];
	int objective_rec_count;
	/* Editor scale and rotation of the widget being drawn, about its group's pivot (cockpit px). */
	struct {
		bool active;
		float pivot_x, pivot_y, scale_x, scale_y, cos_a, sin_a;
	} widget_xf;
	TieCockpitRendererEntry entries[COCKPIT_MAX_CACHED];
	int entry_count;
	TieCockpitRendererCrtMaskEntry crt_masks[COCKPIT_CRT_MASK_MAX];
	int crt_mask_count;
	uint8_t original_vga_palette[576];
	bool original_vga_palette_loaded;
	uint16_t original_font_width;
	uint8_t original_micro_zero_advance;
	float original_font_atlas_scale;
};

static AeronTexture* TieCockpitRenderer_LoadKtx2(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												 const char* path, int* out_w, int* out_h);
static bool TieCockpitRenderer_CockpitAssetErrorImpl(TieCockpitRenderer* cg, const char* path,
													 const char* detail);
#define TIE_COCKPIT_ASSET_ERROR(path, detail) TieCockpitRenderer_CockpitAssetErrorImpl(cg, (path), (detail))

/* Load a compiled shader by name. Application shaders are cooked into Aeron's
 * single shader root (see TIE_SHADER_BINARY_DIR); Aeron_CreateShader
 * resolves <basename>.msl there alongside Aeron's own. */
static AeronShader* TieCockpitRenderer_CompileShader(const char* basename, AeronShaderStage stage,
													 uint32_t num_samplers, uint32_t num_uniform_buffers) {
	AeronShader* sh = Aeron_CreateShader(&(AeronShaderDesc) {
		.name = basename,
		.stage = stage,
		.sampler_count = num_samplers,
		.uniform_buffer_count = num_uniform_buffers,
	});
	if (!sh)
		Aeron_LogError("tie.cockpit", "shader load failed: %s", basename);
	return sh;
}

/* PMA-blend pipeline driven by SV_VertexID for the 4-vertex trianglestrip
 * — no vertex buffer. Mask sampler is NEAREST to keep the engine's
 * scanline-hard cutout edges; color sampler borrowed at draw time. */
static bool TieCockpitRenderer_CockpitInitPipPipeline(TieCockpitRenderer* cg) {
	cg->pip_vs = TieCockpitRenderer_CompileShader("cockpit_pip_compose.vert", AERON_SHADER_STAGE_VERTEX,
												  /*samplers=*/0,
												  /*uniforms=*/1);
	cg->pip_fs = TieCockpitRenderer_CompileShader("cockpit_pip_compose.frag", AERON_SHADER_STAGE_FRAGMENT,
												  /*samplers=*/2,
												  /*uniforms=*/0);
	if (!cg->pip_vs || !cg->pip_fs) {
		Aeron_LogError("tie.cockpit", "PIP shaders unavailable (vs=%p fs=%p, shader_dir=%s)",
					   (void*)cg->pip_vs, (void*)cg->pip_fs, TIE_SHADER_DIR);
		return false;
	}

	cg->pip_mask_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_NEAREST,
		.mag_filter = AERON_FILTER_NEAREST,
		.mip_filter = AERON_FILTER_NEAREST,
		.address_u = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w = AERON_ADDRESS_CLAMP_TO_EDGE,
		.min_lod = 0.0f,
		.max_lod = 1000.0f,
	});
	if (!cg->pip_mask_sampler)
		return false;

	cg->pip_color_sampler = Aeron_CreateSampler(&(AeronSamplerDesc) {
		.min_filter = AERON_FILTER_LINEAR,
		.mag_filter = AERON_FILTER_LINEAR,
		.mip_filter = AERON_FILTER_LINEAR,
		.address_u = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_v = AERON_ADDRESS_CLAMP_TO_EDGE,
		.address_w = AERON_ADDRESS_CLAMP_TO_EDGE,
		.min_lod = 0.0f,
		.max_lod = 1000.0f,
	});
	if (!cg->pip_color_sampler)
		return false;

	/* Depth test/write disabled; the attachment is declared only so the
	 * pipeline's target state matches the flight-main pass we're bound
	 * inside (backend validation rule). */
	AeronGraphicsPipelineDesc info = {
        .vertex_shader   = cg->pip_vs,
        .fragment_shader = cg->pip_fs,
        .primitive_type  = AERON_PRIMITIVE_TRIANGLE_STRIP,
        .cull_mode       = AERON_CULL_NONE,
        .color_format    = cg->rt_format,
        .depth_format    = AERON_TEXTURE_FORMAT_D32_FLOAT,
        .sample_count    = AERON_SAMPLE_COUNT_1,
        .blend = {
            .enabled   = 1,
            .src_color = AERON_BLEND_ONE,
            .dst_color = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
            .color_op  = AERON_BLEND_OP_ADD,
            .src_alpha = AERON_BLEND_ONE,
            .dst_alpha = AERON_BLEND_ONE_MINUS_SRC_ALPHA,
            .alpha_op  = AERON_BLEND_OP_ADD,
        },
    };
	cg->pip_pipeline = Aeron_CreateGraphicsPipeline(&info);
	if (!cg->pip_pipeline) {
		Aeron_LogError("tie.cockpit", "PIP pipeline creation failed");
		return false;
	}
	return true;
}

static const TieFlightAssetBundle* TieCockpitRenderer_Catalog(const TieCockpitRenderer* cg) {
	return cg && TieFlightAssetSource_IsRemastered(cg->assets) ? cg->assets->catalog : NULL;
}

/* Resolve a catalog path below ASSET/remaster. */
static bool TieCockpitRenderer_ComposeFlightPath(const TieCockpitRenderer* cg, const char* rel, char* out,
												 size_t out_cap) {
	const TieFlightAssetBundle* catalog = TieCockpitRenderer_Catalog(cg);
	if (!catalog || !rel || !rel[0])
		return false;
	const char* prefix = TieFlightAssets_ContentPrefix(catalog);
	int n = snprintf(out, out_cap, "%s/%s", prefix, rel);
	return n > 0 && (size_t)n < out_cap;
}

/* Resolve or load the required CRT mask before opening the render pass. */
static AeronTexture* TieCockpitRenderer_EnsureCrtMask(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
													  uint8_t variant, uint16_t classic_w) {
	if (!cg || !cg->assets)
		return NULL;
	if (!TieFlightAssetSource_IsRemastered(cg->assets)) {
		for (int index = 0; index < cg->crt_mask_count; ++index)
			if (cg->crt_masks[index].variant == variant && cg->crt_masks[index].classic_w == classic_w)
				return cg->crt_masks[index].tex;
		if (cg->crt_mask_count >= COCKPIT_CRT_MASK_MAX) {
			TIE_COCKPIT_ASSET_ERROR("compiled CRT mask", "cache capacity exceeded");
			return NULL;
		}
		const uint8_t* data = NULL;
		size_t size = 0;
		data = TieCockpitMaskData_Get(variant, classic_w, &size);
		if (!data) {
			TIE_COCKPIT_ASSET_ERROR("compiled CRT mask", "selected variant is unavailable");
			return NULL;
		}
		uint8_t* rgba = NULL;
		int width = 0, height = 0;
		TieFormatError error = { 0 };
		if (!TieCockpitCrtMask_Decode(data, size, &rgba, &width, &height, &error)) {
			TIE_COCKPIT_ASSET_ERROR("compiled CRT mask", error.message);
			return NULL;
		}
		TieCockpitRendererCrtMaskEntry* mask = &cg->crt_masks[cg->crt_mask_count++];
		mask->variant = variant;
		mask->classic_w = classic_w;
		mask->tex_w = width;
		mask->tex_h = height;
		mask->tried = true;
		mask->tex = Aeron_ImageUploadRgba8(cmd, rgba, width, height, (size_t)width * 4,
										   AERON_TEXTURE_FORMAT_RGBA8_UNORM, AERON_COLOR_SPACE_LINEAR_SRGB,
										   AERON_IMAGE_ALPHA_STRAIGHT, false, "TIE original CRT mask");
		free(rgba);
		if (!mask->tex)
			TIE_COCKPIT_ASSET_ERROR("compiled CRT mask", "GPU upload failed");
		return mask->tex;
	}
	const TieFlightAssetBundle* catalog = TieCockpitRenderer_Catalog(cg);
	if (!catalog)
		return NULL;

	/* Cached? */
	for (int i = 0; i < cg->crt_mask_count; ++i) {
		TieCockpitRendererCrtMaskEntry* mask = &cg->crt_masks[i];
		if (mask->variant == variant && mask->classic_w == classic_w) {
			if (mask->tex)
				return mask->tex;
			if (mask->tried) {
				TIE_COCKPIT_ASSET_ERROR("CRT mask", "previous upload failed");
				return NULL;
			}
			const TieFlightAssetCrtMask* cm = TieFlightAssets_CrtMask(catalog, variant, classic_w);
			char path[1024];
			if (cm && TieCockpitRenderer_ComposeFlightPath(cg, cm->path, path, sizeof path)) {
				mask->tex = TieCockpitRenderer_LoadKtx2(cg, cmd, path, &mask->tex_w, &mask->tex_h);
			}
			mask->tried = true;
			if (!mask->tex)
				TIE_COCKPIT_ASSET_ERROR(cm ? cm->path : "CRT mask", "catalog path or texture is invalid");
			return mask->tex;
		}
	}
	if (cg->crt_mask_count >= COCKPIT_CRT_MASK_MAX) {
		TIE_COCKPIT_ASSET_ERROR("CRT mask", "cache capacity exceeded");
		return NULL;
	}

	const TieFlightAssetCrtMask* cm = TieFlightAssets_CrtMask(catalog, variant, classic_w);
	if (!cm) {
		char identity[96];
		snprintf(identity, sizeof identity, "CRT mask variant %u at width %u", (unsigned)variant,
				 (unsigned)classic_w);
		TIE_COCKPIT_ASSET_ERROR(identity, "catalog has no cockpit_masks row");
		return NULL;
	}

	TieCockpitRendererCrtMaskEntry* mask = &cg->crt_masks[cg->crt_mask_count++];
	mask->variant = variant;
	mask->classic_w = classic_w;
	mask->tried = true;
	char path[1024];
	if (!TieCockpitRenderer_ComposeFlightPath(cg, cm->path, path, sizeof path))
		TIE_COCKPIT_ASSET_ERROR(cm->path, "path is invalid");
	else {
		mask->tex = TieCockpitRenderer_LoadKtx2(cg, cmd, path, &mask->tex_w, &mask->tex_h);
		if (!mask->tex)
			TIE_COCKPIT_ASSET_ERROR(path, "texture upload failed");
	}
	return mask->tex;
}

/* KTX2 → 2D Aeron texture upload. Cockpit assets are flat 2D so this
 * thin loader skips the cubemap/asset-cache machinery elsewhere. */
static AeronTexture* TieCockpitRenderer_LoadKtx2(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												 const char* path, int* out_w, int* out_h) {
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (cg && cg->draw_phase_active)
		++cg->draw_phase_asset_io_count;
	if (!cg || !cg->assets ||
		!AeronVfs_ReadAll(cg->assets->vfs, AERON_VFS_ROOT_ASSET, path, 64u * 1024u * 1024u, &bytes, &size))
		return NULL;
	Ktx2* k = ktx2_open_mem(bytes, size, path);
	if (!k) {
		free(bytes);
		return NULL;
	}
	if (ktx2_face_count(k) != 1) {
		Aeron_LogWarn("tie.assets", "%s: expected a 2D texture (faces=%d)", path, ktx2_face_count(k));
		ktx2_close(k);
		free(bytes);
		return NULL;
	}

	AeronTexture* tex = Aeron_ImageUploadKtx2(cmd, k, path);
	if (!tex)
		Aeron_RequestFatalRendererError("cockpit texture upload");
	if (tex) {
		if (out_w)
			*out_w = ktx2_width(k);
		if (out_h)
			*out_h = ktx2_height(k);
	}
	ktx2_close(k);
	free(bytes);
	return tex;
}

/* ===== Cache lifecycle ===== */

TieCockpitRenderer* TieCockpitRenderer_Init(AeronCommandBuffer* cmd, const TieFlightAssetSource* assets,
											uint32_t rt_format_u32,
											struct TieScene2dTextRenderer* text_renderer) {
	if (!cmd) {
		Aeron_RequestFatalRendererError("cockpit startup command buffer");
		return NULL;
	}

	TieCockpitRenderer* cg = (TieCockpitRenderer*)calloc(1, sizeof *cg);
	if (!cg) {
		Aeron_RequestFatalRendererError("cockpit renderer allocation");
		return NULL;
	}
	cg->assets = assets;
	cg->rt_format = (AeronTextureFormat)rt_format_u32;
	/* Cockpit always renders inside the flight-main render pass,
	 * which binds flight's D32_FLOAT depth attachment. Pipelines used
	 * by cockpit's drawers declare this depth attachment so backend
	 * validation accepts the bind. */
	cg->depth_format = AERON_TEXTURE_FORMAT_D32_FLOAT;
	cg->text_renderer = text_renderer;
	if (assets && assets->vfs &&
		!AeronFontAtlas_LoadVfs(&cg->dc_font, cmd, assets->vfs, AERON_VFS_ROOT_RESOURCE, "assets/fonts/menu",
								8u * 1024u * 1024u))
		Aeron_LogWarn("tie.cockpit", "assets/fonts/menu could not be loaded; TFTC cockpit screens have no text");

	cg->chrome_bake_list = AeronDrawList_Create(512);
	cg->before_pip_list = AeronDrawList_Create(16384);
	cg->after_pip_list = AeronDrawList_Create(16384);
	if (!cg->chrome_bake_list || !cg->before_pip_list || !cg->after_pip_list) {
		Aeron_RequestFatalRendererError("cockpit draw-list creation");
		TieCockpitRenderer_Shutdown(cg);
		return NULL;
	}

	if (!TieCockpitRenderer_CockpitInitPipPipeline(cg)) {
		Aeron_RequestFatalRendererError("cockpit PIP pipeline creation");
		TieCockpitRenderer_Shutdown(cg);
		return NULL;
	}

	return cg;
}

void TieCockpitRenderer_SetFlightRenderer(TieCockpitRenderer* cg, struct TieFlightRenderer* fg) {
	if (cg)
		cg->flight_gpu = fg;
}

static void TieCockpitRenderer_ReleaseEntry(TieCockpitRendererEntry* entry) {
	if (entry->base_tex)
		Aeron_DestroyTexture(entry->base_tex);
	if (entry->original_assets)
		Aeron_RuntimeAtlasRelease(&entry->original_parts);
	else if (entry->parts_tex)
		Aeron_DestroyTexture(entry->parts_tex);
	if (entry->damage_tex)
		Aeron_DestroyTexture(entry->damage_tex);
	free(entry->cover_rects);
	TieCockpitLayout_Free(&entry->layout);
	memset(entry, 0, sizeof *entry);
}

/* Drop every cockpit asset so the next prepare rebuilds the active source.
 * Clears both texture caches — the per-entry atlases
 * (base/parts/damage) + layout via TieCockpitRenderer_ReleaseEntry, AND the separate
 * CRT-mask cache that TieCockpitRenderer_ReleaseEntry doesn't touch — plus the chrome
 * bake (its composite embeds base_tex). */
static void TieCockpitRenderer_CockpitReloadCaches(TieCockpitRenderer* cg) {
	for (int i = 0; i < cg->entry_count; ++i)
		TieCockpitRenderer_ReleaseEntry(&cg->entries[i]);
	cg->entry_count = 0;
	cg->pending_entry = NULL;

	for (int i = 0; i < cg->crt_mask_count; ++i)
		if (cg->crt_masks[i].tex)
			Aeron_DestroyTexture(cg->crt_masks[i].tex);
	memset(cg->crt_masks, 0, sizeof cg->crt_masks);
	cg->crt_mask_count = 0;

	cg->chrome_valid = false;
	Aeron_LogInfo("tie.cockpit", "entry and CRT-mask caches cleared");
}

/* Artist-iteration reload request. Set by TieCockpitRenderer_RequestReload()
 * (debug UI) and consumed at the top of the next TieCockpitRenderer_Prepare().
 * Process-wide static: one cockpit compositor per process, matching the
 * flight_gpu tonemap/EOTF runtime-state convention. Deferring the free
 * to prepare() keeps it off the UI thread and at a frame boundary;
 * the backend defers GPU-side destruction until in-flight frames
 * retire, so clearing mid-session is safe. */
static bool s_cockpit_reload_request;

void TieCockpitRenderer_RequestReload(void) { s_cockpit_reload_request = true; }

void TieCockpitRenderer_Shutdown(TieCockpitRenderer* cg) {
	if (!cg)
		return;
	AeronFontAtlas_Release(&cg->dc_font);
	for (int i = 0; i < cg->entry_count; ++i)
		TieCockpitRenderer_ReleaseEntry(&cg->entries[i]);
	for (int i = 0; i < cg->crt_mask_count; ++i)
		if (cg->crt_masks[i].tex)
			Aeron_DestroyTexture(cg->crt_masks[i].tex);
	if (cg->pip_pipeline)
		Aeron_DestroyGraphicsPipeline(cg->pip_pipeline);
	if (cg->pip_vs)
		Aeron_DestroyShader(cg->pip_vs);
	if (cg->pip_fs)
		Aeron_DestroyShader(cg->pip_fs);
	if (cg->pip_color_sampler)
		Aeron_DestroySampler(cg->pip_color_sampler);
	if (cg->pip_mask_sampler)
		Aeron_DestroySampler(cg->pip_mask_sampler);
	AeronDrawList_Destroy(cg->chrome_bake_list);
	AeronDrawList_Destroy(cg->before_pip_list);
	AeronDrawList_Destroy(cg->after_pip_list);
	if (cg->chrome_rt)
		Aeron_DestroyRenderTarget(cg->chrome_rt);
	free(cg);
}

/* Cache views by their parts identity and resolution; evict the oldest insertion. */
static TieCockpitRendererEntry* TieCockpitRenderer_FindOrAllocEntry(TieCockpitRenderer* cg,
																	const TieCockpitState* cockpit) {
	if (!cockpit->view_name[0])
		return NULL;
	for (int i = 0; i < cg->entry_count; ++i) {
		TieCockpitRendererEntry* entry = &cg->entries[i];
		if (strncmp(entry->view_name, cockpit->view_name, sizeof entry->view_name) == 0 &&
			strncmp(entry->parts_basename, cockpit->parts_basename, sizeof entry->parts_basename) == 0 &&
			entry->parts_shape_count == cockpit->parts_shape_count &&
			entry->classic_w == cockpit->classic_w && entry->classic_h == cockpit->classic_h)
			return entry;
	}
	/* Eviction moves entries, so a cached pointer cannot identify the previous bake. */
	cg->chrome_valid = false;
	if (cg->entry_count >= COCKPIT_MAX_CACHED) {
		TieCockpitRenderer_ReleaseEntry(&cg->entries[0]);
		for (int i = 1; i < cg->entry_count; ++i)
			cg->entries[i - 1] = cg->entries[i];
		cg->entry_count--;
		memset(&cg->entries[cg->entry_count], 0, sizeof cg->entries[0]);
	}
	TieCockpitRendererEntry* entry = &cg->entries[cg->entry_count++];
	memset(entry, 0, sizeof *entry);
	snprintf(entry->view_name, sizeof entry->view_name, "%s", cockpit->view_name);
	snprintf(entry->parts_basename, sizeof entry->parts_basename, "%s", cockpit->parts_basename);
	entry->parts_shape_count = cockpit->parts_shape_count;
	entry->classic_w = cockpit->classic_w;
	entry->classic_h = cockpit->classic_h;
	return entry;
}

/* Fit the cockpit's display aspect inside the HD RT. Classic frames remain
 * full-height and may clip horizontally; authored frames fit completely. */

typedef struct TieCockpitRendererViewport {
	float x, y, w, h;
} TieCockpitRendererViewport;

static void TieCockpitRenderer_CockpitFitViewport(int coord_w, int coord_h, int rt_w, int rt_h,
												  TieCockpitRendererViewport* out) {
	const float cockpit_aspect = TieCockpitCommon_DisplayAspect(coord_w, coord_h);
	const float rt_aspect = (float)rt_w / (float)rt_h;
	if (TieCockpitCommon_IsClassic4x3(coord_w, coord_h) || rt_aspect >= cockpit_aspect) {
		/* Classic frame or wider RT: fit by height. */
		out->h = (float)rt_h;
		out->w = out->h * cockpit_aspect;
		out->x = ((float)rt_w - out->w) * 0.5f;
		out->y = 0.0f;
	} else {
		/* RT taller than cockpit → letterbox (fit by width). */
		out->w = (float)rt_w;
		out->h = out->w / cockpit_aspect;
		out->x = 0.0f;
		out->y = ((float)rt_h - out->h) * 0.5f;
	}
}

static TieCockpitRendererDrawSpace TieCockpitRenderer_CockpitDrawSpace(int coord_w, int coord_h, int target_w,
																	   int target_h) {
	TieCockpitRendererViewport fit;
	TieCockpitRenderer_CockpitFitViewport(coord_w, coord_h, target_w, target_h, &fit);
	TieCockpitRendererDrawSpace space = {
		.target_w = target_w,
		.target_h = target_h,
		.coord_w = coord_w,
		.coord_h = coord_h,
		.fit_x = fit.x,
		.fit_y = fit.y,
		.fit_w = fit.w,
		.fit_h = fit.h,
		.scale_x = fit.w / (float)coord_w,
		.scale_y = fit.h / (float)coord_h,
	};
	return space;
}

/* Resolve the coord frame the cockpit pass should use for `entry`. When
 * the layout YAML declared a `reference: { w, h }` block, that wins;
 * otherwise fall back to the snapshot's classic_w/h. Encapsulates the
 * "0 → 640×480 default" guard the drawers used to open-code. */
static void TieCockpitRenderer_ResolveCoordFrame(const TieCockpitRendererEntry* entry,
												 const TieSnapshot* snap, int* out_w, int* out_h) {
	int w = (int)snap->cockpit.classic_w;
	int h = (int)snap->cockpit.classic_h;
	if (w <= 0)
		w = 640;
	if (h <= 0)
		h = 480;
	TieCockpitLayout_CoordFrame(entry ? &entry->layout : NULL, w, h, &w, &h);
	*out_w = w;
	*out_h = h;
}

/* Rescale one (snap_x, snap_y) from the engine's classic frame onto
 * the layout's reference frame, picking the layout's authored anchor
 * when the YAML listed it. Drawers call this for every snapshot HUD
 * instrument position they consume. With no layout / matched frame,
 * returns the inputs verbatim (the 4:3 path is fast-and-explicit, not
 * the silent fast-path of a hidden snapshot copy). */
static inline void TieCockpitRenderer_InsAnchor(const TieCockpitRendererEntry* entry, int id, uint16_t snap_x,
												uint16_t snap_y, const TieSnapshot* snap, float* out_x,
												float* out_y) {
	TieCockpitLayout_Anchor(entry ? &entry->layout : NULL, id, (int16_t)snap_x, (int16_t)snap_y,
							(int)snap->cockpit.classic_w, (int)snap->cockpit.classic_h, out_x, out_y);
	/* HUD positions placed with the cockpit editor over a TFTC cockpit. */
	float dx, dy;
	TieCockpitEditor_Offset(id, &dx, &dy);
	*out_x += dx;
	*out_y += dy;
}

/* PIP rect resolved into the cockpit's coord frame. Prefers the
 * layout's authored override; otherwise piecewise-rescales the
 * supplied classic-frame rect via TieCockpitRenderer_InsAnchor. */
static inline void TieCockpitRenderer_ResolvePipRect(const TieCockpitRendererEntry* entry,
													 const TieSnapshot* snap, uint16_t src_x, uint16_t src_y,
													 uint16_t src_w, uint16_t src_h, float* out_x,
													 float* out_y, float* out_w, float* out_h) {
	if (entry && entry->layout.pip_present) {
		*out_x = (float)entry->layout.pip_x;
		*out_y = (float)entry->layout.pip_y;
		*out_w = (float)entry->layout.pip_w;
		*out_h = (float)entry->layout.pip_h;
		return;
	}
	float lo_x, lo_y, hi_x, hi_y;
	TieCockpitRenderer_InsAnchor(entry, TIE_HUDI_CMD_3D_CRT, src_x, src_y, snap, &lo_x, &lo_y);
	TieCockpitRenderer_InsAnchor(entry, TIE_HUDI_CMD_3D_CRT, (uint16_t)(src_x + src_w), (uint16_t)(src_y + src_h),
								 snap, &hi_x, &hi_y);
	*out_x = lo_x;
	*out_y = lo_y;
	*out_w = hi_x - lo_x;
	*out_h = hi_y - lo_y;
}

/* Cockpit-text atlas scale. `atlas_scale_x` is atlas-pixels per
 * coord-frame-pixel: glyph_coord_h = atlas_cell_h / atlas_scale.
 * MICRO64 was extracted at 4.5× SVGA (atlas cells take ~14 classic px
 * at coord_h=480). To keep glyphs the same screen fraction when coord_h
 * grows, scale DOWN proportionally — at coord_h=2160 atlas_scale=1.0
 * gives 64-px-tall ref glyphs (~3% of frame, same as 14/480 in 4:3).
 * YAML may override. */
static inline float TieCockpitRenderer_CockpitAtlasScaleFor(const TieCockpitRendererEntry* entry,
															int coord_h) {
	if (entry && entry->layout.font_atlas_scale > 0.0f)
		return entry->layout.font_atlas_scale;
	if (coord_h <= 0)
		return 4.5f;
	return 4.5f * 480.0f / (float)coord_h;
}

/* Lazy-load with retry suppression within the active generation. */
static AeronTexture* TieCockpitRenderer_EnsureAsset(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
													AeronTexture** tex, bool* tried, int* out_w, int* out_h,
													const char* path) {
	(void)cg;
	if (*tex)
		return *tex;
	if (*tried)
		return NULL;
	*tried = true;
	*tex = TieCockpitRenderer_LoadKtx2(cg, cmd, path, out_w, out_h);
	return *tex;
}

static bool TieCockpitRenderer_CockpitAssetErrorImpl(TieCockpitRenderer* cg, const char* path,
													 const char* detail) {
	char message[768];
	snprintf(message, sizeof message, "flight model source %s: required cockpit asset %s: %s",
			 cg && cg->assets ? cg->assets->name : "invalid", path ? path : "(memory)",
			 detail ? detail : "failed");
	Aeron_RequestFatalError("Flight Asset Error", message);
	return false;
}

static bool TieCockpitRenderer_ReadOriginalFile(TieCockpitRenderer* cg, const char* path, size_t maximum,
												uint8_t** out, size_t* out_size) {
	if (cg && cg->draw_phase_active)
		++cg->draw_phase_asset_io_count;
	if (!cg || !cg->assets ||
		!AeronVfs_ReadAll(cg->assets->vfs, AERON_VFS_ROOT_ASSET, path, maximum, out, out_size))
		return TIE_COCKPIT_ASSET_ERROR(path, "cannot read from ASSET");
	return true;
}

static bool TieCockpitRenderer_EnsureOriginalPalette(TieCockpitRenderer* cg) {
	if (cg->original_vga_palette_loaded)
		return true;
	uint8_t* bytes = NULL;
	size_t size = 0;
	if (!TieCockpitRenderer_ReadOriginalFile(cg, "VGA.PAC", 1024u * 1024u, &bytes, &size))
		return false;
	if (size < sizeof cg->original_vga_palette) {
		free(bytes);
		return TIE_COCKPIT_ASSET_ERROR("VGA.PAC", "flight palette is shorter than 576 bytes");
	}
	memcpy(cg->original_vga_palette, bytes, sizeof cg->original_vga_palette);
	free(bytes);
	cg->original_vga_palette_loaded = true;
	return true;
}

static bool TieCockpitRenderer_InstallOriginalFont(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												   const char* path, uint8_t row_bytes, uint8_t slot_id) {
	uint8_t* bytes = NULL;
	size_t size = 0;
	TieCockpitFontAtlas decoded = { 0 };
	TieFormatError codec_error = { 0 };
	if (!TieCockpitRenderer_ReadOriginalFile(cg, path, 1024u * 1024u, &bytes, &size))
		return false;
	if (!TieCockpitFont_Decode(bytes, size, row_bytes, &decoded, &codec_error)) {
		free(bytes);
		return TIE_COCKPIT_ASSET_ERROR(path, codec_error.message);
	}
	free(bytes);
	if (slot_id == TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_MICRO) {
		const uint16_t zero_index = (uint16_t)('0' - decoded.first_char);
		cg->original_micro_zero_advance = 0;
		if (zero_index < decoded.glyph_count)
			cg->original_micro_zero_advance = (uint8_t)decoded.glyphs[zero_index].advance;
	}
	int scaled_width = 0, scaled_height = 0;
	uint8_t* scaled = Aeron_ImageUpscaleNearestRgba8(decoded.rgba, decoded.width, decoded.height, 4,
													 &scaled_width, &scaled_height);
	if (!scaled) {
		TieCockpitFont_Free(&decoded);
		return TIE_COCKPIT_ASSET_ERROR(path, "4x nearest atlas expansion failed");
	}
	AeronFontGlyph* glyphs = calloc(decoded.glyph_count, sizeof *glyphs);
	if (!glyphs) {
		free(scaled);
		TieCockpitFont_Free(&decoded);
		return TIE_COCKPIT_ASSET_ERROR(path, "glyph metric allocation failed");
	}
	bool valid = decoded.width <= UINT16_MAX / 4 && decoded.height <= UINT16_MAX / 4 &&
				 decoded.cell_w <= UINT16_MAX / 4 && decoded.cell_h <= UINT16_MAX / 4 &&
				 decoded.baseline <= UINT16_MAX / 4;
	for (uint16_t index = 0; valid && index < decoded.glyph_count; ++index) {
		const TieCockpitFontGlyph* source = &decoded.glyphs[index];
		valid = source->atlas_x <= UINT16_MAX / 4 && source->atlas_y <= UINT16_MAX / 4 &&
				source->atlas_w <= UINT16_MAX / 4 && source->atlas_h <= UINT16_MAX / 4 &&
				source->advance <= UINT16_MAX / 4;
		glyphs[index] = (AeronFontGlyph) {
			.atlas_x = (uint16_t)(source->atlas_x * 4),
			.atlas_y = (uint16_t)(source->atlas_y * 4),
			.atlas_w = (uint16_t)(source->atlas_w * 4),
			.atlas_h = (uint16_t)(source->atlas_h * 4),
			.advance = (uint16_t)(source->advance * 4),
		};
	}
	if (!valid) {
		free(glyphs);
		free(scaled);
		TieCockpitFont_Free(&decoded);
		return TIE_COCKPIT_ASSET_ERROR(path, "4x font metrics exceed 16-bit limits");
	}
	const AeronFontAtlasRgba8Desc descriptor = {
		.pixels = scaled,
		.width = scaled_width,
		.height = scaled_height,
		.pitch = (size_t)scaled_width * 4,
		.first_char = decoded.first_char,
		.cell_w = (uint16_t)(decoded.cell_w * 4),
		.cell_h = (uint16_t)(decoded.cell_h * 4),
		.baseline = (uint16_t)(decoded.baseline * 4),
		.glyphs = glyphs,
		.glyph_count = decoded.glyph_count,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.color_space = AERON_COLOR_SPACE_LINEAR_SRGB,
		.alpha_mode = AERON_IMAGE_ALPHA_STRAIGHT,
		.generate_mips = true,
		.debug_name = path,
	};
	const bool loaded = TieScene2dTextRenderer_LoadFontRgba8(cg->text_renderer, cmd, slot_id, &descriptor);
	free(glyphs);
	free(scaled);
	TieCockpitFont_Free(&decoded);
	return loaded || TIE_COCKPIT_ASSET_ERROR(path, "font atlas GPU creation failed");
}

static bool TieCockpitRenderer_EnsureOriginalFonts(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												   uint16_t classic_width) {
	if (cg->original_font_width == classic_width)
		return true;
	const bool svga = classic_width == 640;
	if (!svga && classic_width != 320)
		return TIE_COCKPIT_ASSET_ERROR("cockpit fonts", "unsupported classic width");
	if (!TieCockpitRenderer_InstallOriginalFont(cg, cmd, svga ? "MICRO64.FNT" : "MICRO.FNT", svga ? 4 : 1,
												TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_MICRO) ||
		!TieCockpitRenderer_InstallOriginalFont(cg, cmd, svga ? "TINY64.FNT" : "TINY.FNT", svga ? 4 : 1,
												TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_TINY))
		return false;
	/* InstallOriginalFont expands both source variants and their metrics by 4x. */
	cg->original_font_atlas_scale = 4.0f;
	cg->original_font_width = classic_width;
	return true;
}

/* Classic treats these baked placeholders as mutable framebuffer pixels.
 * Remove them before mip generation so linear filtering cannot retain a
 * fringe after the live digit cells overwrite the field. */
static void TieCockpitRenderer_ClearOriginalDigitFields(const TieCockpitRenderer* cg, TieRgbaFrame* base,
														const TieSnapshot* snap, const uint8_t palette[768],
														uint8_t view_idx, uint8_t background_color,
														const int* fields, size_t field_count) {
	if (!cg || !base || !base->rgba || !snap || !palette || !cg->original_micro_zero_advance ||
		snap->cockpit.view_idx != view_idx || base->width != snap->cockpit.classic_w || !fields)
		return;

	const int font_h = TieCockpitCommon_IsSvga(snap) ? 9 : 5;
	const uint8_t* color = &palette[background_color * 3];

	for (size_t field = 0; field < field_count; ++field) {
		const TieHudInstrument* instrument = &snap->hud.instruments[fields[field]];
		if (instrument->param1 == 0)
			continue;
		int x0 = (int)instrument->x;
		int y0 = (int)instrument->y;
		int x1 = x0 + (int)instrument->param1 * cg->original_micro_zero_advance;
		int y1 = y0 + font_h;
		if (x1 > base->width)
			x1 = base->width;
		if (y1 > base->height)
			y1 = base->height;

		for (int y = y0; y < y1; ++y) {
			uint8_t* pixel = base->rgba + ((size_t)y * base->width + (size_t)x0) * 4;
			for (int x = x0; x < x1; ++x, pixel += 4) {
				pixel[0] = color[0];
				pixel[1] = color[1];
				pixel[2] = color[2];
				pixel[3] = 255;
			}
		}
	}
}

static bool TieCockpitRenderer_PrepareOriginalBase(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												   TieCockpitRendererEntry* entry, const TieSnapshot* snap,
												   uint8_t palette[768]) {
	char path[64];
	snprintf(path, sizeof path, "%s/%s.LFD", snap->cockpit.classic_w == 640 ? "CP640" : "CP320",
			 snap->cockpit.view_name);
	uint8_t* bytes = NULL;
	size_t size = 0;
	TiePanel panel = { 0 };
	TieFormatError codec_error = { 0 };
	TieRgbaFrame base = { 0 };
	TieCockpitCoverage coverage = { 0 };
	if (!TieCockpitRenderer_ReadOriginalFile(cg, path, 64u * 1024u * 1024u, &bytes, &size))
		return false;
	if (!TiePanel_Parse(bytes, size, &panel, &codec_error))
		goto failed;
	const TiePanelSection* panl = TiePanel_Find(&panel, TIE_FOURCC('P', 'A', 'N', 'L'));
	const TiePanelSection* mask = TiePanel_Find(&panel, TIE_FOURCC('M', 'A', 'S', 'K'));
	const TiePanelSection* pltt = TiePanel_Find(&panel, TIE_FOURCC('P', 'L', 'T', 'T'));
	if (!panl || !mask || !pltt) {
		snprintf(codec_error.message, sizeof codec_error.message,
				 "requires exactly one PANL, MASK, and PLTT section");
		goto failed;
	}
	int complete_rows = 0;
	if (!TieCockpitBase_Build(panl->data, panl->size, mask->data, mask->size, pltt->data, pltt->size,
							  cg->original_vga_palette, sizeof cg->original_vga_palette, snap->cockpit.view_x,
							  snap->cockpit.view_y, snap->cockpit.view_width, snap->cockpit.view_depth, &base,
							  palette, &complete_rows, &codec_error))
		goto failed;
	(void)complete_rows;
	static const int threat_digit_fields[] = { TIE_HUDI_THREAT_SHIELD_PCT, TIE_HUDI_THREAT_HULL_PCT };
	TieCockpitRenderer_ClearOriginalDigitFields(cg, &base, snap, palette, 20, COCKPIT_BG_THREAT,
												threat_digit_fields,
												sizeof threat_digit_fields / sizeof threat_digit_fields[0]);
	if (!TieCockpitCoverage_Build(base.rgba, base.width, base.height, &coverage, &codec_error))
		goto failed;
	entry->base_tex = Aeron_ImageUploadRgba8(cmd, base.rgba, base.width, base.height, (size_t)base.width * 4,
											 AERON_TEXTURE_FORMAT_RGBA8_SRGB, AERON_COLOR_SPACE_SRGB,
											 AERON_IMAGE_ALPHA_STRAIGHT, true, path);
	if (!entry->base_tex) {
		snprintf(codec_error.message, sizeof codec_error.message, "base bitmap GPU upload failed");
		goto failed;
	}
	entry->base_w = base.width;
	entry->base_h = base.height;
	entry->cover_rects = coverage.rects;
	entry->cover_count = (int)coverage.count;
	entry->cover_img_w = coverage.image_width;
	entry->cover_img_h = coverage.image_height;
	entry->tried_cover = true;
	memset(&coverage, 0, sizeof coverage);
	entry->tried_base = true;
	TiePanel_Free(&panel);
	free(base.rgba);
	free(bytes);
	return true;

failed:
	TiePanel_Free(&panel);
	TieCockpitCoverage_Free(&coverage);
	free(base.rgba);
	free(bytes);
	return TIE_COCKPIT_ASSET_ERROR(path, codec_error.message);
}

static bool TieCockpitRenderer_PrepareOriginalParts(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
													TieCockpitRendererEntry* entry, const TieSnapshot* snap,
													const uint8_t palette[768]) {
	char path[64];
	snprintf(path, sizeof path, "%s/%sP.PNL", snap->cockpit.classic_w == 640 ? "CP640" : "CP320",
			 snap->cockpit.parts_basename);
	uint8_t* bytes = NULL;
	size_t size = 0;
	TieShapeList shapes = { 0 };
	TieRgbaFrames decoded = { 0 };
	TieFormatError codec_error = { 0 };
	uint16_t* skips = NULL;
	AeronRuntimeAtlasFrame* frames = NULL;
	if (!TieCockpitRenderer_ReadOriginalFile(cg, path, 64u * 1024u * 1024u, &bytes, &size))
		return false;
	if (!TieShapeList_Parse(bytes, size, snap->cockpit.parts_shape_count, &shapes, &codec_error))
		goto failed;
	skips = calloc(shapes.count, sizeof *skips);
	frames = calloc(shapes.count, sizeof *frames);
	if (!skips || !frames) {
		snprintf(codec_error.message, sizeof codec_error.message, "parts atlas allocation failed");
		goto failed;
	}
	TieCockpitPartInstrument instruments[TIE_MAX_HUD_INSTRUMENTS];
	for (uint32_t index = 0; index < TIE_MAX_HUD_INSTRUMENTS; ++index) {
		const TieHudInstrument* source = &snap->hud.instruments[index];
		instruments[index] = (TieCockpitPartInstrument) {
			.x = source->x,
			.y = source->y,
			.param1 = source->param1,
			.param2 = source->param2,
		};
	}
	if (!TieCockpitPartTransparency_Build(instruments, TIE_MAX_HUD_INSTRUMENTS, shapes.count, skips,
										  &codec_error) ||
		!TieCockpitShapeFrames_Build(&shapes, palette, skips, 253, &decoded, &codec_error))
		goto failed;
	for (uint32_t index = 0; index < shapes.count; ++index) {
		TieRgbaFrame* frame = &decoded.frames[index];
		frames[index] = (AeronRuntimeAtlasFrame) {
			.rgba = frame->rgba,
			.width = frame->width,
			.height = frame->height,
			.id = (int32_t)index,
		};
	}
	const AeronRuntimeAtlasOptions options = {
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.color_space = AERON_COLOR_SPACE_SRGB,
		.alpha_mode = AERON_IMAGE_ALPHA_STRAIGHT,
		.generate_mips = true,
		.debug_name = path,
	};
	if (!Aeron_RuntimeAtlasBuild(&entry->original_parts, cmd, frames, (int)shapes.count, &options) ||
		entry->original_parts.layout.page_count != 1) {
		snprintf(codec_error.message, sizeof codec_error.message,
				 "parts GPU atlas creation requires exactly one page");
		goto failed;
	}
	entry->layout.reference_w = snap->cockpit.classic_w;
	entry->layout.reference_h = snap->cockpit.classic_h;
	entry->layout.atlas_w = entry->original_parts.pages[0].width;
	entry->layout.atlas_h = entry->original_parts.pages[0].height;
	entry->layout.cels = calloc(shapes.count, sizeof *entry->layout.cels);
	if (!entry->layout.cels) {
		snprintf(codec_error.message, sizeof codec_error.message, "parts layout allocation failed");
		goto failed;
	}
	entry->layout.cel_count = (int)shapes.count;
	entry->layout.font_atlas_scale = 4.0f;
	for (uint32_t index = 0; index < shapes.count; ++index) {
		const AeronSpriteRect* source = &entry->original_parts.layout.frames[index];
		entry->layout.cels[index] = (TieCockpitLayoutCel) {
			.atlas_x = (int)source->x,
			.atlas_y = (int)source->y,
			.atlas_w = (int)source->w,
			.atlas_h = (int)source->h,
			.size_w = decoded.frames[index].width,
			.size_h = decoded.frames[index].height,
		};
	}
	entry->parts_tex = entry->original_parts.pages[0].texture;
	entry->parts_w = entry->original_parts.pages[0].width;
	entry->parts_h = entry->original_parts.pages[0].height;
	entry->tried_parts = entry->tried_layout = true;
	entry->original_assets = true;
	free(frames);
	free(skips);
	TieRgbaFrames_Free(&decoded);
	TieShapeList_Free(&shapes);
	free(bytes);
	return true;

failed:
	Aeron_RuntimeAtlasRelease(&entry->original_parts);
	TieCockpitLayout_Free(&entry->layout);
	free(frames);
	free(skips);
	TieRgbaFrames_Free(&decoded);
	TieShapeList_Free(&shapes);
	free(bytes);
	return TIE_COCKPIT_ASSET_ERROR(path, codec_error.message);
}

static bool TieCockpitRenderer_EnsureOriginalCockpitAssets(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
														   TieCockpitRendererEntry* entry,
														   const TieSnapshot* snap) {
	if (!TieCockpitRenderer_EnsureOriginalPalette(cg) ||
		!TieCockpitRenderer_EnsureOriginalFonts(cg, cmd, snap->cockpit.classic_w))
		return false;
	uint8_t palette[768];
	if (!entry->base_tex) {
		if (!TieCockpitRenderer_PrepareOriginalBase(cg, cmd, entry, snap, palette))
			return false;
	} else {
		/* Parts are prepared in the same call as the base and do not need the
		 * palette again on subsequent frames. */
		return entry->parts_tex != NULL;
	}
	if (!snap->cockpit.parts_basename[0] || !snap->cockpit.parts_shape_count)
		return TIE_COCKPIT_ASSET_ERROR("cockpit parts", "snapshot has no parts identity");
	return TieCockpitRenderer_PrepareOriginalParts(cg, cmd, entry, snap, palette);
}

/* Optional authored 16:9 base art: resources/cockpit/<view>_wide.png, e.g. tieftr12_wide.png. One PNG
 * pixel is one classic pixel, so it must be as tall as the original art (456 rows) and wider than 640;
 * the original 640-wide frame is the middle of it. A missing or mismatched file just means the stretched
 * original art is used. */
static bool TieCockpitRenderer_EnsureCockpitAssets(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												   TieCockpitRendererEntry* entry, const TieSnapshot* snap) {
	if (cg->assets && !TieFlightAssetSource_IsRemastered(cg->assets)) {
		return TieCockpitRenderer_EnsureOriginalCockpitAssets(cg, cmd, entry, snap);
	}
	const TieFlightAssetBundle* catalog = TieCockpitRenderer_Catalog(cg);
	if (!catalog)
		return TIE_COCKPIT_ASSET_ERROR("tie_remaster/flight/assets.yaml",
									   "GLB cockpit catalog is unavailable");
	char path[1024];

	/* Parts atlas + hud_layout.yaml — keyed by parts_basename. Loaded
	 * BEFORE the base bitmap so the layout's reference frame can
	 * select between HD and 4:3 bitmap entries. */
	const char* parts_base = snap->cockpit.parts_basename;
	if (parts_base[0]) {
		const TieFlightAssetCockpitParts* cp = TieFlightAssets_CockpitParts(catalog, parts_base);
		if (!cp)
			return TIE_COCKPIT_ASSET_ERROR(parts_base, "catalog has no cockpit_parts row");
		{
			if (TieCockpitRenderer_ComposeFlightPath(cg, cp->atlas, path, sizeof path))
				TieCockpitRenderer_EnsureAsset(cg, cmd, &entry->parts_tex, &entry->tried_parts,
											   &entry->parts_w, &entry->parts_h, path);
			if (!entry->tried_layout && cp->layout[0] &&
				TieCockpitRenderer_ComposeFlightPath(cg, cp->layout, path, sizeof path)) {
				if (!TieCockpitLayout_Load(&entry->layout, cg->assets->vfs, AERON_VFS_ROOT_ASSET, path))
					return TIE_COCKPIT_ASSET_ERROR(path, "layout decode failed");
				entry->tried_layout = true;
			}
		}
	} else
		return TIE_COCKPIT_ASSET_ERROR("cockpit parts", "snapshot has no parts basename");
	if (!entry->parts_tex)
		return TIE_COCKPIT_ASSET_ERROR(parts_base, "parts texture upload failed");

	const TieFlightAssetCockpitView* cv = TieFlightAssets_CockpitView(catalog, entry->view_name);
	if (!cv)
		return TIE_COCKPIT_ASSET_ERROR(entry->view_name, "catalog has no cockpit_views row");

	/* Select the catalog bitmap that matches the authored layout aspect. */
	if (!entry->base_tex && !entry->tried_base) {
		const bool ref_is_hd = entry->layout.reference_w > 0 && entry->layout.reference_h > 0 &&
							   !(entry->layout.reference_w == 640 && entry->layout.reference_h == 480) &&
							   !(entry->layout.reference_w == 320 && entry->layout.reference_h == 200);
		const char* selected = ref_is_hd ? cv->bitmap_hd : cv->bitmap_4_3;
		if (selected[0] && TieCockpitRenderer_ComposeFlightPath(cg, selected, path, sizeof path)) {
			TieCockpitRenderer_EnsureAsset(cg, cmd, &entry->base_tex, &entry->tried_base, &entry->base_w,
										   &entry->base_h, path);
		} else {
			entry->tried_base = true;
		}
		/* Coverage mesh for the base bitmap. `path` still holds the base
		 * bitmap path that loaded above. */
		entry->tried_cover = true;
	}
	if (!entry->base_tex)
		return TIE_COCKPIT_ASSET_ERROR(path, "base texture upload failed");

	/* Damage overlay — optional. */
	if (cv->damage[0] && TieCockpitRenderer_ComposeFlightPath(cg, cv->damage, path, sizeof path)) {
		TieCockpitRenderer_EnsureAsset(cg, cmd, &entry->damage_tex, &entry->tried_damage, &entry->damage_w,
									   &entry->damage_h, path);
		if (!entry->damage_tex)
			return TIE_COCKPIT_ASSET_ERROR(path, "damage texture upload failed");
	}
	return true;
}

/* Full-form PMA blit. Existing widget helpers emit authored cockpit
 * coordinates; the frame-local draw space maps them to target pixels. */
static void TieCockpitRenderer_BlitCockpitQuadFull(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												   AeronRenderPass* pass, AeronTexture* tex, int coord_w,
												   int coord_h, float dst_x, float dst_y, float dst_w,
												   float dst_h, float src_u0, float src_v0, float src_u1,
												   float src_v1, float tint_r, float tint_g, float tint_b,
												   float tint_a, float bias_r, float bias_g, float bias_b,
												   float bias_a, AeronBlit2DFilter filter) {
	(void)cmd;
	(void)pass;
	(void)coord_w;
	(void)coord_h;
	if (!cg || !cg->record_list || dst_w <= 0.0f || dst_h <= 0.0f)
		return;
	const TieCockpitRendererDrawSpace* space = &cg->pending_space;
	if (cg->widget_xf.active) {
		/* Editor-transformed widget: each corner turns and scales about the pivot. */
		const float px = cg->widget_xf.pivot_x, py = cg->widget_xf.pivot_y;
		const float s_x = cg->widget_xf.scale_x, s_y = cg->widget_xf.scale_y;
		const float c = cg->widget_xf.cos_a, sn = cg->widget_xf.sin_a;
		const float cx[4] = { dst_x, dst_x + dst_w, dst_x, dst_x + dst_w };
		const float cy[4] = { dst_y, dst_y, dst_y + dst_h, dst_y + dst_h };
		const float cu[4] = { src_u0, src_u1, src_u0, src_u1 };
		const float cv[4] = { src_v0, src_v0, src_v1, src_v1 };
		AeronDrawList2DQuad4 quad = {
			.texture = tex,
			.tint = { tint_r, tint_g, tint_b, tint_a },
			.bias = { bias_r, bias_g, bias_b, bias_a },
			.blend = AERON_BLIT2D_BLEND_PMA,
			.filter = filter,
		};
		for (int i = 0; i < 4; ++i) {
			const float rx = (cx[i] - px) * s_x, ry = (cy[i] - py) * s_y;
			quad.corners[i][0] = space->fit_x + (px + rx * c - ry * sn) * space->scale_x;
			quad.corners[i][1] = space->fit_y + (py + rx * sn + ry * c) * space->scale_y;
			quad.corners[i][2] = cu[i];
			quad.corners[i][3] = cv[i];
		}
		if (tex) {
			AeronDrawList_AddQuad4(cg->record_list, &quad);
			return;
		}
		/* Untextured fills cannot turn; they move and scale with the group. */
		const float mid_x = (quad.corners[0][0] + quad.corners[3][0]) * 0.5f;
		const float mid_y = (quad.corners[0][1] + quad.corners[3][1]) * 0.5f;
		const float w = dst_w * s_x * space->scale_x, h = dst_h * s_y * space->scale_y;
		const float color[4] = { tint_r + bias_r, tint_g + bias_g, tint_b + bias_b, tint_a + bias_a };
		AeronDrawList_AddFill(cg->record_list, mid_x - w * 0.5f, mid_y - h * 0.5f, w, h, color,
							  AERON_BLIT2D_BLEND_PMA, NULL);
		return;
	}
	if (!tex) {
		const float color[4] = { tint_r + bias_r, tint_g + bias_g, tint_b + bias_b, tint_a + bias_a };
		AeronDrawList_AddFill(cg->record_list, space->fit_x + dst_x * space->scale_x,
							  space->fit_y + dst_y * space->scale_y, dst_w * space->scale_x,
							  dst_h * space->scale_y, color, AERON_BLIT2D_BLEND_PMA, NULL);
		return;
	}
	AeronDrawList2DSprite sprite = {
		.texture = tex,
		.src_u0 = src_u0,
		.src_v0 = src_v0,
		.src_u1 = src_u1,
		.src_v1 = src_v1,
		.dst_x = space->fit_x + dst_x * space->scale_x,
		.dst_y = space->fit_y + dst_y * space->scale_y,
		.dst_w = dst_w * space->scale_x,
		.dst_h = dst_h * space->scale_y,
		.tint = { tint_r, tint_g, tint_b, tint_a },
		.bias = { bias_r, bias_g, bias_b, bias_a },
		.blend = AERON_BLIT2D_BLEND_PMA,
		.filter = filter,
	};
	AeronDrawList_AddSprite(cg->record_list, &sprite);
}

/* Standard PMA-tinted blit. Equivalent to TieCockpitRenderer_BlitCockpitQuadFull with
 * zero bias — the common case for the cockpit overlay. */
static inline void TieCockpitRenderer_BlitCockpitQuad(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
													  AeronRenderPass* pass, AeronTexture* tex, int coord_w,
													  int coord_h, float dst_x, float dst_y, float dst_w,
													  float dst_h, float src_u0, float src_v0, float src_u1,
													  float src_v1, float tint_r, float tint_g, float tint_b,
													  float tint_a) {
	TieCockpitRenderer_BlitCockpitQuadFull(cg, cmd, pass, tex, coord_w, coord_h, dst_x, dst_y, dst_w, dst_h,
										   src_u0, src_v0, src_u1, src_v1, tint_r, tint_g, tint_b, tint_a,
										   0.0f, 0.0f, 0.0f, 0.0f, AERON_BLIT2D_FILTER_LINEAR);
}

/* Original cockpit parts are native-resolution pixel art. Nearest sampling
 * keeps independently drawn replacement cels aligned with matching pixels
 * embedded in larger panel cels; authored remastered atlases stay linear. */
static inline AeronBlit2DFilter TieCockpitRenderer_CockpitPartFilter(const TieCockpitRendererEntry* entry) {
	return entry && entry->original_assets ? AERON_BLIT2D_FILTER_NEAREST : AERON_BLIT2D_FILTER_LINEAR;
}

static inline void TieCockpitRenderer_BlitCockpitPartQuad(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
														  AeronRenderPass* pass,
														  const TieCockpitRendererEntry* entry, int coord_w,
														  int coord_h, float dst_x, float dst_y, float dst_w,
														  float dst_h, float src_u0, float src_v0,
														  float src_u1, float src_v1, float tint_r,
														  float tint_g, float tint_b, float tint_a) {
	TieCockpitRenderer_BlitCockpitQuadFull(cg, cmd, pass, entry ? entry->parts_tex : NULL, coord_w, coord_h,
										   dst_x, dst_y, dst_w, dst_h, src_u0, src_v0, src_u1, src_v1, tint_r,
										   tint_g, tint_b, tint_a, 0.0f, 0.0f, 0.0f, 0.0f,
										   TieCockpitRenderer_CockpitPartFilter(entry));
}

/* MONO-stencil blit — `tint=(0,0,0,1) + bias=(palette_rgb, 0)` paints
 * the palette color everywhere the source's alpha is non-zero, ignoring
 * the source's baked RGB. Engine equivalent of rtsvga2_drawmonoshape:
 * the cel acts as a mask and the palette index decides the fill color.
 * Used by shield bars and beam arc-LEDs. */
static inline void TieCockpitRenderer_BlitCockpitPartQuadMono(
	TieCockpitRenderer* cg, AeronCommandBuffer* cmd, AeronRenderPass* pass,
	const TieCockpitRendererEntry* entry, int coord_w, int coord_h, float dst_x, float dst_y, float dst_w,
	float dst_h, float src_u0, float src_v0, float src_u1, float src_v1, float r, float g, float b) {
	TieCockpitRenderer_BlitCockpitQuadFull(cg, cmd, pass, entry ? entry->parts_tex : NULL, coord_w, coord_h,
										   dst_x, dst_y, dst_w, dst_h, src_u0, src_v0, src_u1, src_v1, 0.0f,
										   0.0f, 0.0f, 1.0f, r, g, b, 0.0f,
										   TieCockpitRenderer_CockpitPartFilter(entry));
}

/* Draw the base cockpit bitmap. With a coverage mesh, draws only its opaque
 * rectangles so the transparent canopy opening is never rasterized; otherwise
 * a single fullscreen quad. `mirrored` flips horizontally (the engine's
 * mirrored_view), matching the fullscreen path's u0/u1 swap. Original assets
 * retain their decoded aspect in cockpit coordinates; authored layouts use the
 * supplied cockpit-area height. */
static void TieCockpitRenderer_DrawCockpitBase(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											   AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
											   int coord_w, int coord_h, int cockpit_area_h, bool mirrored) {
	if (!entry->base_tex)
		return;
	float base_coord_h = (float)cockpit_area_h;
	if (entry->original_assets && entry->base_w > 0 && entry->base_h > 0)
		base_coord_h = (float)entry->base_h * (float)coord_w / (float)entry->base_w;

	if (entry->cover_count <= 0 || entry->cover_img_w <= 0 || entry->cover_img_h <= 0) {
		const float u0 = mirrored ? 1.0f : 0.0f;
		const float u1 = mirrored ? 0.0f : 1.0f;
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, entry->base_tex, coord_w, coord_h, 0.0f, 0.0f,
										   (float)coord_w, base_coord_h, u0, 0.0f, u1, 1.0f, 1.0f, 1.0f, 1.0f,
										   1.0f);
		return;
	}

	const float iw = (float)entry->cover_img_w;
	const float ih = (float)entry->cover_img_h;
	for (int i = 0; i < entry->cover_count; i++) {
		const TieCockpitRendererCoverRect* r = &entry->cover_rects[i];
		const float fu0 = r->x / iw, fu1 = (r->x + r->width) / iw;
		const float fv0 = r->y / ih, fv1 = (r->y + r->height) / ih;
		float dst_x, u0, u1;
		if (mirrored) { /* flip horizontally, like the fullscreen path */
			dst_x = (float)coord_w * (1.0f - fu1);
			u0 = fu1;
			u1 = fu0;
		} else {
			dst_x = (float)coord_w * fu0;
			u0 = fu0;
			u1 = fu1;
		}
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, entry->base_tex, coord_w, coord_h, dst_x,
										   base_coord_h * fv0, (float)coord_w * (fu1 - fu0),
										   base_coord_h * (fv1 - fv0), u0, fv0, u1, fv1, 1.0f, 1.0f, 1.0f,
										   1.0f);
	}
}

/* Fixed-grid cel UVs — legacy fallback used only when the per-craft
 * hud_layout.yaml is missing. */
static void TieCockpitRenderer_AtlasCelUv(int atlas_w, int atlas_h, int cel_w, int cel_h, int cols, int frame,
										  float* u0, float* v0, float* u1, float* v1) {
	int x = (frame % cols) * cel_w;
	int y = (frame / cols) * cel_h;
	*u0 = (float)x / (float)atlas_w;
	*v0 = (float)y / (float)atlas_h;
	*u1 = (float)(x + cel_w) / (float)atlas_w;
	*v1 = (float)(y + cel_h) / (float)atlas_h;
}

/* Resolve a parts-atlas cel via the per-craft layout; falls back to
 * the fixed-grid heuristic only when no layout is loaded. */
static bool TieCockpitRenderer_ResolveCel(const TieCockpitRendererEntry* entry, int frame, float* u0,
										  float* v0, float* u1, float* v1, int* out_w_classic,
										  int* out_h_classic) {
	if (!entry || !entry->parts_tex)
		return false;
	const TieCockpitLayoutCel* c = TieCockpitLayout_Cel(&entry->layout, frame);
	if (c) {
		*u0 = (float)c->atlas_x / (float)entry->parts_w;
		*v0 = (float)c->atlas_y / (float)entry->parts_h;
		*u1 = (float)(c->atlas_x + c->atlas_w) / (float)entry->parts_w;
		*v1 = (float)(c->atlas_y + c->atlas_h) / (float)entry->parts_h;
		*out_w_classic = c->size_w;
		*out_h_classic = c->size_h;
		return true;
	}
	if (entry->layout.cel_count > 0)
		return false;
	TieCockpitRenderer_AtlasCelUv(entry->parts_w, entry->parts_h, COCKPIT_CEL_W, COCKPIT_CEL_H,
								  COCKPIT_CELS_PER_ROW, frame, u0, v0, u1, v1);
	*out_w_classic = COCKPIT_CEL_W;
	*out_h_classic = COCKPIT_CEL_H;
	return true;
}

/* Blit one parts-atlas cel at (x, y) in classic-cockpit coords with the
 * cel's authored dimensions. `hdr_boost` multiplies the sampled RGB
 * before the PMA blend — pass 1.0 for the engine-faithful "as authored"
 * look, > 1.0 to push the cel above the bloom threshold. Alpha stays
 * neutral so cutouts behave the same regardless of boost.
 *
 * Returns false when the parts atlas is missing or the layout doesn't
 * resolve `frame`. Funnel for the widget drawers that all open-coded
 * the same `TieCockpitRenderer_ResolveCel → TieCockpitRenderer_BlitCockpitQuad` sequence. */
static bool TieCockpitRenderer_BlitInstrumentCel(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												 AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												 int coord_w, int coord_h, float x, float y, int frame,
												 float hdr_boost) {
	if (!entry || !entry->parts_tex)
		return false;
	float u0, v0, u1, v1;
	int cw, ch;
	if (!TieCockpitRenderer_ResolveCel(entry, frame, &u0, &v0, &u1, &v1, &cw, &ch))
		return false;
	TieCockpitRenderer_BlitCockpitPartQuad(cg, cmd, pass, entry, coord_w, coord_h, x, y, (float)cw, (float)ch,
										   u0, v0, u1, v1, hdr_boost, hdr_boost, hdr_boost, 1.0f);
	return true;
}

/* HUD widget dispatch table, indexed by instrument index.
 * Unlisted indices fall through to HUD_W_NONE and skip. */
typedef enum {
	HUD_W_NONE = 0,
	HUD_W_SHAPE_LEVER,
	HUD_W_DIGIT_FIELD,
	HUD_W_VERT_SLIDER,
	HUD_W_LASER_LED_ROW,
	HUD_W_DAMAGE_CRACK,
	HUD_W_COVER,
	HUD_W_SHIELD_QUAD,
	HUD_W_BEAM_ARCBAR,
	HUD_W_RADAR_DISC,
	HUD_W_3DCRT,
	HUD_W_TEXT_FIELD,
	HUD_W_STATIC_LABEL,
	HUD_W_RECORDING_PCT,
	HUD_W_VIEW17_TITLE,
} TieCockpitRendererHudWidgetKind;

static const uint8_t hud_widget_kind[TIE_MAX_HUD_INSTRUMENTS] = {
	[TIE_HUDI_RADAR_LEFT] = HUD_W_RADAR_DISC,
	[TIE_HUDI_RADAR_RIGHT] = HUD_W_RADAR_DISC,
	[TIE_HUDI_CMD_3D_CRT] = HUD_W_3DCRT,
	[3] = HUD_W_LASER_LED_ROW,
	[4] = HUD_W_LASER_LED_ROW,
	[5] = HUD_W_LASER_LED_ROW,
	[6] = HUD_W_LASER_LED_ROW,
	[7] = HUD_W_LASER_LED_ROW,
	[8] = HUD_W_LASER_LED_ROW,
	[9] = HUD_W_LASER_LED_ROW,
	[10] = HUD_W_LASER_LED_ROW,
	/* idx 11..14 are missile-hardpoint ready levers (panel_updatehardpoint
	 * writes here via panel_updatelever(hp_idx + 11, lever_val)). */
	[TIE_HUDI_MISSILE_HP_FIRST] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_MISSILE_HP_FIRST + 1] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_MISSILE_HP_FIRST + 2] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_MISSILE_HP_LAST] = HUD_W_SHAPE_LEVER,
	/* idx 15..18 are the ammo-digit positions next to each hardpoint
	 * (panel_updatehardpoint writes via panel_updatevalue, text-pass). */
	[15] = HUD_W_DIGIT_FIELD,
	[16] = HUD_W_DIGIT_FIELD,
	[17] = HUD_W_DIGIT_FIELD,
	[18] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_SHIELD_FWD_NORMAL] = HUD_W_SHIELD_QUAD,
	[TIE_HUDI_SHIELD_FWD_OVER] = HUD_W_SHIELD_QUAD,
	[TIE_HUDI_SHIELD_REAR_NORMAL] = HUD_W_SHIELD_QUAD,
	[TIE_HUDI_SHIELD_REAR_OVER] = HUD_W_SHIELD_QUAD,
	[TIE_HUDI_HULL_DAMAGE_LEVER] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_SPEED_DIGITS] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_THROTTLE_DIGITS] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_POWER_BALANCE] = HUD_W_VERT_SLIDER,
	[TIE_HUDI_POWER_LASERS] = HUD_W_VERT_SLIDER,
	[TIE_HUDI_POWER_SHIELDS] = HUD_W_VERT_SLIDER,
	[TIE_HUDI_POWER_BEAM] = HUD_W_VERT_SLIDER,
	[TIE_HUDI_CLOCK_DIGITS] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_REC_LED] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_REC_PCT] = HUD_W_RECORDING_PCT,
	[TIE_HUDI_VIEW17_TITLE] = HUD_W_VIEW17_TITLE,
	[TIE_HUDI_BEAM_ARC] = HUD_W_BEAM_ARCBAR,
	[TIE_HUDI_GUNSIGHT] = HUD_W_SHAPE_LEVER,
	[37] = HUD_W_SHAPE_LEVER,
	[38] = HUD_W_SHAPE_LEVER,
	[39] = HUD_W_SHAPE_LEVER,
	[40] = HUD_W_SHAPE_LEVER,
	[41] = HUD_W_SHAPE_LEVER,
	[42] = HUD_W_SHAPE_LEVER,
	[43] = HUD_W_SHAPE_LEVER,
	[44] = HUD_W_SHAPE_LEVER,
	/* 45..57: 13 subsystem damage-crack overlays
	 * (panel_updatecockpitdamage). */
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 0] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 1] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 2] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 3] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 4] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 5] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 6] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 7] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 8] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 9] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 10] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_FIRST + 11] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_DAMAGE_CRACK_LAST] = HUD_W_DAMAGE_CRACK,
	[TIE_HUDI_TARGET_SUBSYSTEM_PCT] = HUD_W_DIGIT_FIELD,
	[59] = HUD_W_DIGIT_FIELD,
	[60] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_TARGET_SHIELD_PCT] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_TARGET_HULL_PCT] = HUD_W_DIGIT_FIELD,
	[63] = HUD_W_TEXT_FIELD,
	[64] = HUD_W_TEXT_FIELD,
	[65] = HUD_W_TEXT_FIELD,
	[TIE_HUDI_WARN_INCOMING] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_WARN_LOCK] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_WARN_IMPACT] = HUD_W_SHAPE_LEVER,
	[69] = HUD_W_TEXT_FIELD,
	[70] = HUD_W_TEXT_FIELD,
	[71] = HUD_W_DIGIT_FIELD,
	[72] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_THREAT_ION] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_THREAT_TORP] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_THREAT_MISSILE] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_THREAT_BEAM] = HUD_W_SHAPE_LEVER,
	[TIE_HUDI_THREAT_SHIELD_PCT] = HUD_W_DIGIT_FIELD,
	[TIE_HUDI_THREAT_HULL_PCT] = HUD_W_DIGIT_FIELD,
	[79] = HUD_W_TEXT_FIELD,
	[80] = HUD_W_TEXT_FIELD,
	[81] = HUD_W_DIGIT_FIELD,
	[82] = HUD_W_DIGIT_FIELD,
	/* 83..85: drop-down covers over shield/beam LED zones
	 * (panel_updatecovers, when subsystem is inactive). */
	[TIE_HUDI_COVER_SHIELDS] = HUD_W_COVER,
	[TIE_HUDI_COVER_BEAM_UP] = HUD_W_COVER,
	[TIE_HUDI_COVER_BEAM_DOWN] = HUD_W_COVER,
	[86] = HUD_W_STATIC_LABEL,
	[87] = HUD_W_STATIC_LABEL,
	[88] = HUD_W_STATIC_LABEL,
	[89] = HUD_W_STATIC_LABEL,
	[90] = HUD_W_TEXT_FIELD,
	[TIE_HUDI_BEAM_FIRE] = HUD_W_SHAPE_LEVER,
};

/* Per-widget cel offset added to instrument.param1 by the drawers.
 * Each source is the snapshot field listed inline; the descriptors
 * walk the unusual ones. */
static int TieCockpitRenderer_HudShapeValue(int idx, const TieSnapshot* snap) {
	const int16_t v = snap->hud.instruments[idx].value;
	return v < 0 ? 0 : v;
}

/* ===== Drawer dispatch ===== */

static const TieDcLayout* TieCockpitRenderer_DcLayout(const TieCockpitRenderer* cg);

static void TieCockpitRenderer_DrawShapeLever(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											  AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
											  int coord_w, int coord_h, int idx, const TieSnapshot* snap) {
	if (!entry->parts_tex)
		return;

	/* Per-instrument subsystem gates matching the engine's writer
	 * early-returns. Without these, levers paint frame 0 over zones the
	 * cockpit cover (instruments 83..85) is hiding. */
	const uint16_t cap = snap->hud.working_subsystems;
	if (idx == TIE_HUDI_BEAM_FIRE) {
		/* panel_updatebeam: gated on working_subsystems & 0x10. */
		if ((cap & 0x10u) == 0)
			return;
	} else if (idx == TIE_HUDI_HULL_DAMAGE_LEVER) {
		/* panel_updateshields: gated on working_subsystems & 0x20. */
		if ((cap & 0x20u) == 0)
			return;
	} else if (idx >= TIE_HUDI_MISSILE_HP_FIRST && idx <= TIE_HUDI_MISSILE_HP_LAST) {
		if (!TieCockpitCommon_MissileHardpointVisible(&snap->hud, idx - TIE_HUDI_MISSILE_HP_FIRST))
			return;
	} else if (idx >= TIE_HUDI_WEAPON_FIRE_FIRST && idx <= TIE_HUDI_WEAPON_FIRE_LAST) {
		/* Fire levers follow the group loop, independently of charge-bar health. */
		const int group = idx - TIE_HUDI_WEAPON_FIRE_FIRST;
		if (group >= snap->hud.weapon_group_cnt ||
			!TieCockpitCommon_InstrumentActive(&snap->hud.instruments[TIE_HUDI_LASER_LED_FIRST + group]))
			return;
	}

	int value = TieCockpitRenderer_HudShapeValue(idx, snap);
	const TieHudInstrument* ins = &snap->hud.instruments[idx];
	float ax, ay;
	TieCockpitRenderer_InsAnchor(entry, idx, ins->x, ins->y, snap, &ax, &ay);
	const bool warning_off = (idx == TIE_HUDI_WARN_INCOMING || idx == TIE_HUDI_WARN_LOCK || idx == TIE_HUDI_WARN_IMPACT) &&
							 value == 0;
	if ((idx == TIE_HUDI_GUNSIGHT || warning_off) && TieCockpitRenderer_DcLayout(cg)) {
		/* TFTC's cockpits keep XWA's grey reticle, the warning lights over it
		 * dark grey until they come on. */
		const float grey = warning_off ? 0.03f : 0.14f; /* linear: TFTC's reticle is a dark grey */
		float u0, v0, u1, v1;
		int cw, ch;
		if (TieCockpitRenderer_ResolveCel(entry, ins->param1 + value, &u0, &v0, &u1, &v1, &cw, &ch))
			TieCockpitRenderer_BlitCockpitPartQuadMono(cg, cmd, pass, entry, coord_w, coord_h, ax, ay, (float)cw,
													   (float)ch, u0, v0, u1, v1, grey, grey, grey);
		return;
	}
	TieCockpitRenderer_BlitInstrumentCel(cg, cmd, pass, entry, coord_w, coord_h, ax, ay, ins->param1 + value,
										 TieCockpitLayout_HdrBoost(&entry->layout, idx));
}

/* Subsystem-indicator pass (HUD instruments 45..57). Mirrors
 * panel_updatecockpitdamage (panel.c:623): paint one cel per installed
 * subsystem — frame 0 = intact-state plate, frame 13 = broken-state
 * plate. The engine writes these from panel_initpanel BEFORE any live
 * widget updates, so the cels sit UNDER live widgets in the
 * framebuffer. In HD we replicate the z-order by running this pass
 * before TieCockpitRenderer_DrawWidgetsForView. View 0 only (engine: `if (pilotview)
 * return;`). */
static void TieCockpitRenderer_DrawDamageCracks(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												int coord_w, int coord_h, const TieSnapshot* snap) {
	if (!entry->parts_tex)
		return;
	if (snap->cockpit.view_idx != 0)
		return;

	const uint16_t inst_mask = snap->hud.installed_subsystems; /* installed bits */
	const uint16_t cap_mask = snap->hud.working_subsystems;    /* working bits   */

	for (int bit = 0; bit < 13; ++bit) {
		uint16_t mask = (uint16_t)(1u << bit);
		if (!(inst_mask & mask))
			continue; /* not installed */

		int idx = TIE_HUDI_DAMAGE_CRACK_FIRST + bit;
		const TieHudInstrument* ins = &snap->hud.instruments[idx];
		if (!TieCockpitCommon_InstrumentActive(ins))
			continue; /* unused slot */

		int frame = ins->param1 + ((cap_mask & mask) ? 0 : 13);
		float ax, ay;
		TieCockpitRenderer_InsAnchor(entry, idx, ins->x, ins->y, snap, &ax, &ay);
		TieCockpitRenderer_BlitInstrumentCel(cg, cmd, pass, entry, coord_w, coord_h, ax, ay, frame,
											 TieCockpitLayout_HdrBoost(&entry->layout, idx));
	}
}

/* Subsystem covers (HUD instruments 83/84/85). Mirrors panel_updatecovers
 * (panel.c:605): drop a "covered" graphic over the shield-LED zone when
 * shields are inactive (SF_SHIELDS bit clear in subsystem_active), or
 * over the beam-charge zone when the tractor beam is inactive
 * (SF_TRACTOR_BEAM bit clear). Each cover has a single cel at param1.
 * Forward view except the TIE Fighter. Classic paints covers before
 * instrument plates and live widgets during panel_initpanel. */
static void TieCockpitRenderer_DrawCovers(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
										  AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
										  int coord_w, int coord_h, const TieSnapshot* snap) {
	if (!entry->parts_tex)
		return;
	if (!snap->cockpit.covers_allowed)
		return;

	const uint16_t active = snap->hud.subsystem_active;

	/* Triplet: (idx, gating mask). */
	const struct {
		int idx;
		uint16_t mask;
	} covers[3] = {
		{ TIE_HUDI_COVER_SHIELDS, 0x0001u }, /* SF_SHIELDS */
		{ TIE_HUDI_COVER_BEAM_UP, 0x0100u }, /* SF_TRACTOR_BEAM */
		{ TIE_HUDI_COVER_BEAM_DOWN, 0x0100u },
	};

	for (int i = 0; i < 3; ++i) {
		if (active & covers[i].mask)
			continue; /* subsystem on → no cover */
		const TieHudInstrument* ins = &snap->hud.instruments[covers[i].idx];
		if (!TieCockpitCommon_InstrumentActive(ins))
			continue; /* unused slot */
		/* engine always writes value=0 — cel = param1 verbatim */
		float ax, ay;
		TieCockpitRenderer_InsAnchor(entry, covers[i].idx, ins->x, ins->y, snap, &ax, &ay);
		TieCockpitRenderer_BlitInstrumentCel(cg, cmd, pass, entry, coord_w, coord_h, ax, ay, ins->param1,
											 TieCockpitLayout_HdrBoost(&entry->layout, covers[i].idx));
	}
}

/* ===== Chrome cache (static cockpit layer) ===== */

/* (Re)create the chrome RT at the flight RT's full resolution. Returns
 * false if creation fails (caller falls back to the per-frame direct
 * draws). A dimension change drops the old RT and invalidates the bake. */
static bool TieCockpitRenderer_EnsureChromeRt(TieCockpitRenderer* cg, int rt_w, int rt_h) {
	if (rt_w <= 0 || rt_h <= 0)
		return false;
	if (cg->chrome_rt && cg->chrome_w == rt_w && cg->chrome_h == rt_h)
		return true;
	if (cg->chrome_rt) {
		Aeron_DestroyRenderTarget(cg->chrome_rt);
		cg->chrome_rt = NULL;
	}
	cg->chrome_valid = false;
	cg->chrome_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = rt_w,
		.height = rt_h,
		.format = COCKPIT_CHROME_FORMAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	if (!cg->chrome_rt) {
		Aeron_RequestFatalRendererError("cockpit chrome target creation");
		return false;
	}
	cg->chrome_w = rt_w;
	cg->chrome_h = rt_h;
	return true;
}

/* True when the baked chrome still matches the live inputs. The static
 * layer changes only on view/craft switch, mirror toggle, a subsystem
 * knockout/repair (working_subsystems), or a resolution change — all rare,
 * so the bake amortises to ~nothing. subsystem_active (covers) only changes
 * at craft creation, which already shifts `entry`, but it is keyed anyway
 * for completeness. */
static bool TieCockpitRenderer_ChromeKeyMatches(const TieCockpitRenderer* cg,
												const TieCockpitRendererEntry* entry, const TieSnapshot* snap,
												int coord_w, int coord_h, int rt_w, int rt_h) {
	return cg->chrome_valid && cg->chrome_key.entry == entry &&
		   cg->chrome_key.view_idx == snap->cockpit.view_idx &&
		   cg->chrome_key.mirrored_view == snap->cockpit.mirrored_view &&
		   cg->chrome_key.installed_subsystems == snap->hud.installed_subsystems &&
		   cg->chrome_key.working_subsystems == snap->hud.working_subsystems &&
		   cg->chrome_key.subsystem_active == snap->hud.subsystem_active &&
		   cg->chrome_key.covers_allowed == snap->cockpit.covers_allowed &&
		   cg->chrome_key.coord_w == coord_w && cg->chrome_key.coord_h == coord_h &&
		   cg->chrome_key.rt_w == rt_w && cg->chrome_key.rt_h == rt_h;
}

/* Bake base bitmap + damage cracks + subsystem covers into chrome_rt at
 * the flight RT's full resolution, screen-aligned via the fit viewport so
 * the per-frame composite is a 1:1 sample. Must run OUTSIDE the flight-main
 * render pass (own color-only pass, no depth attachment). The three draw
 * helpers are reused verbatim — their view-0 gating and per-instrument HDR
 * boosts come along, so the RT holds correct per-view content with the
 * boosts pre-baked (RGBA16F carries the >1.0 values and the cutout alpha). */
static bool TieCockpitRenderer_BakeCockpitChrome(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												 const TieCockpitRendererEntry* entry,
												 const TieSnapshot* snap, int coord_w, int coord_h, int rt_w,
												 int rt_h) {
	if (!TieCockpitRenderer_EnsureChromeRt(cg, rt_w, rt_h))
		return false;

	const int cockpit_area_h = (coord_h * 19) / 20;
	static const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	cg->pending_space = TieCockpitRenderer_CockpitDrawSpace(coord_w, coord_h, rt_w, rt_h);
	cg->record_list = cg->chrome_bake_list;
	AeronDrawList_Begin(cg->chrome_bake_list, cg->chrome_rt, rt_w, rt_h, AERON_DRAWLIST2D_CLEAR, clear);
	TIE_GPU_PUSH(cmd, "Cockpit chrome bake");
	TieCockpitRenderer_DrawCockpitBase(cg, cmd, NULL, entry, coord_w, coord_h, cockpit_area_h,
									   snap->cockpit.mirrored_view);
	TieCockpitRenderer_DrawCovers(cg, cmd, NULL, entry, coord_w, coord_h, snap);
	TieCockpitRenderer_DrawDamageCracks(cg, cmd, NULL, entry, coord_w, coord_h, snap);
	if (!AeronDrawList_Prepare(cg->chrome_bake_list, cmd)) {
		TIE_GPU_POP(cmd);
		cg->record_list = NULL;
		Aeron_RequestFatalRendererError("cockpit chrome draw-list preparation");
		return false;
	}
	AeronDrawList_Render(cg->chrome_bake_list, cmd);
	TIE_GPU_POP(cmd);
	cg->record_list = NULL;

	cg->chrome_key.entry = entry;
	cg->chrome_key.view_idx = snap->cockpit.view_idx;
	cg->chrome_key.mirrored_view = snap->cockpit.mirrored_view;
	cg->chrome_key.installed_subsystems = snap->hud.installed_subsystems;
	cg->chrome_key.working_subsystems = snap->hud.working_subsystems;
	cg->chrome_key.subsystem_active = snap->hud.subsystem_active;
	cg->chrome_key.covers_allowed = snap->cockpit.covers_allowed;
	cg->chrome_key.coord_w = coord_w;
	cg->chrome_key.coord_h = coord_h;
	cg->chrome_key.rt_w = rt_w;
	cg->chrome_key.rt_h = rt_h;
	cg->chrome_valid = true;
	return true;
}

/* Composite the baked chrome over the scene inside the flight-main pass.
 * The RT is screen-aligned, so this samples it 1:1 — but restricted to the
 * base coverage rects (mapped through the same fit viewport) so the
 * transparent canopy opening is never rasterised, preserving the
 * coverage-mesh fragment savings. The composite runs in full-RT viewport
 * space; the caller's cockpit fit viewport is saved and restored. Mirroring
 * and HDR boosts are already baked, so tint is identity. */
static void TieCockpitRenderer_DrawCockpitChrome(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												 AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												 int coord_w, int coord_h, int rt_w, int rt_h,
												 bool mirrored) {
	if (!cg->chrome_rt)
		return;
	AeronTexture* chrome_tex = Aeron_RenderTargetGetTexture(cg->chrome_rt);
	if (!chrome_tex)
		return;

	(void)cmd;
	(void)pass;
	(void)entry;
	(void)coord_w;
	(void)coord_h;
	(void)mirrored;
	if (!cg->record_list)
		return;
	AeronDrawList2DSprite sprite = {
		.texture = chrome_tex,
		.src_u0 = 0.0f,
		.src_v0 = 0.0f,
		.src_u1 = 1.0f,
		.src_v1 = 1.0f,
		.dst_x = 0.0f,
		.dst_y = 0.0f,
		.dst_w = (float)rt_w,
		.dst_h = (float)rt_h,
		.tint = { 1.0f, 1.0f, 1.0f, 1.0f },
		.blend = AERON_BLIT2D_BLEND_PMA,
		.filter = AERON_BLIT2D_FILTER_LINEAR,
	};
	AeronDrawList_AddSprite(cg->record_list, &sprite);
}

/* Power-distribution sliders (HUD instruments 26..29). Mirrors tie_core's
 * panel_updatepower (panel.c:743). Each slider has 12 rungs drawn
 * upward from ins->y in `step` pixels; rung i lights when i < value.
 *   26 (0x1A) — balance / engine output (capability 0x400)
 *   27 (0x1B) — lasers (capability 0x200)
 *   28 (0x1C) — shields (capability 0x800 + subsystem 0x001)
 *   29 (0x1D) — beam   (capability 0x1000 + subsystem 0x100)
 * Capability/subsystem gates closed → engine skips drawshape; we match. */
/* Per-cell stride for a 1-D repeated-cel widget (rungs / LEDs), in the
 * layout reference frame. The layout's authored stride on the requested
 * axis (unsigned) wins, re-applying the sign of the engine `step`;
 * otherwise the engine `step` scaled by the layout's classic pixel size
 * on that axis. Shared by TieCockpitRenderer_DrawVertSlider (Y) and TieCockpitRenderer_DrawLaserLedRow
 * (X). The 2-D arc bar and the radius-scaled radar disc resolve their
 * own stride — their authored sources and defaults differ. */
static float TieCockpitRenderer_CockpitResolveStride(const TieCockpitRendererEntry* entry, int idx,
													 const TieSnapshot* snap, int step, bool axis_x) {
	int16_t stride_x_auth = 0, stride_y_auth = 0;
	TieCockpitLayout_Stride(&entry->layout, idx, &stride_x_auth, &stride_y_auth);
	const int16_t authored = axis_x ? stride_x_auth : stride_y_auth;
	if (authored != 0)
		return (step < 0) ? -(float)authored : (float)authored;
	float pixel_sx, pixel_sy;
	TieCockpitLayout_ClassicPixelSize(&entry->layout, (int)snap->cockpit.classic_w,
									  (int)snap->cockpit.classic_h, &pixel_sx, &pixel_sy);
	return (float)step * (axis_x ? pixel_sx : pixel_sy);
}

static void TieCockpitRenderer_DrawVertSlider(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											  AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
											  int coord_w, int coord_h, int idx, const TieSnapshot* snap) {
	if (!entry->parts_tex)
		return;
	const TieHudInstrument* ins = &snap->hud.instruments[idx];

	const uint16_t cap = snap->hud.working_subsystems;
	const uint16_t sub = snap->hud.subsystem_active;
	const int lp = snap->hud.laser_power;
	const int sp = snap->hud.shield_power;
	const int bp = snap->hud.beam_power;
	const bool shield_on = (sub & 0x001u) != 0;
	const bool beam_on = (sub & 0x100u) != 0;

	int value;
	switch (idx) {
		case TIE_HUDI_POWER_BALANCE:
			if (!(cap & 0x400u))
				return;
			value = (2 - lp) + 6;
			if (shield_on)
				value += (2 - sp);
			if (beam_on)
				value += (2 - bp);
			break;
		case TIE_HUDI_POWER_LASERS:
			if (!(cap & 0x200u))
				return;
			value = 3 * lp;
			break;
		case TIE_HUDI_POWER_SHIELDS:
			if (!(cap & 0x800u) || !shield_on)
				return;
			value = 3 * sp;
			break;
		case TIE_HUDI_POWER_BEAM:
			if (!(cap & 0x1000u) || !beam_on)
				return;
			value = 3 * bp;
			break;
		default:
			return;
	}
	if (value < 0)
		value = 0;
	if (value > 12)
		value = 12;

	int step = TieCockpitCommon_IsSvga(snap) ? 6 : 2;
	int unlit_frame = ins->param1;
	int lit_frame = ins->param1 + 1;
	/* HDR bloom: lit rungs pick up the instrument's per-id boost so
	 * they glow; unlit rungs stay at 1.0 so the engraved-but-dark
	 * cells in the panel chrome don't pollute the bloom band. */
	const float lit_boost = TieCockpitLayout_HdrBoost(&entry->layout, idx);

	/* Base anchor + rung stride (authored `stride_y:` wins, else engine
	 * step × classic pixel size). step is always positive here. */
	float base_x, base_y;
	TieCockpitRenderer_InsAnchor(entry, idx, ins->x, ins->y, snap, &base_x, &base_y);
	const float step_ref = TieCockpitRenderer_CockpitResolveStride(entry, idx, snap, step, /*axis_x=*/false);

	/* Engine draws bottom rung at ins->y then steps upward by `-step`. */
	for (int rung = 0; rung < 12; ++rung) {
		const bool lit = (rung < value);
		const int frame = lit ? lit_frame : unlit_frame;
		const float boost = lit ? lit_boost : 1.0f;
		TieCockpitRenderer_BlitInstrumentCel(cg, cmd, pass, entry, coord_w, coord_h, base_x,
											 base_y - (float)rung * step_ref, frame, boost);
	}
}

static void TieCockpitRenderer_DrawRadarDisc(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											 AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
											 int coord_w, int coord_h, int idx, const TieSnapshot* snap) {
	if (!TieCockpitCommon_RadarWorking(&snap->hud))
		return;

	/* The disc bezel is baked into the cockpit bitmap; this drawer
	 * only emits blip pixels. Engine writes them 1×1 (VGA) or 1×2
	 * (SVGA) — we drop classic's "only-over-radar-background" check
	 * since the HD path doesn't render to a palette FB. */
	(void)idx;
	const TieHudBlip* blips = (idx == 0) ? snap->hud.blips_left : snap->hud.blips_right;
	uint16_t count = (idx == 0) ? snap->hud.blip_count_left : snap->hud.blip_count_right;
	/* Anchor-relative composition: disc anchor (authored) + offset
	 * (snapshot, classic-px) × per-radar scale (radius_ref /
	 * radar_classic_radius). Each radar disc gets its own scale,
	 * independent of any global classic→ref transform. */
	const int radar_id = (idx == 0) ? TIE_HUDI_RADAR_LEFT : TIE_HUDI_RADAR_RIGHT;
	float disc_x, disc_y;
	TieCockpitRenderer_InsAnchor(entry, radar_id, snap->hud.instruments[radar_id].x,
								 snap->hud.instruments[radar_id].y, snap, &disc_x, &disc_y);
	/* Per-radar scale. Layout's authored `radius:` wins; else fall
	 * back to the layout's classic_pixel_size × engine classic radius
	 * (matches the pre-anchor-relative behaviour). */
	const int radar_classic_r = (int)snap->hud.radar_classic_radius;
	const int radius_ref = TieCockpitLayout_RadarRadius(&entry->layout, radar_id);
	float scale_x, scale_y;
	if (radius_ref > 0 && radar_classic_r > 0) {
		scale_x = scale_y = (float)radius_ref / (float)radar_classic_r;
	} else {
		TieCockpitLayout_ClassicPixelSize(&entry->layout, (int)snap->cockpit.classic_w,
										  (int)snap->cockpit.classic_h, &scale_x, &scale_y);
	}
	const float blip_w = scale_x;
	const float blip_h = (TieCockpitCommon_IsSvga(snap) ? 2.0f : 1.0f) * scale_y;
	for (uint16_t b = 0; b < count; ++b) {
		const TieHudBlip* bl = &blips[b];
		uint32_t pal = snap->palette[bl->color & 0xFFu];
		float r, g, bC;
		TieScene2dSrgb_PalToLinearRgb(pal, &r, &g, &bC);
		const float bx = disc_x + (float)bl->offset_x * scale_x;
		const float by = disc_y + (float)bl->offset_y * scale_y;
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, bx, by, blip_w, blip_h,
										   0.0f, 0.0f, 1.0f, 1.0f, r, g, bC, 1.0f);
	}
}

/* Engine bracket-pixel tables (rtsvga2.c:60-70). Each (dx, dy)
 * signed-byte pair offsets from (bracket_x, bracket_y); colour 0xCE.
 * 10-pt = VGA layout, 12-pt = SVGA. */
static const int8_t k_bracket_10pt[20] = { -1, +1, -2, +1, -2, 0, -2, -1, -1, -1,
										   +1, -1, +2, -1, +2, 0, +2, +1, +1, +1 };
static const int8_t k_bracket_12pt[24] = { -1, +2, -2, +2, -2, +1, -2, 0,  -2, -1, -1, -1,
										   +1, -1, +2, -1, +2, 0,  +2, +1, +2, +2, +1, +2 };

/* Engine `panel_drawboxinxtrans(left_x, top_y, w, h)` hollow rectangle
 * emitted via 4 blit_cockpit_quads in classic cockpit-coord space.
 *
 * Visible geometry (rasteriser scan-fill of 4 vertical polygon edges):
 *   width  = w + 1  (cols left_x .. left_x+w)
 *   height = h      (rows top_y  .. top_y+h-1)
 *   1-px borders on all four sides at the outermost cols/rows
 *
 * 1 classic px becomes ~2.25 swap px at 1080p / ~4.5 at 4K through the
 * cockpit-pass 4:3 fit — so the border stays visible at any HD scale
 * (single-swap-px borders would disappear at 4K). Used by both the
 * main viewport target box (w=13, h=10 SVGA) and the PIP subsystem
 * box (w=4, h=4). */
static void TieCockpitRenderer_BlitEngineHollowBox(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												   AeronRenderPass* pass, int coord_w, int coord_h,
												   float left_x_cl, float top_y_cl, int box_w, int box_h,
												   float pixel_sx, float pixel_sy, float r, float g,
												   float b) {
	if (box_w <= 0 || box_h <= 0)
		return;
	/* Edge thickness = 1 classic-px scaled into the layout's ref
	 * frame so the border stays visible at HD ref sizes. */
	const float edge_w = pixel_sx;
	const float edge_h = pixel_sy;
	const float outer_w = (float)box_w + edge_w;
	const float side_h = (float)box_h - 2.0f * edge_h;
	const float bottom_y = top_y_cl + (float)box_h - edge_h;
	const float right_x = left_x_cl + (float)box_w;
	/* Top edge. */
	TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, left_x_cl, top_y_cl, outer_w,
									   edge_h, 0, 0, 1, 1, r, g, b, 1.0f);
	/* Bottom edge. */
	TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, left_x_cl, bottom_y, outer_w,
									   edge_h, 0, 0, 1, 1, r, g, b, 1.0f);
	/* Side edges. */
	if (side_h > 0.0f) {
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, left_x_cl,
										   top_y_cl + edge_h, edge_w, side_h, 0, 0, 1, 1, r, g, b, 1.0f);
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, right_x, top_y_cl + edge_h,
										   edge_w, side_h, 0, 0, 1, 1, r, g, b, 1.0f);
	}
}

/* 1998-look target box: four L-shaped corner brackets. Arm length =
 * clamp(dim/8, 3, dim) classic-px (scaled by pixel_sx/sy to the
 * layout ref frame); middle of every side blank. The arms' THICKNESS
 * (1 classic-px) also scales via pixel_sx/sy so single-classic-px
 * borders stay visible at HD ref sizes. */
static void TieCockpitRenderer_BlitEngineCornerBox(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												   AeronRenderPass* pass, int coord_w, int coord_h,
												   float left_x_cl, float top_y_cl, float box_w, float box_h,
												   float pixel_sx, float pixel_sy, float r, float g,
												   float b) {
	if (box_w <= 0.0f || box_h <= 0.0f)
		return;

	/* Arm length floor = 3 classic-px scaled. */
	const float arm_min_x = 3.0f * pixel_sx;
	const float arm_min_y = 3.0f * pixel_sy;
	float bx = box_w * 0.125f;
	if (bx < arm_min_x)
		bx = arm_min_x;
	if (bx > box_w)
		bx = box_w;
	float by = box_h * 0.125f;
	if (by < arm_min_y)
		by = arm_min_y;
	if (by > box_h)
		by = box_h;

	/* Arm thickness = 1 classic-px scaled. */
	const float edge_w = pixel_sx;
	const float edge_h = pixel_sy;

	const float right_arm_x = left_x_cl + box_w - bx;
	const float bottom_y = top_y_cl + box_h - edge_h;
	const float right_col_x = left_x_cl + box_w - edge_w;
	const float bot_arm_y = top_y_cl + box_h - by;

	/* Horizontal arms: top-L, top-R, bottom-L, bottom-R. */
	TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, left_x_cl, top_y_cl, bx, edge_h,
									   0, 0, 1, 1, r, g, b, 1.0f);
	TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, right_arm_x, top_y_cl, bx,
									   edge_h, 0, 0, 1, 1, r, g, b, 1.0f);
	TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, left_x_cl, bottom_y, bx, edge_h,
									   0, 0, 1, 1, r, g, b, 1.0f);
	TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, right_arm_x, bottom_y, bx,
									   edge_h, 0, 0, 1, 1, r, g, b, 1.0f);

	/* Vertical arms (length by - edge_h), inset one row so the corner
	 * pixel is shared with the horizontal arm. */
	if (by > edge_h) {
		const float v_h = by - edge_h;
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, left_x_cl,
										   top_y_cl + edge_h, edge_w, v_h, 0, 0, 1, 1, r, g, b, 1.0f);
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, right_col_x,
										   top_y_cl + edge_h, edge_w, v_h, 0, 0, 1, 1, r, g, b, 1.0f);
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, left_x_cl, bot_arm_y,
										   edge_w, v_h, 0, 0, 1, 1, r, g, b, 1.0f);
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, right_col_x, bot_arm_y,
										   edge_w, v_h, 0, 0, 1, 1, r, g, b, 1.0f);
	}
}

/* world → clip via 4×4 view_proj, then NDC. Returns false when the
 * point is behind the camera (clip.w <= 0). Used by the target-box
 * overlays which project tie_core's world coords through HD's own
 * camera so the box lands at the right screen position regardless
 * of aspect ratio. */
static bool TieCockpitRenderer_ProjectWorldToNdc(const float view_proj[16], float wx, float wy, float wz,
												 float* out_ndc_x, float* out_ndc_y, float* out_clip_w) {
	const float cx = view_proj[0] * wx + view_proj[1] * wy + view_proj[2] * wz + view_proj[3];
	const float cy = view_proj[4] * wx + view_proj[5] * wy + view_proj[6] * wz + view_proj[7];
	const float cw = view_proj[12] * wx + view_proj[13] * wy + view_proj[14] * wz + view_proj[15];
	if (cw <= 0.0f)
		return false;
	*out_ndc_x = cx / cw;
	*out_ndc_y = cy / cw;
	*out_clip_w = cw;
	return true;
}

/* Look up a flight-object's world position by engine-side slot index
 * (== TieFlightObjectState.slot). Returns false when no flight object
 * with that slot is present in the snapshot. Linear scan over
 * flight_count — typically ≤120 entries. */
static const int32_t* TieCockpitRenderer_FlightWorldPosition(const TieSnapshot* snap, uint16_t slot) {
	for (uint16_t i = 0; i < snap->flight_count; ++i) {
		if (snap->flights[i].slot == slot)
			return snap->flights[i].world_pos;
	}
	return NULL;
}

/* Static-object world position by engine static-table index (target_obj_slot
 * - 0x3800). HD static slots are sparse; iterate to find a match. */
static const int32_t* TieCockpitRenderer_StaticWorldPosition(const TieSnapshot* snap, uint16_t static_idx) {
	for (uint16_t i = 0; i < snap->static_count; ++i) {
		if (snap->statics[i].slot == static_idx)
			return snap->statics[i].world_pos;
	}
	return NULL;
}

/* Main-viewport target indicator box (engine: USER_targetonscreen,
 * user.c:559).
 *
 * Pipeline anchor: the ship's swapchain pixel is set when flight_gpu
 * rasterises through the FLIGHT viewport; this drawer runs inside the
 * cockpit render pass, which has its own (different) viewport — the
 * 4:3 pillarbox fit of the cockpit's coord frame (see
 * TieCockpitRenderer_Draw:2887). The flight projection spans the full output,
 * so projected positions must be remapped through swapchain pixels into
 * the cockpit viewport before drawing the box. */
static void TieCockpitRenderer_DrawMainTargetBox(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												 AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												 int rt_w, int rt_h, const TieSnapshot* snap,
												 bool look_1998) {
	/* engine_ok = 1995 gates (includes pilotview filter).
	 * inputs_ok = 1998 gates (no pilotview filter). */
	const bool gate_ok = look_1998 ? snap->hud.target_box_inputs_ok : snap->hud.target_box_engine_ok;
	if (!gate_ok)
		return;
	const uint16_t slot = snap->hud.target_obj_slot;
	if (slot == 0xFFFFu)
		return;
	if (rt_w <= 0 || rt_h <= 0)
		return;

	const int32_t* world_pos = (slot < 0x3800u)
								   ? TieCockpitRenderer_FlightWorldPosition(snap, slot)
								   : TieCockpitRenderer_StaticWorldPosition(snap, (uint16_t)(slot - 0x3800u));
	if (!world_pos)
		return;
	float local[3];
	AeronWorld_LocalI32(snap->camera.world_pos, world_pos, local);
	if (look_1998 && slot < 0x3800u) {
		local[0] += (float)snap->hud.target_box_center_offset_1998[0];
		local[1] += (float)snap->hud.target_box_center_offset_1998[1];
		local[2] += (float)snap->hud.target_box_center_offset_1998[2];
	}

	TieFlightCamera fcam;
	TieRenderMath_BuildCamera(&fcam, &snap->camera, rt_w, rt_h);
	if (fcam.fit_w <= 0.0f || fcam.fit_h <= 0.0f)
		return;

	float ndc_x, ndc_y, clip_w;
	if (!TieCockpitRenderer_ProjectWorldToNdc(fcam.view_proj, local[0], local[1], local[2], &ndc_x, &ndc_y,
											  &clip_w))
		return;

	/* 1995 rejects off-viewport targets explicitly; 1998 lets the
	 * primitive clip per-strip. */
	if (!look_1998) {
		if (ndc_x < -1.0f || ndc_x > 1.0f)
			return;
		if (ndc_y < -1.0f || ndc_y > 1.0f)
			return;
	}

	/* Apparent half-extent in NDC:
	 *   ndc_extent = (1 / tan(v_half)) * bound / depth
	 * Multiplied by halfpixelsdeep this equals retail's
	 *   v12 = perspFactor * bound / depth   (in engine pixels). */
	const uint16_t bound_raw = look_1998 ? snap->hud.target_bound_hwidth_1998 : snap->hud.target_bound_hwidth;
	const float bound_f = (float)bound_raw;
	const float inv_tan_v = 1.0f / tanf(fcam.v_half_rad);
	const float ndc_extent_y = (bound_f / clip_w) * inv_tan_v;

	/* Engine's classic 3D aperture half-height, retained for the 1998
	 * fallback below. */
	const float halfpixelsdeep_gate = snap->camera.viewport_frac_h * (float)snap->cockpit.classic_h * 0.5f;
	if (halfpixelsdeep_gate <= 0.0f)
		return;

	/* 1995 hides the box when the ship gets visibly big on screen. */
	if (!look_1998) {
		const float threshold_pixels = (snap->cockpit.classic_h >= 400) ? 10.0f : 5.0f;
		const float threshold_ndc =
			2.0f * threshold_pixels * fcam.fit_h / ((float)snap->cockpit.classic_h * fcam.flight_vp_h);
		if (ndc_extent_y > threshold_ndc)
			return;
	}

	/* Round-trip the projected position through swapchain pixels then
	 * into the cockpit pass's coord frame. coord = layout reference
	 * (16:9) or classic (4:3); cockpit_vp = matching fit on the RT
	 * (full RT or pillarbox). The TieCockpitRenderer_BlitCockpitQuad NDC math reverses
	 * the same transform so strips stay multi-swap-px-thick at 4K. */
	const float ship_px = fcam.flight_vp_x + (ndc_x + 1.0f) * 0.5f * fcam.flight_vp_w;
	const float ship_py = fcam.flight_vp_y + (1.0f - ndc_y) * 0.5f * fcam.flight_vp_h;
	int coord_w, coord_h;
	TieCockpitRenderer_ResolveCoordFrame(entry, snap, &coord_w, &coord_h);
	TieCockpitRendererViewport cockpit_vp;
	TieCockpitRenderer_CockpitFitViewport(coord_w, coord_h, rt_w, rt_h, &cockpit_vp);
	const float center_x_cl = (ship_px - cockpit_vp.x) * (float)coord_w / cockpit_vp.w;
	const float center_y_cl = (ship_py - cockpit_vp.y) * (float)coord_h / cockpit_vp.h;
	if (!look_1998) {
		if (center_x_cl < 0.0f || center_x_cl > (float)coord_w)
			return;
		if (center_y_cl < 0.0f || center_y_cl > (float)coord_h)
			return;
	}

	/* 1995 fixed palette 0xCE, 1998 caller passes 59. Raw indices, no remap. */
	const uint32_t pal = snap->palette[look_1998 ? 59u : 0xCEu];
	float r, g, b;
	TieScene2dSrgb_PalToLinearRgb(pal, &r, &g, &b);

	float pixel_sx, pixel_sy;
	TieCockpitLayout_ClassicPixelSize(entry ? &entry->layout : NULL, (int)snap->cockpit.classic_w,
									  (int)snap->cockpit.classic_h, &pixel_sx, &pixel_sy);

	if (!look_1998) {
		/* 1995: fixed-size locator. Integer halving on offset matches
		 * user.c:627-629 — odd box widths land one classic px left of
		 * true center; float halving would shift by 0.5 classic px. */
		const int box_w = coord_w / 48;
		const int box_h = coord_h / 48;
		const float left_x_cl = center_x_cl - (float)(box_w / 2);
		const float top_y_cl = center_y_cl - (float)(box_h / 2);
		TieCockpitRenderer_BlitEngineHollowBox(cg, cmd, pass, coord_w, coord_h, left_x_cl, top_y_cl, box_w,
											   box_h, pixel_sx, pixel_sy, r, g, b);
	} else {
		/* 1998: side = clamp(reticle_px, [4 VGA | 8 SVGA] .. 0.75·coord_w) + 4.
		 * halfpixelsdeep_size converts full-output NDC into cockpit-layout
		 * pixels so the box scales with the rendered ship.
		 * side_min and the +4 padding are engine classic-px constants; scale
		 * them into the layout's ref frame so the box stays visible at HD. */
		const float halfpixelsdeep_size = (fcam.fit_h > 0.0f)
											  ? (fcam.flight_vp_h / fcam.fit_h) * (float)coord_h * 0.5f
											  : halfpixelsdeep_gate;
		const float apparent_px = ndc_extent_y * halfpixelsdeep_size;
		const float side_min_classic = (snap->cockpit.classic_h >= 400) ? 8.0f : 4.0f;
		const float side_min = side_min_classic * pixel_sy;
		const float side_max = 0.75f * (float)coord_w;
		float side_f = apparent_px;
		if (side_f < side_min)
			side_f = side_min;
		if (side_f > side_max)
			side_f = side_max;
		side_f += 4.0f * pixel_sy;
		const float left_x_cl = center_x_cl - side_f * 0.5f;
		const float top_y_cl = center_y_cl - side_f * 0.5f;
		TieCockpitRenderer_BlitEngineCornerBox(cg, cmd, pass, coord_w, coord_h, left_x_cl, top_y_cl, side_f,
											   side_f, pixel_sx, pixel_sy, r, g, b);
	}
}

/* Draw the radar bracket — N 1×1 solid quads tinted palette[0xCE].
 * No-op when no target is acquired (bracket_present = 0). */
static void TieCockpitRenderer_DrawRadarBracket(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												int coord_w, int coord_h, const TieSnapshot* snap) {
	if (!snap->hud.bracket_present || !TieCockpitCommon_RadarWorking(&snap->hud))
		return;

	const int8_t* def;
	int pair_count;
	if (TieCockpitCommon_IsSvga(snap)) {
		def = k_bracket_12pt;
		pair_count = 12;
	} else {
		def = k_bracket_10pt;
		pair_count = 10;
	}

	uint32_t pal = snap->palette[0xCEu]; /* engine-fixed bracket color */
	float r, g, bC;
	TieScene2dSrgb_PalToLinearRgb(pal, &r, &g, &bC);

	/* Anchor-relative composition (same as TieCockpitRenderer_DrawRadarDisc). The
	 * bracket sits on top of the targeted blip, so it lives on
	 * whichever radar disc the engine assigned it. */
	const int radar_id = (snap->hud.bracket_radar_idx == 0) ? TIE_HUDI_RADAR_LEFT : TIE_HUDI_RADAR_RIGHT;
	float disc_x, disc_y;
	TieCockpitRenderer_InsAnchor(entry, radar_id, snap->hud.instruments[radar_id].x,
								 snap->hud.instruments[radar_id].y, snap, &disc_x, &disc_y);
	const int radar_classic_r = (int)snap->hud.radar_classic_radius;
	const int radius_ref = TieCockpitLayout_RadarRadius(&entry->layout, radar_id);
	float scale_x, scale_y;
	if (radius_ref > 0 && radar_classic_r > 0) {
		scale_x = scale_y = (float)radius_ref / (float)radar_classic_r;
	} else {
		TieCockpitLayout_ClassicPixelSize(&entry->layout, (int)snap->cockpit.classic_w,
										  (int)snap->cockpit.classic_h, &scale_x, &scale_y);
	}
	const float cx = disc_x + (float)snap->hud.bracket_offset_x * scale_x;
	const float cy = disc_y + (float)snap->hud.bracket_offset_y * scale_y;

	for (int i = 0; i < pair_count; ++i) {
		int dx = def[2 * i + 0];
		int dy = def[2 * i + 1];
		TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, cx + (float)dx * scale_x,
										   cy + (float)dy * scale_y, scale_x, scale_y, 0.0f, 0.0f, 1.0f, 1.0f,
										   r, g, bC, 1.0f);
	}
}

/* Message-bar text composition (pick_msg_base_color, build_msg_body_with_colors,
 * color tables, TieCockpitText_BuildmsgBarText) lives in cockpit_text.c. */

/* Logical palette index → RGB tint via snap->palette + TieCockpitText_RemapColor. */
static void TieCockpitRenderer_MsgColorToTint(const TieSnapshot* snap, uint8_t logical_color, float* r,
											  float* g, float* b) {
	const uint32_t pal = snap->palette[TieCockpitText_RemapColor(logical_color)];
	TieScene2dSrgb_PalToLinearRgb(pal, r, g, b);
}

/* setbackcolor defines (COCKPIT_BG_CMD/HUD/THREAT/AMMO) live in
 * cockpit_text.h — shared with the per-builder text composition. */

/* Paint a solid backcolor rectangle — engine equivalent of
 * festring_setbound + clearwindow. Used to repaint the dynamic-text
 * areas of the cockpit panel that classic clears once at panel_initpanel
 * and that re-expose the cockpit base bitmap's placeholder in HD because
 * we re-blit the base every frame. Logical bg goes through
 * TieCockpitText_RemapColor → snap->palette → RGB. */
static void TieCockpitRenderer_ClearPanelRect(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											  AeronRenderPass* pass, int coord_w, int coord_h,
											  const TieSnapshot* snap, int x, int y, int w, int h,
											  uint8_t logical_bg) {
	if (w <= 0 || h <= 0)
		return;
	float r, g, b;
	TieCockpitRenderer_MsgColorToTint(snap, logical_bg, &r, &g, &b);
	TieCockpitRenderer_BlitCockpitQuad(cg, cmd, pass, NULL, coord_w, coord_h, (float)x, (float)y, (float)w,
									   (float)h, 0.0f, 0.0f, 1.0f, 1.0f, r, g, b, 1.0f);
}

/* Per-frame clearwindow pass for cockpit text areas that classic
 * paints once during panel_initpanel but that we re-blit every frame.
 * Currently covers:
 *   - rec-% counter (idx 32) — panel_updatereplaystuff backcolor 0x40,
 *     bound = (12 VGA / 18 SVGA) × fontheight(2) (= 5 VGA / 9 SVGA).
 * View 0 only (matches engine writers' camera.pilotview gate). */
static void TieCockpitRenderer_DrawClearwindows(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												int coord_w, int coord_h, const TieSnapshot* snap) {
	if (snap->cockpit.view_idx != 0)
		return;
	const bool svga = TieCockpitCommon_IsSvga(snap);
	const int micro_h = svga ? 9 : 5; /* fontheight at fontsize 2 */

	/* Rec-% counter (panel.c:657-666). */
	const TieHudInstrument* rec = &snap->hud.instruments[TIE_HUDI_REC_PCT];
	if (rec->x || rec->y) {
		const int rec_w = svga ? 18 : 12;
		float ax, ay;
		TieCockpitRenderer_InsAnchor(entry, TIE_HUDI_REC_PCT, rec->x, rec->y, snap, &ax, &ay);
		TieCockpitRenderer_ClearPanelRect(cg, cmd, pass, coord_w, coord_h, snap, (int)ax, (int)ay, rec_w,
										  micro_h, COCKPIT_BG_HUD);
	}
}

/* Message banner (msg.c::msg_messagedisplay). Paints the backcolor
 * strip + 1-px separator at top-1; the text records (body, time-warp,
 * and training bonus readouts) are composed by
 * TieCockpitText_BuildmsgBarText. Engine's invisible-drop (dropcolor
 * remaps to backcolor) isn't reproduced — the HD records carry no shadow. */
static void TieCockpitRenderer_DrawMessageBar(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											  AeronRenderPass* pass, const TieSnapshot* snap) {
	(void)cmd;
	(void)pass;
	if (!cg || !cg->record_list)
		return;
	const TieCockpitRendererDrawSpace* target = &cg->pending_space;
	TieFlightHudMessageBarGeometry geometry;
	if (!TieFlightHud_MessageBarGeometry(snap, target->target_w, target->target_h, &geometry))
		return;

	const bool centered = TiePresentation_CenteredMessages() != 0;
	if (!centered) {
		/* Original msg_messagedisplay clears the full framebuffer-width band. */
		float r, g, b;
		TieCockpitRenderer_MsgColorToTint(snap, 0x2C, &r, &g, &b);
		const float body_color[4] = { r, g, b, 1.0f };
		AeronDrawList_AddFill(cg->record_list, 0.0f, geometry.bar_y, (float)target->target_w, geometry.bar_h,
							  body_color, AERON_BLIT2D_BLEND_PMA, NULL);
		TieCockpitRenderer_MsgColorToTint(snap, 0x2D, &r, &g, &b);
		const float separator_color[4] = { r, g, b, 1.0f };
		AeronDrawList_AddFill(cg->record_list, 0.0f, geometry.separator_y, (float)target->target_w,
							  geometry.separator_h, separator_color, AERON_BLIT2D_BLEND_PMA, NULL);
	}

	/* Text retains the original body indent and time-warp column. */
	TieUIText recs[4];
	int n_recs = TieCockpitText_BuildmsgBarText(snap, (int)snap->hud.msg_bar.line_top, 2,
												(int)snap->hud.msg_bar.line_right, recs,
												(int)(sizeof recs / sizeof recs[0]));
	if (n_recs > 0 && cg->text_renderer) {
		const float atlas_scale = cg->original_font_atlas_scale > 0.0f
									  ? cg->original_font_atlas_scale
									  : TieCockpitRenderer_CockpitAtlasScaleFor(NULL, geometry.classic_h);
		const TieScene2dTextSpace message_space = {
			.classic_w = geometry.classic_w,
			.classic_h = geometry.classic_h,
			.atlas_scale_x = atlas_scale,
			.atlas_scale_y = atlas_scale,
			.space_between_classic = 0.0f,
			.fit = TIE_SCENE2D_TEXT_FIT_FILL,
		};
		/* Message body: smaller font, horizontally centered, at the top of the
		 * screen. The time-warp readout keeps its bottom-right corner. */
		if (centered && snap->hud.msg_bar.present) {
			recs[0].font_id = FONT_MICRO64;
			const float width = TieScene2dTextRenderer_MeasureFestringClassic(
				cg->text_renderer, recs[0].font_domain, recs[0].font_id, &message_space, recs[0].text);
			recs[0].x = (int16_t)(((float)geometry.classic_w - width) * 0.5f);
			recs[0].y = 6;
		}
		/* The time-warp readout follows the body: at the screen's right edge,
		 * past the 4:3 frame on a wide screen. */
		const int warp = snap->hud.msg_bar.present ? 1 : 0;
		if (warp < n_recs && geometry.text_scale_x > 0.0f) {
			const float width = TieScene2dTextRenderer_MeasureFestringClassic(
				cg->text_renderer, recs[warp].font_domain, recs[warp].font_id, &message_space, recs[warp].text);
			const float right = ((float)target->target_w - geometry.text_fit_x) / geometry.text_scale_x;
			recs[warp].x = (int16_t)(right - width - 6.0f);
			recs[warp].y = (int16_t)((float)geometry.classic_h * 0.945f);
		}
		TieScene2dTextRenderer_RecordFestringScaled(
			cg->text_renderer, cg->record_list, geometry.classic_w, geometry.classic_h, &message_space, recs,
			n_recs, snap->palette, geometry.text_fit_x, 0.0f, geometry.text_scale_x, geometry.scale_y,
			target->target_w, target->target_h);
	}
}

/* panel_updatelasers LED row (panel.c:776). One call per weapon
 * group; idx 3..14 = group 0..11. step = SVGA 6 / VGA 3, negated by
 * the param2 RTL flag. Always paints 10 cels (filled or empty) so
 * the row matches classic's first-paint behaviour. */
static void TieCockpitRenderer_DrawLaserLedRow(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											   AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
											   int coord_w, int coord_h, int idx, const TieSnapshot* snap) {
	if (!entry->parts_tex)
		return;

	int g = idx - 3;
	if (g < 0 || g >= snap->hud.weapon_group_cnt)
		return;

	const TieHudInstrument* ins = &snap->hud.instruments[idx];
	if (!TieCockpitCommon_InstrumentActive(ins))
		return;

	if (snap->cockpit.view_idx != 0)
		return;
	if (!(snap->hud.working_subsystems & 0x02u))
		return;
	if (!(snap->hud.working_subsystems & 0x04u))
		return;

	/* panel_updatelasers writes value = led_count (0..10) and color =
	 * filled_frame (1 normal, 2 overcharge, 0 = uncharged). */
	const int led_count = ins->value;
	const int filled_frame = ins->color;
	const int empty_frame = filled_frame > 0 ? filled_frame - 1 : 0;

	const int shape_base = ins->param1;
	const bool flip = (ins->param2 != 0);
	int step = TieCockpitCommon_IsSvga(snap) ? 6 : 3;
	if (flip)
		step = -step;

	/* HDR bloom: lit LEDs (filled, charged) pick up the instrument's
	 * boost so they glow. Empty cels and the uncharged-weapon base
	 * (filled_frame == 0, where filled and empty would render the same
	 * dim cel) stay at 1.0 so unlit row positions don't pollute bloom. */
	const float lit_boost = TieCockpitLayout_HdrBoost(&entry->layout, idx);
	const bool can_glow = (filled_frame > 0);

	/* Base anchor + LED stride (authored `stride_x:` wins, else engine
	 * step × classic pixel size; step may be negated above for RTL). */
	float base_x, base_y;
	TieCockpitRenderer_InsAnchor(entry, idx, ins->x, ins->y, snap, &base_x, &base_y);
	const float step_ref = TieCockpitRenderer_CockpitResolveStride(entry, idx, snap, step, /*axis_x=*/true);

	float led_x = base_x;
	for (int l = 0; l < 10; ++l) {
		const bool lit = can_glow && (l < led_count);
		int frame = shape_base + (lit ? filled_frame : empty_frame);
		float u0, v0, u1, v1;
		int cw, ch;
		if (!TieCockpitRenderer_ResolveCel(entry, frame, &u0, &v0, &u1, &v1, &cw, &ch)) {
			led_x += step_ref;
			continue;
		}
		/* RTL flip = engine drawshape's flip_x=1: cel's right edge
		 * lands at x, content mirrored. For a top-left-anchored
		 * quad: shift left by (cw-1) and swap U coords. */
		const float dx = flip ? (led_x - (float)(cw - 1)) : led_x;
		const float fu0 = flip ? u1 : u0;
		const float fu1 = flip ? u0 : u1;
		// const float boost = lit ? lit_boost : 2.0f;
		const float boost = lit_boost;
		TieCockpitRenderer_BlitCockpitPartQuad(cg, cmd, pass, entry, coord_w, coord_h, dx, base_y, (float)cw,
											   (float)ch, fu0, v0, fu1, v1, boost, boost, boost, 1.0f);
		led_x += step_ref;
	}
}

/* SHIELD_QUAD widgets (HUD instruments 19/20 = forward normal/over,
 * 21/22 = rear normal/over). Each bar is one cockpit-parts cel
 * (instruments[idx].param1) recoloured per frame to indicate fill
 * level — engine semantics from panel_updatemonolever +
 * rtsvga2_drawmonoshapeVGA. The shape acts as a stencil; every painted
 * pixel adopts the chosen palette colour. We achieve that via the
 * existing blit pipeline by zeroing `tint` and setting `bias` to the
 * resolved RGB (bias is alpha-weighted, so transparent pixels stay
 * clear). */
static void TieCockpitRenderer_DrawShieldQuad(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											  AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
											  int coord_w, int coord_h, int idx, const TieSnapshot* snap) {
	if (!entry->parts_tex)
		return;
	if ((snap->hud.working_subsystems & 0x20u) == 0)
		return; /* no shields */

	const TieHudInstrument* ins = &snap->hud.instruments[idx];
	const uint32_t pal = snap->palette[(uint8_t)ins->value];
	float r, g, b;
	TieScene2dSrgb_PalToLinearRgb(pal, &r, &g, &b);

	float u0, v0, u1, v1;
	int cw, ch;
	if (!TieCockpitRenderer_ResolveCel(entry, ins->param1, &u0, &v0, &u1, &v1, &cw, &ch))
		return;

	float ax, ay;
	TieCockpitRenderer_InsAnchor(entry, idx, ins->x, ins->y, snap, &ax, &ay);
	TieCockpitRenderer_BlitCockpitPartQuadMono(cg, cmd, pass, entry, coord_w, coord_h, ax, ay, (float)cw,
											   (float)ch, u0, v0, u1, v1, r, g, b);
}

/* BEAM_ARCBAR (HUD instrument 35). Mirrors panel_updatebeam
 * (panel.c:1052): 9 LED slots along an arc, each tinted from
 * kBeamColors[] based on whether the cumulative beam_charge fills,
 * partially-fills, or is below this bucket of 1000 charge units.
 * Each LED uses a distinct cel (param1 + i). Rendered MONO_SHAPE so the
 * cel acts as an alpha mask × the chosen palette colour. */
static void TieCockpitRenderer_DrawBeamArcbar(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
											  AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
											  int coord_w, int coord_h, int idx, const TieSnapshot* snap) {
	if (!entry->parts_tex)
		return;
	if ((snap->hud.working_subsystems & 0x10u) == 0)
		return; /* no beam capability */

	const TieHudInstrument* ins = &snap->hud.instruments[idx];
	const bool svga = TieCockpitCommon_IsSvga(snap);

	/* Base anchor + arc stride. Authored `stride_x/y:` wins. */
	float base_x, base_y;
	TieCockpitRenderer_InsAnchor(entry, idx, ins->x, ins->y, snap, &base_x, &base_y);
	int16_t stride_x_auth = 0, stride_y_auth = 0;
	TieCockpitLayout_Stride(&entry->layout, idx, &stride_x_auth, &stride_y_auth);
	float x_step, y_step; /* per-i step in ref-frame px */
	if (stride_x_auth != 0 && stride_y_auth != 0) {
		x_step = (float)stride_x_auth;
		y_step = (float)stride_y_auth;
	} else {
		float x_scale, y_scale;
		TieCockpitLayout_ClassicPixelSize(&entry->layout, (int)snap->cockpit.classic_w,
										  (int)snap->cockpit.classic_h, &x_scale, &y_scale);
		x_step = (svga ? 3.0f : 2.0f) * x_scale;
		y_step = (svga ? 3.0f : 1.0f) * y_scale;
	}

	/* step_rev runs 8..0 across i = 0..8. */
	for (int i = 0, step_rev = 8; i < 9; ++i, --step_rev) {
		const float led_x = base_x + (float)step_rev * x_step;
		const float led_y = base_y + (float)step_rev * y_step;

		int frame = ins->param1 + i;
		float u0, v0, u1, v1;
		int cw, ch;
		if (!TieCockpitRenderer_ResolveCel(entry, frame, &u0, &v0, &u1, &v1, &cw, &ch))
			continue;

		const uint32_t pal = snap->palette[snap->hud.beam_arc_led_colors[i]];
		float r, g, b;
		TieScene2dSrgb_PalToLinearRgb(pal, &r, &g, &b);
		TieCockpitRenderer_BlitCockpitPartQuadMono(cg, cmd, pass, entry, coord_w, coord_h, led_x, led_y,
												   (float)cw, (float)ch, u0, v0, u1, v1, r, g, b);
	}
}

/* Lookup-only — TieCockpitRenderer_EnsureCrtMask must have run earlier this frame. */
static AeronTexture* TieCockpitRenderer_CrtMaskLookup(const TieCockpitRenderer* cg, uint8_t variant,
													  uint16_t classic_w) {
	for (int i = 0; i < cg->crt_mask_count; ++i) {
		const TieCockpitRendererCrtMaskEntry* entry = &cg->crt_masks[i];
		if (entry->variant == variant && entry->classic_w == classic_w)
			return entry->tex;
	}
	return NULL;
}

/* Matches cockpit_pip_compose.vert.hlsl's PipComposeVS cbuffer. */
typedef struct {
	float dst_rect[4]; /* NDC: (x_bl, y_bl, width, height) */
	float src_rect[4]; /* UV:  (u0, v0, u1, v1) */
} TieCockpitRendererPipComposeUniforms;

/* color_tex × mask_tex's alpha into the cockpit RT rect, in cockpit-
 * coord space. Mask was baked at the PIP RT's native resolution so
 * a full (0..1, 0..1) UV is correct. */
static void TieCockpitRenderer_DrawPipCompose(TieCockpitRenderer* cg, AeronRenderPass* pass,
											  AeronTexture* color_tex, AeronTexture* mask_tex, float dst_x,
											  float dst_y, float dst_w, float dst_h) {
	if (!cg->pip_pipeline || !color_tex || !mask_tex)
		return;

	const TieCockpitRendererDrawSpace* space = &cg->pending_space;
	const float px = space->fit_x + dst_x * space->scale_x;
	const float py = space->fit_y + dst_y * space->scale_y;
	const float pw = dst_w * space->scale_x;
	const float ph = dst_h * space->scale_y;
	float ndc_x = (px / (float)space->target_w) * 2.0f - 1.0f;
	float ndc_w = (pw / (float)space->target_w) * 2.0f;
	float ndc_y = 1.0f - ((py + ph) / (float)space->target_h) * 2.0f;
	float ndc_h = (ph / (float)space->target_h) * 2.0f;

	TieCockpitRendererPipComposeUniforms u = {
		.dst_rect = { ndc_x, ndc_y, ndc_w, ndc_h },
		.src_rect = { 0.0f, 0.0f, 1.0f, 1.0f },
	};

	Aeron_BindGraphicsPipeline(pass, cg->pip_pipeline);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 0, color_tex, cg->pip_color_sampler);
	Aeron_BindTextureSampler(pass, AERON_SHADER_STAGE_FRAGMENT, 1, mask_tex, cg->pip_mask_sampler);
	Aeron_BindUniformData(pass, AERON_SHADER_STAGE_VERTEX, 0, &u, sizeof u);
	Aeron_Draw(pass, 4, 0);
}

/* 3DCRT widget (idx 2). Composites the PIP pre-pass RT onto the CRT
 * rect through the per-spec cutout mask. Engine analogue:
 * panel_update3Dcrt's xtrans2 path (panel.c:2497). A snapshot without
 * a live PIP target leaves the cockpit bitmap's CRT bezel visible. */
static void TieCockpitRenderer_Draw3dcrt(TieCockpitRenderer* cg, AeronRenderPass* pass,
										 const TieCockpitRendererEntry* entry, const TieSnapshot* snap) {
	const TieHudInstrument* ins = &snap->hud.instruments[2];
	float dst_x, dst_y, dst_w, dst_h;
	TieCockpitRenderer_ResolvePipRect(entry, snap, ins->x, ins->y, ins->param1, ins->param2, &dst_x, &dst_y,
									  &dst_w, &dst_h);

	if (dst_w <= 0.0f || dst_h <= 0.0f)
		return;
	if (!snap->cockpit.pip_target_present || !(snap->hud.working_subsystems & 0x01u))
		return;
	if (!cg->flight_gpu)
		return;

	AeronTexture* pip_texture = TieFlightRenderer_PipTexture(cg->flight_gpu);
	if (!pip_texture)
		return;

	AeronTexture* mask_tex =
		TieCockpitRenderer_CrtMaskLookup(cg, snap->cockpit.mask_variant, snap->cockpit.classic_w);
	if (!mask_tex)
		return;
	TieCockpitRenderer_DrawPipCompose(cg, pass, pip_texture, mask_tex, dst_x, dst_y, dst_w, dst_h);
}

/* PIP subsystem target box (engine: panel.c:2683 inside
 * panel_update3Dcrt). Always 4×4 classic px (no close-range
 * threshold) centred on the world-projected position of the
 * currently-selected subsystem on the target ship. Drawn AFTER the
 * 3DCRT compose so it sits on top of the composited PIP image at the
 * cockpit instruments[2] rect. Aspect-independent: HD does its own
 * PIP-camera projection using snapshot pip_cam_ori + pip_back_step
 * (the same back-step-distance-pre-subtracted trick TieCockpitRenderer_Draw3dcrt's
 * the flight PIP camera uses for far-target precision). */
static void TieCockpitRenderer_DrawPipTargetBox(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												int coord_w, int coord_h, const TieSnapshot* snap) {
	if (!snap->hud.target_subsystem_box_engine_ok)
		return;
	if (!snap->cockpit.pip_target_present || !(snap->hud.working_subsystems & 0x01u))
		return;
	/* Engine path: panel_drawboxinxtrans only fires inside
	 * panel_update3Dcrt, which panel_updatepanel calls strictly when
	 * camera.pilotview == 0 (panel.c:355). Other views never call
	 * panel_update3Dcrt, so the box never rasterises into xtrans even
	 * when the cached s_pip_subsys_* fields remain valid. HD's
	 * snapshot publishes those fields as sticky state across ticks
	 * (so the PIP image survives the engine's pause), which means
	 * the box snapshot stays "engine_ok" in non-cockpit views too.
	 * Mirror the engine gate explicitly: view_idx == 0 only. */
	if (snap->cockpit.view_idx != 0)
		return;

	const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_CMD_3D_CRT];
	float dst_x, dst_y, dst_w, dst_h;
	TieCockpitRenderer_ResolvePipRect(entry, snap, ins->x, ins->y, ins->param1, ins->param2, &dst_x, &dst_y,
									  &dst_w, &dst_h);
	if (dst_w <= 0.0f || dst_h <= 0.0f)
		return;

	/* Engine final gate (panel.c:2689):
	 *     radar_enable && target_obj_idx < NUM_CRAFTS  (= 32)
	 * The engine_ok flag already implies "valid subsystem mesh on the
	 * target ship" (set inside the `if (mesh)` block at panel.c:2647);
	 * the NUM_CRAFTS gate keeps the box restricted to the 32 active-
	 * fighter slots — warheads (32..79), capital ships (80..119) and
	 * statics (>= 0x3800) are excluded even when engine_ok is true. */
	const uint16_t slot = snap->hud.target_obj_slot;
	if (slot >= 32u)
		return; /* NUM_CRAFTS = 32 */

	if (!TieCockpitRenderer_FlightWorldPosition(snap, slot))
		return;

	/* PIP camera via the shared precision-preserving recipe — same
	 * cam_pos_centered + view matrix the PIP ship rendering uses, so
	 * the box and the ship share the same float32 cancellation pattern
	 * (see TieRenderMath_BuildPipCamera comment). */
	float cam_pos[3];
	float view[16];
	TieRenderMath_BuildPipCamera(cam_pos, view, snap->cockpit.pip_back_step, snap->cockpit.pip_cam_ori);

	/* PIP projection. */
	float h_half = snap->cockpit.pip_fov_h_half_rad;
	float v_half = snap->cockpit.pip_fov_v_half_rad;
	if (h_half <= 0.0f)
		h_half = 0.5586f;
	if (v_half <= 0.0f)
		v_half = 0.3f;
	float proj[16];
	TieRenderMath_Mat4PerspectiveReverseZ(proj, h_half, v_half, 1.0f);
	float view_proj[16];
	TieRenderMath_Mat4Multiply(view_proj, proj, view);

	/* The target is the PIP origin, so its integral subsystem offset is
	 * already a precise scene-local point. */
	const float sub_wx = (float)snap->hud.target_subsystem_offset[0];
	const float sub_wy = (float)snap->hud.target_subsystem_offset[1];
	const float sub_wz = (float)snap->hud.target_subsystem_offset[2];

	float ndc_x, ndc_y, clip_w;
	if (!TieCockpitRenderer_ProjectWorldToNdc(view_proj, sub_wx, sub_wy, sub_wz, &ndc_x, &ndc_y, &clip_w))
		return;
	if (ndc_x < -1.0f || ndc_x > 1.0f)
		return;
	if (ndc_y < -1.0f || ndc_y > 1.0f)
		return;

	/* Engine arithmetic (panel.c:2693-2695):
	 *     sx = transfm2_getscreenx(...);              // projected x
	 *     sy = transfm2_getscreeny(...) - 2;          // projected y - 2
	 *     panel_drawboxinxtrans(sx - 2, sy, 4, 4, 0xCE);
	 * Box dimensions (4×4) and the -2 offset are classic-px; scale
	 * into the layout's ref frame so the box stays visible at HD
	 * sizes. */
	const float pip_proj_x_cl = dst_x + (ndc_x + 1.0f) * 0.5f * dst_w;
	const float pip_proj_y_cl = dst_y + (1.0f - ndc_y) * 0.5f * dst_h;

	/* Raw palette index 0xCE — same direct lookup as the radar bracket. */
	const uint32_t pal = snap->palette[0xCEu];
	float r, g, b;
	TieScene2dSrgb_PalToLinearRgb(pal, &r, &g, &b);

	float pixel_sx, pixel_sy;
	TieCockpitLayout_ClassicPixelSize(&entry->layout, (int)snap->cockpit.classic_w,
									  (int)snap->cockpit.classic_h, &pixel_sx, &pixel_sy);
	const int box_w = (int)(4.0f * pixel_sx + 0.5f);
	const int box_h = (int)(4.0f * pixel_sy + 0.5f);
	TieCockpitRenderer_BlitEngineHollowBox(cg, cmd, pass, coord_w, coord_h, pip_proj_x_cl - 2.0f * pixel_sx,
										   pip_proj_y_cl - 2.0f * pixel_sy, box_w, box_h, pixel_sx, pixel_sy,
										   r, g, b);
}

/* Per-view widget allow-list. The engine paints different widget
 * subsets per pilotview (panel_updatepanel:339-358). The full table
 * (view 0) runs every widget. Forward (19), threat (20), and the
 * side / padlock views run subsets — encoded as (idx, kind) gates
 * relative to the dispatch table. Returns true when (view, idx, kind)
 * should draw. */
static bool TieCockpitRenderer_ViewAllowsWidget(uint8_t view_idx, int idx,
												TieCockpitRendererHudWidgetKind kind) {
	/* Threat-weapon LED slots are only painted in threat view (20).
	 * In other views, engine's panel_updatethreatweapons never runs;
	 * leaving these on would paint frame-0 "absent" cels over unrelated
	 * cockpit areas (the slots carry threat-view layout positions). */
	if (idx >= TIE_HUDI_THREAT_ION && idx <= TIE_HUDI_THREAT_BEAM && view_idx != 20)
		return false;

	switch (view_idx) {
		case 0:
			/* Full HUD — every kind allowed. */
			return true;

		case 19: {
			/* Forward view (panel.c:354-358): radar + laser LEDs + gunsight
			 * + weapon-warnings + weapon-fire status levers. */
			if (kind == HUD_W_RADAR_DISC)
				return true;
			if (kind == HUD_W_LASER_LED_ROW)
				return true;
			if (kind == HUD_W_SHAPE_LEVER) {
				if (idx == TIE_HUDI_GUNSIGHT)
					return true;
				if (idx == TIE_HUDI_WARN_INCOMING)
					return true;
				if (idx == TIE_HUDI_WARN_LOCK)
					return true;
				if (idx == TIE_HUDI_WARN_IMPACT)
					return true;
				if (idx >= TIE_HUDI_WEAPON_FIRE_FIRST && idx <= TIE_HUDI_WEAPON_FIRE_LAST)
					return true;
			}
			return false;
		}

		case 20:
			/* Threat readout view — threat-weapon LEDs + threat shield/hull
			 * digit fields + the CMD-readout text slot (idx 90). */
			if (idx >= TIE_HUDI_THREAT_ION && idx <= TIE_HUDI_THREAT_HULL_PCT)
				return true;
			if (idx == 90)
				return true;
			return false;

		default:
			/* Side / look-around views (1..16, 21..27): engine makes no
			 * HUD writes here. Title view (17) is handled by text only.
			 * View 18 (panel hidden) gates upstream. */
			return false;
	}
}

/* Pilotview dispatch. Full view runs the whole widget table;
 * forward / threat / padlock variants are subsets via TieCockpitRenderer_ViewAllowsWidget. */
/* Surface targets (defined with the cockpit text helpers below). */
static bool TieCockpitRenderer_BeginSurface(TieCockpitRenderer* cg, int id);
static void TieCockpitRenderer_EndSurface(TieCockpitRenderer* cg, bool began);
static bool TieCockpitRenderer_DcReplaces(const TieCockpitRenderer* cg, int id);

static void TieCockpitRenderer_DrawWidgetsForView(TieCockpitRenderer* cg, AeronCommandBuffer* cmd,
												  AeronRenderPass* pass, const TieCockpitRendererEntry* entry,
												  int coord_w, int coord_h, int first_idx, int last_idx,
												  const TieSnapshot* snap) {
	const uint8_t v = snap->cockpit.view_idx;
	if (v == 17 || v == 18)
		return; /* title=text-only, 18=hidden */

	if (first_idx < 0)
		first_idx = 0;
	if (last_idx >= TIE_MAX_HUD_INSTRUMENTS)
		last_idx = TIE_MAX_HUD_INSTRUMENTS - 1;
	for (int idx = first_idx; idx <= last_idx; ++idx) {
		const TieHudInstrument* ins = &snap->hud.instruments[idx];
		if (!TieCockpitCommon_InstrumentActive(ins))
			continue;
		TieCockpitRendererHudWidgetKind kind = (TieCockpitRendererHudWidgetKind)hud_widget_kind[idx];
		if (!TieCockpitRenderer_ViewAllowsWidget(v, idx, kind) || TieCockpitRenderer_DcReplaces(cg, idx))
			continue;

		float scale_x, scale_y, angle_deg;
		TieCockpitEditor_Transform(idx, &scale_x, &scale_y, &angle_deg);
		cg->widget_xf.active = scale_x != 1.0f || scale_y != 1.0f || angle_deg != 0.0f;
		cg->widget_xf.pivot_x = cg->editor_pivot_x[idx];
		cg->widget_xf.pivot_y = cg->editor_pivot_y[idx];
		cg->widget_xf.scale_x = scale_x;
		cg->widget_xf.scale_y = scale_y;
		cg->widget_xf.cos_a = cosf(angle_deg * 3.14159265f / 180.0f);
		cg->widget_xf.sin_a = sinf(angle_deg * 3.14159265f / 180.0f);
		const bool on_surface = TieCockpitRenderer_BeginSurface(cg, idx);
		switch (kind) {
			case HUD_W_RADAR_DISC:
				TieCockpitRenderer_DrawRadarDisc(cg, cmd, pass, entry, coord_w, coord_h, idx, snap);
				break;
			case HUD_W_LASER_LED_ROW:
				TieCockpitRenderer_DrawLaserLedRow(cg, cmd, pass, entry, coord_w, coord_h, idx, snap);
				break;
			case HUD_W_3DCRT:
				TieCockpitRenderer_Draw3dcrt(cg, pass, entry, snap);
				break;
			case HUD_W_VERT_SLIDER:
				TieCockpitRenderer_DrawVertSlider(cg, cmd, pass, entry, coord_w, coord_h, idx, snap);
				break;
			case HUD_W_SHIELD_QUAD:
				TieCockpitRenderer_DrawShieldQuad(cg, cmd, pass, entry, coord_w, coord_h, idx, snap);
				break;
			case HUD_W_SHAPE_LEVER:
				TieCockpitRenderer_DrawShapeLever(cg, cmd, pass, entry, coord_w, coord_h, idx, snap);
				break;
			case HUD_W_BEAM_ARCBAR:
				TieCockpitRenderer_DrawBeamArcbar(cg, cmd, pass, entry, coord_w, coord_h, idx, snap);
				break;
			case HUD_W_DAMAGE_CRACK:
			case HUD_W_COVER:
				/* Drawn by TieCockpitRenderer_DrawDamageCracks / TieCockpitRenderer_DrawCovers which run
				 * BEFORE this pass — engine z-order requires the indicator
				 * plates and covers sit UNDER live widgets. */
				break;
			default:
				break;
		}
		TieCockpitRenderer_EndSurface(cg, on_surface);
	}
	cg->widget_xf.active = false;
}

static bool TieCockpitRenderer_CockpitRecordLists(TieCockpitRenderer* cg, AeronCommandBuffer* cmd, int rt_w,
												  int rt_h, const TieSnapshot* snap);
static bool TieCockpitRenderer_FlightHudRecordLists(TieCockpitRenderer* cg, AeronCommandBuffer* cmd, int rt_w,
													int rt_h, const TieSnapshot* snap);

bool TieCockpitRenderer_Prepare(TieCockpitRenderer* cg, AeronCommandBuffer* cmd, const TieSnapshot* snap) {
	if (!cg)
		return false;
	cg->pending_ready = false;
	cg->pending_entry = NULL;

	/* Consume a debug-UI reload request before the entry lookup so the
	 * active cockpit re-loads from disk this frame (others lazily). */
	if (s_cockpit_reload_request) {
		TieCockpitRenderer_CockpitReloadCaches(cg);
		s_cockpit_reload_request = false;
	}

	if (!cmd || !snap)
		return false;
	const bool flight = snap->scene_kind == TIE_SCENE_FLIGHT && snap->replay_mode != 2;
	/* The cockpit editor is disabled: TFTC cockpits take their HUD layout
	 * from their DynamicCockpit .dc files (dc_layout.c). */
	TieCockpitEditor_UseModel(NULL, NULL, false, false);
	if (!flight)
		return true;

	/* Original-installation fonts used by global flight HUD text must be
	 * available even when no cockpit assets are needed in external view. */
	const uint16_t font_classic_w = snap->cockpit.classic_w ? snap->cockpit.classic_w : 640;
	if (cg->assets && !TieFlightAssetSource_IsRemastered(cg->assets) &&
		!TieCockpitRenderer_EnsureOriginalFonts(cg, cmd, font_classic_w))
		return false;

	int rt_w = 0, rt_h = 0;
	if (cg->flight_gpu)
		TieFlightRenderer_GetRtDims(cg->flight_gpu, &rt_w, &rt_h);
	if (rt_w <= 0 || rt_h <= 0) {
		Aeron_RequestFatalRendererError("flight HUD render-target dimensions");
		return false;
	}

	/* View 18 hides the cockpit panel, not framebuffer-level flight HUD. */
	if (snap->cockpit.view_idx == 18 || !snap->cockpit.view_name[0]) {
		cg->pending_ready = TieCockpitRenderer_FlightHudRecordLists(cg, cmd, rt_w, rt_h, snap);
		return cg->pending_ready;
	}

	/* Asset loads + PIP pre-pass + CRT-mask load all need the command
	 * buffer OUTSIDE any render pass (the backend forbids overlapping
	 * copy / render passes). All of this MUST run before the caller
	 * opens the flight-main render pass. */
	TieCockpitRendererEntry* entry = TieCockpitRenderer_FindOrAllocEntry(cg, &snap->cockpit);
	if (!entry) {
		cg->pending_ready = TieCockpitRenderer_FlightHudRecordLists(cg, cmd, rt_w, rt_h, snap);
		return cg->pending_ready;
	}
	if (!TieCockpitRenderer_EnsureCockpitAssets(cg, cmd, entry, snap))
		return false;

	/* Nothing to draw if neither bitmap nor parts atlas loaded. */
	if (!entry->base_tex && !entry->parts_tex) {
		cg->pending_ready = TieCockpitRenderer_FlightHudRecordLists(cg, cmd, rt_w, rt_h, snap);
		return cg->pending_ready;
	}

	if (snap->cockpit.pip_target_present)
		(void)TieCockpitRenderer_EnsureCrtMask(cg, cmd, snap->cockpit.mask_variant, snap->cockpit.classic_w);

	/* Bake the static chrome layer if any keyed input changed. Runs here,
	 * before the flight-main pass opens, because it needs its own render
	 * pass. Needs the flight RT dims — only available via flight_gpu, which
	 * the prepare call site guarantees is bound. Without it the composite
	 * path stays disabled (chrome_valid false) and draw falls back to the
	 * per-frame direct draws. */
	int coord_w, coord_h;
	TieCockpitRenderer_ResolveCoordFrame(entry, snap, &coord_w, &coord_h);
	if (!TieCockpitRenderer_ChromeKeyMatches(cg, entry, snap, coord_w, coord_h, rt_w, rt_h) &&
		!TieCockpitRenderer_BakeCockpitChrome(cg, cmd, entry, snap, coord_w, coord_h, rt_w, rt_h))
		return false;

	cg->pending_entry = entry;
	cg->pending_space = TieCockpitRenderer_CockpitDrawSpace(coord_w, coord_h, rt_w, rt_h);
	cg->pending_ready = TieCockpitRenderer_CockpitRecordLists(cg, cmd, rt_w, rt_h, snap);
	return cg->pending_ready;
}

static bool TieCockpitRenderer_FlightHudRecordLists(TieCockpitRenderer* cg, AeronCommandBuffer* cmd, int rt_w,
													int rt_h, const TieSnapshot* snap) {
	if (!cg || !cmd || !snap)
		return false;
	int coord_w, coord_h;
	TieCockpitRenderer_ResolveCoordFrame(NULL, snap, &coord_w, &coord_h);
	cg->pending_space = TieCockpitRenderer_CockpitDrawSpace(coord_w, coord_h, rt_w, rt_h);

	TIE_GPU_PUSH(cmd, "Flight HUD");
	AeronDrawList_Begin(cg->before_pip_list, NULL, rt_w, rt_h, AERON_DRAWLIST2D_LOAD, NULL);
	AeronDrawList_Begin(cg->after_pip_list, NULL, rt_w, rt_h, AERON_DRAWLIST2D_LOAD, NULL);

	cg->record_list = cg->before_pip_list;
	const bool look_1998 = (cg->flight_gpu != NULL) && TieFlightRenderer_UsesSceneModels(cg->flight_gpu);
	TIE_GPU_MARKER(cmd, "Flight target box");
	TieCockpitRenderer_DrawMainTargetBox(cg, cmd, NULL, NULL, rt_w, rt_h, snap, look_1998);

	cg->record_list = cg->after_pip_list;
	TIE_GPU_MARKER(cmd, "Flight message bar");
	TieCockpitRenderer_DrawMessageBar(cg, cmd, NULL, snap);
	cg->record_list = NULL;

	bool ok = AeronDrawList_Prepare(cg->before_pip_list, cmd) != 0;
	ok = AeronDrawList_Prepare(cg->after_pip_list, cmd) != 0 && ok;
	TIE_GPU_POP(cmd);
	if (!ok)
		Aeron_RequestFatalRendererError("flight HUD draw-list preparation");
	return ok;
}

static const TieDcLayout* TieCockpitRenderer_DcLayout(const TieCockpitRenderer* cg) {
	return cg->flight_gpu ? TieFlightRenderer_Cockpit3dLayout(cg->flight_gpu) : NULL;
}

static struct TieCockpitRendererSurface* TieCockpitRenderer_SurfaceFor(TieCockpitRenderer* cg, int id) {
	const TieDcLayout* dc = TieCockpitRenderer_DcLayout(cg);
	if (dc) {
		const TieDcSource source = TieDcLayout_SourceOf(id);
		for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES && source != TIE_DC_NONE; ++i)
			if (cg->surfaces[i].live && cg->surfaces[i].screen >= 0 &&
				dc->screens[cg->surfaces[i].screen].source == source)
				return &cg->surfaces[i];
		return NULL;
	}
	if (!TieCockpitEditor_Surface(id))
		return NULL;
	const int group = TieCockpitEditor_Group(id);
	for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES; ++i)
		if (cg->surfaces[i].live && cg->surfaces[i].group == group)
			return &cg->surfaces[i];
	return NULL;
}

/* Redirects drawing of instrument `id` into its surface target, if it has
 * one; TieCockpitRenderer_EndSurface restores the screen. */
static bool TieCockpitRenderer_BeginSurface(TieCockpitRenderer* cg, int id) {
	struct TieCockpitRendererSurface* surface = TieCockpitRenderer_SurfaceFor(cg, id);
	if (!surface)
		return false;
	cg->surface_saved_list = cg->record_list;
	cg->surface_saved_space = cg->pending_space;
	cg->record_list = surface->list;
	cg->pending_space = surface->space;
	return true;
}

static void TieCockpitRenderer_EndSurface(TieCockpitRenderer* cg, bool began) {
	if (!began)
		return;
	cg->record_list = cg->surface_saved_list;
	cg->pending_space = cg->surface_saved_space;
}

/* TFTC DC sources drawn as XWA draws them, in place of the TIE instruments:
 * the laser charge bars, the recharge strips, the throttle bar, the radio
 * messages and mission goals boxes, the beam sphere, the targeting computer
 * and the speed, clock, warhead, countermeasure and shield readouts. */
static bool TieCockpitRenderer_DcGaugeSource(TieDcSource source) {
	switch (source) {
		case TIE_DC_LASERS:
		case TIE_DC_LASER_POWER:
		case TIE_DC_SHIELD_POWER:
		case TIE_DC_BEAM_POWER:
		case TIE_DC_ENGINE_POWER:
		case TIE_DC_THROTTLE:
		case TIE_DC_SHIELD_FRONT_TEXT:
		case TIE_DC_SHIELD_BACK_TEXT:
		case TIE_DC_LEFT_TEXT:
		case TIE_DC_RIGHT_TEXT:
		case TIE_DC_BEAM:
		case TIE_DC_SPEED:
		case TIE_DC_CLOCK:
		case TIE_DC_MISSILES:
		case TIE_DC_CHAFF:
		case TIE_DC_TARGETING:
		case TIE_DC_TARGET_NAME:
		case TIE_DC_TARGET_SHIELDS:
		case TIE_DC_TARGET_HULL:
		case TIE_DC_TARGET_SYSTEM:
		case TIE_DC_TARGET_DISTANCE:
		case TIE_DC_TARGET_CARGO:
		case TIE_DC_TARGET_PART:
		case TIE_DC_MISSILES_ALL:
			return true;
		default:
			return false;
	}
}

/* Text in the TFTC screens' font: `height` px tall, `x` the anchor for
 * `align`, `y` the top. Returns the drawn width. */
static float TieCockpitRenderer_DcText(TieCockpitRenderer* cg, AeronDrawList2D* list, float x, float y, float height,
									   AeronTextAlign align, const char* text, const float rgba[4],
									   const AeronRectI* scissor) {
	if (!cg->dc_font.loaded || !cg->dc_font.cell_h || !text[0])
		return 0.0f;
	const AeronTextStyle style = {
		.font = &cg->dc_font,
		.scale = height / (float)cg->dc_font.cell_h,
		.tint = { rgba[0], rgba[1], rgba[2], rgba[3] },
		.filter = AERON_BLIT2D_FILTER_LINEAR,
	};
	AeronText_Draw(list, &style, x, y, align, text, -1, scissor);
	return AeronText_MeasureWidth(&style, text, -1);
}

/* A short readout centred in a w x h target, as large as fits. */
static void TieCockpitRenderer_DcReadout(TieCockpitRenderer* cg, AeronDrawList2D* list, float w, float h,
										 const char* text, const float rgba[4]) {
	if (!cg->dc_font.loaded || !cg->dc_font.cell_h)
		return;
	const AeronTextStyle unit = { .font = &cg->dc_font, .scale = 1.0f / (float)cg->dc_font.cell_h };
	const float width_per_px = AeronText_MeasureWidth(&unit, text, -1);
	float height = h * 0.8f;
	if (width_per_px > 0.0f && width_per_px * height > w * 0.9f)
		height = w * 0.9f / width_per_px;
	TieCockpitRenderer_DcText(cg, list, w * 0.5f, (h - height) * 0.5f, height, AERON_TEXT_CENTER, text, rgba, NULL);
}

/* A palette entry as straight RGBA, brightened toward XWA's lit screen text. */
static void TieCockpitRenderer_DcPaletteColour(const TieSnapshot* snap, uint8_t index, float out[4]) {
	const uint32_t c = snap->palette[index];
	const float r = (float)((c >> 16) & 0xFFu) / 255.0f, g = (float)((c >> 8) & 0xFFu) / 255.0f,
				b = (float)(c & 0xFFu) / 255.0f;
	const float peak = fmaxf(fmaxf(r, g), fmaxf(b, 0.05f));
	const float lift = peak < 0.9f ? 0.9f / peak : 1.0f;
	out[0] = fminf(r * lift, 1.0f);
	out[1] = fminf(g * lift, 1.0f);
	out[2] = fminf(b * lift, 1.0f);
	out[3] = 1.0f;
}

static const float k_dc_white[4] = { 0.95f, 0.95f, 0.95f, 1.0f };

/* A paragraph flowing in the TFTC screens' font. Text starts at `*pen` on
 * line `*line` (lines `line_h` apart from `top`), wraps at words between
 * `left` and `right`, and continues from `indent` on later lines. Lines at
 * or past `max_lines` are counted but not drawn; a NULL list only counts. */
typedef struct TieDcFlow {
	float top, line_h, left, right, indent;
	int max_lines;
	float pen;
	int line;
} TieDcFlow;

static void TieCockpitRenderer_DcFlow(TieCockpitRenderer* cg, AeronDrawList2D* list, TieDcFlow* flow,
									  const char* text, const float rgba[4], const AeronRectI* clip) {
	if (!cg->dc_font.loaded || !cg->dc_font.cell_h)
		return;
	const float height = flow->line_h * 0.92f;
	const AeronTextStyle style = {
		.font = &cg->dc_font,
		.scale = height / (float)cg->dc_font.cell_h,
		.tint = { rgba[0], rgba[1], rgba[2], rgba[3] },
		.filter = AERON_BLIT2D_FILTER_LINEAR,
	};
	const float space = AeronText_MeasureWidth(&style, " ", 1);
	for (const char* p = text; *p;) {
		while (*p == ' ')
			++p;
		const char* end = p;
		while (*end && *end != ' ')
			++end;
		if (end == p)
			break;
		const int length = (int)(end - p);
		float word_w = AeronText_MeasureWidth(&style, p, length);
		const float line_start = flow->line ? flow->indent : flow->left;
		if (flow->pen > line_start && flow->pen + word_w > flow->right) {
			++flow->line;
			flow->pen = flow->indent;
		}
		/* A word longer than the line is cut at the margin. */
		int shown = length;
		while (shown > 1 && flow->pen + word_w > flow->right)
			word_w = AeronText_MeasureWidth(&style, p, --shown);
		if (list && flow->line < flow->max_lines)
			AeronText_Draw(list, &style, flow->pen, flow->top + (float)flow->line * flow->line_h, AERON_TEXT_LEFT, p,
						   shown, clip);
		flow->pen += word_w + space;
		p = end;
	}
}

/* The screens sample their targets clamped at the edges, so text kept off
 * the last texels avoids streaks across the rest of the display. */
#define TIE_DC_TEXT_MARGIN 0.06f

/* XWA's radio messages box: its title, then the newest messages that fit,
 * each after its mission time, wrapping under the message. */
static void TieCockpitRenderer_DrawDcRadio(TieCockpitRenderer* cg, AeronDrawList2D* list, float w, float h,
										   const TieSnapshot* snap) {
	const float line_h = h / (cg->dc_hologram ? 22.0f : 20.0f), left = w * TIE_DC_TEXT_MARGIN,
				right = w * (1.0f - TIE_DC_TEXT_MARGIN);
	const AeronRectI clip = { 1, 1, (int)w - 2, (int)h - 2 };
	TieCockpitRenderer_DcText(cg, list, left, line_h * 1.2f, line_h * 1.25f, AERON_TEXT_LEFT, "Radio Messages",
							  k_dc_white, &clip);
	const float top = line_h * 3.0f;
	const int max_lines = (int)((h * (1.0f - TIE_DC_TEXT_MARGIN) - top) / line_h);
	char times[TIE_HUD_RADIO_LINES][16];
	int lines[TIE_HUD_RADIO_LINES];
	float time_w[TIE_HUD_RADIO_LINES];
	const AeronTextStyle unit = { .font = &cg->dc_font, .scale = line_h * 0.92f / (float)(cg->dc_font.cell_h ? cg->dc_font.cell_h : 1) };
	for (int i = 0; i < snap->hud.radio_count; ++i) {
		if (snap->hud.radio[i].hours)
			snprintf(times[i], sizeof times[i], "%u:%02u:%02u", snap->hud.radio[i].hours, snap->hud.radio[i].minutes,
					 snap->hud.radio[i].seconds);
		else
			snprintf(times[i], sizeof times[i], "%u:%02u", snap->hud.radio[i].minutes, snap->hud.radio[i].seconds);
		time_w[i] = AeronText_MeasureWidth(&unit, times[i], -1) + line_h * 0.4f;
		TieDcFlow measure = { .left = left + time_w[i], .right = right, .indent = left + time_w[i], .pen = left + time_w[i],
							  .line_h = line_h, .max_lines = 0 };
		TieCockpitRenderer_DcFlow(cg, NULL, &measure, snap->hud.radio[i].text, k_dc_white, &clip);
		lines[i] = measure.line + 1;
	}
	/* Oldest first, dropping the oldest messages that do not fit. */
	int first = snap->hud.radio_count, used = 0;
	while (first > 0 && used + lines[first - 1] <= max_lines)
		used += lines[--first];
	float y = top;
	for (int i = first; i < snap->hud.radio_count; ++i) {
		TieCockpitRenderer_DcText(cg, list, left, y, line_h * 0.92f, AERON_TEXT_LEFT, times[i], k_dc_white, &clip);
		float colour[4];
		TieCockpitRenderer_DcPaletteColour(snap, TieCockpitText_RemapColor(snap->hud.radio[i].color), colour);
		TieDcFlow flow = { .top = y, .left = left + time_w[i], .right = right, .indent = left + time_w[i],
						   .pen = left + time_w[i], .line_h = line_h, .max_lines = lines[i] };
		TieCockpitRenderer_DcFlow(cg, list, &flow, snap->hud.radio[i].text, colour, &clip);
		y += (float)lines[i] * line_h;
	}
}

/* XWA's mission goals box: its title, the outcome, score and bonus, then the
 * goals screen's rows. Rows the screen centres (the goal categories) stay
 * centred; goal rows wrap with their continuation indented. */
static void TieCockpitRenderer_DrawDcGoals(TieCockpitRenderer* cg, AeronDrawList2D* list, float w, float h,
										   const TieSnapshot* snap) {
	const float line_h = h / (cg->dc_hologram ? 24.0f : 22.0f), left = w * TIE_DC_TEXT_MARGIN,
				right = w * (1.0f - TIE_DC_TEXT_MARGIN);
	const AeronRectI clip = { 1, 1, (int)w - 2, (int)h - 2 };
	TieCockpitRenderer_DcText(cg, list, (left + right) * 0.5f, line_h * 1.2f, line_h * 1.3f, AERON_TEXT_CENTER,
							  "Mission Goals", k_dc_white, &clip);
	static const char* const k_outcomes[3] = { "Unresolved", "Victory", "Failed" };
	float header[4];
	TieCockpitRenderer_DcPaletteColour(snap, 0x3A, header);
	char text[96];
	snprintf(text, sizeof text, "Outcome: %s",
			 k_outcomes[snap->hud.goals_outcome < 3 ? snap->hud.goals_outcome : 0]);
	const float centre = (left + right) * 0.5f, row_h = line_h * 0.92f;
	float y = line_h * 2.9f;
	TieCockpitRenderer_DcText(cg, list, centre, y, row_h, AERON_TEXT_CENTER, text, header, &clip);
	y += line_h;
	snprintf(text, sizeof text, "Score: %d   Bonus: %d", (int)snap->hud.goals_score, (int)snap->hud.goals_bonus);
	TieCockpitRenderer_DcText(cg, list, centre, y, row_h, AERON_TEXT_CENTER, text, header, &clip);
	y += line_h * 1.4f;
	const float bottom = h * (1.0f - TIE_DC_TEXT_MARGIN);
	for (int i = 0; i < snap->hud.objective_count && y + line_h <= bottom;) {
		const int first = i;
		const bool centred = snap->hud.objectives[first].x > 12;
		if (centred) {
			/* A category heading: its runs measured and centred together. */
			float total = 0.0f;
			for (int j = i; j < snap->hud.objective_count && snap->hud.objectives[j].y == snap->hud.objectives[first].y; ++j) {
				const AeronTextStyle unit = { .font = &cg->dc_font,
											  .scale = row_h / (float)(cg->dc_font.cell_h ? cg->dc_font.cell_h : 1) };
				total += AeronText_MeasureWidth(&unit, snap->hud.objectives[j].text, -1);
			}
			/* XWA's goal categories read in the outcome's yellow. */
			float pen = centre - total * 0.5f;
			for (; i < snap->hud.objective_count && snap->hud.objectives[i].y == snap->hud.objectives[first].y; ++i)
				pen += TieCockpitRenderer_DcText(cg, list, pen, y, row_h, AERON_TEXT_LEFT, snap->hud.objectives[i].text,
												 header, &clip);
			y += line_h;
			continue;
		}
		TieDcFlow flow = { .top = y, .left = left, .right = right, .indent = left + line_h, .pen = left,
						   .line_h = line_h, .max_lines = (int)((bottom - y) / line_h) };
		for (; i < snap->hud.objective_count && snap->hud.objectives[i].y == snap->hud.objectives[first].y; ++i) {
			float colour[4];
			TieCockpitRenderer_DcPaletteColour(snap, snap->hud.objectives[i].color, colour);
			TieCockpitRenderer_DcFlow(cg, list, &flow, snap->hud.objectives[i].text, colour, &clip);
		}
		y += (float)(flow.line + 1) * line_h;
	}
}

/* XWA's beam display: a striped blue sphere in a red ring. */
static void TieCockpitRenderer_DrawDcBeamSphere(AeronDrawList2D* list, float w, float h) {
	const float r = fminf(w, h) * 0.34f, cx = w * 0.5f, cy = h * 0.5f;
	AeronDrawList2DRRect ring = {
		.dst_x = cx - r * 1.18f,
		.dst_y = cy - r * 1.18f,
		.dst_w = r * 2.36f,
		.dst_h = r * 2.36f,
		.radius_px = r * 1.18f,
		.border_px = fmaxf(r * 0.12f, 1.5f),
		.soft_px = 1.0f,
		.border = { 0.55f * 0.9f, 0.08f * 0.9f, 0.12f * 0.9f, 0.9f },
	};
	AeronDrawList_AddRRect(list, &ring);
	enum { BANDS = 7 };
	const float band = 2.0f * r / (float)BANDS;
	for (int i = 0; i < BANDS; ++i) {
		const float mid = -r + band * ((float)i + 0.5f);
		const float half = sqrtf(fmaxf(r * r - mid * mid, 0.0f));
		const float thick = band * 0.62f;
		/* Lit from the upper right: brighter toward the top. */
		const float light = 0.55f + 0.45f * (1.0f - (float)i / (float)(BANDS - 1));
		AeronDrawList2DRRect stripe = {
			.dst_x = cx - half * 0.55f,
			.dst_y = cy + mid - thick * 0.5f,
			.dst_w = half * 1.5f,
			.dst_h = thick,
			.radius_px = thick * 0.5f,
			.soft_px = 1.0f,
			.fill_top = { 0.35f * light, 0.62f * light, 0.9f * light, 1.0f },
			.fill_bottom = { 0.25f * light, 0.48f * light, 0.78f * light, 1.0f },
		};
		AeronDrawList_AddRRect(list, &stripe);
	}
}

/* Front or rear shield strength, 0..200 percent (above 100 overcharged), or
 * -1 for a craft without shields or with them down (status flag 0x01, which
 * panel_updateshields also tests; the TIE fighter's never come up). It
 * paints each bar's normal and overcharge rows with shieldcolor[lit LEDs
 * 0..9] (10 flashes). */
static int TieCockpitRenderer_DcShieldPercent(const TieSnapshot* snap, bool front) {
	static const uint8_t k_shieldcolor[11] = { 0x2C, 0x35, 0x36, 0x37, 0x39, 0x3A, 0x3B, 0x3D, 0x3E, 0x3F, 0x2F };
	if (!(snap->hud.working_subsystems & 0x20u) || !(snap->hud.status_flags & 0x01u))
		return -1;
	int leds[2] = { 0, 0 };
	const int ids[2] = { front ? TIE_HUDI_SHIELD_FWD_NORMAL : TIE_HUDI_SHIELD_REAR_NORMAL,
						 front ? TIE_HUDI_SHIELD_FWD_OVER : TIE_HUDI_SHIELD_REAR_OVER };
	for (int row = 0; row < 2; ++row) {
		const uint8_t colour = (uint8_t)snap->hud.instruments[ids[row]].value;
		for (int i = 0; i < 11; ++i)
			if (k_shieldcolor[i] == colour)
				leds[row] = i > 9 ? 9 : i;
	}
	if (leds[1] > 0)
		leds[0] = 9;
	return (leds[0] + leds[1]) * 100 / 9;
}

/* XWA's shield colours by strength: dark, red, yellow, green, cyan when overcharged. */
static void TieCockpitRenderer_DcShieldColour(int percent, float out[4]) {
	const float dark[4] = { 0.05f, 0.12f, 0.06f, 0.6f }, red[4] = { 0.95f, 0.2f, 0.15f, 1.0f },
				yellow[4] = { 0.95f, 0.85f, 0.1f, 1.0f }, green[4] = { 0.1f, 0.9f, 0.25f, 1.0f },
				cyan[4] = { 0.3f, 0.9f, 1.0f, 1.0f };
	const float* c = percent <= 0 ? dark : percent < 25 ? red : percent < 50 ? yellow : percent <= 100 ? green : cyan;
	memcpy(out, c, sizeof dark);
}

/* The shield ring around the craft on the shields screen: its top half the
 * front shield, its bottom half the rear, dark on a craft without shields. */
static void TieCockpitRenderer_DrawDcShieldRing(const struct TieCockpitRendererSurface* surface,
												const TieSnapshot* snap) {
	const float w = (float)surface->w, h = (float)surface->h;
	const float size = fminf(w, h) * 0.8f, x = (w - size) * 0.5f, y = (h - size) * 0.5f;
	for (int half = 0; half < 2; ++half) {
		float colour[4];
		TieCockpitRenderer_DcShieldColour(TieCockpitRenderer_DcShieldPercent(snap, half == 0), colour);
		AeronDrawList2DRRect ring = {
			.dst_x = x,
			.dst_y = y,
			.dst_w = size,
			.dst_h = size,
			.radius_px = size * 0.5f,
			.border_px = fmaxf(size * 0.06f, 2.0f),
			.soft_px = 1.0f,
			.border = { colour[0] * colour[3], colour[1] * colour[3], colour[2] * colour[3], colour[3] },
			.scissor = { 0, half ? (int)(h * 0.5f) + 1 : 0, (int)w, (int)(h * 0.5f) - 1 },
		};
		AeronDrawList_AddRRect(surface->list, &ring);
	}
}

/* XWA's radar screen under the blips: a dark glass over the cockpit's dimly
 * lit radar backdrop, with a short white tick at each quarter of its rim. */
static void TieCockpitRenderer_DrawDcRadarBackdrop(const struct TieCockpitRendererSurface* surface) {
	const float w = (float)surface->w, h = (float)surface->h;
	const float glass = 0.75f, tick[4] = { 0.4f, 0.4f, 0.4f, 1.0f };
	AeronDrawList2DRRect fill = {
		.dst_w = w,
		.dst_h = h,
		.fill_top = { 0.0f, 0.0f, 0.0f, glass },
		.fill_bottom = { 0.0f, 0.0f, 0.0f, glass },
	};
	AeronDrawList_AddRRect(surface->list, &fill);
	const float cx = w * 0.5f, cy = h * 0.5f, r = fminf(w, h) * 0.42f;
	const float length = fminf(w, h) * 0.08f, stroke = fmaxf(fminf(w, h) * 0.012f, 1.5f);
	const float ticks[4][4] = {
		{ cx - stroke * 0.5f, cy - r - length * 0.5f, stroke, length },
		{ cx - stroke * 0.5f, cy + r - length * 0.5f, stroke, length },
		{ cx - r - length * 0.5f, cy - stroke * 0.5f, length, stroke },
		{ cx + r - length * 0.5f, cy - stroke * 0.5f, length, stroke },
	};
	for (int i = 0; i < 4; ++i) {
		AeronDrawList2DRRect mark = {
			.dst_x = ticks[i][0],
			.dst_y = ticks[i][1],
			.dst_w = ticks[i][2],
			.dst_h = ticks[i][3],
			.soft_px = 1.0f,
			.fill_top = { tick[0], tick[1], tick[2], tick[3] },
			.fill_bottom = { tick[0], tick[1], tick[2], tick[3] },
		};
		AeronDrawList_AddRRect(surface->list, &mark);
	}
}

static void TieCockpitRenderer_DcFill(AeronDrawList2D* list, float x, float y, float w, float h, float r, float g,
									  float b, float a) {
	AeronDrawList2DRRect rect = {
		.dst_x = x,
		.dst_y = y,
		.dst_w = w,
		.dst_h = h,
		.soft_px = 1.0f,
		.fill_top = { r * a, g * a, b * a, a },
		.fill_bottom = { r * a, g * a, b * a, a },
	};
	AeronDrawList_AddRRect(list, &rect);
}

/* A sleek screen for a hologram text box: dark gunmetal glass with rounded
 * corners, fading down the screen. */
static void TieCockpitRenderer_DrawDcHologramPanel(AeronDrawList2D* list, float w, float h) {
	AeronDrawList2DRRect glass = {
		.dst_w = w,
		.dst_h = h,
		.radius_px = fminf(w, h) * 0.06f,
		.soft_px = 1.5f,
		.fill_top = { 0.014f, 0.015f, 0.017f, 1.0f },
		.fill_bottom = { 0.006f, 0.006f, 0.007f, 1.0f },
	};
	AeronDrawList_AddRRect(list, &glass);
}

/* True when instrument `id` is not drawn as itself on this cockpit: a DC
 * gauge stands in for it, or it belongs to a display the cockpit has no
 * screen for, which XWA leaves off the HUD too. */
static bool TieCockpitRenderer_DcReplaces(const TieCockpitRenderer* cg, int id) {
	if (id == TIE_HUDI_REC_LED || id == TIE_HUDI_REC_PCT)
		return true; /* the replay recording indicator is not shown */
	const TieDcLayout* dc = TieCockpitRenderer_DcLayout(cg);
	const TieDcSource source = TieDcLayout_SourceOf(id);
	if (!dc || source == TIE_DC_NONE)
		return false;
	if (TieCockpitRenderer_DcGaugeSource(source))
		return true;
	for (int s = 0; s < dc->count; ++s)
		if (dc->screens[s].source == source)
			return false;
	return true;
}

/* Displays of a few characters (XWA's AUTOSIZE_* sources): their screens
 * hug the text so it fills them. */
static bool TieCockpitRenderer_DcTextSource(TieDcSource source) {
	return source == TIE_DC_SPEED || source == TIE_DC_CLOCK || source == TIE_DC_THROTTLE;
}

/* A free surface target of w x h px for DC screen `s`, cleared and live. */
static struct TieCockpitRendererSurface* TieCockpitRenderer_OpenDcSurface(TieCockpitRenderer* cg, int s, int w,
																		  int h) {
	struct TieCockpitRendererSurface* surface = NULL;
	for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES && !surface; ++i)
		if (!cg->surfaces[i].live && (cg->surfaces[i].screen == s || !cg->surfaces[i].rt))
			surface = &cg->surfaces[i];
	for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES && !surface; ++i)
		if (!cg->surfaces[i].live)
			surface = &cg->surfaces[i];
	if (!surface)
		return NULL;
	if (!surface->rt || surface->w != w || surface->h != h) {
		if (surface->rt)
			Aeron_DestroyRenderTarget(surface->rt);
		surface->rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width = w,
			.height = h,
			.format = COCKPIT_CHROME_FORMAT,
			.sample_count = AERON_SAMPLE_COUNT_1,
		});
		surface->w = w;
		surface->h = h;
	}
	if (!surface->list)
		surface->list = AeronDrawList_Create(TIE_COCKPIT_SURFACE_RECORDS);
	if (!surface->rt || !surface->list)
		return NULL;
	surface->group = -1;
	surface->screen = s;
	surface->brightness = 1.0f;
	static const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	AeronDrawList_Begin(surface->list, surface->rt, w, h, AERON_DRAWLIST2D_CLEAR, clear);
	surface->live = true;
	return surface;
}

static void TieCockpitRenderer_DrawDcTargeting(TieCockpitRenderer* cg, const struct TieCockpitRendererSurface* surface,
											   const TieSnapshot* snap);
static void TieCockpitRenderer_DrawDcTargetReadout(TieCockpitRenderer* cg,
												  const struct TieCockpitRendererSurface* surface, TieDcSource source,
												  const TieSnapshot* snap);

/* Draws a DC gauge across its whole target, left to right along the
 * display. `occurrence` counts earlier screens of the source: the second
 * laser bar shows the craft's second cannon when it has one. */
static void TieCockpitRenderer_DrawDcGauge(TieCockpitRenderer* cg, const struct TieCockpitRendererSurface* surface,
										   const TieCockpitRendererEntry* entry, TieDcSource source, int occurrence,
										   const TieSnapshot* snap) {
	const float w = (float)surface->w, h = (float)surface->h;
	switch (source) {
		case TIE_DC_LEFT_TEXT:
			TieCockpitRenderer_DrawDcRadio(cg, surface->list, w, h, snap);
			return;
		case TIE_DC_RIGHT_TEXT:
			TieCockpitRenderer_DrawDcGoals(cg, surface->list, w, h, snap);
			return;
		case TIE_DC_BEAM:
			TieCockpitRenderer_DrawDcBeamSphere(surface->list, w, h);
			return;
		case TIE_DC_TARGETING:
			TieCockpitRenderer_DrawDcTargeting(cg, surface, snap);
			return;
		case TIE_DC_TARGET_NAME:
		case TIE_DC_TARGET_SHIELDS:
		case TIE_DC_TARGET_HULL:
		case TIE_DC_TARGET_SYSTEM:
		case TIE_DC_TARGET_DISTANCE:
		case TIE_DC_TARGET_CARGO:
		case TIE_DC_TARGET_PART:
			TieCockpitRenderer_DrawDcTargetReadout(cg, surface, source, snap);
			return;
		case TIE_DC_MISSILES_ALL: {
			int count = 0;
			for (int i = TIE_HUDI_MISSILE_AMMO_FIRST; i <= TIE_HUDI_MISSILE_AMMO_LAST; ++i)
				if (TieCockpitCommon_InstrumentActive(&snap->hud.instruments[i]) && snap->hud.instruments[i].value > 0)
					count += snap->hud.instruments[i].value;
			char text[8];
			snprintf(text, sizeof text, "%d", count);
			TieCockpitRenderer_DcReadout(cg, surface->list, w, h, text, k_dc_white);
			return;
		}
		case TIE_DC_SPEED: {
			/* panel_updatespeed: percent of the craft's top speed. */
			const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_SPEED_DIGITS];
			if (!TieCockpitCommon_InstrumentActive(ins) || ins->value < 0)
				return;
			char text[8];
			snprintf(text, sizeof text, "%d", ins->value);
			TieCockpitRenderer_DcReadout(cg, surface->list, w, h, text, k_dc_white);
			return;
		}
		case TIE_DC_CLOCK: {
			/* panel_updateclock: mission seconds, or the time left in training. */
			const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_CLOCK_DIGITS];
			if (ins->value < 0)
				return;
			char text[12];
			snprintf(text, sizeof text, "%d:%02d", ins->value / 60, ins->value % 60);
			TieCockpitRenderer_DcReadout(cg, surface->list, w, h, text, k_dc_white);
			return;
		}
		case TIE_DC_MISSILES: {
			/* The warheads left on the craft's left (first, third) or right
			 * (second, fourth) launchers. */
			int count = 0;
			for (int i = TIE_HUDI_MISSILE_AMMO_FIRST + (occurrence & 1); i <= TIE_HUDI_MISSILE_AMMO_LAST; i += 2)
				if (TieCockpitCommon_InstrumentActive(&snap->hud.instruments[i]) && snap->hud.instruments[i].value > 0)
					count += snap->hud.instruments[i].value;
			char text[8];
			snprintf(text, sizeof text, "%d", count);
			TieCockpitRenderer_DcReadout(cg, surface->list, w, h, text, k_dc_white);
			return;
		}
		case TIE_DC_CHAFF:
			TieCockpitRenderer_DcReadout(cg, surface->list, w, h, "0", k_dc_white);
			return;
		default:
			break;
	}
	if (source == TIE_DC_SHIELD_FRONT_TEXT || source == TIE_DC_SHIELD_BACK_TEXT) {
		/* "200%": the shield percentage centred, in the screens' font. */
		const int percent = TieCockpitRenderer_DcShieldPercent(snap, source == TIE_DC_SHIELD_FRONT_TEXT);
		if (percent < 0)
			return;
		if (cg->dc_font.loaded) {
			char text[8];
			snprintf(text, sizeof text, "%d%%", percent);
			TieCockpitRenderer_DcReadout(cg, surface->list, w, h, text, k_dc_white);
			return;
		}
		if (!cg->text_renderer)
			return;
		char text[8];
		snprintf(text, sizeof text, "%d%%", percent);
		TieUIText rec[1];
		int count = 0;
		if (!TieCockpitText_Push(rec, &count, 1, 0, 0, text, TieCockpitText_RemapColor(0x3F)))
			return;
		/* About 5 x 7 px a character in the 640 x 480 frame. */
		const float text_w = 5.0f * (float)strlen(text), text_h = 7.0f;
		const float k = fminf(w / text_w, h / text_h) * 0.85f;
		const float atlas_scale = TieCockpitRenderer_CockpitAtlasScaleFor(entry, 480);
		const TieScene2dTextSpace text_space = {
			.classic_w = 640,
			.classic_h = 480,
			.atlas_scale_x = atlas_scale,
			.atlas_scale_y = atlas_scale,
			.space_between_classic = 0.0f,
			.fit = TIE_SCENE2D_TEXT_FIT_FILL,
		};
		TieScene2dTextRenderer_RecordFestringScaled(cg->text_renderer, surface->list, 640, 480, &text_space, rec, count,
													snap->palette, (w - text_w * k) * 0.5f, (h - text_h * k) * 0.5f,
													k, k, surface->w, surface->h);
		return;
	}
	if (source == TIE_DC_LASERS) {
		/* XWA's charge bar: a row of glowing dashes per cannon on its side,
		 * the left cannons (even slots) on the first bar and the right on
		 * the second, on a dark backing with white ends; lasers green, ion
		 * cannons blue, later slots higher up (the TIE Defender's ions on
		 * top). panel_updatelasers: value = lit LEDs 0..10 for weapon slot
		 * (instrument - 3); color 0 = uncharged. */
		TieCockpitRenderer_DcFill(surface->list, 0.0f, 0.0f, w, h, 0.004f, 0.015f, 0.006f, 1.0f);
		const float tick = fmaxf(w * 0.008f, 1.5f);
		TieCockpitRenderer_DcFill(surface->list, w * 0.02f, h * 0.15f, tick, h * 0.7f, 0.85f, 0.85f, 0.85f, 1.0f);
		TieCockpitRenderer_DcFill(surface->list, w * 0.98f - tick, h * 0.15f, tick, h * 0.7f, 0.85f, 0.85f, 0.85f, 1.0f);
		const int cannons = snap->hud.weapon_group_cnt < 1 ? 1 : snap->hud.weapon_group_cnt > 8 ? 8 : snap->hud.weapon_group_cnt;
		const int side = cannons >= 2 ? (occurrence & 1) : 0, rows = (cannons + 1) / 2;
		const float left = w * 0.05f, step = (w * 0.90f) / 10.0f, pitch = h * 0.6f / (float)rows;
		const float dash_h = pitch * (rows == 1 ? 0.37f : 0.55f);
		for (int row = 0; row < rows; ++row) {
			const int slot = side + row * 2;
			if (slot >= cannons)
				continue;
			const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_LASER_LED_FIRST + slot];
			if (!TieCockpitCommon_InstrumentActive(ins))
				continue;
			const int lit = ins->color != 0 ? (ins->value < 0 ? 0 : ins->value > 10 ? 10 : ins->value) : 0;
			const bool ion = (snap->hud.weapon_ion_mask >> slot) & 1u;
			const float r = ion ? 0.15f : 0.1f, g = ion ? 0.45f : 1.0f, b = ion ? 1.0f : 0.1f;
			const float y = h * 0.2f + pitch * ((float)(rows - 1 - row) + 0.5f) - dash_h * 0.5f;
			for (int i = 0; i < lit; ++i) {
				const float x = left + step * (float)i;
				TieCockpitRenderer_DcFill(surface->list, x, y - dash_h * 0.4f, step * 0.94f, dash_h * 1.8f, r * 0.6f,
										  g * 0.6f, b * 0.6f, 0.2f);
				TieCockpitRenderer_DcFill(surface->list, x, y, step * 0.88f, dash_h, r, g, b, 1.0f);
			}
		}
		return;
	}
	if (source == TIE_DC_THROTTLE) {
		/* panel_updatethrottle: the digits read 0..100 percent. */
		const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_THROTTLE_DIGITS];
		if (!TieCockpitCommon_InstrumentActive(ins) || ins->value < 0)
			return;
		/* XWA's render_throttle in throttle_color (TFTC: yellow): the bar lit
		 * from the left to the throttle setting, the rest left dark so the
		 * cockpit's dot row shows through. */
		const float lit = (float)(ins->value < 0 ? 0 : ins->value > 100 ? 100 : ins->value) / 100.0f;
		if (lit > 0.0f)
			TieCockpitRenderer_DcFill(surface->list, 0.0f, 0.0f, w * lit, h, 1.0f, 0.92f, 0.15f, 1.0f);
		return;
	}
	/* XWA's recharge screens: a solid bar of the system's energy, lasers
	 * green, shields yellow, beam magenta (full rings on the TIE Avenger at
	 * a mission's start), dark on a craft without the system, filling from
	 * the left or the bottom. The recharge setting the TIE power sliders
	 * show (0..4, 2 normal) marks the bar like a dial: a bright tick at the
	 * setting among faint ticks at the steps. The engine power screen shows
	 * its setting alone. */
	const uint16_t cap = snap->hud.working_subsystems;
	const uint16_t sub = snap->hud.subsystem_active;
	float fraction = 0.0f;
	int level = -1;
	if (source == TIE_DC_LASER_POWER && (cap & 0x200u)) {
		for (int gr = 0; gr < snap->hud.weapon_group_cnt && gr < 8; ++gr) {
			const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_LASER_LED_FIRST + gr];
			if (TieCockpitCommon_InstrumentActive(ins) && ins->color != 0)
				fraction = fmaxf(fraction, (float)(ins->value < 0 ? 0 : ins->value > 10 ? 10 : ins->value) / 10.0f);
		}
		level = snap->hud.laser_power;
	} else if (source == TIE_DC_SHIELD_POWER && (cap & 0x800u) && (sub & 0x001u)) {
		const int front = TieCockpitRenderer_DcShieldPercent(snap, true), rear = TieCockpitRenderer_DcShieldPercent(snap, false);
		fraction = fminf((float)((front > 0 ? front : 0) + (rear > 0 ? rear : 0)) / 200.0f, 1.0f);
		level = snap->hud.shield_power;
	} else if (source == TIE_DC_BEAM_POWER && (cap & 0x1000u) && (sub & 0x100u)) {
		const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_BEAM_ARC];
		fraction = TieCockpitCommon_InstrumentActive(ins) && ins->value > 0 ? fminf((float)ins->value / 7.0f, 1.0f) : 0.0f;
		level = snap->hud.beam_power;
	} else if (source == TIE_DC_ENGINE_POWER) {
		const TieHudInstrument* ins = &snap->hud.instruments[TIE_HUDI_POWER_BALANCE];
		if (TieCockpitCommon_InstrumentActive(ins))
			level = ins->value;
	}
	static const float k_laser[3] = { 0.1f, 0.9f, 0.2f }, k_shield[3] = { 0.95f, 0.85f, 0.1f },
					   k_beam[3] = { 0.8f, 0.2f, 0.9f }, k_engine[3] = { 0.3f, 0.6f, 1.0f };
	const float* colour = source == TIE_DC_LASER_POWER	  ? k_laser
						  : source == TIE_DC_SHIELD_POWER ? k_shield
						  : source == TIE_DC_BEAM_POWER	  ? k_beam
														  : k_engine;
	const bool along_x = w >= h;
	const float length = along_x ? w : h;
	TieCockpitRenderer_DcFill(surface->list, 0.0f, 0.0f, w, h, 0.004f, 0.012f, 0.006f, 1.0f);
	if (fraction > 0.0f) {
		if (along_x)
			TieCockpitRenderer_DcFill(surface->list, 0.0f, 0.0f, w * fraction, h, colour[0], colour[1], colour[2], 1.0f);
		else
			TieCockpitRenderer_DcFill(surface->list, 0.0f, h * (1.0f - fraction), w, h * fraction, colour[0], colour[1],
									  colour[2], 1.0f);
	}
	if (level < 0)
		return;
	level = level > 4 ? 4 : level;
	if (source == TIE_DC_ENGINE_POWER) {
		/* The engine setting as a bar of its own: a quarter per step. */
		const float lit = (float)level / 4.0f;
		if (along_x)
			TieCockpitRenderer_DcFill(surface->list, 0.0f, 0.0f, w * lit, h, colour[0], colour[1], colour[2], 1.0f);
		else
			TieCockpitRenderer_DcFill(surface->list, 0.0f, h * (1.0f - lit), w, h * lit, colour[0], colour[1], colour[2],
									  1.0f);
	}
	const float tick = fmaxf(length * 0.012f, 1.5f), mark = fmaxf(length * 0.03f, 3.0f);
	for (int step = 0; step <= 4; ++step) {
		const bool at = step == level;
		const float size = at ? mark : tick;
		/* Step positions run from the bar's start to its end, the marks kept inside it. */
		const float along = (float)step / 4.0f * (length - size);
		const float x = along_x ? along : 0.0f, y = along_x ? 0.0f : length - along - size;
		const float sw = along_x ? size : w, sh = along_x ? h : size;
		if (at) {
			TieCockpitRenderer_DcFill(surface->list, x, y, sw, sh, 0.0f, 0.0f, 0.0f, 1.0f);
			const float inset = size * 0.25f;
			TieCockpitRenderer_DcFill(surface->list, x + (along_x ? inset : 0.0f), y + (along_x ? 0.0f : inset),
									  sw - (along_x ? 2.0f * inset : 0.0f), sh - (along_x ? 0.0f : 2.0f * inset), 0.9f,
									  0.9f, 0.9f, 1.0f);
		} else if (step > 0 && step < 4) {
			TieCockpitRenderer_DcFill(surface->list, x, y, sw, sh, 0.0f, 0.0f, 0.0f, 0.55f);
		}
	}
}

/* `text` without the 0xFE colour escapes panel_buildobjectname leaves in it. */
static void TieCockpitRenderer_DcPlainText(const char* text, char* out, size_t capacity) {
	size_t n = 0;
	for (const unsigned char* p = (const unsigned char*)text; *p && n + 1 < capacity; ++p) {
		if (*p == 0xFEu) {
			if (!p[1])
				break;
			++p;
			continue;
		}
		out[n++] = (char)*p;
	}
	out[n] = '\0';
}

/* A target readout value: the digits, or "" when the engine has none. */
static void TieCockpitRenderer_DcTargetValue(const TieSnapshot* snap, int id, char* out, size_t capacity) {
	const TieHudInstrument* ins = &snap->hud.instruments[id];
	if (TieCockpitCommon_InstrumentActive(ins) && ins->value >= 0)
		snprintf(out, capacity, "%d", ins->value);
	else
		out[0] = '\0';
}

/* One target readout on a screen of its own (the Missile Boat's), as large as
 * fits: the name in red, the rest in the targeting computer's blue. Empty
 * with no target. */
static void TieCockpitRenderer_DrawDcTargetReadout(TieCockpitRenderer* cg,
												  const struct TieCockpitRendererSurface* surface, TieDcSource source,
												  const TieSnapshot* snap) {
	const TieHudState* hud = &snap->hud;
	if (hud->target_obj_slot == 0xFFFFu || !(hud->working_subsystems & 0x01u))
		return;
	static const float k_red[4] = { 0.95f, 0.22f, 0.18f, 1.0f }, k_blue[4] = { 0.6f, 0.78f, 1.0f, 1.0f };
	char text[48] = "";
	switch (source) {
		case TIE_DC_TARGET_NAME:
			TieCockpitRenderer_DcPlainText(hud->target_name, text, sizeof text);
			break;
		case TIE_DC_TARGET_SHIELDS:
			TieCockpitRenderer_DcTargetValue(snap, TIE_HUDI_TARGET_SHIELD_PCT, text, sizeof text);
			break;
		case TIE_DC_TARGET_HULL:
			TieCockpitRenderer_DcTargetValue(snap, TIE_HUDI_TARGET_HULL_PCT, text, sizeof text);
			break;
		case TIE_DC_TARGET_SYSTEM:
			TieCockpitRenderer_DcTargetValue(snap, TIE_HUDI_TARGET_SUBSYSTEM_PCT, text, sizeof text);
			break;
		case TIE_DC_TARGET_DISTANCE: {
			TieCockpitRenderer_DcTargetValue(snap, TIE_HUDI_TARGET_DIST_KM_INT, text, sizeof text);
			const TieHudInstrument* frac = &hud->instruments[TIE_HUDI_TARGET_DIST_KM_FRAC];
			const size_t length = strlen(text);
			if (length)
				snprintf(text + length, sizeof text - length, ".%02d", frac->value >= 0 ? frac->value % 100 : 0);
			break;
		}
		case TIE_DC_TARGET_CARGO:
			snprintf(text, sizeof text, "%s", hud->target_cargo[0] ? hud->target_cargo : "No Cargo");
			break;
		case TIE_DC_TARGET_PART:
			TieCockpitRenderer_DcPlainText(hud->target_subsystem_text, text, sizeof text);
			break;
		default:
			break;
	}
	if (text[0])
		TieCockpitRenderer_DcReadout(cg, surface->list, (float)surface->w, (float)surface->h, text,
									 source == TIE_DC_TARGET_NAME ? k_red : k_blue);
}

/* XWA's targeting computer, filling its screen: the target's name in red
 * across the top, its 3D view tinted red in the middle, shields and hull on
 * the left, the targeted system and distance on the right, its cargo and the
 * targeted part along the bottom. Empty with no target (panel_updatecmd). */
static void TieCockpitRenderer_DrawDcTargeting(TieCockpitRenderer* cg, const struct TieCockpitRendererSurface* surface,
											   const TieSnapshot* snap) {
	const TieHudState* hud = &snap->hud;
	const float w = (float)surface->w, h = (float)surface->h;
	AeronDrawList2D* list = surface->list;
	/* XWA erases the screen to black under the display, covering what the
	 * cockpit's own texture paints there (the gunboat's glyph blocks). */
	TieCockpitRenderer_DcFill(list, 0.0f, 0.0f, w, h, 0.0f, 0.0f, 0.0f, 1.0f);
	if (hud->target_obj_slot == 0xFFFFu || !(hud->working_subsystems & 0x01u))
		return;
	const AeronRectI clip = { 1, 1, (int)w - 2, (int)h - 2 };
	/* The TIE cockpits' round screens leave room for a tenth more. */
	const char* model = cg->flight_gpu ? TieFlightRenderer_Cockpit3dModel(cg->flight_gpu) : NULL;
	const float k = model && strncmp(model, "Tie", 3) == 0 ? 1.1f : 1.0f;
	static const float k_red[4] = { 0.95f, 0.22f, 0.18f, 1.0f }, k_orange[4] = { 0.95f, 0.55f, 0.15f, 1.0f },
					   k_label[4] = { 0.35f, 0.55f, 1.0f, 1.0f }, k_value[4] = { 0.6f, 0.78f, 1.0f, 1.0f },
					   k_green[4] = { 0.3f, 0.95f, 0.4f, 1.0f };

	/* The target's view, as tall as the screen allows between the name and
	 * the bottom row, at the PIP's aspect, tinted red. */
	AeronTexture* pip_texture = cg->flight_gpu ? TieFlightRenderer_PipTexture(cg->flight_gpu) : NULL;
	if (pip_texture && snap->cockpit.pip_target_present && snap->cockpit.pip_w && snap->cockpit.pip_h) {
		const float aspect = (float)snap->cockpit.pip_w / (float)snap->cockpit.pip_h;
		float view_h = h * 0.88f * k, view_w = view_h * aspect;
		if (view_w > w * 0.5f * k) {
			view_w = w * 0.5f * k;
			view_h = view_w / aspect;
		}
		const float x0 = (w - view_w) * 0.5f, y0 = h * 0.1f + (h * 0.88f - view_h) * 0.5f;
		const float x1 = x0 + view_w, y1 = y0 + view_h;
		AeronDrawList2DQuad4 quad = {
			.texture = pip_texture,
			.tint = { 1.0f, 0.3f, 0.25f, 1.0f },
			.blend = AERON_BLIT2D_BLEND_PMA,
			.filter = AERON_BLIT2D_FILTER_LINEAR,
			.q = { 1.0f, 1.0f, 1.0f, 1.0f },
			/* The PIP frames its target with a margin XWA's wireframe has not: the middle of it fills the view. */
			.corners = { { x0, y0, 0.12f, 0.12f }, { x1, y0, 0.88f, 0.12f }, { x0, y1, 0.12f, 0.88f }, { x1, y1, 0.88f, 0.88f } },
		};
		AeronDrawList_AddQuad4(list, &quad);
	}

	const float left = w * 0.1f, right = w * 0.9f, row_h = h * 0.072f * k;
	char text[48];
	TieCockpitRenderer_DcPlainText(hud->target_name, text, sizeof text);
	TieCockpitRenderer_DcText(cg, list, w * 0.5f, h * 0.02f, h * 0.125f * k, AERON_TEXT_CENTER, text, k_red, &clip);
	if (hud->target_order_text[0]) {
		TieCockpitRenderer_DcPlainText(hud->target_order_text, text, sizeof text);
		TieCockpitRenderer_DcText(cg, list, w * 0.5f, h * 0.15f, h * 0.065f * k, AERON_TEXT_CENTER, text, k_orange, &clip);
	}

	/* Label over value, a column on each side of the view. */
	static const struct {
		const char* label;
		int id;
		bool right;
		float y;
	} k_readouts[] = {
		{ "SHD%", TIE_HUDI_TARGET_SHIELD_PCT, false, 0.3f },
		{ "HULL%", TIE_HUDI_TARGET_HULL_PCT, false, 0.54f },
		{ "SYS%", TIE_HUDI_TARGET_SUBSYSTEM_PCT, true, 0.3f },
		{ "DIST", TIE_HUDI_TARGET_DIST_KM_INT, true, 0.54f },
	};
	for (size_t i = 0; i < sizeof k_readouts / sizeof k_readouts[0]; ++i) {
		const float x = k_readouts[i].right ? right : left;
		const AeronTextAlign align = k_readouts[i].right ? AERON_TEXT_RIGHT : AERON_TEXT_LEFT;
		TieCockpitRenderer_DcText(cg, list, x, h * k_readouts[i].y, row_h, align, k_readouts[i].label, k_label, &clip);
		TieCockpitRenderer_DcTargetValue(snap, k_readouts[i].id, text, sizeof text);
		if (k_readouts[i].id == TIE_HUDI_TARGET_DIST_KM_INT && text[0]) {
			const TieHudInstrument* frac = &hud->instruments[TIE_HUDI_TARGET_DIST_KM_FRAC];
			const size_t length = strlen(text);
			snprintf(text + length, sizeof text - length, ".%02d", frac->value >= 0 ? frac->value % 100 : 0);
		}
		TieCockpitRenderer_DcText(cg, list, x, h * k_readouts[i].y + row_h * 0.95f, row_h, align, text, k_value, &clip);
	}

	TieCockpitRenderer_DcText(cg, list, left, h * 0.97f - row_h, row_h, AERON_TEXT_LEFT,
							  hud->target_cargo[0] ? hud->target_cargo : "No Cargo", k_label, &clip);
	if (hud->target_subsystem_text[0]) {
		TieCockpitRenderer_DcPlainText(hud->target_subsystem_text, text, sizeof text);
		TieCockpitRenderer_DcText(cg, list, right, h * 0.97f - row_h, row_h, AERON_TEXT_RIGHT, text, k_green, &clip);
	}
}

/* Opens a surface target for every TFTC DC screen with something to show.
 * Gauge sources get a target of the screen's shape with the gauge drawn in.
 * Instrument sources, on the first screen of each, get the box around their
 * instruments, widened to the screen's shape so nothing is stretched, at the
 * resolution it covers on screen. */
static void TieCockpitRenderer_BeginDcSurfaces(TieCockpitRenderer* cg, const TieDcLayout* dc, int coord_w,
											   int coord_h, const TieCockpitRendererEntry* entry,
											   const TieSnapshot* snap) {
	const TieCockpitRendererDrawSpace* screen = &cg->pending_space;
	int seen[TIE_DC_SOURCE_COUNT] = { 0 };
	for (int s = 0; s < dc->count; ++s) {
		const TieDcScreen* dc_screen = &dc->screens[s];
		const int occurrence = seen[dc_screen->source]++;
		if (TieCockpitRenderer_DcGaugeSource(dc_screen->source)) {
			const float aspect = dc_screen->aspect > 0.0f ? dc_screen->aspect : 1.0f;
			const float base = dc_screen->source == TIE_DC_TARGETING								   ? 1024.0f
							   : dc_screen->source == TIE_DC_LEFT_TEXT || dc_screen->source == TIE_DC_RIGHT_TEXT ? 768.0f
																												 : 256.0f;
			const int w = aspect >= 1.0f ? (int)base : (int)fmaxf(base * aspect, 8.0f);
			const int h = aspect >= 1.0f ? (int)fmaxf(base / aspect, 8.0f) : (int)base;
			struct TieCockpitRendererSurface* surface = TieCockpitRenderer_OpenDcSurface(cg, s, w, h);
			if (!surface)
				continue;
			/* XWA projects a hologram text box in the air; here it gets a screen. */
			if (dc_screen->hologram &&
				(dc_screen->source == TIE_DC_LEFT_TEXT || dc_screen->source == TIE_DC_RIGHT_TEXT))
				TieCockpitRenderer_DrawDcHologramPanel(surface->list, (float)w, (float)h);
			cg->dc_hologram = dc_screen->hologram;
			TieCockpitRenderer_DrawDcGauge(cg, surface, entry, dc_screen->source, occurrence, snap);
			cg->dc_hologram = false;
			continue;
		}
		if (occurrence > 0)
			continue;
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		for (int m = 0; m < TIE_COCKPIT_EDITOR_ID_COUNT; ++m) {
			if (!cg->editor_active[m] || TieDcLayout_SourceOf(m) != dc_screen->source)
				continue;
			x0 = fminf(x0, cg->editor_anchor_x[m]);
			y0 = fminf(y0, cg->editor_anchor_y[m]);
			x1 = fmaxf(x1, cg->editor_anchor_x[m] + cg->editor_extent_w[m]);
			y1 = fmaxf(y1, cg->editor_anchor_y[m] + cg->editor_extent_h[m]);
		}
		if (x1 < x0)
			continue;
		/* Instruments are anchored at a point; leave room for what they draw. */
		const float pad = TieCockpitRenderer_DcTextSource(dc_screen->source) ? 1.0f : 12.0f;
		float hw = (x1 - x0) * 0.5f + pad, hh = (y1 - y0) * 0.5f + pad;
		if (dc_screen->aspect > 0.0f) {
			const float aspect = dc_screen->aspect;
			if (hw / hh < aspect)
				hw = hh * aspect;
			else
				hh = hw / aspect;
		}
		const int w = (int)fminf(fmaxf(2.0f * hw * screen->scale_x, 16.0f), 2048.0f);
		const int h = (int)fminf(fmaxf(2.0f * hh * screen->scale_y, 16.0f), 2048.0f);
		struct TieCockpitRendererSurface* surface = TieCockpitRenderer_OpenDcSurface(cg, s, w, h);
		if (!surface)
			continue;
		const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
		surface->x0 = cx - hw;
		surface->y0 = cy - hh;
		surface->x1 = cx + hw;
		surface->y1 = cy + hh;
		const float k_x = (float)w / (2.0f * hw), k_y = (float)h / (2.0f * hh);
		surface->space = (TieCockpitRendererDrawSpace) {
			.target_w = w,
			.target_h = h,
			.coord_w = coord_w,
			.coord_h = coord_h,
			.fit_x = -surface->x0 * k_x,
			.fit_y = -surface->y0 * k_y,
			.fit_w = (float)coord_w * k_x,
			.fit_h = (float)coord_h * k_y,
			.scale_x = k_x,
			.scale_y = k_y,
		};
		if (dc_screen->source == TIE_DC_SHIELDS) {
			/* XWA shows a craft without shields as just its dim outline. */
			const bool shielded = TieCockpitRenderer_DcShieldPercent(snap, true) >= 0 ||
								  TieCockpitRenderer_DcShieldPercent(snap, false) >= 0;
			if (shielded)
				TieCockpitRenderer_DrawDcShieldRing(surface, snap);
			else
				surface->brightness = 0.12f; /* its instruments carry an HDR boost */
		} else if (dc_screen->source == TIE_DC_LEFT_SENSOR || dc_screen->source == TIE_DC_RIGHT_SENSOR) {
			TieCockpitRenderer_DrawDcRadarBackdrop(surface);
		}
	}
}

/* Opens a surface target for every editor group set to match the cockpit
 * surface: its box (scaled like the editor's handle) at screen resolution. */
static void TieCockpitRenderer_BeginSurfaces(TieCockpitRenderer* cg, int coord_w, int coord_h,
											 const TieCockpitRendererEntry* entry, const TieSnapshot* snap) {
	for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES; ++i)
		cg->surfaces[i].live = false;
	if (!cg->flight_gpu || !TieFlightRenderer_Cockpit3dActive(cg->flight_gpu))
		return;
	const TieDcLayout* dc = TieCockpitRenderer_DcLayout(cg);
	if (dc) {
		TieCockpitRenderer_BeginDcSurfaces(cg, dc, coord_w, coord_h, entry, snap);
		return;
	}
	const TieCockpitRendererDrawSpace* screen = &cg->pending_space;
	for (int id = 0; id < TIE_COCKPIT_EDITOR_ID_COUNT; ++id) {
		if (!cg->editor_active[id] || !TieCockpitEditor_Surface(id) || TieCockpitRenderer_SurfaceFor(cg, id))
			continue;
		const int group = TieCockpitEditor_Group(id);
		float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
		for (int m = 0; m < TIE_COCKPIT_EDITOR_ID_COUNT; ++m) {
			if (!cg->editor_active[m] || TieCockpitEditor_Group(m) != group)
				continue;
			x0 = fminf(x0, cg->editor_anchor_x[m]);
			y0 = fminf(y0, cg->editor_anchor_y[m]);
			x1 = fmaxf(x1, cg->editor_anchor_x[m] + cg->editor_extent_w[m]);
			y1 = fmaxf(y1, cg->editor_anchor_y[m] + cg->editor_extent_h[m]);
		}
		/* Instruments are anchored at a point; leave room for what they draw. */
		const float pad = 24.0f;
		float scale_x, scale_y, angle_deg;
		TieCockpitEditor_Transform(id, &scale_x, &scale_y, &angle_deg);
		const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
		const float hw = ((x1 - x0) * 0.5f + pad) * scale_x * 1.42f, hh = ((y1 - y0) * 0.5f + pad) * scale_y * 1.42f;
		struct TieCockpitRendererSurface* surface = NULL;
		for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES && !surface; ++i)
			if (cg->surfaces[i].group == group || !cg->surfaces[i].rt)
				surface = &cg->surfaces[i];
		for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES && !surface; ++i)
			if (!cg->surfaces[i].live)
				surface = &cg->surfaces[i];
		if (!surface)
			return;
		const int w = (int)fminf(fmaxf(2.0f * hw * screen->scale_x, 16.0f), 2048.0f);
		const int h = (int)fminf(fmaxf(2.0f * hh * screen->scale_y, 16.0f), 2048.0f);
		if (!surface->rt || surface->w != w || surface->h != h) {
			if (surface->rt)
				Aeron_DestroyRenderTarget(surface->rt);
			surface->rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
				.width = w,
				.height = h,
				.format = COCKPIT_CHROME_FORMAT,
				.sample_count = AERON_SAMPLE_COUNT_1,
			});
			surface->w = w;
			surface->h = h;
		}
		if (!surface->list)
			surface->list = AeronDrawList_Create(TIE_COCKPIT_SURFACE_RECORDS);
		if (!surface->rt || !surface->list)
			continue;
		surface->group = group;
		surface->screen = -1;
		surface->x0 = cx - hw;
		surface->y0 = cy - hh;
		surface->x1 = cx + hw;
		surface->y1 = cy + hh;
		const float k_x = (float)w / (2.0f * hw), k_y = (float)h / (2.0f * hh);
		surface->space = (TieCockpitRendererDrawSpace) {
			.target_w = w,
			.target_h = h,
			.coord_w = coord_w,
			.coord_h = coord_h,
			.fit_x = -surface->x0 * k_x,
			.fit_y = -surface->y0 * k_y,
			.fit_w = (float)coord_w * k_x,
			.fit_h = (float)coord_h * k_y,
			.scale_x = k_x,
			.scale_y = k_y,
		};
		static const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		AeronDrawList_Begin(surface->list, surface->rt, w, h, AERON_DRAWLIST2D_CLEAR, clear);
		surface->live = true;
	}
}

/* Lays a TFTC DC screen's target on the cockpit through each of its
 * triangles, perspective-correct: a quad whose last corner repeats the third
 * draws just the triangle. Display coordinates outside 0..1 sample the
 * target's transparent border. */
static void TieCockpitRenderer_DrawDcFaces(TieCockpitRenderer* cg, AeronTexture* texture, const float tint[4],
										   const TieDcScreen* dc_screen) {
	const TieCockpitRendererDrawSpace* screen = &cg->pending_space;
	for (int t = 0; t < dc_screen->triangle_count; ++t) {
		const TieDcTriangle* tri = &dc_screen->triangles[t];
		float clip[3][4];
		bool visible = true;
		for (int c = 0; c < 3 && visible; ++c)
			visible = TieFlightRenderer_Cockpit3dProject(cg->flight_gpu, tri->mesh, tri->position[c], clip[c]);
		if (!visible)
			continue;
		AeronDrawList2DQuad4 quad = {
			.texture = texture,
			.tint = { tint[0], tint[1], tint[2], tint[3] },
			.blend = AERON_BLIT2D_BLEND_PMA,
			.filter = AERON_BLIT2D_FILTER_LINEAR,
		};
		static const int k_corner[4] = { 0, 1, 2, 2 };
		for (int c = 0; c < 4; ++c) {
			const int i = k_corner[c];
			quad.corners[c][0] = (clip[i][0] / clip[i][3] + 1.0f) * 0.5f * (float)screen->target_w;
			quad.corners[c][1] = (1.0f - clip[i][1] / clip[i][3]) * 0.5f * (float)screen->target_h;
			quad.corners[c][2] = tri->display[i][0];
			quad.corners[c][3] = tri->display[i][1];
			/* q carries relative depth for perspective-correct sampling. */
			quad.q[c] = clip[i][3] / clip[0][3];
		}
		AeronDrawList_AddQuad4(cg->record_list, &quad);
	}
}

static void TieCockpitRenderer_DrawDcScreen(TieCockpitRenderer* cg, const struct TieCockpitRendererSurface* surface,
											const TieDcScreen* dc_screen) {
	const float b = surface->brightness, tint[4] = { b, b, b, b };
	TieCockpitRenderer_DrawDcFaces(cg, Aeron_RenderTargetGetTexture(surface->rt), tint, dc_screen);
}

/* The cockpit's effects image sequences on their textures' faces, under the
 * DC screens: frames covering a face, then light frames added over them (a
 * premultiplied colour with no alpha adds). XWA lights the covering frames
 * like the rest of the cockpit, so they are dimmed toward its light, and its
 * light frames glow faintly. */
static void TieCockpitRenderer_DrawDcEffects(TieCockpitRenderer* cg, bool over) {
	double seconds = 0.0;
	const TieDcEffects* effects = cg->flight_gpu ? TieFlightRenderer_Cockpit3dEffects(cg->flight_gpu, &seconds) : NULL;
	if (over) {
		/* The covers over the DC screens, as XWA draws them, unlit like them. */
		static const float k_over[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		for (int e = 0; effects && e < effects->count; ++e) {
			const TieDcEffect* effect = &effects->items[e];
			AeronTexture* frame = effect->over ? TieDcEffects_Frame(effects, effect, seconds) : NULL;
			if (frame)
				TieCockpitRenderer_DrawDcFaces(cg, frame, k_over, &effect->faces);
		}
		return;
	}
	static const float k_cover[4] = { 0.19f, 0.19f, 0.19f, 1.0f }, k_light[4] = { 0.15f, 0.15f, 0.15f, 0.0f };
	for (int pass = 0; effects && pass < 2; ++pass)
		for (int e = 0; e < effects->count; ++e) {
			const TieDcEffect* effect = &effects->items[e];
			AeronTexture* frame =
				!effect->over && effect->glow == (pass == 1) ? TieDcEffects_Frame(effects, effect, seconds) : NULL;
			if (frame)
				TieCockpitRenderer_DrawDcFaces(cg, frame, effect->glow ? k_light : k_cover, &effect->faces);
		}
}

/* Renders each surface target and lays it on the cockpit: the surface under
 * the box centre gives a point and normal; the box, as seen on a plane facing
 * the eye there, turns onto the surface plane and projects back as a
 * perspective quad. Without a hit the target is drawn flat. */
static void TieCockpitRenderer_FinishSurfaces(TieCockpitRenderer* cg, AeronCommandBuffer* cmd) {
	const TieCockpitRendererDrawSpace* screen = &cg->pending_space;
	TieCockpitRenderer_DrawDcEffects(cg, false);
	for (int i = 0; i < TIE_COCKPIT_MAX_SURFACES; ++i) {
		struct TieCockpitRendererSurface* surface = &cg->surfaces[i];
		if (!surface->live)
			continue;
		if (!AeronDrawList_Prepare(surface->list, cmd))
			continue;
		AeronDrawList_Render(surface->list, cmd);

		const float sx[4] = { surface->x0, surface->x1, surface->x0, surface->x1 }; /* TL, TR, BL, BR */
		const float sy[4] = { surface->y0, surface->y0, surface->y1, surface->y1 };
		float px[4], py[4], q[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		for (int c = 0; c < 4; ++c) {
			px[c] = screen->fit_x + sx[c] * screen->scale_x;
			py[c] = screen->fit_y + sy[c] * screen->scale_y;
		}
		const TieDcLayout* dc = surface->screen >= 0 ? TieCockpitRenderer_DcLayout(cg) : NULL;
		if (dc) {
			/* An instrument source shows on each of its screens: the TIE Bomber
			 * repeats its radars on the dials beside the canopy. Gauge sources
			 * draw each screen separately. */
			const TieDcSource source = dc->screens[surface->screen].source;
			for (int s = 0; s < dc->count; ++s)
				if (s == surface->screen || (dc->screens[s].source == source && !TieCockpitRenderer_DcGaugeSource(source)))
					TieCockpitRenderer_DrawDcScreen(cg, surface, &dc->screens[s]);
			continue;
		}
		const float centre_x = (px[0] + px[3]) * 0.5f, centre_y = (py[0] + py[3]) * 0.5f;
		float hit[3], normal[3], origin[3], dir0[3];
		if (!dc && TieFlightRenderer_Cockpit3dSurfaceAt(cg->flight_gpu, centre_x, centre_y, hit, normal) &&
			TieFlightRenderer_ScreenRay(cg->flight_gpu, centre_x, centre_y, origin, dir0)) {
			float corner[4][3];
			bool ok = true;
			for (int c = 0; c < 4 && ok; ++c) {
				float o[3], d[3];
				ok = TieFlightRenderer_ScreenRay(cg->flight_gpu, px[c], py[c], o, d);
				const float denom = d[0] * dir0[0] + d[1] * dir0[1] + d[2] * dir0[2];
				const float t = ((hit[0] - o[0]) * dir0[0] + (hit[1] - o[1]) * dir0[1] + (hit[2] - o[2]) * dir0[2]) /
								(denom != 0.0f ? denom : 1.0f);
				for (int k = 0; k < 3; ++k)
					corner[c][k] = o[k] + d[k] * t;
			}
			/* Camera-plane axes from the box edges; surface axes keep them as close as the plane allows. */
			float right[3], down[3], u[3], v[3];
			float right_len = 0.0f, down_len = 0.0f;
			for (int k = 0; k < 3; ++k) {
				right[k] = corner[1][k] - corner[0][k];
				down[k] = corner[2][k] - corner[0][k];
				right_len += right[k] * right[k];
				down_len += down[k] * down[k];
			}
			right_len = sqrtf(right_len);
			down_len = sqrtf(down_len);
			const float rn = right[0] * normal[0] + right[1] * normal[1] + right[2] * normal[2];
			float u_len = 0.0f;
			for (int k = 0; k < 3; ++k) {
				u[k] = right[k] - normal[k] * rn;
				u_len += u[k] * u[k];
			}
			u_len = sqrtf(u_len);
			ok = ok && right_len > 0.0f && down_len > 0.0f && u_len > 1e-6f;
			if (ok) {
				for (int k = 0; k < 3; ++k)
					u[k] /= u_len;
				v[0] = normal[1] * u[2] - normal[2] * u[1];
				v[1] = normal[2] * u[0] - normal[0] * u[2];
				v[2] = normal[0] * u[1] - normal[1] * u[0];
				if (v[0] * down[0] + v[1] * down[1] + v[2] * down[2] < 0.0f)
					for (int k = 0; k < 3; ++k)
						v[k] = -v[k];
				float placed_x[4], placed_y[4], placed_q[4];
				for (int c = 0; c < 4 && ok; ++c) {
					float a = 0.0f, b = 0.0f;
					for (int k = 0; k < 3; ++k) {
						a += (corner[c][k] - hit[k]) * right[k] / right_len;
						b += (corner[c][k] - hit[k]) * down[k] / down_len;
					}
					const float on_surface[3] = { hit[0] + u[0] * a + v[0] * b, hit[1] + u[1] * a + v[1] * b,
												  hit[2] + u[2] * a + v[2] * b };
					float clip[4];
					TieFlightRenderer_ProjectLocal(cg->flight_gpu, on_surface, clip);
					ok = clip[3] > 1e-4f;
					if (ok) {
						placed_x[c] = (clip[0] / clip[3] + 1.0f) * 0.5f * (float)screen->target_w;
						placed_y[c] = (1.0f - clip[1] / clip[3]) * 0.5f * (float)screen->target_h;
						placed_q[c] = clip[3];
					}
				}
				if (ok) {
					/* q carries relative depth for perspective-correct sampling. */
					const float base = placed_q[0];
					for (int c = 0; c < 4; ++c) {
						px[c] = placed_x[c];
						py[c] = placed_y[c];
						q[c] = placed_q[c] / base;
					}
				}
			}
		}
		AeronDrawList2DQuad4 quad = {
			.texture = Aeron_RenderTargetGetTexture(surface->rt),
			.tint = { 1.0f, 1.0f, 1.0f, 1.0f },
			.blend = AERON_BLIT2D_BLEND_PMA,
			.filter = AERON_BLIT2D_FILTER_LINEAR,
		};
		static const float k_u[4] = { 0.0f, 1.0f, 0.0f, 1.0f }, k_v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		for (int c = 0; c < 4; ++c) {
			quad.corners[c][0] = px[c];
			quad.corners[c][1] = py[c];
			quad.corners[c][2] = k_u[c];
			quad.corners[c][3] = k_v[c];
			quad.q[c] = q[c];
		}
		AeronDrawList_AddQuad4(cg->record_list, &quad);
	}
	TieCockpitRenderer_DrawDcEffects(cg, true);
}

/* Records text placed about a pivot (cockpit px): each record's origin is
 * scaled and turned about it, glyphs scaled but upright. Text left of or
 * above the frame is shifted into the unsigned range and back through the
 * fit origin. */
static void TieCockpitRenderer_RecordPlacedText(TieCockpitRenderer* cg, const TieCockpitRendererEntry* entry,
												int coord_w, int coord_h, TieUIText* recs, int n,
												const uint32_t* palette, float pivot_x, float pivot_y, float scale_x,
												float scale_y, float angle_deg) {
	if (!cg->text_renderer || n <= 0)
		return;
	const float atlas_scale = TieCockpitRenderer_CockpitAtlasScaleFor(entry, coord_h);
	const TieScene2dTextSpace text_space = {
		.classic_w = coord_w,
		.classic_h = coord_h,
		.atlas_scale_x = atlas_scale,
		.atlas_scale_y = atlas_scale,
		.space_between_classic = 0.0f,
		.fit = TIE_SCENE2D_TEXT_FIT_FILL,
	};
	const TieCockpitRendererDrawSpace* space = &cg->pending_space;
	const float c = cosf(angle_deg * 3.14159265f / 180.0f), sn = sinf(angle_deg * 3.14159265f / 180.0f);
	for (int r = 0; r < n; ++r) {
		const float rx = ((float)recs[r].x - pivot_x) * scale_x, ry = ((float)recs[r].y - pivot_y) * scale_y;
		const float ox = pivot_x + rx * c - ry * sn, oy = pivot_y + rx * sn + ry * c;
		TieUIText one = recs[r];
		one.x = 0;
		one.y = 0;
		TieScene2dTextRenderer_RecordFestringScaled(cg->text_renderer, cg->record_list, coord_w, coord_h,
													&text_space, &one, 1, palette, space->fit_x + ox * space->scale_x,
													space->fit_y + oy * space->scale_y, space->scale_x * scale_x,
													space->scale_y * scale_y, space->target_w, space->target_h);
	}
}

/* Repairs HUD for TFTC cockpits: the damage-control readout of
 * damage_outputsystem, one row per system, as a movable, scalable panel. */
static void TieCockpitRenderer_DrawRepairs(TieCockpitRenderer* cg, const TieCockpitRendererEntry* entry,
										   int coord_w, int coord_h, const TieSnapshot* snap) {
	const int id = TIE_COCKPIT_EDITOR_REPAIRS_ID;
	const float x = cg->editor_anchor_x[id], y = cg->editor_anchor_y[id];
	float scale_x, scale_y, angle_deg;
	TieCockpitEditor_Transform(id, &scale_x, &scale_y, &angle_deg);
	const float px = cg->editor_pivot_x[id], py = cg->editor_pivot_y[id];

	/* Backing, scaled about the pivot (fills do not turn). */
	const TieCockpitRendererDrawSpace* space = &cg->pending_space;
	const float bx = px + (x - 3.0f - px) * scale_x, by = py + (y - 2.0f - py) * scale_y;
	static const float k_backing[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
	AeronDrawList_AddFill(cg->record_list, space->fit_x + bx * space->scale_x, space->fit_y + by * space->scale_y,
						  (cg->editor_extent_w[id] + 6.0f) * scale_x * space->scale_x,
						  (cg->editor_extent_h[id] + 4.0f) * scale_y * space->scale_y, k_backing,
						  AERON_BLIT2D_BLEND_PMA, NULL);

	/* Colours of damage_outputsystem (front-end palette). */
	TieUIText recs[TIE_HUD_REPAIR_SYSTEMS * 2];
	int n = 0;
	for (int i = 0; i < TIE_HUD_REPAIR_SYSTEMS; ++i) {
		const float row_y = y + (float)i * TIE_COCKPIT_REPAIRS_LINE_H;
		const uint8_t installed = snap->hud.repairs[i].installed;
		const unsigned health = snap->hud.repairs[i].health_percent;
		char value[8];
		uint8_t color;
		if (!installed) {
			snprintf(value, sizeof value, "N/A");
			color = 0x41;
		} else if (health == 0) {
			const unsigned seconds = snap->hud.repairs[i].repair_seconds;
			snprintf(value, sizeof value, "%02u:%02u", seconds / 60u % 100u, seconds % 60u);
			color = 0x4A;
		} else if (health >= 100) {
			snprintf(value, sizeof value, "100%%");
			color = 0x52;
		} else {
			snprintf(value, sizeof value, "%u%%", health);
			color = 0x4E;
		}
		if (TieCockpitText_Push(recs, &n, TIE_HUD_REPAIR_SYSTEMS * 2, (int16_t)0, (int16_t)0,
								snap->hud.repairs[i].name, TieCockpitText_RemapColor(0x43))) {
			recs[n - 1].font_id = FONT_MICRO64;
			recs[n - 1].x = (int16_t)x;
			recs[n - 1].y = (int16_t)row_y;
		}
		if (TieCockpitText_Push(recs, &n, TIE_HUD_REPAIR_SYSTEMS * 2, (int16_t)0, (int16_t)0, value,
								TieCockpitText_RemapColor(color))) {
			recs[n - 1].font_id = FONT_MICRO64;
			recs[n - 1].x = (int16_t)(x + TIE_COCKPIT_REPAIRS_VALUE_X);
			recs[n - 1].y = (int16_t)row_y;
		}
	}
	TieCockpitRenderer_RecordPlacedText(cg, entry, coord_w, coord_h, recs, n, snap->palette, px, py, scale_x,
										scale_y, angle_deg);
}

/* Objectives HUD for TFTC cockpits: the mission goals screen's own text
 * (goals_capturegoals). Runs on one room line join into a row; rows the room
 * centred (category titles, the OR joiner) start at the left and goal rows
 * indent. Headings can be hidden, and rows word-wrap at the panel width when
 * the editor's wrap option is on. Lays out cg->objective_recs at (x, y) and
 * returns the row count. */
static int TieCockpitRenderer_LayoutObjectives(TieCockpitRenderer* cg, const TieCockpitRendererEntry* entry,
											   int coord_w, int coord_h, const TieSnapshot* snap, float x, float y) {
	cg->objective_rec_count = 0;
	if (!cg->text_renderer)
		return 0;
	bool wrap, headings;
	float width, height, scale_x, scale_y, angle_deg;
	TieCockpitEditor_ObjectivesLayout(&wrap, &headings, &width, &height);
	TieCockpitEditor_Transform(TIE_COCKPIT_EDITOR_OBJECTIVES_ID, &scale_x, &scale_y, &angle_deg);
	/* Width and height are on screen; the text is laid out before scaling. */
	width /= scale_x;
	const int max_rows = height > 0.0f ? (int)(height / (TIE_COCKPIT_OBJECTIVES_LINE_H * scale_y)) : INT32_MAX;
	const float atlas_scale = TieCockpitRenderer_CockpitAtlasScaleFor(entry, coord_h);
	const TieScene2dTextSpace text_space = {
		.classic_w = coord_w,
		.classic_h = coord_h,
		.atlas_scale_x = atlas_scale,
		.atlas_scale_y = atlas_scale,
		.space_between_classic = 0.0f,
		.fit = TIE_SCENE2D_TEXT_FIT_FILL,
	};
	const uint8_t heading_color = TieCockpitText_RemapColor(0x46);
	int rows = 0;
	for (int i = 0; i < snap->hud.objective_count && rows < max_rows;) {
		/* Join the runs of one room line, each opening with its colour. */
		const int first = i;
		char row[256];
		size_t length = 0;
		for (; i < snap->hud.objective_count && snap->hud.objectives[i].y == snap->hud.objectives[first].y; ++i) {
			if (length + 2 < sizeof row) {
				row[length++] = (char)0xFE;
				row[length++] = (char)snap->hud.objectives[i].color;
			}
			for (const char* c = snap->hud.objectives[i].text; *c && length + 1 < sizeof row; ++c)
				row[length++] = *c;
		}
		row[length] = '\0';
		const bool centred = snap->hud.objectives[first].x > 12;
		if (centred && !headings && snap->hud.objectives[first].color == heading_color)
			continue;
		const float indent = centred ? 0.0f : TIE_COCKPIT_OBJECTIVES_INDENT;

		/* Greedy word wrap; escapes travel with their word. */
		char line[TIE_UI_TEXT_MAX_CHARS] = "";
		size_t line_length = 0;
		uint8_t color = snap->hud.objectives[first].color, line_color = color;
		const char* p = row;
		while (*p) {
			while (*p == ' ')
				++p;
			const uint8_t word_color = color;
			char word[TIE_UI_TEXT_MAX_CHARS];
			size_t word_length = 0;
			while (*p && *p != ' ') {
				if (word_length + 1 < sizeof word)
					word[word_length++] = *p;
				if ((uint8_t)*p == 0xFE && p[1]) {
					++p;
					color = (uint8_t)*p;
					if (word_length + 1 < sizeof word)
						word[word_length++] = *p;
				}
				++p;
			}
			word[word_length] = '\0';
			if (!word_length)
				break;
			char candidate[2 * TIE_UI_TEXT_MAX_CHARS];
			snprintf(candidate, sizeof candidate, "%s%s%s", line, line_length ? " " : "", word);
			const bool fits = !wrap || !line_length ||
							  TieScene2dTextRenderer_MeasureFestringClassic(cg->text_renderer, TIE_FONT_DOMAIN_COCKPIT,
																			FONT_MICRO64, &text_space,
																			candidate) <= width - indent;
			if (!fits) {
				if (rows < max_rows && TieCockpitText_Push(cg->objective_recs, &cg->objective_rec_count, TIE_COCKPIT_OBJECTIVE_ROWS, 0,
										0, line, line_color)) {
					TieUIText* rec = &cg->objective_recs[cg->objective_rec_count - 1];
					rec->font_id = FONT_MICRO64;
					rec->x = (int16_t)(x + indent);
					rec->y = (int16_t)(y + (float)rows * TIE_COCKPIT_OBJECTIVES_LINE_H);
				}
				++rows;
				line_color = word_color;
				snprintf(candidate, sizeof candidate, "%s", word);
			}
			snprintf(line, sizeof line, "%s", candidate);
			line_length = strlen(line);
		}
		if (line_length && rows < max_rows) {
			if (TieCockpitText_Push(cg->objective_recs, &cg->objective_rec_count, TIE_COCKPIT_OBJECTIVE_ROWS, 0, 0,
									line, line_color)) {
				TieUIText* rec = &cg->objective_recs[cg->objective_rec_count - 1];
				rec->font_id = FONT_MICRO64;
				rec->x = (int16_t)(x + indent);
				rec->y = (int16_t)(y + (float)rows * TIE_COCKPIT_OBJECTIVES_LINE_H);
			}
			++rows;
		}
	}
	return rows < max_rows ? rows : max_rows;
}

/* Draws the laid-out objectives, scaled about their pivot, on a backing. */
static void TieCockpitRenderer_DrawObjectives(TieCockpitRenderer* cg, const TieCockpitRendererEntry* entry,
											  int coord_w, int coord_h, const TieSnapshot* snap) {
	const int id = TIE_COCKPIT_EDITOR_OBJECTIVES_ID;
	if (cg->objective_rec_count <= 0)
		return;
	float scale_x, scale_y, angle_deg;
	TieCockpitEditor_Transform(id, &scale_x, &scale_y, &angle_deg);
	const float px = cg->editor_pivot_x[id], py = cg->editor_pivot_y[id];
	const TieCockpitRendererDrawSpace* space = &cg->pending_space;
	const float bx = px + (cg->editor_anchor_x[id] - 3.0f - px) * scale_x;
	const float by = py + (cg->editor_anchor_y[id] - 2.0f - py) * scale_y;
	static const float k_backing[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
	AeronDrawList_AddFill(cg->record_list, space->fit_x + bx * space->scale_x, space->fit_y + by * space->scale_y,
						  (cg->editor_extent_w[id] + 6.0f) * scale_x * space->scale_x,
						  (cg->editor_extent_h[id] + 4.0f) * scale_y * space->scale_y, k_backing,
						  AERON_BLIT2D_BLEND_PMA, NULL);
	TieCockpitRenderer_RecordPlacedText(cg, entry, coord_w, coord_h, cg->objective_recs, cg->objective_rec_count,
										snap->palette, px, py, scale_x, scale_y, 0.0f);
}

/* Records the cockpit editor's text in the cockpit micro font, at
 * TIE_COCKPIT_EDITOR_TEXT_SCALE. Labels may sit left of or above the frame, so
 * they are shifted into the unsigned range and back through the fit origin,
 * like the instrument text. Positions are divided by the text scale so the
 * scaled fit puts them back where the editor placed them. */
static void TieCockpitRenderer_RecordEditorLabels(TieCockpitRenderer* cg, const TieCockpitRendererEntry* entry,
												  int coord_w, int coord_h, const TieCockpitEditorLabel* labels,
												  int n, const uint32_t* palette) {
	if (!cg->text_renderer || n <= 0)
		return;
	const float k = TIE_COCKPIT_EDITOR_TEXT_SCALE;
	float bias_x = 0.0f, bias_y = 0.0f;
	for (int i = 0; i < n; ++i) {
		if (-labels[i].x / k > bias_x)
			bias_x = ceilf(-labels[i].x / k);
		if (-labels[i].y / k > bias_y)
			bias_y = ceilf(-labels[i].y / k);
	}
	static const uint8_t k_colors[] = { 0x43, 0x46, 0x4E }; /* white, blue, yellow (cockpit palette) */
	TieUIText recs[TIE_COCKPIT_EDITOR_MAX_LABELS];
	int count = 0;
	for (int i = 0; i < n && count < TIE_COCKPIT_EDITOR_MAX_LABELS; ++i) {
		if (TieCockpitText_Push(recs, &count, TIE_COCKPIT_EDITOR_MAX_LABELS,
								(int16_t)(labels[i].x / k + bias_x + 0.5f), (int16_t)(labels[i].y / k + bias_y + 0.5f),
								labels[i].text, TieCockpitText_RemapColor(k_colors[labels[i].color])))
			recs[count - 1].font_id = FONT_MICRO64;
	}
	const float atlas_scale = TieCockpitRenderer_CockpitAtlasScaleFor(entry, coord_h);
	const TieScene2dTextSpace text_space = {
		.classic_w = coord_w,
		.classic_h = coord_h,
		.atlas_scale_x = atlas_scale,
		.atlas_scale_y = atlas_scale,
		.space_between_classic = 0.0f,
		.fit = TIE_SCENE2D_TEXT_FIT_FILL,
	};
	const TieCockpitRendererDrawSpace* space = &cg->pending_space;
	TieScene2dTextRenderer_RecordFestringScaled(cg->text_renderer, cg->record_list, coord_w, coord_h, &text_space,
												recs, count, palette, space->fit_x - bias_x * space->scale_x * k,
												space->fit_y - bias_y * space->scale_y * k, space->scale_x * k,
												space->scale_y * k, space->target_w, space->target_h);
}

/* Records cockpit text. A record belongs to the instrument whose anchor is
 * nearest its origin and follows that instrument's editor scale and rotation:
 * its origin turns about the group pivot and its glyphs scale, upright.
 * Coordinates carry the text bias (see the instrument remap). */
static void TieCockpitRenderer_RecordCockpitText(TieCockpitRenderer* cg, int coord_w, int coord_h,
												 const TieScene2dTextSpace* text_space, TieUIText* recs, int n,
												 const uint32_t* palette, float bias_x, float bias_y) {
	const TieCockpitRendererDrawSpace* space = &cg->pending_space;
	const float fit_x = space->fit_x - bias_x * space->scale_x;
	const float fit_y = space->fit_y - bias_y * space->scale_y;
	/* The original paints solid strips behind readouts for its bitmap
	 * screens; over a TFTC cockpit's own screens only the ink is drawn. */
	const bool ink_only = cg->flight_gpu && TieFlightRenderer_Cockpit3dActive(cg->flight_gpu);
	int plain = 0;
	for (int r = 0; r < n; ++r) {
		if (ink_only)
			recs[r].background = 0;
		int nearest = -1;
		float best = 0.0f;
		for (int i = 0; i < TIE_MAX_HUD_INSTRUMENTS; ++i) {
			if (!cg->editor_active[i])
				continue;
			const float dx = cg->editor_anchor_x[i] + bias_x - (float)recs[r].x;
			const float dy = cg->editor_anchor_y[i] + bias_y - (float)recs[r].y;
			if (nearest < 0 || dx * dx + dy * dy < best) {
				nearest = i;
				best = dx * dx + dy * dy;
			}
		}
		/* Readouts of an instrument a DC gauge replaces or the cockpit leaves out. */
		if (nearest >= 0 && TieCockpitRenderer_DcReplaces(cg, nearest))
			continue;
		float scale_x = 1.0f, scale_y = 1.0f, angle_deg = 0.0f;
		if (nearest >= 0)
			TieCockpitEditor_Transform(nearest, &scale_x, &scale_y, &angle_deg);
		if (scale_x == 1.0f && scale_y == 1.0f && angle_deg == 0.0f &&
			!(nearest >= 0 && TieCockpitRenderer_SurfaceFor(cg, nearest))) {
			recs[plain++] = recs[r];
			continue;
		}
		const bool on_surface = nearest >= 0 && TieCockpitRenderer_SurfaceFor(cg, nearest);
		if (on_surface) {
			/* Into the surface target, without the bias (its fit takes negative coordinates). */
			TieCockpitRenderer_BeginSurface(cg, nearest);
			TieUIText one = recs[r];
			const float px = cg->editor_pivot_x[nearest], py = cg->editor_pivot_y[nearest];
			const float c = cosf(angle_deg * 3.14159265f / 180.0f), sn = sinf(angle_deg * 3.14159265f / 180.0f);
			const float rx = ((float)one.x - bias_x - px) * scale_x, ry = ((float)one.y - bias_y - py) * scale_y;
			const float ox = px + rx * c - ry * sn, oy = py + rx * sn + ry * c;
			const TieCockpitRendererDrawSpace* s = &cg->pending_space;
			one.x = 0;
			one.y = 0;
			TieScene2dTextRenderer_RecordFestringScaled(cg->text_renderer, cg->record_list, coord_w, coord_h,
														text_space, &one, 1, palette, s->fit_x + ox * s->scale_x,
														s->fit_y + oy * s->scale_y, s->scale_x * scale_x,
														s->scale_y * scale_y, s->target_w, s->target_h);
			TieCockpitRenderer_EndSurface(cg, true);
			continue;
		}
		const float px = cg->editor_pivot_x[nearest] + bias_x, py = cg->editor_pivot_y[nearest] + bias_y;
		const float c = cosf(angle_deg * 3.14159265f / 180.0f), sn = sinf(angle_deg * 3.14159265f / 180.0f);
		const float rx = ((float)recs[r].x - px) * scale_x, ry = ((float)recs[r].y - py) * scale_y;
		const float ox = px + rx * c - ry * sn, oy = py + rx * sn + ry * c;
		TieUIText one = recs[r];
		TieScene2dTextRenderer_RecordFestringScaled(cg->text_renderer, cg->record_list, coord_w, coord_h, text_space,
													&one, 1, palette,
													fit_x + (ox - (float)one.x * scale_x) * space->scale_x,
													fit_y + (oy - (float)one.y * scale_y) * space->scale_y,
													space->scale_x * scale_x, space->scale_y * scale_y,
													space->target_w, space->target_h);
	}
	if (plain > 0)
		TieScene2dTextRenderer_RecordFestringScaled(cg->text_renderer, cg->record_list, coord_w, coord_h, text_space,
													recs, plain, palette, fit_x, fit_y, space->scale_x,
													space->scale_y, space->target_w, space->target_h);
}

static bool TieCockpitRenderer_CockpitRecordLists(TieCockpitRenderer* cg, AeronCommandBuffer* cmd, int rt_w,
												  int rt_h, const TieSnapshot* snap) {
	if (!cg || !cmd || !snap || !cg->pending_entry)
		return false;
	const TieCockpitRendererEntry* entry = cg->pending_entry;
	const uint32_t asset_io_before = cg->draw_phase_asset_io_count;
	cg->draw_phase_active = true;

	/* Reference coord frame: layout's reference when a YAML is loaded,
	 * snapshot's classic_w/h otherwise. Drivers, blits, and cel sizes
	 * all live in this frame; TieCockpitRenderer_InsAnchor() rescales engine instrument
	 * positions into it. */
	int coord_w, coord_h;
	TieCockpitRenderer_ResolveCoordFrame(entry, snap, &coord_w, &coord_h);

	TIE_GPU_PUSH(cmd, "Cockpit HUD");

	for (int i = 0; i < TIE_MAX_HUD_INSTRUMENTS; ++i) {
		const TieHudInstrument* ins = &snap->hud.instruments[i];
		cg->editor_active[i] = TieCockpitCommon_InstrumentActive(ins) &&
							   TieCockpitRenderer_ViewAllowsWidget(snap->cockpit.view_idx, i,
																   (TieCockpitRendererHudWidgetKind)hud_widget_kind[i]);
		cg->editor_anchor_x[i] = cg->editor_anchor_y[i] = 0.0f;
		cg->editor_extent_w[i] = cg->editor_extent_h[i] = 0.0f;
		if (cg->editor_active[i])
			TieCockpitRenderer_InsAnchor(entry, i, ins->x, ins->y, snap, &cg->editor_anchor_x[i],
										 &cg->editor_anchor_y[i]);
	}
	/* On TFTC DC screens the digit fields are sized by their text, about 5 x 7
	 * px a character in the 640 x 480 frame, so their screens fit it. */
	if (TieCockpitRenderer_DcLayout(cg)) {
		const float k = (float)coord_w / 640.0f;
		static const int k_text_ids[] = { TIE_HUDI_SPEED_DIGITS, TIE_HUDI_THROTTLE_DIGITS, TIE_HUDI_CLOCK_DIGITS };
		for (size_t t = 0; t < sizeof k_text_ids / sizeof k_text_ids[0]; ++t) {
			const int id = k_text_ids[t];
			if (!cg->editor_active[id])
				continue;
			const int chars = id == TIE_HUDI_CLOCK_DIGITS ? (int)strlen(snap->hud.mission_clock_text)
														  : (snap->hud.instruments[id].digits ? snap->hud.instruments[id].digits : 3);
			cg->editor_extent_w[id] = 5.0f * (float)(chars > 0 ? chars : 1) * k;
			cg->editor_extent_h[id] = 7.0f * k;
		}
		/* The hull shape sits inside the shield ring, which centres on it. */
		const TieHudInstrument* hull = &snap->hud.instruments[TIE_HUDI_HULL_DAMAGE_LEVER];
		float u0, v0, u1, v1;
		int cel_w, cel_h;
		if (cg->editor_active[TIE_HUDI_HULL_DAMAGE_LEVER] &&
			TieCockpitRenderer_ResolveCel(entry, hull->param1, &u0, &v0, &u1, &v1, &cel_w, &cel_h)) {
			cg->editor_extent_w[TIE_HUDI_HULL_DAMAGE_LEVER] = (float)cel_w;
			cg->editor_extent_h[TIE_HUDI_HULL_DAMAGE_LEVER] = (float)cel_h;
		}
	}
	/* TFTC-only panels, placed and sized like instruments with an extent. */
	{
		const int id = TIE_COCKPIT_EDITOR_REPAIRS_ID;
		float dx, dy;
		TieCockpitEditor_Offset(id, &dx, &dy);
		cg->editor_active[id] = cg->flight_gpu && TieFlightRenderer_Cockpit3dActive(cg->flight_gpu);
		cg->editor_anchor_x[id] = TIE_COCKPIT_REPAIRS_X + dx;
		cg->editor_anchor_y[id] = TIE_COCKPIT_REPAIRS_Y + dy;
		cg->editor_extent_w[id] = TIE_COCKPIT_REPAIRS_W;
		cg->editor_extent_h[id] = TIE_COCKPIT_REPAIRS_LINE_H * TIE_HUD_REPAIR_SYSTEMS;
	}
	{
		const int id = TIE_COCKPIT_EDITOR_OBJECTIVES_ID;
		float dx, dy;
		TieCockpitEditor_Offset(id, &dx, &dy);
		cg->editor_active[id] = cg->flight_gpu && TieFlightRenderer_Cockpit3dActive(cg->flight_gpu);
		cg->editor_anchor_x[id] = TIE_COCKPIT_OBJECTIVES_X + dx;
		cg->editor_anchor_y[id] = TIE_COCKPIT_OBJECTIVES_Y + dy;
		bool wrap, headings;
		float width, height, scale_x, scale_y, angle_deg;
		TieCockpitEditor_ObjectivesLayout(&wrap, &headings, &width, &height);
		TieCockpitEditor_Transform(id, &scale_x, &scale_y, &angle_deg);
		const int rows = cg->editor_active[id] ? TieCockpitRenderer_LayoutObjectives(
														 cg, entry, coord_w, coord_h, snap, cg->editor_anchor_x[id],
														 cg->editor_anchor_y[id])
											   : 0;
		cg->editor_extent_w[id] = (wrap ? width : TIE_COCKPIT_OBJECTIVES_W * scale_x) / scale_x;
		cg->editor_extent_h[id] = TIE_COCKPIT_OBJECTIVES_LINE_H * (float)(rows ? rows : 1);
	}
	TieCockpitEditor_Pivots(cg->editor_anchor_x, cg->editor_anchor_y, cg->editor_extent_w, cg->editor_extent_h,
							cg->editor_active, TIE_COCKPIT_EDITOR_ID_COUNT, cg->editor_pivot_x, cg->editor_pivot_y);
	TieCockpitRenderer_BeginSurfaces(cg, coord_w, coord_h, entry, snap);

	AeronDrawList_Begin(cg->before_pip_list, NULL, rt_w, rt_h, AERON_DRAWLIST2D_LOAD, NULL);
	AeronDrawList_Begin(cg->after_pip_list, NULL, rt_w, rt_h, AERON_DRAWLIST2D_LOAD, NULL);
	cg->record_list = cg->before_pip_list;

	/* Original PANL images retain their decoded panel-area aspect inside
	 * TieCockpitRenderer_DrawCockpitBase: 640x456 for SVGA and 320x189 for VGA. */
	const int cockpit_area_h = (coord_h * 19) / 20;

	/* Target box paints before cockpit chrome so the canopy's alpha
	 * cutout occludes it where the frame is opaque. Look toggled by
	 * TieFlightRenderer model backend: scene meshes use 1998 corner brackets,
	 * false=1995 (hollow rect, fixed coord/48). */
	const bool look_1998 = (cg->flight_gpu != NULL) && TieFlightRenderer_UsesSceneModels(cg->flight_gpu);
	TIE_GPU_MARKER(cmd, "Flight target box");
	TieCockpitRenderer_DrawMainTargetBox(cg, cmd, NULL, entry, rt_w, rt_h, snap, look_1998);

	/* Static cockpit chrome — base bitmap + damage cracks + subsystem
	 * covers. These change only on view/craft switch, mirror toggle, or a
	 * subsystem knockout/repair, so they are pre-baked into chrome_rt
	 * (TieCockpitRenderer_Prepare) and composited here as a single coverage-rect
	 * pass — replacing 1 base blit + N crack/cover blits with one. When the
	 * bake is unavailable (no flight_gpu / RT create failed) the original
	 * per-frame direct draws run instead. Damage cracks and covers are
	 * panel-area cels within the base's opaque footprint, so the base
	 * coverage rects bound all chrome content. */
	/* The 3D cockpit model replaces the cockpit art. */
	const bool cockpit_3d = cg->flight_gpu && TieFlightRenderer_Cockpit3dActive(cg->flight_gpu);
	if (cockpit_3d) {
	} else if (cg->chrome_valid) {
		TIE_GPU_MARKER(cmd, "Cockpit chrome composite");
		TieCockpitRenderer_DrawCockpitChrome(cg, cmd, NULL, entry, coord_w, coord_h, rt_w, rt_h,
											 snap->cockpit.mirrored_view);
	} else {
		TIE_GPU_MARKER(cmd, "Cockpit base");
		TieCockpitRenderer_DrawCockpitBase(cg, cmd, NULL, entry, coord_w, coord_h, cockpit_area_h,
										   snap->cockpit.mirrored_view);
		TIE_GPU_MARKER(cmd, "Cockpit covers");
		TieCockpitRenderer_DrawCovers(cg, cmd, NULL, entry, coord_w, coord_h, snap);
		TIE_GPU_MARKER(cmd, "Cockpit damage");
		TieCockpitRenderer_DrawDamageCracks(cg, cmd, NULL, entry, coord_w, coord_h, snap);
	}
	/* Per-frame clearwindow rects for dynamic-text areas the cockpit
	 * base bitmap leaves with placeholder graphics. View-0 gated. */
	if (!cockpit_3d)
		TieCockpitRenderer_DrawClearwindows(cg, cmd, NULL, entry, coord_w, coord_h, snap);
	TieCockpitRenderer_DrawWidgetsForView(cg, cmd, NULL, entry, coord_w, coord_h, 0, 1, snap);

	cg->record_list = cg->after_pip_list;
	TieCockpitRenderer_DrawWidgetsForView(cg, cmd, NULL, entry, coord_w, coord_h, 3,
										  TIE_MAX_HUD_INSTRUMENTS - 1, snap);
	/* PIP subsystem indicator — runs after the 3DCRT compose inside
	 * TieCockpitRenderer_DrawWidgetsForView, paints a small 4×4 box at the world-
	 * projected position of the selected component. */
	TieCockpitRenderer_DrawPipTargetBox(cg, cmd, NULL, entry, coord_w, coord_h, snap);
	/* Radar bracket is written by panel_updateradar, which the engine
	 * only calls in views 0 and 19 (panel_updatepanel:339-358). In other
	 * views bracket_x/y are stale from the last view-0 tick; painting
	 * them would overlay the side-view cockpit bitmap. */
	if (snap->cockpit.view_idx == 0 || snap->cockpit.view_idx == 19) {
		/* On the surface of the radar its blip is on (a 3D cockpit's sensor
		 * screen), like the blips. */
		const int radar_id = snap->hud.bracket_radar_idx == 0 ? TIE_HUDI_RADAR_LEFT : TIE_HUDI_RADAR_RIGHT;
		const bool on_surface = TieCockpitRenderer_BeginSurface(cg, radar_id);
		TieCockpitRenderer_DrawRadarBracket(cg, cmd, NULL, entry, coord_w, coord_h, snap);
		TieCockpitRenderer_EndSurface(cg, on_surface);
	}

	/* Cockpit text — clock + HUD/CMD readouts. Both builders share
	 * the same TieScene2dTextSpace so build_hud_text's measurements
	 * match what TieScene2dText_DrawFestringInSpace will paint.
	 * MICRO64 atlas extracted at 4.5× SVGA. */
	if (cg->text_renderer) {
		const float atlas_scale = TieCockpitRenderer_CockpitAtlasScaleFor(entry, coord_h);
		const TieScene2dTextSpace cockpit_space = {
			.classic_w = coord_w,
			.classic_h = coord_h,
			.atlas_scale_x = atlas_scale,
			.atlas_scale_y = atlas_scale,
			.space_between_classic = 0.0f,
			.fit = TIE_SCENE2D_TEXT_FIT_FILL,
		};

		/* Build a remapped instrument array once for both text builders.
		 * For 4:3 layouts the helper produces a verbatim copy of
		 * snap->hud.instruments; for 16:9 layouts it rescales x/y into
		 * the layout reference frame (authored anchor or piecewise). */
		TieHudInstrument instruments_remapped[TIE_MAX_HUD_INSTRUMENTS];
		/* Editor offsets can place text left of or above the frame, which the
		 * unsigned instrument coordinates cannot hold; such text is shifted
		 * into range here and back by moving the fit origin. */
		float text_bias_x = 0.0f, text_bias_y = 0.0f;
		for (int i = 0; i < TIE_MAX_HUD_INSTRUMENTS; ++i) {
			if (!TieCockpitCommon_InstrumentActive(&snap->hud.instruments[i]))
				continue;
			float ax, ay;
			TieCockpitRenderer_InsAnchor(entry, i, snap->hud.instruments[i].x, snap->hud.instruments[i].y,
										 snap, &ax, &ay);
			if (-ax > text_bias_x)
				text_bias_x = ceilf(-ax);
			if (-ay > text_bias_y)
				text_bias_y = ceilf(-ay);
		}
		for (int i = 0; i < TIE_MAX_HUD_INSTRUMENTS; ++i) {
			instruments_remapped[i] = snap->hud.instruments[i];
			/* Layout anchors position existing instruments; unused slots stay absent. */
			if (!TieCockpitCommon_InstrumentActive(&snap->hud.instruments[i]))
				continue;
			float ax, ay;
			TieCockpitRenderer_InsAnchor(entry, i, snap->hud.instruments[i].x, snap->hud.instruments[i].y,
										 snap, &ax, &ay);
			ax += text_bias_x;
			ay += text_bias_y;
			if (ax < 0.0f)
				ax = 0.0f;
			if (ay < 0.0f)
				ay = 0.0f;
			if (ax > 65535.0f)
				ax = 65535.0f;
			if (ay > 65535.0f)
				ay = 65535.0f;
			instruments_remapped[i].x = (uint16_t)(ax + 0.5f);
			instruments_remapped[i].y = (uint16_t)(ay + 0.5f);
		}

		TieUIText clock_recs[2];
		int clock_n = TieCockpitText_BuildclockText(snap, instruments_remapped, clock_recs,
													(int)(sizeof clock_recs / sizeof clock_recs[0]));
		if (clock_n > 0) {
			const TieCockpitRendererDrawSpace* space = &cg->pending_space;
			TieCockpitRenderer_RecordCockpitText(cg, coord_w, coord_h, &cockpit_space, clock_recs, clock_n, snap->palette,
												 text_bias_x, text_bias_y);
		}

		/* HUD / CMD text. 64 records is comfortably above the
		 * worst case (CMD ~13 + threat ~14 + speed/throttle/etc.). */
		TieUIText hud_recs[64];
		int hud_n = TieCockpitText_BuildHudText(cg->text_renderer, snap, instruments_remapped, &entry->layout,
												&cockpit_space, hud_recs,
												(int)(sizeof hud_recs / sizeof hud_recs[0]));
		if (hud_n > 0) {
			const TieCockpitRendererDrawSpace* space = &cg->pending_space;
			TieCockpitRenderer_RecordCockpitText(cg, coord_w, coord_h, &cockpit_space, hud_recs, hud_n, snap->palette,
												 text_bias_x, text_bias_y);
		}
	}
	if (cg->editor_active[TIE_COCKPIT_EDITOR_REPAIRS_ID] &&
		!TieCockpitRenderer_DcReplaces(cg, TIE_COCKPIT_EDITOR_REPAIRS_ID)) {
		const bool on_surface = TieCockpitRenderer_BeginSurface(cg, TIE_COCKPIT_EDITOR_REPAIRS_ID);
		TieCockpitRenderer_DrawRepairs(cg, entry, coord_w, coord_h, snap);
		TieCockpitRenderer_EndSurface(cg, on_surface);
	}
	if (cg->editor_active[TIE_COCKPIT_EDITOR_OBJECTIVES_ID] &&
		!TieCockpitRenderer_DcReplaces(cg, TIE_COCKPIT_EDITOR_OBJECTIVES_ID)) {
		const bool on_surface = TieCockpitRenderer_BeginSurface(cg, TIE_COCKPIT_EDITOR_OBJECTIVES_ID);
		TieCockpitRenderer_DrawObjectives(cg, entry, coord_w, coord_h, snap);
		TieCockpitRenderer_EndSurface(cg, on_surface);
	}
	TieCockpitRenderer_FinishSurfaces(cg, cmd);
	if (TieCockpitEditor_Open()) {
		const TieCockpitRendererDrawSpace* space = &cg->pending_space;
		TieCockpitEditorLabel labels[TIE_COCKPIT_EDITOR_MAX_LABELS];
		const int label_count = TieCockpitEditor_Frame(
			cg->record_list, cg->editor_anchor_x, cg->editor_anchor_y, cg->editor_extent_w, cg->editor_extent_h,
			cg->editor_active, TIE_COCKPIT_EDITOR_ID_COUNT, space->fit_x, space->fit_y, space->scale_x,
			space->scale_y, rt_w, rt_h, labels, TIE_COCKPIT_EDITOR_MAX_LABELS);
		TieCockpitRenderer_RecordEditorLabels(cg, entry, coord_w, coord_h, labels, label_count, snap->palette);
	}
	TIE_GPU_MARKER(cmd, "Flight message bar");
	TieCockpitRenderer_DrawMessageBar(cg, cmd, NULL, snap);

	cg->record_list = NULL;
	cg->draw_phase_active = false;
	if (cg->draw_phase_asset_io_count != asset_io_before)
		Aeron_LogWarn("tie.cockpit", "asset I/O occurred while recording cockpit draw commands (%u total)",
					  cg->draw_phase_asset_io_count);
	bool ok = AeronDrawList_Prepare(cg->before_pip_list, cmd) != 0;
	ok = AeronDrawList_Prepare(cg->after_pip_list, cmd) != 0 && ok;
	TIE_GPU_POP(cmd); /* "Cockpit HUD" */
	if (!ok)
		Aeron_RequestFatalRendererError("cockpit draw-list preparation");
	return ok;
}

void TieCockpitRenderer_RenderInPass(TieCockpitRenderer* cg, AeronCommandBuffer* cmd, AeronRenderPass* pass,
									 AeronRenderTarget* target, const TieSnapshot* snap) {
	if (!cg || !cmd || !pass || !target || !snap || !cg->pending_ready)
		return;

	AeronDrawList_RenderIntoPass(cg->before_pip_list, cmd, pass, target);

	const TieHudInstrument* pip = &snap->hud.instruments[2];
	TieCockpitRendererHudWidgetKind kind = (TieCockpitRendererHudWidgetKind)hud_widget_kind[2];
	/* On a TFTC DC targeting screen the target view went into its target. */
	if (cg->pending_entry && TieCockpitCommon_InstrumentActive(pip) &&
		TieCockpitRenderer_ViewAllowsWidget(snap->cockpit.view_idx, 2, kind) &&
		!TieCockpitRenderer_SurfaceFor(cg, TIE_HUDI_CMD_3D_CRT)) {
		TieCockpitRenderer_Draw3dcrt(cg, pass, cg->pending_entry, snap);
	}

	AeronDrawList_RenderIntoPass(cg->after_pip_list, cmd, pass, target);
}
