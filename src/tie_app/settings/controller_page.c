#include "tie_app/settings/controller_page.h"

#include "tie_app/settings/bindings_editor.h"
#include "tie_runtime/input/controller_mapping.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char* const k_axis_names[TIE_INPUT_AXIS_COUNT] = { "Yaw", "Pitch", "Roll", "Throttle" };

/* Rows of the bindings list: the flight axes, then every action, each group
 * under a heading row. */
typedef enum ControllerRowKind {
	CONTROLLER_ROW_HEADING = 0,
	CONTROLLER_ROW_AXIS,
	CONTROLLER_ROW_ACTION,
} ControllerRowKind;

typedef struct ControllerRow {
	ControllerRowKind kind;
	int value; /* TieInputAxis, TieInputAction, or a heading's category (-1 for the axes) */
} ControllerRow;

#define CONTROLLER_ROW_CAP (TIE_INPUT_AXIS_COUNT + TIE_INPUT_ACTION_COUNT + TIE_INPUT_ACTION_CATEGORY_COUNT + 1)

static size_t TieControllerPage_BuildRows(ControllerRow* rows) {
	size_t count = 0;
	rows[count++] = (ControllerRow) { CONTROLLER_ROW_HEADING, -1 };
	for (int axis = 0; axis < TIE_INPUT_AXIS_COUNT; ++axis)
		rows[count++] = (ControllerRow) { CONTROLLER_ROW_AXIS, axis };
	for (int category = 0; category < TIE_INPUT_ACTION_CATEGORY_COUNT; ++category) {
		rows[count++] = (ControllerRow) { CONTROLLER_ROW_HEADING, category };
		for (int action = TIE_INPUT_ACTION_NONE + 1; action < TIE_INPUT_ACTION_COUNT; ++action)
			if ((int)TieInputActions_Category((TieInputAction)action) == category)
				rows[count++] = (ControllerRow) { CONTROLLER_ROW_ACTION, action };
	}
	return count;
}

/* Devices are numbered 1.. in the order the device list shows them. */
static int TieControllerPage_DeviceNumber(const AeronInputSnapshot* input, const AeronControllerSnapshot* device) {
	int number = 0;
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		if (!input->controllers[i].connected)
			continue;
		++number;
		if (&input->controllers[i] == device)
			return number;
	}
	return 0;
}

static const AeronControllerSnapshot* TieControllerPage_Instance(const AeronInputSnapshot* input,
																 uint32_t instance_id) {
	for (int i = 0; input && instance_id && i < AERON_CONTROLLER_MAX; ++i)
		if (input->controllers[i].connected && input->controllers[i].instance_id == instance_id)
			return &input->controllers[i];
	return NULL;
}

/* The connected device a saved model's bindings apply to, if any. */
static const AeronControllerSnapshot* TieControllerPage_ModelDevice(const AeronInputSnapshot* input,
																	const TieControllerModel* model) {
	for (int i = 0; input && i < AERON_CONTROLLER_MAX; ++i) {
		const AeronControllerSnapshot* d = &input->controllers[i];
		if (d->connected && d->kind == model->kind && !strcmp(d->guid, model->guid))
			return d;
	}
	return NULL;
}

static bool TieControllerPage_AnyConnected(const AeronInputSnapshot* input) {
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i)
		if (input->controllers[i].connected && input->controllers[i].kind != AERON_CONTROLLER_KIND_NONE)
			return true;
	return false;
}

static void TieControllerSettings_ApplyDraft(TieControllerSettings* settings) {
	settings->dirty = !TieControllerMapping_Optionsequal(&settings->draft, &settings->original);
	if (!TieControllerMapping_OptionsValid(&settings->draft, settings->error, sizeof settings->error))
		return;
	TieControllerMapping_SetOptions(&settings->draft);
	settings->error[0] = 0;
}

/* The model holding `device`'s bindings in `options`, added if missing. A
 * model saved for another device type starts over empty. */
static int TieControllerSettings_EnsureModel(TieControllerSettings* settings, TieControllerOptions* options,
											 const AeronControllerSnapshot* device) {
	int model = TieControllerMapping_FindModel(options, device->guid);
	if (model < 0) {
		if (!TieControllerMapping_AddModel(options, device, settings->error, sizeof settings->error))
			return -1;
		return (int)options->count - 1;
	}
	if (options->models[model].kind != device->kind) {
		options->models[model].kind = device->kind;
		TieControllerMapping_ClearProfile(&options->models[model].profile, device->kind);
	}
	return model;
}

