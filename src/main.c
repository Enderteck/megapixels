#include "main.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <linux/kdev_t.h>
#include <sys/sysmacros.h>
#include <asm/errno.h>
#include <wordexp.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <zbar.h>
#include "gl_util.h"
#include "camera_config.h"
#include "io_pipeline.h"
#include "process_pipeline.h"

#define RENDERDOC

#ifdef RENDERDOC
#include <dlfcn.h>
#include <renderdoc/app.h>
RENDERDOC_API_1_1_2 *rdoc_api = NULL;
#endif

enum user_control { USER_CONTROL_ISO, USER_CONTROL_SHUTTER };

static bool camera_is_initialized = false;
static const struct mp_camera_config *camera = NULL;
static MPCameraMode mode;

static int preview_width = -1;
static int preview_height = -1;

static bool gain_is_manual = false;
static int gain;
static int gain_max;

static bool exposure_is_manual = false;
static int exposure;

static bool has_auto_focus_continuous;
static bool has_auto_focus_start;

static MPProcessPipelineBuffer *current_preview_buffer = NULL;
static int preview_buffer_width = -1;
static int preview_buffer_height = -1;

static cairo_surface_t *status_surface = NULL;
static char last_path[260] = "";

static MPZBarScanResult *zbar_result = NULL;

static int burst_length = 3;

static enum user_control current_control;

// Widgets
GtkWidget *preview;
GtkWidget *error_box;
GtkWidget *error_message;
GtkWidget *main_stack;
GtkWidget *open_last_stack;
GtkWidget *thumb_last;
GtkWidget *process_spinner;
GtkWidget *control_box;
GtkWidget *control_name;
GtkAdjustment *control_slider;
GtkWidget *control_auto;

int
remap(int value, int input_min, int input_max, int output_min, int output_max)
{
	const long long factor = 1000000000;
	long long output_spread = output_max - output_min;
	long long input_spread = input_max - input_min;

	long long zero_value = value - input_min;
	zero_value *= factor;
	long long percentage = zero_value / input_spread;

	long long zero_output = percentage * output_spread / factor;

	long long result = output_min + zero_output;
	return (int)result;
}

static void
update_io_pipeline()
{
	struct mp_io_pipeline_state io_state = {
		.camera = camera,
		.burst_length = burst_length,
		.preview_width = preview_width,
		.preview_height = preview_height,
		.gain_is_manual = gain_is_manual,
		.gain = gain,
		.exposure_is_manual = exposure_is_manual,
		.exposure = exposure,
	};
	mp_io_pipeline_update_state(&io_state);
}

static bool
update_state(const struct mp_main_state *state)
{
	if (!camera_is_initialized) {
		camera_is_initialized = true;
	}

	if (camera == state->camera) {
		mode = state->mode;

		if (!gain_is_manual) {
			gain = state->gain;
		}
		gain_max = state->gain_max;

		if (!exposure_is_manual) {
			exposure = state->exposure;
		}

		has_auto_focus_continuous = state->has_auto_focus_continuous;
		has_auto_focus_start = state->has_auto_focus_start;
	}

	preview_buffer_width = state->image_width;
	preview_buffer_height = state->image_height;

	return false;
}

void
mp_main_update_state(const struct mp_main_state *state)
{
	struct mp_main_state *state_copy = malloc(sizeof(struct mp_main_state));
	*state_copy = *state;

	g_main_context_invoke_full(g_main_context_default(), G_PRIORITY_DEFAULT_IDLE,
				   (GSourceFunc)update_state, state_copy, free);
}

static bool set_zbar_result(MPZBarScanResult *result)
{
	if (zbar_result) {
		for (uint8_t i = 0; i < zbar_result->size; ++i) {
			free(zbar_result->codes[i].data);
		}

		free(zbar_result);
	}

	zbar_result = result;
	gtk_widget_queue_draw(preview);

	return false;
}

void mp_main_set_zbar_result(MPZBarScanResult *result)
{
	g_main_context_invoke_full(g_main_context_default(), G_PRIORITY_DEFAULT_IDLE,
				   (GSourceFunc)set_zbar_result, result, NULL);
}

