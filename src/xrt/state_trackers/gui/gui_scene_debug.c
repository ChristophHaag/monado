// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A debugging scene.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup gui
 */

#include "xrt/xrt_config_have.h"

#include "os/os_time.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_sink.h"

#ifdef XRT_HAVE_OPENCV
#include "tracking/t_tracking.h"
#endif

#include "xrt/xrt_frame.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_tracking.h"
#include "xrt/xrt_settings.h"
#include "xrt/xrt_frameserver.h"

#include "math/m_api.h"
#include "math/m_filter_fifo.h"

#include "gui_common.h"
#include "gui_imgui.h"
#include "gui_window_record.h"

#include "imgui_monado/cimgui_monado.h"

#include <float.h>

/*
 *
 * Structs and defines.
 *
 */

/*!
 * @defgroup gui_debug Debug GUI
 * @ingroup gui
 *
 * @brief GUI for live inspecting Monado.
 */

/*!
 * A single record window, here only used to draw a single element in a object
 * window, holds all the needed state.
 *
 * @ingroup gui_debug
 */
struct debug_record
{
	void *ptr;

	struct gui_record_window rw;
};

/*!
 * A GUI scene for debugging Monado while it is running, it uses the variable
 * tracking code in the @ref util/u_var.h file to provide live updates state.
 *
 * @implements gui_scene
 * @ingroup gui_debug
 */
struct debug_scene
{
	struct gui_scene base;
	struct xrt_frame_context *xfctx;

	struct debug_record recs[32];
	uint32_t num_recrs;
};

//! How many nested gui headers can we show, overly large.
#define MAX_HEADER_NESTING 256

//! Shared flags for color gui elements.
#define COLOR_FLAGS (ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueWheel)

/*!
 * One "frame" of draw state, what is passed to the variable tracking visitor
 * functions, holds pointers to the program and live state such as visibility
 * stack of gui headers.
 *
 * @ingroup gui_debug
 */
struct draw_state
{
	struct gui_program *p;
	struct debug_scene *ds;

	//! Visibility stack for nested headers.
	bool vis_stack[MAX_HEADER_NESTING];
	int vis_i;

	//! Should we show the GUI headers for record sinks.
	bool inhibit_sink_headers;
};

/*!
 * State for plotting @ref m_ff_vec3_f32, assumes it's relative to now.
 *
 * @ingroup gui_debug
 */
struct plot_state
{
	//! The filter fifo we are plotting.
	struct m_ff_vec3_f32 *ff;

	//! When now is, all entries are made relative to this.
	uint64_t now;
};


/*
 *
 * Helper functions.
 *
 */

static void
conv_rgb_f32_to_u8(struct xrt_colour_rgb_f32 *from, struct xrt_colour_rgb_u8 *to)
{
	to->r = (uint8_t)(from->r * 255.0f);
	to->g = (uint8_t)(from->g * 255.0f);
	to->b = (uint8_t)(from->b * 255.0f);
}

static void
conv_rgb_u8_to_f32(struct xrt_colour_rgb_u8 *from, struct xrt_colour_rgb_f32 *to)
{
	to->r = from->r / 255.0f;
	to->g = from->g / 255.0f;
	to->b = from->b / 255.0f;
}

static void
handle_draggable_vec3_f32(const char *name, struct xrt_vec3 *v)
{
	float min = -256.0f;
	float max = 256.0f;

	igDragFloat3(name, (float *)v, 0.005f, min, max, "%+f", 1.0f);
}

static void
handle_draggable_quat(const char *name, struct xrt_quat *q)
{
	float min = -1.0f;
	float max = 1.0f;

	igDragFloat4(name, (float *)q, 0.005f, min, max, "%+f", 1.0f);

	// Avoid invalid
	if (q->x == 0.0f && q->y == 0.0f && q->z == 0.0f && q->w == 0.0f) {
		q->w = 1.0f;
	}

	// And make sure it's a unit rotation.
	math_quat_normalize(q);
}