static bool TieControllerSettings_DigitalSourceEqual(const AeronControllerDigitalSource* left,
													 const AeronControllerDigitalSource* right) {
	return left->kind == right->kind && left->index == right->index &&
		   (left->kind != AERON_CONTROLLER_DIGITAL_HAT || left->hat_direction == right->hat_direction);
}

static const char* TieControllerPage_GamepadAxisDisplay(int source) {
	static const char* const names[AERON_GAMEPAD_AXIS_COUNT] = { "Left X",  "Left Y",       "Right X",
																 "Right Y", "Left Trigger", "Right Trigger" };
	return source >= 0 && source < AERON_GAMEPAD_AXIS_COUNT ? names[source] : NULL;
}

static const char* TieControllerPage_GamepadButtonDisplay(int source) {
	static const char* const names[AERON_GAMEPAD_BUTTON_COUNT] = {
		"South",          "East",          "West",        "North",         "Back",           "Guide",
		"Start",          "Left Stick",    "Right Stick", "Left Shoulder", "Right Shoulder", "D-pad Up",
		"D-pad Down",     "D-pad Left",    "D-pad Right", "Misc 1",        "Right Paddle 1", "Left Paddle 1",
		"Right Paddle 2", "Left Paddle 2", "Touchpad",    "Misc 2",        "Misc 3",         "Misc 4",
		"Misc 5",         "Misc 6"
	};
	return source >= 0 && source < AERON_GAMEPAD_BUTTON_COUNT ? names[source] : NULL;
}

static bool TieControllerSettings_SourceAvailable(const AeronControllerSnapshot* controller,
												  const AeronControllerDigitalSource* source) {
	if (!controller || !controller->connected)
		return false;
	const AeronControllerKind kind = controller->kind;
	if (source->kind == AERON_CONTROLLER_DIGITAL_BUTTON)
		return kind == AERON_CONTROLLER_KIND_GAMEPAD
				   ? source->index < AERON_GAMEPAD_BUTTON_COUNT &&
						 (controller->gamepad_available_buttons & (1u << source->index))
				   : source->index < controller->button_count;
	if (source->kind == AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE ||
		source->kind == AERON_CONTROLLER_DIGITAL_AXIS_NEGATIVE)
		return kind == AERON_CONTROLLER_KIND_GAMEPAD
				   ? source->index < AERON_GAMEPAD_AXIS_COUNT &&
						 (controller->gamepad_available_axes & (1u << source->index))
				   : source->index < controller->axis_count;
	return source->kind == AERON_CONTROLLER_DIGITAL_HAT && kind == AERON_CONTROLLER_KIND_JOYSTICK &&
		   source->index < controller->hat_count;
}

/* "[2] " for a binding on the second listed device; "[-] " when the device
 * holding it is not connected. */
static void TieControllerPage_DevicePrefix(char* buffer, size_t capacity, const AeronInputSnapshot* input,
										   const AeronControllerSnapshot* device) {
	const int number = device ? TieControllerPage_DeviceNumber(input, device) : 0;
	if (number)
		snprintf(buffer, capacity, "[%d] ", number);
	else
		snprintf(buffer, capacity, "[-] ");
}

static void TieControllerSettings_FormatAxisSource(char* buffer, size_t capacity, int source,
												   AeronControllerKind kind,
												   const AeronControllerSnapshot* controller) {
	const char* name = kind == AERON_CONTROLLER_KIND_GAMEPAD ? TieControllerPage_GamepadAxisDisplay(source) : NULL;
	const bool available =
		controller && (kind == AERON_CONTROLLER_KIND_GAMEPAD
						   ? source < AERON_GAMEPAD_AXIS_COUNT && (controller->gamepad_available_axes & (1u << source))
						   : source < controller->axis_count);
	const char* suffix = !controller ? " (not connected)" : available ? "" : " (unavailable)";
	if (name)
		snprintf(buffer, capacity, "%s%s", name, suffix);
	else
		snprintf(buffer, capacity, "Axis %d%s", source, suffix);
}

static const char* TieControllerPage_HatDirectionName(uint8_t direction) {
	switch (direction) {
		case AERON_CONTROLLER_HAT_UP:
			return "Up";
		case AERON_CONTROLLER_HAT_RIGHT:
			return "Right";
		case AERON_CONTROLLER_HAT_DOWN:
			return "Down";
		default:
			return "Left";
	}
}

