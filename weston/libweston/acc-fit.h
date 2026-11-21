/* Logical content in the window interior, excluding client-side decoration.
 * Use the identical integer rectangle for GL and input; size differences of
 * one physical pixel are inevitable at non-integral scales. */
#pragma once
enum acc_window_mode { ACC_FREE, ACC_FIX, ACC_RATIO };
static inline enum acc_window_mode
acc_window_mode(void)
{
	const char *v = getenv("ACC_WINDOW_MODE");
	if (v && !strcmp(v, "ratio")) return ACC_RATIO;
	if (v && !strcmp(v, "fix")) return ACC_FIX;
	return ACC_FREE;
}

static inline struct weston_geometry
acc_fit(enum acc_window_mode mode, int sw, int sh, int w, int h)
{
	int dw = w, dh = h;
	if (mode == ACC_FIX) {
		dw = MIN(dw, sw);
		dh = MIN(dh, sh);
	}
	if ((int64_t)dw * sh > (int64_t)dh * sw)
		dw = MAX(1, (int)((int64_t)dh * sw / sh));
	else
		dh = MAX(1, (int)((int64_t)dw * sh / sw));
	return (struct weston_geometry){(w-dw)/2, (h-dh)/2, dw, dh};
}
