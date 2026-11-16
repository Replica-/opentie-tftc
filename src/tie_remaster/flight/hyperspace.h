#ifndef TIE_FLIGHT_HYPERSPACE_H
#define TIE_FLIGHT_HYPERSPACE_H

/* OpenXWA's HD hyperspace (src/xwa_remaster/hyperspace.c), carried over to
 * TIE: the jump's star streaks and scene warp on entry and exit, and the
 * tunnel between them. */

#include "aeron/render.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The radiance of a white classic pixel in the flight scene: OpenXWA's
 * classic_lighting.image_radiance, here 1 as the TIE scene draws a white
 * classic pixel at 1. */
#define TIE_FLIGHT_HYPERSPACE_RADIANCE 1.0f

/* The phases the effect draws, with OpenXWA's values. */
typedef enum TieFlightHyperspacePhase {
	TIE_FLIGHT_HYPERSPACE_NONE = 0,
	TIE_FLIGHT_HYPERSPACE_OUTBOUND = 2,
	TIE_FLIGHT_HYPERSPACE_INBOUND = 3,
	TIE_FLIGHT_HYPERSPACE_TUNNEL = 4,
} TieFlightHyperspacePhase;

/* Procedural hyperspace-tunnel settings loaded from remaster/config.yaml and
 * live-editable through the debug inspector. The preview override affects
 * rendering only; it never changes the captured flight simulation state. */
typedef struct TieFlightHyperspaceTunnelParams {
	float travel_speed;
	float rotation_speed;
	float noise_scale;
	float brightness;
	float highlight_strength;
	float focal_length;
	float twist;
	float cap_radius;
	float cap_falloff;
	float mesh_ambient_strength;
	float mesh_environment_roughness;
	float mesh_key_strength;
	float dark_color[3];
	float body_color[3];
	float highlight_color[3];
	float cap_color[3];
} TieFlightHyperspaceTunnelParams;

/* Star streaks of the hyperspace entry and exit (docs/hd_hyperspace_jump.md),
 * loaded from the `hyperspace_jump` section of OpenXWA's remaster/config.yaml and
 * live-editable through the debug inspector. */
typedef struct TieFlightHyperspaceJumpParams {
	int star_count;
	float streak_brightness;
	float whiteout_brightness;
	float brightness_range;
	float streak_width_px;
	float width_from_brightness;
	float width_variation;
	float head_widening;
	float radial_widening;
	float tail_taper;
	float head_taper;
	float taper_variation;
	float glow_strength;
	float glow_width;
	float tail_brightness;
	float tail_color[3];
	float stretch_scale;
	float white_fade_seconds;
	float tunnel_entry_brightness;
	float tunnel_settle_seconds;
	float mesh_light_strength;
	float mesh_light_start;
	float mesh_light_curve;
	float tint[3];
	/* The scene warp (docs/hd_hyperspace_jump.md, Scene warp). */
	float warp_start;
	float warp_end;
	float warp_curve;
	float warp_distance_m;
	float warp_sky_zoom;
	float warp_smear;
	float warp_sky_highlights;
	float warp_reveal_softness;
	int warp_taps;
	int warp_sky_taps;
	/* 1: the sky passes run at half the output resolution. */
	int warp_sky_half_resolution;
	/* 0: the exit plays the entry backward. 1: the exit carries on forward,
	   so the streaks shrink toward the camera and the scene comes in toward
	   it. */
	int exit_forward;
} TieFlightHyperspaceJumpParams;

/* The lighting the tunnel gives the cockpit (OpenXWA's ship ambient cube,
 * environment map and directional light). */
typedef struct TieFlightHyperspaceAmbientCube {
	float pos_x[3];
	float neg_x[3];
	float pos_y[3];
	float neg_y[3];
	float pos_z[3];
	float neg_z[3];
	float minimum[3];
} TieFlightHyperspaceAmbientCube;

typedef struct TieFlightHyperspaceEnvironmentMap {
	AeronTexture* texture;
	AeronSampler* sampler;
	float right[3];
	float up[3];
	float forward[3];
	float strength;
} TieFlightHyperspaceEnvironmentMap;

typedef struct TieFlightHyperspaceDirLight {
	float world_dir[3]; /* from the surface toward the light, world space, unit */
	float color[3];
	uint16_t source_backdrop_object_type;
	int16_t source_backdrop_index;
} TieFlightHyperspaceDirLight;

typedef struct TieFlightHyperspace TieFlightHyperspace;

typedef struct TieFlightHyperspaceTunnelView {
	float right[3];
	float up[3];
	float forward[3];
	float tan_half_fov_x;
	float tan_half_fov_y;
	float proj_offset_x;
	float proj_offset_y;
} TieFlightHyperspaceTunnelView;

typedef struct TieFlightHyperspaceLighting {
	TieFlightHyperspaceAmbientCube ambient_add;
	TieFlightHyperspaceEnvironmentMap environment;
	TieFlightHyperspaceDirLight key;
	uint32_t key_count;
	uint8_t active;
} TieFlightHyperspaceLighting;

/* The length of the recovered tunnel phase: it ends after this many ticks. */
#define TIE_FLIGHT_HYPERSPACE_TUNNEL_TICKS 1298u

/* The moment of the hyperspace sequence to draw. Real hyperspace takes it
 * from the snapshot; the debug preview makes it up. */
typedef struct TieFlightHyperspaceMoment {
	uint8_t phase;      /* TieFlightHyperspacePhase */
	float phase_ticks;  /* ticks since the phase started; may have a fraction */
	float tunnel_ticks; /* the tunnel phase's length */
	uint32_t seed;      /* picks the jump's stars */
	uint8_t tunnel_only; /* the tunnel without end, and without the jump's brightening */
	int look;            /* the jump's look, or -1 */
} TieFlightHyperspaceMoment;