static void TieControllerSettings_FormatDigitalSource(char* buffer, size_t capacity,
													  const AeronControllerDigitalSource* source,
													  AeronControllerKind kind,
													  const AeronControllerSnapshot* controller) {
	const char* name = NULL;
	if (source->kind == AERON_CONTROLLER_DIGITAL_BUTTON) {
		name = kind == AERON_CONTROLLER_KIND_GAMEPAD ? TieControllerPage_GamepadButtonDisplay(source->index) : NULL;
		if (name)
			snprintf(buffer, capacity, "%s", name);
		else
			snprintf(buffer, capacity, "Button %u", (unsigned)source->index);
	} else if (source->kind == AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE ||
			   source->kind == AERON_CONTROLLER_DIGITAL_AXIS_NEGATIVE) {
		name = kind == AERON_CONTROLLER_KIND_GAMEPAD ? TieControllerPage_GamepadAxisDisplay(source->index) : NULL;
		if (name)
			snprintf(buffer, capacity, "%s %c", name,
					 source->kind == AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE ? '+' : '-');
		else
			snprintf(buffer, capacity, "Axis %u %c", (unsigned)source->index,
					 source->kind == AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE ? '+' : '-');
	} else {
		snprintf(buffer, capacity, "Hat %u %s", (unsigned)source->index,
				 TieControllerPage_HatDirectionName(source->hat_direction));
	}
	if (controller && !TieControllerSettings_SourceAvailable(controller, source)) {
		const size_t used = strlen(buffer);
		if (used < capacity)
			snprintf(buffer + used, capacity - used, " (unavailable)");
	}
}

static void TieControllerSettings_AppendText(char* buffer, size_t capacity, const char* text) {
	const size_t used = strlen(buffer);
	if (used < capacity)
		snprintf(buffer + used, capacity - used, "%s%s", used ? ", " : "", text);
}

/* The model whose profile binds logical `axis`, or -1. */
static int TieControllerSettings_AxisOwner(const TieControllerOptions* options, TieInputAxis axis) {
	for (size_t i = 0; i < options->count; ++i)
		if (options->models[i].profile.mapping.axes[axis].source >= 0)
			return (int)i;
	return -1;
}

static void TieControllerSettings_DescribeAxis(char* buffer, size_t capacity, const TieControllerSettings* settings,
											   const AeronInputSnapshot* input, TieInputAxis axis) {
	const int owner = TieControllerSettings_AxisOwner(&settings->draft, axis);
	if (owner < 0) {
		snprintf(buffer, capacity, "Not Bound");
		return;
	}
	const TieControllerModel* model = &settings->draft.models[owner];
	const AeronControllerSnapshot* device = TieControllerPage_ModelDevice(input, model);
	char prefix[16], source[96];
	TieControllerPage_DevicePrefix(prefix, sizeof prefix, input, device);
	TieControllerSettings_FormatAxisSource(source, sizeof source, model->profile.mapping.axes[axis].source,
										   model->kind, device);
	snprintf(buffer, capacity, "%s%s%s", prefix, source, model->profile.mapping.axes[axis].invert ? ", inverted" : "");
}

static void TieControllerSettings_DescribeAction(char* buffer, size_t capacity, const TieControllerSettings* settings,
												 const AeronInputSnapshot* input, TieInputAction action) {
	buffer[0] = '\0';
	for (size_t m = 0; m < settings->draft.count; ++m) {
		const TieControllerModel* model = &settings->draft.models[m];
		const AeronControllerSnapshot* device = TieControllerPage_ModelDevice(input, model);
		for (size_t i = 0; i < model->profile.binding_count; ++i) {
			if (model->profile.bindings[i].action != action)
				continue;
			char prefix[16], source[96], text[112];
			TieControllerPage_DevicePrefix(prefix, sizeof prefix, input, device);
			TieControllerSettings_FormatDigitalSource(source, sizeof source, &model->profile.bindings[i].source,
													  model->kind, device);
			snprintf(text, sizeof text, "%s%s", prefix, source);
			TieControllerSettings_AppendText(buffer, capacity, text);
		}
	}
	if (!buffer[0])
		snprintf(buffer, capacity, "Not Bound");
}

void TieControllerSettings_Open(TieControllerSettings* settings, const TieAppConfigState* config) {
	if (!settings || !config)
		return;
	memset(settings, 0, sizeof *settings);
	settings->original = config->requested.controller;
	settings->draft = settings->original;
	settings->row_selected = SIZE_MAX;
	settings->pending_axis = TIE_INPUT_AXIS_YAW;
}