static bool
set_preview(MPProcessPipelineBuffer *buffer)
{
	if (current_preview_buffer) {
		mp_process_pipeline_buffer_unref(current_preview_buffer);
	}
	current_preview_buffer = buffer;
	gtk_widget_queue_draw(preview);
	return false;
}

void
mp_main_set_preview(MPProcessPipelineBuffer *buffer)
{
	g_main_context_invoke_full(g_main_context_default(), G_PRIORITY_DEFAULT_IDLE,
				   (GSourceFunc)set_preview, buffer, NULL);
}

static void transform_centered(cairo_t *cr, uint32_t dst_width, uint32_t dst_height,
	                       int src_width, int src_height)
{
	cairo_translate(cr, dst_width / 2, dst_height / 2);

	double scale = MIN(dst_width / (double)src_width, dst_height / (double)src_height);
	cairo_scale(cr, scale, scale);

	cairo_translate(cr, -src_width / 2, -src_height / 2);
}

void
draw_surface_scaled_centered(cairo_t *cr, uint32_t dst_width, uint32_t dst_height,
			     cairo_surface_t *surface)
{
	cairo_save(cr);

	int width = cairo_image_surface_get_width(surface);
	int height = cairo_image_surface_get_height(surface);
	transform_centered(cr, dst_width, dst_height, width, height);

	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
}

struct capture_completed_args {
	cairo_surface_t *thumb;
	char *fname;
};

static bool
capture_completed(struct capture_completed_args *args)
{
	strncpy(last_path, args->fname, 259);

	// gtk_image_set_from_surface(GTK_IMAGE(thumb_last), args->thumb);

	gtk_spinner_stop(GTK_SPINNER(process_spinner));
	gtk_stack_set_visible_child(GTK_STACK(open_last_stack), thumb_last);

	cairo_surface_destroy(args->thumb);
	g_free(args->fname);

	return false;
}

void
mp_main_capture_completed(cairo_surface_t *thumb, const char *fname)
{
	struct capture_completed_args *args = malloc(sizeof(struct capture_completed_args));
	args->thumb = thumb;
	args->fname = g_strdup(fname);
	g_main_context_invoke_full(g_main_context_default(), G_PRIORITY_DEFAULT_IDLE,
				   (GSourceFunc)capture_completed, args, free);
}

static void
draw_controls()
{
	// cairo_t *cr;
	char iso[6];
	int temp;
	char shutterangle[6];

	if (exposure_is_manual) {
		temp = (int)((float)exposure / (float)camera->capture_mode.height *
			     360);
		sprintf(shutterangle, "%d\u00b0", temp);
	} else {
		sprintf(shutterangle, "auto");
	}

	if (gain_is_manual) {
		temp = remap(gain - 1, 0, gain_max, camera->iso_min,
			     camera->iso_max);
		sprintf(iso, "%d", temp);
	} else {
		sprintf(iso, "auto");
	}

	if (status_surface)
		cairo_surface_destroy(status_surface);

	// Make a service to show status of controls, 32px high
	// if (gtk_widget_get_window(preview) == NULL) {
	// 	return;
	// }
	// status_surface =
	// 	gdk_window_create_similar_surface(gtk_widget_get_window(preview),
	// 					  CAIRO_CONTENT_COLOR_ALPHA,
	// 					  preview_width, 32);

	// cr = cairo_create(status_surface);
	// cairo_set_source_rgba(cr, 0, 0, 0, 0.0);
	// cairo_paint(cr);

	// // Draw the outlines for the headings
	// cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
	// 		       CAIRO_FONT_WEIGHT_BOLD);
	// cairo_set_font_size(cr, 9);
	// cairo_set_source_rgba(cr, 0, 0, 0, 1);

	// cairo_move_to(cr, 16, 16);
	// cairo_text_path(cr, "ISO");
	// cairo_stroke(cr);

	// cairo_move_to(cr, 60, 16);
	// cairo_text_path(cr, "Shutter");
	// cairo_stroke(cr);

	// // Draw the fill for the headings
	// cairo_set_source_rgba(cr, 1, 1, 1, 1);
	// cairo_move_to(cr, 16, 16);
	// cairo_show_text(cr, "ISO");
	// cairo_move_to(cr, 60, 16);
	// cairo_show_text(cr, "Shutter");

	// // Draw the outlines for the values
	// cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
	// 		       CAIRO_FONT_WEIGHT_NORMAL);
	// cairo_set_font_size(cr, 11);
	// cairo_set_source_rgba(cr, 0, 0, 0, 1);

	// cairo_move_to(cr, 16, 26);
	// cairo_text_path(cr, iso);
	// cairo_stroke(cr);

	// cairo_move_to(cr, 60, 26);
	// cairo_text_path(cr, shutterangle);
	// cairo_stroke(cr);

	// // Draw the fill for the values
	// cairo_set_source_rgba(cr, 1, 1, 1, 1);
	// cairo_move_to(cr, 16, 26);
	// cairo_show_text(cr, iso);
	// cairo_move_to(cr, 60, 26);
	// cairo_show_text(cr, shutterangle);

	// cairo_destroy(cr);

	// gtk_widget_queue_draw_area(preview, 0, 0, preview_width, 32);
}