static struct debug_record *
ensure_debug_record_created(void *ptr, struct draw_state *state)
{
	struct debug_scene *ds = state->ds;
	struct u_sink_debug *usd = (struct u_sink_debug *)ptr;

	if (usd->sink == NULL) {
		struct debug_record *dr = &ds->recs[ds->num_recrs++];

		dr->ptr = ptr;

		gui_window_record_init(&dr->rw);
		u_sink_debug_set_sink(usd, &dr->rw.sink);

		return dr;
	}

	for (size_t i = 0; i < ARRAY_SIZE(ds->recs); i++) {
		struct debug_record *dr = &ds->recs[i];

		if ((ptrdiff_t)dr->ptr == (ptrdiff_t)ptr) {
			return dr;
		}
	}

	return NULL;
}

// Currently unused.
XRT_MAYBE_UNUSED static void
draw_sink_to_background(struct u_var_info *var, struct draw_state *state)
{
	struct debug_record *dr = ensure_debug_record_created(var->ptr, state);
	if (dr == NULL) {
		return;
	}

	gui_window_record_to_background(&dr->rw, state->p);
}


/*
 *
 * Plot helpers.
 *
 */

#define PLOT_HELPER(elm)                                                                                               \
	static ImPlotPoint plot_vec3_f32_##elm(void *ptr, int index)                                                   \
	{                                                                                                              \
		struct plot_state *state = (struct plot_state *)ptr;                                                   \
		struct xrt_vec3 value;                                                                                 \
		uint64_t timestamp;                                                                                    \
		m_ff_vec3_f32_get(state->ff, index, &value, &timestamp);                                               \
		ImPlotPoint point = {time_ns_to_s(state->now - timestamp), value.elm};                                 \
		return point;                                                                                          \
	}

PLOT_HELPER(x)
PLOT_HELPER(y)
PLOT_HELPER(z)

static ImPlotPoint
plot_curve_point(void *ptr, int i)
{
	struct u_var_curve *c = (struct u_var_curve *)ptr;
	struct u_var_curve_point point = c->getter(c->data, i);
	ImPlotPoint implot_point = {point.x, point.y};
	return implot_point;
}

static float
plot_f32_array_value(void *ptr, int i)
{
	float *arr = ptr;
	return arr[i];
}


/*
 *
 * Main debug gui visitor functions.
 *
 */


static void
on_color_rgb_f32(const char *name, void *ptr)
{
	igColorEdit3(name, (float *)ptr, COLOR_FLAGS);
	igSameLine(0.0f, 4.0f);
	igText("%s", name);
}

static void
on_color_rgb_u8(const char *name, void *ptr)
{
	struct xrt_colour_rgb_f32 tmp;
	conv_rgb_u8_to_f32((struct xrt_colour_rgb_u8 *)ptr, &tmp);
	igColorEdit3(name, (float *)&tmp, COLOR_FLAGS);
	igSameLine(0.0f, 4.0f);
	igText("%s", name);
	conv_rgb_f32_to_u8(&tmp, (struct xrt_colour_rgb_u8 *)ptr);
}

static void
on_f32_arr(const char *name, void *ptr)
{
	struct u_var_f32_arr *f32_arr = ptr;
	int index = *f32_arr->index_ptr;
	int length = f32_arr->length;
	float *arr = (float *)f32_arr->data;

	float w = igGetWindowContentRegionWidth();
	ImVec2 graph_size = {w, 200};

	float stats_min = FLT_MAX;
	float stats_max = FLT_MAX;

	igPlotLinesFnFloatPtr(    //
	    name,                 //
	    plot_f32_array_value, //
	    arr,                  //
	    length,               //
	    index,                //
	    NULL,                 //
	    stats_min,            //
	    stats_max,            //
	    graph_size);          //
}