void TieControllerSettings_CancelCapture(TieControllerSettings* settings, AeronUiContext* ui) {
	if (ui)
		AeronUi_CancelControllerCapture(ui);
	if (settings) {
		settings->axis_conflict_open = 0;
		settings->binding_conflict_open = 0;
	}
}

bool TieControllerSettings_Commit(TieControllerSettings* settings, TieAppConfigState* config, char* error,
								  size_t error_capacity) {
	if (!settings || !config) {
		if (error && error_capacity)
			snprintf(error, error_capacity, "controller settings are unavailable");
		return false;
	}
	if (!settings->dirty)
		return true;
	const bool result = TieAppConfig_SetController(config, &settings->draft, error, error_capacity);
	if (!result)
		return false;
	settings->draft = config->requested.controller;
	settings->original = settings->draft;
	settings->dirty = false;
	return true;
}

void TieControllerSettings_Discover(TieControllerSettings* settings, AeronUiContext* ui,
									const AeronInputSnapshot* input, const TieControllerProfile* defaults) {
	size_t count = settings->draft.count;
	bool ok = TieControllerMapping_InitializeGamepads(&settings->draft, defaults, input, settings->error,
													  sizeof settings->error);
	if (settings->draft.count != count) {
		TieControllerSettings_CancelCapture(settings, ui);
		settings->dirty = true;
		TieControllerMapping_SetOptions(&settings->draft);
	}
	if (ok && settings->capacity_warned)
		settings->error[0] = 0;
	settings->capacity_warned = !ok;
}

/* ---- Devices ---- */

static void TieControllerSettings_DeviceList(const TieControllerSettings* settings, AeronUiContext* ui,
											 const AeronInputSnapshot* input) {
	AeronUi_Header(ui, "Devices");
	int number = 0;
	for (int i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		const AeronControllerSnapshot* d = &input->controllers[i];
		if (!d->connected)
			continue;
		++number;
		char text[256];
		if (d->kind == AERON_CONTROLLER_KIND_GAMEPAD)
			snprintf(text, sizeof text, "[%d]  %s  (gamepad)", number, d->name);
		else
			snprintf(text, sizeof text, "[%d]  %s  (joystick: %d axes, %d buttons, %d hats)", number, d->name,
					 d->axis_count, d->button_count, d->hat_count);
		AeronUi_Label(ui, text);
	}
	if (!number)
		AeronUi_Help(ui, "Connect a controller to bind its controls.");
	char missing[384] = "";
	for (size_t m = 0; m < settings->draft.count; ++m)
		if (!TieControllerPage_ModelDevice(input, &settings->draft.models[m]))
			TieControllerSettings_AppendText(missing, sizeof missing, settings->draft.models[m].name);
	if (missing[0]) {
		char text[448];
		snprintf(text, sizeof text, "Saved bindings, not connected: %s", missing);
		AeronUi_Help(ui, text);
	}
}

/* ---- Axes ---- */

static void TieControllerSettings_InstallAxis(TieControllerSettings* settings, const AeronControllerSnapshot* device,
											  TieInputAxis axis, int source) {
	TieControllerOptions candidate = settings->draft;
	const int model = TieControllerSettings_EnsureModel(settings, &candidate, device);
	if (model < 0)
		return;
	/* Keep the axis' invert and deadzone when it moves to another device. */
	const int owner = TieControllerSettings_AxisOwner(&candidate, axis);
	const TieInputAxisBinding previous = owner >= 0 ? candidate.models[owner].profile.mapping.axes[axis]
													: candidate.models[model].profile.mapping.axes[axis];
	for (size_t i = 0; i < candidate.count; ++i)
		candidate.models[i].profile.mapping.axes[axis].source = -1;
	TieControllerProfile* p = &candidate.models[model].profile;
	for (int i = 0; i < TIE_INPUT_AXIS_COUNT; ++i)
		if (p->mapping.axes[i].source == source)
			p->mapping.axes[i].source = -1;
	p->mapping.axes[axis] = previous;
	p->mapping.axes[axis].source = (int8_t)source;
	if (owner >= 0 && candidate.models[owner].kind != device->kind)
		p->mapping.axes[axis].invert = axis == TIE_INPUT_AXIS_THROTTLE && device->kind == AERON_CONTROLLER_KIND_JOYSTICK;
	if (!TieControllerMapping_OptionsValid(&candidate, settings->error, sizeof settings->error))
		return;
	settings->draft = candidate;
	TieControllerSettings_ApplyDraft(settings);
}