static GLuint blit_program;
static GLuint blit_uniform_texture;
static GLuint quad;

static void
preview_realize(GtkGLArea *area)
{
	gtk_gl_area_make_current(area);

	if (gtk_gl_area_get_error(area) != NULL) {
		return;
	}

	// Make a VAO for OpenGL
	if (!gtk_gl_area_get_use_es(area)) {
		GLuint vao;
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
		check_gl();
	}

	GLuint blit_shaders[] = {
		gl_util_load_shader("/org/postmarketos/Megapixels/blit.vert", GL_VERTEX_SHADER, NULL, 0),
		gl_util_load_shader("/org/postmarketos/Megapixels/blit.frag", GL_FRAGMENT_SHADER, NULL, 0),
	};

	blit_program = gl_util_link_program(blit_shaders, 2);
	glBindAttribLocation(blit_program, GL_UTIL_VERTEX_ATTRIBUTE, "vert");
	glBindAttribLocation(blit_program, GL_UTIL_TEX_COORD_ATTRIBUTE, "tex_coord");
	check_gl();

	blit_uniform_texture = glGetUniformLocation(blit_program, "texture");

	quad = gl_util_new_quad();
}

static gboolean
preview_draw(GtkGLArea *area, GdkGLContext *ctx, gpointer data)
{
	if (gtk_gl_area_get_error(area) != NULL) {
		return FALSE;
	}

	if (!camera_is_initialized) {
		return FALSE;
	}

#ifdef RENDERDOC
	if (rdoc_api) rdoc_api->StartFrameCapture(NULL, NULL);
#endif

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	double ratio = preview_buffer_height / (double)preview_buffer_width;
	glViewport(0,
		   preview_height - preview_width * ratio,
		   preview_width,
		   preview_width * ratio);

	if (current_preview_buffer) {
		glUseProgram(blit_program);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, mp_process_pipeline_buffer_get_texture_id(current_preview_buffer));
		glUniform1i(blit_uniform_texture, 0);
		check_gl();

		gl_util_bind_quad(quad);
		gl_util_draw_quad(quad);
	}

	/*
	// Clear preview area with black
	cairo_paint(cr);

	if (surface) {
		// Draw camera preview
		cairo_save(cr);

		int width = cairo_image_surface_get_width(surface);
		int height = cairo_image_surface_get_height(surface);
		transform_centered(cr, preview_width, preview_height, width, height);

		cairo_set_source_surface(cr, surface, 0, 0);
		cairo_paint(cr);

		// Draw zbar image
		if (zbar_result) {
			for (uint8_t i = 0; i < zbar_result->size; ++i) {
				MPZBarCode *code = &zbar_result->codes[i];

				cairo_set_line_width(cr, 3.0);
				cairo_set_source_rgba(cr, 0, 0.5, 1, 0.75);
				cairo_new_path(cr);
				cairo_move_to(cr, code->bounds_x[0], code->bounds_y[0]);
				for (uint8_t i = 0; i < 4; ++i) {
					cairo_line_to(cr, code->bounds_x[i], code->bounds_y[i]);
				}
				cairo_close_path(cr);
				cairo_stroke(cr);

				cairo_save(cr);
				cairo_translate(cr, code->bounds_x[0], code->bounds_y[0]);
				cairo_show_text(cr, code->data);
				cairo_restore(cr);
			}
		}

		cairo_restore(cr);
	}

	// Draw control overlay
	cairo_set_source_surface(cr, status_surface, 0, 0);
	cairo_paint(cr);
	*/

	glFlush();