TieFlightHyperspace* TieFlightHyperspace_Create(const TieFlightHyperspaceTunnelParams* tunnel,
													const TieFlightHyperspaceJumpParams* jump);
void TieFlightHyperspace_Destroy(TieFlightHyperspace* hyperspace);
void TieFlightHyperspace_SetParams(TieFlightHyperspace* hyperspace,
									 const TieFlightHyperspaceTunnelParams* params);
void TieFlightHyperspace_SetJumpParams(TieFlightHyperspace* hyperspace,
										 const TieFlightHyperspaceJumpParams* params);

/* A look whose colours spread through the tunnel from its far end toward the
 * camera. The edge between the two sets of colours is the front.
 *
 * The shader measures how deep into the tunnel a point of the walls is by
 * how far it is along the tunnel's axis for each unit of distance from the
 * axis. That depth is about 0.5 at the screen's corners and grows toward the
 * far end of the tunnel. The front's position, edge thickness and bumps are
 * measured in log2 of the depth: a step of 1 moves the front about halfway to
 * or from the centre of the screen. */
typedef struct TieFlightHyperspaceFront {
	int active;
	/* How far the front has come, from 0 (a small area at the far end) to 1
	   (the whole tunnel). */
	float progress;
	/* The look's colours, where the front has passed. */
	float dark_color[3];
	float body_color[3];
	float highlight_color[3];
	float cap_color[3];
	/* The edge: its colour, half its thickness, how far its bumps reach
	   ahead, how many bumps there are (the scale of their noise), and how
	   much they twist. */
	float edge_color[3];
	float edge_width;
	float noise;
	float noise_scale;
	float warp;
} TieFlightHyperspaceFront;

/* Sets the front that the tunnel draws, or turns it off when `front` is
   NULL or not active. */
void TieFlightHyperspace_SetFront(TieFlightHyperspace* hyperspace,
									const TieFlightHyperspaceFront* front);

/* Builds the tunnel and streak state for `moment`. Must run before
 * AeronScene_Render opens its render passes. `rt_w` and `rt_h` are the
 * scene's render size. Returns 0 for a phase that draws nothing.
 *
 * With `world_scene`, the effect is drawn over the normal flight scene (the
 * entry and the exit) by TieFlightHyperspace_RenderWarp, after the scene.
 * Without it, the effect is the background of the dedicated hyperspace scene
 * (the tunnel alone), and the tunnel's lighting cube is generated. */
int TieFlightHyperspace_Prepare(TieFlightHyperspace* hyperspace, AeronCommandBuffer* command_buffer,
								  const float camera_rows[9], int rt_w, int rt_h,
								  const TieFlightHyperspaceMoment* moment,
								  const TieFlightHyperspaceTunnelView* tunnel_view, int world_scene);

/* What the scene warp reads from the finished flight scene. */
typedef struct TieFlightHyperspaceWarpInput {
	AeronTexture* scene; /* the finished scene, at output size */
	AeronTexture* depth; /* the scene's reversed-Z depth, at render size */
	int output_w;
	int output_h;
	int render_w;
	int render_h;
	float near_z;          /* the camera's near plane, in view units */
	float keep_depth;      /* the depth at the keep distance; nearer is larger */
	float units_per_meter; /* view units per metre */
	int view;              /* TieFlightHyperspaceWarpView */
	/* The cockpit's screens at render size (AeronScene_ScreenLayer), drawn
	 * over everything, or NULL. */
	AeronTexture* screens;
	/* The region's sky as a cube, without its suns, or NULL. The forward
	 * exit reads the shrunk sky from it, since most of it lies outside the
	 * screen. Without it, the forward exit only shows trails. */
	AeronTexture* sky_cube;
	AeronSampler* sky_sampler;
	float sky_view_to_cube[9]; /* row-major, from view space (x right, y down, z forward) */
	float sky_scale;           /* multiplies the cube's colour */
	int sky_max_level;         /* the highest mip level to read */
	/* A cube with premultiplied alpha drawn over the sky, or NULL. */
	AeronTexture* overlay_cube;
	AeronSampler* overlay_sampler;
	float overlay_view_to_cube[9];
	float overlay_scale;
	int overlay_max_level;
} TieFlightHyperspaceWarpInput;

/* Draws the entry or the exit prepared with `world_scene` over the finished
 * scene: the streaks and the tunnel's fades into their layer, the sky
 * passes, then the composite (docs/hd_hyperspace_jump.md, Scene warp).
 * Returns the image to present instead of the scene, or NULL after a
 * failure, which is recorded on the command buffer. */
AeronTexture* TieFlightHyperspace_RenderWarp(TieFlightHyperspace* hyperspace,
											   AeronCommandBuffer* command_buffer,
											   const TieFlightHyperspaceWarpInput* input);

/* Whether the exposure meter should keep its value this frame, so it doesn't
 * darken the white-out or the bright start and end of the tunnel. Set by the
 * latest successful Prepare. */
int TieFlightHyperspace_HoldsExposure(const TieFlightHyperspace* hyperspace);

/* Returns the lighting contribution built by the latest successful Prepare. */
int TieFlightHyperspace_GetPreparedLighting(const TieFlightHyperspace* hyperspace,
											  TieFlightHyperspaceLighting* out);

/* AeronScene BEFORE_OPAQUE hook, in the dedicated scene only: the tunnel. */
void TieFlightHyperspace_Draw(AeronCommandBuffer* command_buffer, AeronRenderPass* render_pass, int rt_w,
								int rt_h, void* user);

#ifdef __cplusplus
}
#endif

#endif /* TIE_FLIGHT_HYPERSPACE_H */