/* A captured axis replaces the logical axis' binding on whichever device had
 * it. Taking a physical axis from another logical axis asks first. */
static void TieControllerSettings_AssignAxis(TieControllerSettings* settings, const AeronControllerSnapshot* device,
											 TieInputAxis axis, int source) {
	settings->conflict_text[0] = 0;
	const int model = TieControllerMapping_FindModel(&settings->draft, device->guid);
	if (model >= 0 && settings->draft.models[model].kind == device->kind)
		for (int i = 0; i < TIE_INPUT_AXIS_COUNT; ++i)
			if (i != (int)axis && settings->draft.models[model].profile.mapping.axes[i].source == source)
				TieControllerSettings_AppendText(settings->conflict_text, sizeof settings->conflict_text,
												 k_axis_names[i]);
	if (settings->conflict_text[0]) {
		settings->pending_instance = device->instance_id;
		settings->pending_axis = axis;
		settings->pending_axis_source = source;
		settings->axis_conflict_open = 1;
	} else
		TieControllerSettings_InstallAxis(settings, device, axis, source);
}

static void TieControllerSettings_AxisEditor(TieControllerSettings* settings, AeronUiContext* ui,
											 const AeronInputSnapshot* input, TieInputAxis axis) {
	char current[160];
	TieControllerSettings_DescribeAxis(current, sizeof current, settings, input, axis);
	const AeronUiControllerCaptureDesc desc = {
		TieControllerPage_AnyConnected(input) ? AERON_UI_CONTROLLER_ANY : 0,
		AERON_UI_CONTROLLER_CAPTURE_ANALOG_AXIS,
	};
	AeronUiControllerInput captured;
	if (AeronUi_ControllerCapture(ui, "Modify", current, &desc, &captured) == AERON_UI_CONTROLLER_CAPTURE_CAPTURED) {
		const AeronControllerSnapshot* device = TieControllerPage_Instance(input, captured.instance_id);
		if (device && device->kind == captured.controller_kind)
			TieControllerSettings_AssignAxis(settings, device, axis, captured.value.axis);
	}
	const int owner = TieControllerSettings_AxisOwner(&settings->draft, axis);
	if (owner < 0) {
		AeronUi_Help(ui, "Click Modify, then move the axis on any device.");
		return;
	}
	TieControllerModel* model = &settings->draft.models[owner];
	TieInputAxisBinding* binding = &model->profile.mapping.axes[axis];
	const AeronControllerSnapshot* device = TieControllerPage_ModelDevice(input, model);
	if (axis == TIE_INPUT_AXIS_THROTTLE) {
		if (device && Aeron_ControllerAxisAvailable(device, binding->source)) {
			const int16_t raw = Aeron_ControllerAxisValue(device, binding->source);
			const uint16_t position =
				TieControllerMapping_ThrottlePosition(raw, model->kind, binding->source, binding->invert);
			AeronUi_PercentageMeter(ui, "Position", (float)position / UINT16_MAX);
		}
	} else if (device && Aeron_ControllerAxisAvailable(device, binding->source)) {
		const int16_t raw = Aeron_ControllerAxisValue(device, binding->source);
		float live = raw < 0 ? (float)raw / 32768.0f : (float)raw / 32767.0f;
		if (TieControllerMapping_EffectiveAxisInvert(model->kind, axis, binding->invert))
			live = -live;
		AeronUi_ControllerAxisMeter(ui, "Input", live, TieInput_AxisDeadzonePercent(binding->deadzone) / 100.0f);
	}
	int invert = binding->invert;
	if (AeronUi_Toggle(ui, "Invert", &invert)) {
		binding->invert = invert != 0;
		TieControllerSettings_ApplyDraft(settings);
	}
	if (axis != TIE_INPUT_AXIS_THROTTLE) {
		float deadzone_percent = TieInput_AxisDeadzonePercent(binding->deadzone);
		const float minimum = TieInput_AxisDeadzonePercent(0.0f);
		if (AeronUi_SliderFloat(ui, "Deadzone", &deadzone_percent, minimum, 100.0f, 1.0f, "%.1f%%")) {
			binding->deadzone = TieInput_AxisDeadzoneFromPercent(deadzone_percent);
			TieControllerSettings_ApplyDraft(settings);
		}
	}
	if (AeronUi_Button(ui, "Clear")) {
		binding->source = -1;
		TieControllerSettings_ApplyDraft(settings);
	}
}