#ifdef RENDERDOC
	if(rdoc_api) rdoc_api->EndFrameCapture(NULL, NULL);
#endif

	return FALSE;
}

static gboolean
preview_resize(GtkWidget *widget, int width, int height, gpointer data)
{
	if (preview_width != width || preview_height != height) {
		preview_width = width;
		preview_height = height;
		update_io_pipeline();
	}

	draw_controls();

	return TRUE;
}

void
on_open_last_clicked(GtkWidget *widget, gpointer user_data)
{
	char uri[275];
	GError *error = NULL;

	if (strlen(last_path) == 0) {
		return;
	}
	sprintf(uri, "file://%s", last_path);
	if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
		g_printerr("Could not launch image viewer for '%s': %s\n", uri, error->message);
	}
}

void
on_open_directory_clicked(GtkWidget *widget, gpointer user_data)
{
	char uri[270];
	GError *error = NULL;
	sprintf(uri, "file://%s", g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
	if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
		g_printerr("Could not launch image viewer: %s\n", error->message);
	}
}

void
run_capture_action(GSimpleAction *action, GVariant *param, gpointer user_data)
{
	gtk_spinner_start(GTK_SPINNER(process_spinner));
	gtk_stack_set_visible_child(GTK_STACK(open_last_stack), process_spinner);
	mp_io_pipeline_capture();
}

void
run_quit_action(GSimpleAction *action, GVariant *param, GApplication *app)
{
	g_application_quit(app);
}

// static bool
// check_point_inside_bounds(int x, int y, int *bounds_x, int *bounds_y)
// {
// 	bool right = false, left = false, top = false, bottom = false;

// 	for (int i = 0; i < 4; ++i) {
// 		if (x <= bounds_x[i])
// 			left = true;
// 		if (x >= bounds_x[i])
// 			right = true;
// 		if (y <= bounds_y[i])
// 			top = true;
// 		if (y >= bounds_y[i])
// 			bottom = true;
// 	}

// 	return right && left && top && bottom;
// }

// static void
// on_zbar_code_tapped(GtkWidget *widget, const MPZBarCode *code)
// {
// 	GtkWidget *dialog;
// 	GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
// 	bool data_is_url = g_uri_is_valid(
// 		code->data, G_URI_FLAGS_PARSE_RELAXED, NULL);

// 	char* data = strdup(code->data);

// 	if (data_is_url) {
// 		dialog = gtk_message_dialog_new(
// 			GTK_WINDOW(gtk_widget_get_toplevel(widget)),
// 			flags,
// 			GTK_MESSAGE_QUESTION,
// 			GTK_BUTTONS_NONE,
// 			"Found a URL '%s' encoded in a %s code.",
// 			code->data,
// 			code->type);
// 		gtk_dialog_add_buttons(
// 			GTK_DIALOG(dialog),
// 			"_Open URL",
// 			GTK_RESPONSE_YES,
// 			NULL);
// 	} else {
// 		dialog = gtk_message_dialog_new(
// 			GTK_WINDOW(gtk_widget_get_toplevel(widget)),
// 			flags,
// 			GTK_MESSAGE_QUESTION,
// 			GTK_BUTTONS_NONE,
// 			"Found '%s' encoded in a %s code.",
// 			code->data,
// 			code->type);
// 	}
// 	gtk_dialog_add_buttons(
// 		GTK_DIALOG(dialog),
// 		"_Copy",
// 		GTK_RESPONSE_ACCEPT,
// 		"_Cancel",
// 		GTK_RESPONSE_CANCEL,
// 		NULL);