static void
on_timing(const char *name, void *ptr)
{
	struct u_var_timing *frametime_arr = ptr;
	struct u_var_f32_arr *f32_arr = &frametime_arr->values;
	int index = *f32_arr->index_ptr;
	int length = f32_arr->length;
	float *arr = (float *)f32_arr->data;

	float w = igGetWindowContentRegionWidth();
	ImVec2 graph_size = {w, 200};


	float stats_min = FLT_MAX;
	float stats_max = 0;

	for (int f = 0; f < length; f++) {
		if (arr[f] < stats_min)
			stats_min = arr[f];
		if (arr[f] > stats_max)
			stats_max = arr[f];
	}

	igPlotTimings(                              //
	    name,                                   //
	    plot_f32_array_value,                   //
	    arr,                                    //
	    length,                                 //
	    index,                                  //
	    NULL,                                   //
	    0,                                      //
	    stats_max,                              //
	    graph_size,                             //
	    frametime_arr->reference_timing,        //
	    frametime_arr->center_reference_timing, //
	    frametime_arr->range,                   //
	    frametime_arr->unit,                    //
	    frametime_arr->dynamic_rescale);        //
}

static void
on_pose(const char *name, void *ptr)
{
	struct xrt_pose *pose = (struct xrt_pose *)ptr;
	char text[512];
	snprintf(text, 512, "%s.position", name);
	handle_draggable_vec3_f32(text, &pose->position);
	snprintf(text, 512, "%s.orientation", name);
	handle_draggable_quat(text, &pose->orientation);
}

static void
on_ff_vec3_var(struct u_var_info *info, struct gui_program *p)
{
	char tmp[512];
	const char *name = info->name;
	struct m_ff_vec3_f32 *ff = (struct m_ff_vec3_f32 *)info->ptr;


	struct xrt_vec3 value = {0};

	uint64_t timestamp;

	m_ff_vec3_f32_get(ff, 0, &value, &timestamp);
	float value_arr[3] = {value.x, value.y, value.z};

	snprintf(tmp, sizeof(tmp), "%s.toggle", name);
	igToggleButton(tmp, &info->gui.graphed);
	igSameLine(0, 0);
	igInputFloat3(name, value_arr, "%+f", ImGuiInputTextFlags_ReadOnly);

	if (!info->gui.graphed) {
		return;
	}


	/*
	 * Showing the plot
	 */

	struct plot_state state = {ff, os_monotonic_get_ns()};
	ImPlotFlags flags = 0;
	ImPlotAxisFlags x_flags = 0;
	ImPlotAxisFlags y_flags = 0;
	ImPlotAxisFlags y2_flags = 0;
	ImPlotAxisFlags y3_flags = 0;

	ImVec2 size = {igGetWindowContentRegionWidth(), 256};
	bool shown = ImPlot_BeginPlot(name, "time", "value", size, flags, x_flags, y_flags, y2_flags, y3_flags);
	if (!shown) {
		return;
	}

	size_t num = m_ff_vec3_f32_get_num(ff);
	ImPlot_PlotLineG("z", plot_vec3_f32_z, &state, num, 0); // ZXY order to match RGB colors with default color map
	ImPlot_PlotLineG("x", plot_vec3_f32_x, &state, num, 0);
	ImPlot_PlotLineG("y", plot_vec3_f32_y, &state, num, 0);

	ImPlot_EndPlot();
}

static void
on_sink_debug_var(const char *name, void *ptr, struct draw_state *state)
{
	bool gui_header = !state->inhibit_sink_headers;

	struct debug_record *dr = ensure_debug_record_created(ptr, state);
	if (dr == NULL) {
		return;
	}

	if (gui_header) {
		const ImGuiTreeNodeFlags_ flags = ImGuiTreeNodeFlags_DefaultOpen;
		if (!igCollapsingHeaderBoolPtr(name, NULL, flags)) {
			return;
		}
	}

	gui_window_record_render(&dr->rw, state->p);
}

static void
on_button_var(const char *name, void *ptr)
{
	struct u_var_button *btn = (struct u_var_button *)ptr;
	ImVec2 dims = {btn->width, btn->height};
	const char *label = strlen(btn->label) == 0 ? name : btn->label;
	bool disabled = btn->disabled;

	if (disabled) {
		igPushStyleVarFloat(ImGuiStyleVar_Alpha, 0.6f);
		igPushItemFlag(ImGuiItemFlags_Disabled, true);
	}

	if (igButton(label, dims)) {
		btn->cb(btn->ptr);
	}

	if (disabled) {
		igPopItemFlag();
		igPopStyleVar(1);
	}
}

