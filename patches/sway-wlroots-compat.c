// Shims for functions that Droidian's wlroots (hwcomposer branch) lacks but Sway 1.6.1 calls.
#include <math.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_layer_shell_v1.h>
void wlr_box_rotated_bounds(struct wlr_box *dest, const struct wlr_box *box, float rotation) {
	if (rotation == 0) { *dest = *box; return; }
	double ox = box->x + (double)box->width / 2, oy = box->y + (double)box->height / 2;
	double c = fabs(cos(rotation)), s = fabs(sin(rotation));
	double x1 = ox + (box->x - ox) * c + (box->y - oy) * s;
	double x2 = ox + (box->x + box->width - ox) * c + (box->y + box->height - oy) * s;
	double y1 = oy + (box->x - ox) * s + (box->y - oy) * c;
	double y2 = oy + (box->x + box->width - ox) * s + (box->y + box->height - oy) * c;
	dest->x = floor(fmin(x1, x2)); dest->width = ceil(fmax(x1, x2) - fmin(x1, x2));
	dest->y = floor(fmin(y1, y2)); dest->height = ceil(fmax(y1, y2) - fmin(y1, y2));
}
void wlr_layer_surface_v1_close(struct wlr_layer_surface_v1 *surface) { wlr_layer_surface_v1_destroy(surface); }