// 	int result = gtk_dialog_run(GTK_DIALOG(dialog));

// 	GError *error = NULL;
// 	switch (result) {
// 		case GTK_RESPONSE_YES:
// 			if (!g_app_info_launch_default_for_uri(data,
// 							       NULL, &error)) {
// 				g_printerr("Could not launch application: %s\n",
// 					   error->message);
// 			}
// 		case GTK_RESPONSE_ACCEPT:
// 			gtk_clipboard_set_text(
// 				gtk_clipboard_get(GDK_SELECTION_PRIMARY),
// 				data, -1);
// 		case GTK_RESPONSE_CANCEL:
// 			break;
// 		default:
// 			g_printerr("Wrong dialog result: %d\n", result);
// 	}
// 	gtk_widget_destroy(dialog);
// }

void
preview_pressed(GtkGestureClick *gesture, int n_press, double x, double y)
{
	// Handle taps on the controls
	// if (event->y < 32) {
	// 	if (gtk_widget_is_visible(control_box)) {
	// 		gtk_widget_hide(control_box);
	// 		return;
	// 	} else {
	// 		gtk_widget_show(control_box);
	// 	}

	// 	if (event->x < 60) {
	// 		// ISO
	// 		current_control = USER_CONTROL_ISO;
	// 		gtk_label_set_text(GTK_LABEL(control_name), "ISO");
	// 		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control_auto),
	// 					     !gain_is_manual);
	// 		gtk_adjustment_set_lower(control_slider, 0.0);
	// 		gtk_adjustment_set_upper(control_slider, (float)gain_max);
	// 		gtk_adjustment_set_value(control_slider, (double)gain);

	// 	} else if (event->x > 60 && event->x < 120) {
	// 		// Shutter angle
	// 		current_control = USER_CONTROL_SHUTTER;
	// 		gtk_label_set_text(GTK_LABEL(control_name), "Shutter");
	// 		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control_auto),
	// 					     !exposure_is_manual);
	// 		gtk_adjustment_set_lower(control_slider, 1.0);
	// 		gtk_adjustment_set_upper(control_slider, 360.0);
	// 		gtk_adjustment_set_value(control_slider, (double)exposure);
	// 	}

	// 	return;
	// }

	// Tapped zbar result
	// if (zbar_result) {
	// 	// Transform the event coordinates to the image
	// 	int width = cairo_image_surface_get_width(surface);
	// 	int height = cairo_image_surface_get_height(surface);
	// 	double scale = MIN(preview_width / (double)width, preview_height / (double)height);
	// 	int x = (event->x - preview_width / 2) / scale + width / 2;
	// 	int y = (event->y - preview_height / 2) / scale + height / 2;

	// 	for (uint8_t i = 0; i < zbar_result->size; ++i) {
	// 		MPZBarCode *code = &zbar_result->codes[i];

	// 		if (check_point_inside_bounds(x, y, code->bounds_x, code->bounds_y)) {
	// 			on_zbar_code_tapped(widget, code);
	// 			return;
	// 		}
	// 	}
	// }

	// Tapped preview image itself, try focussing
	if (has_auto_focus_start) {
		mp_io_pipeline_focus();
	}
}

void
on_error_close_clicked(GtkWidget *widget, gpointer user_data)
{
	gtk_widget_hide(error_box);
}

void
on_camera_switch_clicked(GtkWidget *widget, gpointer user_data)
{
	size_t next_index = camera->index + 1;
	const struct mp_camera_config *next_camera =
		mp_get_camera_config(next_index);

	if (!next_camera) {
		next_index = 0;
		next_camera = mp_get_camera_config(next_index);
	}

	camera = next_camera;
	update_io_pipeline();
}