static void
on_combo_var(const char *name, void *ptr)
{
	struct u_var_combo *combo = (struct u_var_combo *)ptr;
	igComboStr(name, combo->value, combo->options, combo->count);
}

static void
on_histogram_f32_var(const char *name, void *ptr)
{
	struct u_var_histogram_f32 *h = (struct u_var_histogram_f32 *)ptr;
	ImVec2 zero = {h->width, h->height};
	igPlotHistogramFloatPtr(name, h->values, h->count, 0, NULL, FLT_MAX, FLT_MAX, zero, sizeof(float));
}

static void
on_curve_var(const char *name, void *ptr)
{
	struct u_var_curve *c = (struct u_var_curve *)ptr;
	ImVec2 size = {igGetWindowContentRegionWidth(), 256};

	bool shown = ImPlot_BeginPlot(name, c->xlabel, c->ylabel, size, 0, 0, 0, 0, 0);
	if (!shown) {
		return;
	}

	ImPlot_PlotLineG(c->label, plot_curve_point, c, c->count, 0);
	ImPlot_EndPlot();
}

static void
on_curves_var(const char *name, void *ptr)
{
	struct u_var_curves *cs = (struct u_var_curves *)ptr;
	ImVec2 size = {igGetWindowContentRegionWidth(), 256};

	bool shown = ImPlot_BeginPlot(name, cs->xlabel, cs->ylabel, size, 0, 0, 0, 0, 0);
	if (!shown) {
		return;
	}

	for (int i = 0; i < cs->curve_count; i++) {
		struct u_var_curve *c = &cs->curves[i];
		ImPlot_PlotLineG(c->label, plot_curve_point, c, c->count, 0);
	}
	ImPlot_EndPlot();
}

static void
on_draggable_f32_var(const char *name, void *ptr)
{
	struct u_var_draggable_f32 *d = (struct u_var_draggable_f32 *)ptr;
	igDragFloat(name, &d->val, d->step, d->min, d->max, "%+f", ImGuiSliderFlags_None);
}

static void
on_draggable_u16_var(const char *name, void *ptr)
{
	struct u_var_draggable_u16 *d = (struct u_var_draggable_u16 *)ptr;
	igDragScalar(name, ImGuiDataType_U16, d->val, d->step, &d->min, &d->max, NULL, ImGuiSliderFlags_None);
}

static void
on_gui_header(const char *name, struct draw_state *state)
{

	assert(state->vis_i == 0 && "Do not mix GUI_HEADER with GUI_HEADER_BEGIN/END");
	state->vis_stack[state->vis_i] = igCollapsingHeaderBoolPtr(name, NULL, 0);
}

static void
on_gui_header_begin(const char *name, struct draw_state *state)
{
	bool is_open = igCollapsingHeaderBoolPtr(name, NULL, 0);
	state->vis_stack[state->vis_i] = is_open;
	if (is_open) {
		igIndent(8.0f);
	}
}

static void
on_gui_header_end(void)
{
	igDummy((ImVec2){0, 8.0f});
	igUnindent(8.0f);
}

static void
on_root_enter(struct u_var_root_info *info, void *priv)
{
	struct draw_state *state = (struct draw_state *)priv;
	state->vis_i = 0;
	state->vis_stack[0] = true;

	igBegin(info->name, NULL, 0);
}