static void TieControllerSettings_AxisConflictModal(TieControllerSettings* settings, AeronUiContext* ui,
													const AeronInputSnapshot* input) {
	if (!AeronUi_BeginModal(ui, "AXIS ALREADY ASSIGNED", &settings->axis_conflict_open, NULL))
		return;
	char text[640];
	snprintf(text, sizeof text, "That axis is assigned to %s. Use it for %s instead?", settings->conflict_text,
			 k_axis_names[settings->pending_axis]);
	AeronUi_Error(ui, text);
	AeronUi_BeginColumns(ui, 2, NULL);
	if (AeronUi_Button(ui, "Replace")) {
		const AeronControllerSnapshot* device = TieControllerPage_Instance(input, settings->pending_instance);
		if (device)
			TieControllerSettings_InstallAxis(settings, device, settings->pending_axis, settings->pending_axis_source);
		settings->axis_conflict_open = 0;
	}
	AeronUi_NextColumn(ui);
	if (AeronUi_Button(ui, "Cancel"))
		settings->axis_conflict_open = 0;
	AeronUi_EndColumns(ui);
	AeronUi_EndModal(ui);
}

/* ---- Actions ---- */

static size_t TieControllerSettings_FindSourceBinding(const TieControllerProfile* profile,
													  const AeronControllerDigitalSource* source) {
	for (size_t index = 0; index < profile->binding_count; ++index)
		if (TieControllerSettings_DigitalSourceEqual(&profile->bindings[index].source, source))
			return index;
	return SIZE_MAX;
}

static void TieControllerSettings_RemoveBinding(TieControllerProfile* profile, size_t index) {
	if (index >= profile->binding_count)
		return;
	if (index + 1 < profile->binding_count)
		memmove(&profile->bindings[index], &profile->bindings[index + 1],
				(profile->binding_count - index - 1) * sizeof profile->bindings[0]);
	--profile->binding_count;
}

static void TieControllerSettings_RemoveAction(TieControllerOptions* options, TieInputAction action) {
	for (size_t m = 0; m < options->count; ++m) {
		TieControllerProfile* p = &options->models[m].profile;
		for (size_t i = p->binding_count; i-- > 0;)
			if (p->bindings[i].action == action)
				TieControllerSettings_RemoveBinding(p, i);
	}
}

static bool TieControllerSettings_ActionBound(const TieControllerOptions* options, TieInputAction action) {
	for (size_t m = 0; m < options->count; ++m)
		for (size_t i = 0; i < options->models[m].profile.binding_count; ++i)
			if (options->models[m].profile.bindings[i].action == action)
				return true;
	return false;
}

/* Binds `source` on `device` to `action`, taking it from any other action;
 * `replace` first drops the action's other bindings on every device. */
static void TieControllerSettings_InstallDigital(TieControllerSettings* settings,
												 const AeronControllerSnapshot* device,
												 const AeronControllerDigitalSource* source, TieInputAction action,
												 bool replace) {
	TieControllerOptions candidate = settings->draft;
	const int model = TieControllerSettings_EnsureModel(settings, &candidate, device);
	if (model < 0)
		return;
	TieControllerProfile* p = &candidate.models[model].profile;
	const size_t existing = TieControllerSettings_FindSourceBinding(p, source);
	if (existing != SIZE_MAX)
		TieControllerSettings_RemoveBinding(p, existing);
	if (replace)
		TieControllerSettings_RemoveAction(&candidate, action);
	if (p->binding_count >= TIE_CONTROLLER_BINDING_CAP) {
		snprintf(settings->error, sizeof settings->error, "%s has reached its binding capacity.", device->name);
		return;
	}
	p->bindings[p->binding_count++] = (TieInputActionBinding) { .source = *source, .action = action };
	if (!TieControllerMapping_OptionsValid(&candidate, settings->error, sizeof settings->error))
		return;
	settings->draft = candidate;
	TieControllerSettings_ApplyDraft(settings);
}

static void TieControllerSettings_AssignDigital(TieControllerSettings* settings, const AeronControllerSnapshot* device,
												const AeronControllerDigitalSource* source, TieInputAction action,
												bool replace) {
	const int model = TieControllerMapping_FindModel(&settings->draft, device->guid);
	if (model >= 0 && settings->draft.models[model].kind == device->kind) {
		const TieControllerProfile* p = &settings->draft.models[model].profile;
		const size_t existing = TieControllerSettings_FindSourceBinding(p, source);
		if (existing != SIZE_MAX && p->bindings[existing].action != action) {
			settings->pending_instance = device->instance_id;
			settings->pending_digital = *source;
			settings->pending_action = action;
			settings->pending_replace = replace;
			settings->conflicting_action = p->bindings[existing].action;
			settings->binding_conflict_open = 1;
			return;
		}
	}
	TieControllerSettings_InstallDigital(settings, device, source, action, replace);
}