void
on_settings_btn_clicked(GtkWidget *widget, gpointer user_data)
{
	gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "settings");
}

void
on_back_clicked(GtkWidget *widget, gpointer user_data)
{
	gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "main");
}

void
on_control_auto_toggled(GtkToggleButton *widget, gpointer user_data)
{
	bool is_manual = gtk_toggle_button_get_active(widget) ? false : true;
	bool has_changed;

	switch (current_control) {
	case USER_CONTROL_ISO:
		if (gain_is_manual != is_manual) {
			gain_is_manual = is_manual;
			has_changed = true;
		}
		break;
	case USER_CONTROL_SHUTTER:
		if (exposure_is_manual != is_manual) {
			exposure_is_manual = is_manual;
			has_changed = true;
		}
		break;
	}

	if (has_changed) {
		// The slider might have been moved while Auto mode is active. When entering
		// Manual mode, first read the slider value to sync with those changes.
		double value = gtk_adjustment_get_value(control_slider);
		switch (current_control) {
		case USER_CONTROL_ISO:
			if (value != gain) {
				gain = (int)value;
			}
			break;
		case USER_CONTROL_SHUTTER: {
			// So far all sensors use exposure time in number of sensor rows
			int new_exposure =
				(int)(value / 360.0 * camera->capture_mode.height);
			if (new_exposure != exposure) {
				exposure = new_exposure;
			}
			break;
		}
		}

		update_io_pipeline();
		draw_controls();
	}
}

void
on_control_slider_changed(GtkAdjustment *widget, gpointer user_data)
{
	double value = gtk_adjustment_get_value(widget);

	bool has_changed = false;
	switch (current_control) {
	case USER_CONTROL_ISO:
		if (value != gain) {
			gain = (int)value;
			has_changed = true;
		}
		break;
	case USER_CONTROL_SHUTTER: {
		// So far all sensors use exposure time in number of sensor rows
		int new_exposure =
			(int)(value / 360.0 * camera->capture_mode.height);
		if (new_exposure != exposure) {
			exposure = new_exposure;
			has_changed = true;
		}
		break;
	}
	}

	if (has_changed) {
		update_io_pipeline();
		draw_controls();
	}
}

static void
on_realize(GtkWidget *window, gpointer *data)
{
	GtkNative *native = gtk_widget_get_native(window);
	mp_process_pipeline_init_gl(gtk_native_get_surface(native));

	camera = mp_get_camera_config(0);
	update_io_pipeline();
}