static void
on_elem(struct u_var_info *info, void *priv)
{
	const char *name = info->name;
	void *ptr = info->ptr;
	enum u_var_kind kind = info->kind;

	struct draw_state *state = (struct draw_state *)priv;

	bool visible = state->vis_stack[state->vis_i];

	// Handle the visibility stack.
	switch (kind) {
	case U_VAR_KIND_GUI_HEADER_BEGIN: // Increment stack and copy the current visible stack.
		state->vis_i++;
		state->vis_stack[state->vis_i] = visible;
		break;
	case U_VAR_KIND_GUI_HEADER_END: // Decrement the stack.
		state->vis_i--;
		break;
	case U_VAR_KIND_GUI_HEADER: // Always visible.
		on_gui_header(name, state);
		return; // Not doing anything more.
	default: break;
	}

	// Check balanced GUI_HEADER_BEGIN/END pairs
	assert(state->vis_i >= 0 && state->vis_i < MAX_HEADER_NESTING);

	if (!visible) {
		return;
	}

	const float drag_speed = 0.2f;
	const float power = 1.0f;
	ImGuiInputTextFlags i_flags = ImGuiInputTextFlags_None;
	ImGuiInputTextFlags ro_i_flags = ImGuiInputTextFlags_ReadOnly;

	switch (kind) {
	case U_VAR_KIND_BOOL: igCheckbox(name, (bool *)ptr); break;
	case U_VAR_KIND_RGB_F32: on_color_rgb_f32(name, ptr); break;
	case U_VAR_KIND_RGB_U8: on_color_rgb_u8(name, ptr); break;
	case U_VAR_KIND_U8: igDragScalar(name, ImGuiDataType_U8, ptr, drag_speed, NULL, NULL, NULL, power); break;
	case U_VAR_KIND_U16: igDragScalar(name, ImGuiDataType_U16, ptr, drag_speed, NULL, NULL, NULL, power); break;
	case U_VAR_KIND_U64: igDragScalar(name, ImGuiDataType_U64, ptr, drag_speed, NULL, NULL, NULL, power); break;
	case U_VAR_KIND_I32: igInputInt(name, (int *)ptr, 1, 10, i_flags); break;
	case U_VAR_KIND_I64: igInputScalar(name, ImGuiDataType_S64, ptr, NULL, NULL, NULL, i_flags); break;
	case U_VAR_KIND_VEC3_I32: igInputInt3(name, (int *)ptr, i_flags); break;
	case U_VAR_KIND_F32: igInputFloat(name, (float *)ptr, 1, 10, "%+f", i_flags); break;
	case U_VAR_KIND_F64: igInputDouble(name, (double *)ptr, 0.1, 1, "%+f", i_flags); break;
	case U_VAR_KIND_F32_ARR: on_f32_arr(name, ptr); break;
	case U_VAR_KIND_TIMING: on_timing(name, ptr); break;
	case U_VAR_KIND_VEC3_F32: igInputFloat3(name, (float *)ptr, "%+f", i_flags); break;
	case U_VAR_KIND_POSE: on_pose(name, ptr); break;
	case U_VAR_KIND_LOG_LEVEL: igComboStr(name, (int *)ptr, "Trace\0Debug\0Info\0Warn\0Error\0\0", 5); break;
	case U_VAR_KIND_RO_TEXT: igText("%s: '%s'", name, (char *)ptr); break;
	case U_VAR_KIND_RO_FTEXT: igText(ptr ? (char *)ptr : "%s", name); break;
	case U_VAR_KIND_RO_I32: igInputScalar(name, ImGuiDataType_S32, ptr, NULL, NULL, NULL, ro_i_flags); break;
	case U_VAR_KIND_RO_U32: igInputScalar(name, ImGuiDataType_U32, ptr, NULL, NULL, NULL, ro_i_flags); break;
	case U_VAR_KIND_RO_F32: igInputScalar(name, ImGuiDataType_Float, ptr, NULL, NULL, "%+f", ro_i_flags); break;
	case U_VAR_KIND_RO_I64: igInputScalar(name, ImGuiDataType_S64, ptr, NULL, NULL, NULL, ro_i_flags); break;
	case U_VAR_KIND_RO_U64: igInputScalar(name, ImGuiDataType_S64, ptr, NULL, NULL, NULL, ro_i_flags); break;
	case U_VAR_KIND_RO_F64: igInputScalar(name, ImGuiDataType_Double, ptr, NULL, NULL, "%+f", ro_i_flags); break;
	case U_VAR_KIND_RO_VEC3_I32: igInputInt3(name, (int *)ptr, ro_i_flags); break;
	case U_VAR_KIND_RO_VEC3_F32: igInputFloat3(name, (float *)ptr, "%+f", ro_i_flags); break;
	case U_VAR_KIND_RO_QUAT_F32: igInputFloat4(name, (float *)ptr, "%+f", ro_i_flags); break;
	case U_VAR_KIND_RO_FF_VEC3_F32: on_ff_vec3_var(info, state->p); break;
	case U_VAR_KIND_GUI_HEADER: assert(false && "Should be handled before this"); break;
	case U_VAR_KIND_GUI_HEADER_BEGIN: on_gui_header_begin(name, state); break;
	case U_VAR_KIND_GUI_HEADER_END: on_gui_header_end(); break;
	case U_VAR_KIND_SINK_DEBUG: on_sink_debug_var(name, ptr, state); break;
	case U_VAR_KIND_DRAGGABLE_F32: on_draggable_f32_var(name, ptr); break;
	case U_VAR_KIND_BUTTON: on_button_var(name, ptr); break;
	case U_VAR_KIND_COMBO: on_combo_var(name, ptr); break;
	case U_VAR_KIND_DRAGGABLE_U16: on_draggable_u16_var(name, ptr); break;
	case U_VAR_KIND_HISTOGRAM_F32: on_histogram_f32_var(name, ptr); break;
	case U_VAR_KIND_CURVE: on_curve_var(name, ptr); break;
	case U_VAR_KIND_CURVES: on_curves_var(name, ptr); break;
	default: igLabelText(name, "Unknown tag '%i'", kind); break;
	}
}