static void TieControllerSettings_ActionEditor(TieControllerSettings* settings, AeronUiContext* ui,
											   const AeronInputSnapshot* input, TieInputAction action) {
	char current[384];
	TieControllerSettings_DescribeAction(current, sizeof current, settings, input, action);
	const uint32_t any = TieControllerPage_AnyConnected(input) ? AERON_UI_CONTROLLER_ANY : 0;
	const AeronUiControllerCaptureDesc desc = { any, AERON_UI_CONTROLLER_CAPTURE_DIGITAL };
	AeronUiControllerInput captured;
	for (int add = 0; add < 2; ++add) {
		const AeronUiControllerCaptureResult result =
			add ? AeronUi_ControllerCapture(ui, "Add Another", "Press to add", &desc, &captured)
				: AeronUi_ControllerCapture(ui, "Modify", current, &desc, &captured);
		if (result != AERON_UI_CONTROLLER_CAPTURE_CAPTURED)
			continue;
		const AeronControllerSnapshot* device = TieControllerPage_Instance(input, captured.instance_id);
		if (device && device->kind == captured.controller_kind)
			TieControllerSettings_AssignDigital(settings, device, &captured.value.digital, action, !add);
	}
	if (AeronUi_ButtonEnabled(ui, "Clear", TieControllerSettings_ActionBound(&settings->draft, action))) {
		TieControllerSettings_RemoveAction(&settings->draft, action);
		TieControllerSettings_ApplyDraft(settings);
	}
}

static void TieControllerSettings_BindingConflictModal(TieControllerSettings* settings, AeronUiContext* ui,
													   const AeronInputSnapshot* input) {
	const AeronControllerSnapshot* device = TieControllerPage_Instance(input, settings->pending_instance);
	if (!device) {
		settings->binding_conflict_open = 0;
		return;
	}
	char prefix[16], source[96], text[112];
	TieControllerPage_DevicePrefix(prefix, sizeof prefix, input, device);
	TieControllerSettings_FormatDigitalSource(source, sizeof source, &settings->pending_digital, device->kind, device);
	snprintf(text, sizeof text, "%s%s", prefix, source);
	if (TieBindingsEditor_Conflict(ui, &settings->binding_conflict_open, text, settings->conflicting_action,
								   settings->pending_action))
		TieControllerSettings_InstallDigital(settings, device, &settings->pending_digital, settings->pending_action,
											 settings->pending_replace);
}

/* ---- Page ---- */

static void TieControllerSettings_RestoreModal(TieControllerSettings* settings, AeronUiContext* ui,
											   const AeronInputSnapshot* input) {
	if (!AeronUi_BeginModal(ui, "CLEAR ALL CONTROLLER BINDINGS", &settings->restore_modal_open, NULL))
		return;
	AeronUi_Help(ui, "Clear all saved controller bindings, including disconnected devices?");
	AeronUi_BeginColumns(ui, 2, NULL);
	if (AeronUi_Button(ui, "Clear All")) {
		TieControllerOptions candidate = settings->draft;
		bool ok = true;
		for (int i = 0; input && i < AERON_CONTROLLER_MAX; ++i) {
			const AeronControllerSnapshot* d = &input->controllers[i];
			if (d->connected && d->kind == AERON_CONTROLLER_KIND_GAMEPAD &&
				!TieControllerMapping_AddModel(&candidate, d, settings->error, sizeof settings->error))
				ok = false;
		}
		if (ok) {
			for (int i = 0; input && i < AERON_CONTROLLER_MAX; ++i) {
				const AeronControllerSnapshot* d = &input->controllers[i];
				const int model = d->connected ? TieControllerMapping_FindModel(&candidate, d->guid) : -1;
				if (model >= 0)
					candidate.models[model].kind = d->kind;
			}
			for (size_t i = 0; i < candidate.count; ++i)
				TieControllerMapping_ClearProfile(&candidate.models[i].profile, candidate.models[i].kind);
			settings->draft = candidate;
			settings->dirty = !TieControllerMapping_Optionsequal(&settings->draft, &settings->original);
			TieControllerMapping_SetOptions(&settings->draft);
			settings->error[0] = 0;
		}
		settings->restore_modal_open = 0;
	}
	AeronUi_NextColumn(ui);
	if (AeronUi_Button(ui, "Cancel"))
		settings->restore_modal_open = 0;
	AeronUi_EndColumns(ui);
	AeronUi_EndModal(ui);
}