static void
activate(GtkApplication *app, gpointer data)
{
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
		     TRUE, NULL);

	assert(g_resources_lookup_data("/org/postmarketos/Megapixels/camera.ui", 0, NULL) != NULL);

	GtkBuilder *builder = gtk_builder_new_from_resource(
		"/org/postmarketos/Megapixels/camera.ui");

	GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "window"));
	GtkWidget *switch_btn =
		GTK_WIDGET(gtk_builder_get_object(builder, "switch_camera"));
	GtkWidget *settings_btn =
		GTK_WIDGET(gtk_builder_get_object(builder, "settings"));
	GtkWidget *settings_back =
		GTK_WIDGET(gtk_builder_get_object(builder, "settings_back"));
	GtkWidget *error_close =
		GTK_WIDGET(gtk_builder_get_object(builder, "error_close"));
	GtkWidget *open_last =
		GTK_WIDGET(gtk_builder_get_object(builder, "open_last"));
	GtkWidget *open_directory =
		GTK_WIDGET(gtk_builder_get_object(builder, "open_directory"));
	preview = GTK_WIDGET(gtk_builder_get_object(builder, "preview"));
	error_box = GTK_WIDGET(gtk_builder_get_object(builder, "error_box"));
	error_message = GTK_WIDGET(gtk_builder_get_object(builder, "error_message"));
	main_stack = GTK_WIDGET(gtk_builder_get_object(builder, "main_stack"));
	open_last_stack = GTK_WIDGET(gtk_builder_get_object(builder, "open_last_stack"));
	thumb_last = GTK_WIDGET(gtk_builder_get_object(builder, "thumb_last"));
	process_spinner = GTK_WIDGET(gtk_builder_get_object(builder, "process_spinner"));
	control_box = GTK_WIDGET(gtk_builder_get_object(builder, "control_box"));
	control_name = GTK_WIDGET(gtk_builder_get_object(builder, "control_name"));
	control_slider =
		GTK_ADJUSTMENT(gtk_builder_get_object(builder, "control_adj"));
	control_auto = GTK_WIDGET(gtk_builder_get_object(builder, "control_auto"));
	g_signal_connect(window, "realize", G_CALLBACK(on_realize), NULL);
	g_signal_connect(error_close, "clicked", G_CALLBACK(on_error_close_clicked),
			 NULL);
	g_signal_connect(switch_btn, "clicked", G_CALLBACK(on_camera_switch_clicked),
			 NULL);
	g_signal_connect(settings_btn, "clicked",
			 G_CALLBACK(on_settings_btn_clicked), NULL);
	g_signal_connect(settings_back, "clicked", G_CALLBACK(on_back_clicked),
			 NULL);
	g_signal_connect(open_last, "clicked", G_CALLBACK(on_open_last_clicked),
			 NULL);
	g_signal_connect(open_directory, "clicked",
			 G_CALLBACK(on_open_directory_clicked), NULL);

	g_signal_connect(preview, "realize", G_CALLBACK(preview_realize), NULL);
	g_signal_connect(preview, "render", G_CALLBACK(preview_draw), NULL);
	g_signal_connect(preview, "resize", G_CALLBACK(preview_resize), NULL);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(preview_pressed), NULL);
	gtk_widget_add_controller(preview, GTK_EVENT_CONTROLLER(click));

	g_signal_connect(control_auto, "toggled",
			 G_CALLBACK(on_control_auto_toggled), NULL);
	g_signal_connect(control_slider, "value-changed",
			 G_CALLBACK(on_control_slider_changed), NULL);

	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_resource(
		provider, "/org/postmarketos/Megapixels/camera.css");
	GtkStyleContext *context = gtk_widget_get_style_context(error_box);
	gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
				       GTK_STYLE_PROVIDER_PRIORITY_USER);
	context = gtk_widget_get_style_context(control_box);
	gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
				       GTK_STYLE_PROVIDER_PRIORITY_USER);

	// Setup capture action
	GSimpleAction *capture_action = g_simple_action_new("capture", NULL);
	g_signal_connect(capture_action, "activate", G_CALLBACK(run_capture_action), NULL);
	g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(capture_action));

	const char *capture_accels[] = { "space", NULL };
	gtk_application_set_accels_for_action(app, "app.capture", capture_accels);

	// Setup quit action
	GSimpleAction *quit_action = g_simple_action_new("quit", NULL);
	g_signal_connect(quit_action, "activate", G_CALLBACK(run_quit_action), app);
	g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit_action));

	const char *quit_accels[] = { "<Ctrl>q", "<Ctrl>w", NULL };
	gtk_application_set_accels_for_action(app, "app.quit", quit_accels);

	mp_io_pipeline_start();

	gtk_application_add_window(app, GTK_WINDOW(window));
	gtk_widget_show(window);
}

static void
shutdown(GApplication *app, gpointer data)
{
	// Only do cleanup in development, let the OS clean up otherwise
#ifdef DEBUG
	mp_io_pipeline_stop();
#endif
}

int
main(int argc, char *argv[])
{
#ifdef RENDERDOC
	{
		void *mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
		if (mod)
		{
		    pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
		    int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void **)&rdoc_api);
		    assert(ret == 1);
		}
		else
		{
			printf("Renderdoc not found\n");
		}
	}
#endif

	if (!mp_load_config())
		return 1;

	setenv("LC_NUMERIC", "C", 1);

	GtkApplication *app = gtk_application_new("org.postmarketos.Megapixels", 0);

	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(app, "shutdown", G_CALLBACK(shutdown), NULL);

	g_application_run(G_APPLICATION(app), argc, argv);

	return 0;
}