static void
on_root_exit(struct u_var_root_info *info, void *priv)
{
	struct draw_state *state = (struct draw_state *)priv;
	assert(state->vis_i == 0 && "Unbalanced GUI_HEADER_BEGIN/END pairs");
	state->vis_i = 0;
	state->vis_stack[0] = false;

	igEnd();
}


/*
 *
 * Sink interception.
 *
 */

static void
on_root_enter_sink(struct u_var_root_info *info, void *priv)
{}

static void
on_elem_sink_debug_remove(struct u_var_info *info, void *null_ptr)
{
	void *ptr = info->ptr;
	enum u_var_kind kind = info->kind;

	if (kind != U_VAR_KIND_SINK_DEBUG) {
		return;
	}

	struct u_sink_debug *usd = (struct u_sink_debug *)ptr;
	u_sink_debug_set_sink(usd, NULL);
}

static void
on_root_exit_sink(struct u_var_root_info *info, void *priv)
{}


/*
 *
 * Scene functions.
 *
 */

static void
scene_render(struct gui_scene *scene, struct gui_program *p)
{
	struct debug_scene *ds = (struct debug_scene *)scene;
	struct draw_state state = {p, ds, {0}, 0, false};

	u_var_visit(on_root_enter, on_root_exit, on_elem, &state);
}

static void
scene_destroy(struct gui_scene *scene, struct gui_program *p)
{
	struct debug_scene *ds = (struct debug_scene *)scene;

	// Remove the sink interceptors.
	u_var_visit(on_root_enter_sink, on_root_exit_sink, on_elem_sink_debug_remove, NULL);

	if (ds->xfctx != NULL) {
		xrt_frame_context_destroy_nodes(ds->xfctx);
		ds->xfctx = NULL;
	}

	free(ds);
}


/*
 *
 * 'Exported' functions.
 *
 */

void
gui_scene_debug(struct gui_program *p)
{
	// Only create devices if we have a instance and no system devices.
	if (p->instance != NULL && p->xsysd == NULL) {
		gui_prober_select(p);
	}

	struct debug_scene *ds = U_TYPED_CALLOC(struct debug_scene);

	ds->base.render = scene_render;
	ds->base.destroy = scene_destroy;

	gui_scene_push_front(p, &ds->base);
}