void TieControllerSettings_Draw(TieControllerSettings* settings, AeronUiContext* ui,
								const AeronInputSnapshot* input) {
	if (!settings || !ui || !input)
		return;
	TieControllerSettings_DeviceList(settings, ui, input);
	AeronUi_Spacer(ui, 8.0f);

	ControllerRow rows[CONTROLLER_ROW_CAP];
	const size_t row_count = TieControllerPage_BuildRows(rows);
	AeronUiListItem items[CONTROLLER_ROW_CAP];
	static char details[CONTROLLER_ROW_CAP][384];
	for (size_t i = 0; i < row_count; ++i) {
		const ControllerRow* row = &rows[i];
		details[i][0] = '\0';
		const char* label;
		uint32_t flags = AERON_UI_LIST_ITEM_NONE;
		if (row->kind == CONTROLLER_ROW_HEADING) {
			label = row->value < 0 ? "FLIGHT AXES"
								   : TieInputActions_CategoryName((TieInputActionCategory)row->value);
			flags = AERON_UI_LIST_ITEM_DISABLED;
		} else if (row->kind == CONTROLLER_ROW_AXIS) {
			label = k_axis_names[row->value];
			TieControllerSettings_DescribeAxis(details[i], sizeof details[i], settings, input,
											   (TieInputAxis)row->value);
		} else {
			label = TieInputActions_DisplayName((TieInputAction)row->value);
			TieControllerSettings_DescribeAction(details[i], sizeof details[i], settings, input,
												 (TieInputAction)row->value);
		}
		items[i] = (AeronUiListItem) { .id = i, .label = label, .detail = details[i], .flags = flags };
	}
	if (settings->row_selected >= row_count || rows[settings->row_selected].kind == CONTROLLER_ROW_HEADING)
		settings->row_selected = SIZE_MAX;

	/* Room under the list for the selected row's editor and the page's buttons. */
	float trailing_height = 470.0f;
	if (settings->error[0])
		trailing_height += AeronUi_MeasureHelpHeight(ui, settings->error, 0.0f) + 60.0f;
	float list_height = AeronUi_AvailableHeight(ui) - trailing_height;
	if (list_height < 200.0f)
		list_height = 200.0f;
	AeronUi_Header(ui, "Bindings");
	const size_t before = settings->row_selected;
	AeronUi_ListBox(ui, "Bindings", items, row_count, &settings->row_selected, list_height);
	if (settings->row_selected != before)
		AeronUi_CancelControllerCapture(ui);

	AeronUi_Separator(ui);
	if (settings->row_selected == SIZE_MAX)
		AeronUi_Help(ui, "Select an axis or action, click Modify, then press a button or move an axis on any "
						 "device. The number in brackets is the device it is on.");
	else {
		const ControllerRow* row = &rows[settings->row_selected];
		char title[128];
		snprintf(title, sizeof title, "%s", items[settings->row_selected].label);
		AeronUi_Header(ui, title);
		AeronUi_PushId(ui, (int)settings->row_selected);
		if (row->kind == CONTROLLER_ROW_AXIS)
			TieControllerSettings_AxisEditor(settings, ui, input, (TieInputAxis)row->value);
		else
			TieControllerSettings_ActionEditor(settings, ui, input, (TieInputAction)row->value);
		AeronUi_PopId(ui);
	}
	if (settings->error[0]) {
		AeronUi_Error(ui, settings->error);
		if (AeronUi_Button(ui, "Dismiss Error"))
			settings->error[0] = '\0';
	}
	if (AeronUi_Button(ui, "Clear All Controller Bindings"))
		settings->restore_modal_open = 1;
}

void TieControllerSettings_DrawModals(TieControllerSettings* settings, AeronUiContext* ui,
									  const AeronInputSnapshot* input, const TieAppConfigState* config) {
	(void)config;
	if (!settings || !ui || !input)
		return;
	if (settings->axis_conflict_open) {
		TieControllerSettings_AxisConflictModal(settings, ui, input);
		return;
	}
	if (settings->binding_conflict_open) {
		TieControllerSettings_BindingConflictModal(settings, ui, input);
		return;
	}
	if (settings->restore_modal_open)
		TieControllerSettings_RestoreModal(settings, ui, input);
}
