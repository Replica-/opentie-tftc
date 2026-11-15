#include "tie_app/music_overlay.h"

#include "aeron/scene/ui.h"
#include "tie_runtime/audio/music_policy.h"

#include <stddef.h>

static AeronUiContext* s_overlay_ui;

bool TieMusicOverlay_Init(const AeronFontAtlas* font) {
	if (s_overlay_ui || !font)
		return s_overlay_ui != NULL;
	s_overlay_ui = AeronUi_Create(NULL);
	if (!s_overlay_ui)
		return false;
	AeronUiTheme theme = *AeronUi_DefaultTheme();
	theme.surface[3] = 0.6f;
	AeronUi_SetTheme(s_overlay_ui, &theme);
	AeronUi_SetFonts(s_overlay_ui, &(AeronUiFontSet) { .regular = font, .title = font });
	return true;
}

void TieMusicOverlay_Shutdown(void) {
	AeronUi_Destroy(s_overlay_ui);
	s_overlay_ui = NULL;
}

void TieMusicOverlay_Frame(const AeronInputSnapshot* input, float dt_seconds) {
	if (!s_overlay_ui || !input)
		return;
	char line[256];
	TieMusicPolicy_Describe(line, sizeof line);
	AeronUi_BeginFrame(s_overlay_ui, &(AeronUiFrameDesc) { .input = input, .dt_seconds = dt_seconds });
	if (AeronUi_BeginWindow(s_overlay_ui, "Music",
							&(AeronUiWindowDesc) { .width_ref = 760.0f, .x_ref = 12.0f, .y_ref = 12.0f,
												   .flags = AERON_UI_WINDOW_NO_TITLE })) {
		AeronUi_Label(s_overlay_ui, line);
		AeronUi_EndWindow(s_overlay_ui);
	}
	(void)AeronUi_EndFrame(s_overlay_ui);
	AeronUi_Submit(s_overlay_ui);
}
