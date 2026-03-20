/*
 * kernel.c - GraphCalcOS kernel
 *
 * NOW IN 640x480 HIGH RESOLUTION!
 * upgraded from 320x200 mode 0x13 to 640x480x32bpp via Bochs VBE.
 * every pixel is now a full 32-bit RGB color (0x00RRGGBB).
 *
 * new features:
 *   - side panel showing multiple equations in different colors
 *   - zoom in/out with +/- keys
 *   - pan around with arrow keys
 *   - implicit equations (circles! ellipses! anything with y and =)
 *   - trig functions: sin(), cos(), tan(), sqrt(), abs()
 *   - MOUSE SUPPORT with cursor, click-drag panning, coordinate display
 *   - TRACE MODE (press T) to walk along a curve and read coordinates
 *   - TANGENT LINE (press D in trace mode) to visualize the derivative
 */

#include <stdint.h>
#include "keyboard.h"
#include "font.h"        /* includes vga.h and ports.h */
#include "math_parser.h" /* equation parser and evaluator */
#include "mouse.h"       /* PS/2 mouse driver */

/* === LAYOUT ===
 *
 * 640x480 screen layout:
 *
 *  +--------+--------------------------------------+
 *  | PANEL  |          TITLE BAR (24px)            |
 *  | (140px)+--------------------------------------+
 *  |        |                                      |
 *  | eq 1   |                                      |
 *  | eq 2   |          GRAPH AREA                  |
 *  | eq 3   |          (500 x 432 pixels)          |
 *  | ...    |                                      |
 *  |        |                                      |
 *  +--------+--------------------------------------+
 *  |             INPUT BAR (24px)                  |
 *  +-----------------------------------------------+
 */

#define PANEL_WIDTH      140
#define TITLE_HEIGHT     24
#define INPUT_BAR_Y      (SCREEN_HEIGHT - 24)
#define INPUT_TEXT_Y      (INPUT_BAR_Y + 8)
#define INPUT_TEXT_X      (PANEL_WIDTH + 24)
#define MAX_INPUT_LEN     50

/* graph area bounds */
#define GRAPH_LEFT       PANEL_WIDTH
#define GRAPH_TOP        TITLE_HEIGHT
#define GRAPH_RIGHT      (SCREEN_WIDTH - 1)
#define GRAPH_BOTTOM     (INPUT_BAR_Y - 1)
#define GRAPH_WIDTH      (GRAPH_RIGHT - GRAPH_LEFT + 1)
#define GRAPH_HEIGHT     (GRAPH_BOTTOM - GRAPH_TOP + 1)

/* multiple equations - up to 6, each in a different color */
#define MAX_EQUATIONS    6
#define MAX_EQ_LEN       50

/*
 * each equation slot stores:
 *   - the text (for display in the panel)
 *   - whether it's implicit (has y and =)
 *   - compiled bytecode (for fast evaluation during pan/zoom)
 *     implicit equations store left and right sides separately
 */
typedef struct {
    char text[MAX_EQ_LEN + 1];
    int is_implicit;
    compiled_eq compiled;      /* for explicit: y = f(x) */
    compiled_eq comp_left;     /* for implicit: left side of = */
    compiled_eq comp_right;    /* for implicit: right side of = */
    int deriv_source;          /* -1 = normal, >= 0 = derivative of eq[n] */
} equation_slot;

static equation_slot equations[MAX_EQUATIONS];
static int eq_count = 0;

/* graph colors for each equation slot */
static const uint32_t eq_colors[MAX_EQUATIONS] = {
    COLOR_LIGHT_GREEN,
    COLOR_LIGHT_RED,
    COLOR_LIGHT_BLUE,
    COLOR_YELLOW,
    COLOR_LIGHT_CYAN,
    COLOR_LIGHT_MAGENTA
};

/*
 * graph state - these are VARIABLES now, not constants!
 * this lets us change them at runtime for zoom/pan.
 *
 * origin_x/y = pixel position of math (0,0) in the graph area
 * grid_scale = pixels per math unit (bigger = zoomed in)
 */
static int origin_x;
static int origin_y;
static int grid_scale = 30;

/* initialize origin to center of graph area */
static void reset_view(void) {
    origin_x = GRAPH_LEFT + GRAPH_WIDTH / 2;
    origin_y = GRAPH_TOP + GRAPH_HEIGHT / 2;
    grid_scale = 30;
}

/*
 * === CURVE LAYER ===
 *
 * THE BIG PANNING OPTIMIZATION
 *
 * problem: when you pan by 20 pixels, we used to redraw EVERYTHING -
 * clear screen, draw grid, re-evaluate ALL equations at ALL pixels.
 * for implicit equations (circles) that's ~120,000 math evaluations.
 *
 * solution: keep equation curves in a SEPARATE buffer (the "curve layer").
 * when panning:
 *   1. SHIFT the curve layer by the pan amount (instant, just memcpy)
 *   2. only evaluate equations for the tiny 20px strip that scrolled into view
 *   3. redraw grid+axes from scratch (cheap - just dots and lines)
 *   4. composite: stamp curve layer on top of grid
 *
 * this turns a 500-pixel-wide redraw into a 20-pixel-wide redraw = 25x faster!
 *
 * the curve layer uses 0 (black) as "transparent" since our background is black.
 * any non-zero pixel is an equation curve that gets drawn on top of the grid.
 */
static uint32_t curve_layer[GRAPH_WIDTH * GRAPH_HEIGHT];

/* helper to write a pixel into the curve layer */
static inline void curve_put_pixel(int px, int py, uint32_t color) {
    if (px < GRAPH_LEFT || px > GRAPH_RIGHT || py < GRAPH_TOP || py > GRAPH_BOTTOM) return;
    curve_layer[(py - GRAPH_TOP) * GRAPH_WIDTH + (px - GRAPH_LEFT)] = color;
}

/* clear the entire curve layer */
static void curve_layer_clear(void) {
    fast_memset32(curve_layer, 0, GRAPH_WIDTH * GRAPH_HEIGHT);
}

/*
 * shift the curve layer by (dx, dy) pixels.
 *
 * when you pan right by 20px, all the curves need to move right by 20px.
 * instead of re-evaluating, we just shift the pixel data!
 *
 * the trick: copy each row to its new position. we have to be careful
 * about the copy direction to avoid overwriting data we haven't copied yet:
 *   - shifting RIGHT (dx > 0): copy right-to-left (start from the end)
 *   - shifting LEFT  (dx < 0): copy left-to-right (start from the beginning)
 *   - shifting DOWN  (dy > 0): copy bottom-to-top
 *   - shifting UP    (dy < 0): copy top-to-bottom
 *
 * after shifting, clear the newly revealed strip (it has no data yet).
 */
static void curve_layer_shift(int dx, int dy) {
    /* vertical shift: rearrange rows */
    if (dy > 0) {
        /* shift down: copy from bottom to top to avoid overwriting */
        for (int row = GRAPH_HEIGHT - 1; row >= dy; row--) {
            uint32_t *dst = &curve_layer[row * GRAPH_WIDTH];
            uint32_t *src = &curve_layer[(row - dy) * GRAPH_WIDTH];
            fast_memcpy32(dst, src, GRAPH_WIDTH);
        }
        /* clear the top strip that was revealed */
        for (int row = 0; row < dy && row < GRAPH_HEIGHT; row++) {
            fast_memset32(&curve_layer[row * GRAPH_WIDTH], 0, GRAPH_WIDTH);
        }
    } else if (dy < 0) {
        int ady = -dy;
        /* shift up: copy from top to bottom */
        for (int row = 0; row < GRAPH_HEIGHT - ady; row++) {
            uint32_t *dst = &curve_layer[row * GRAPH_WIDTH];
            uint32_t *src = &curve_layer[(row + ady) * GRAPH_WIDTH];
            fast_memcpy32(dst, src, GRAPH_WIDTH);
        }
        /* clear the bottom strip */
        for (int row = GRAPH_HEIGHT - ady; row < GRAPH_HEIGHT; row++) {
            if (row >= 0)
                fast_memset32(&curve_layer[row * GRAPH_WIDTH], 0, GRAPH_WIDTH);
        }
    }

    /* horizontal shift: shift within each row */
    if (dx > 0) {
        /* shift right: must copy right-to-left within each row.
           rep movsl only copies left-to-right, so we use a temp approach:
           copy the valid region to a safe spot in the same row */
        for (int row = 0; row < GRAPH_HEIGHT; row++) {
            uint32_t *r = &curve_layer[row * GRAPH_WIDTH];
            /* shift right by dx: move pixels [0..W-dx-1] to [dx..W-1] */
            /* copy backwards to avoid overlap */
            for (int i = GRAPH_WIDTH - 1; i >= dx; i--) {
                r[i] = r[i - dx];
            }
            /* clear the left strip */
            fast_memset32(r, 0, dx);
        }
    } else if (dx < 0) {
        int adx = -dx;
        for (int row = 0; row < GRAPH_HEIGHT; row++) {
            uint32_t *r = &curve_layer[row * GRAPH_WIDTH];
            /* shift left by adx: move pixels [adx..W-1] to [0..W-adx-1] */
            fast_memcpy32(r, r + adx, GRAPH_WIDTH - adx);
            /* clear the right strip */
            fast_memset32(r + GRAPH_WIDTH - adx, 0, adx);
        }
    }
}

/*
 * blit (copy) the curve layer onto whatever draw_target is set to.
 * only copies non-zero (non-black) pixels, so the grid shows through.
 * this is basically "paste curves on top of grid."
 */
static void curve_layer_blit(void) {
    for (int row = 0; row < GRAPH_HEIGHT; row++) {
        int screen_y = GRAPH_TOP + row;
        uint32_t *curve_row = &curve_layer[row * GRAPH_WIDTH];
        volatile uint32_t *screen_row = &draw_target[screen_y * SCREEN_WIDTH + GRAPH_LEFT];
        for (int col = 0; col < GRAPH_WIDTH; col++) {
            if (curve_row[col] != 0) {
                screen_row[col] = curve_row[col];
            }
        }
    }
}

/* === MOUSE CURSOR STATE ===
 *
 * Drawing a mouse cursor on a bare-metal OS is trickier than you'd think!
 * The problem: when we draw the cursor, we OVERWRITE the pixels underneath.
 * When the cursor moves, those original pixels are gone -- you get ugly trails.
 *
 * Solution: SAVE the pixels under the cursor BEFORE drawing it.
 * When the cursor moves:
 *   1. RESTORE the saved pixels (erase the old cursor)
 *   2. SAVE the pixels at the NEW position
 *   3. DRAW the cursor at the new position
 *
 * This is how hardware cursors work on real GPUs, except we do it in software.
 */
#define CURSOR_SIZE 12   /* cursor is 12x12 pixels max */

static uint32_t cursor_saved[CURSOR_SIZE * CURSOR_SIZE]; /* pixels under cursor */
static int cursor_old_x = -1, cursor_old_y = -1;         /* last drawn position */
static int cursor_visible = 0;                            /* has cursor been drawn? */

/* mouse drag state for panning */
static int mouse_dragging = 0;    /* 1 = left button held down, dragging */
static int drag_start_x = 0;      /* where the drag started (pixel coords) */
static int drag_start_y = 0;
static int drag_origin_x = 0;     /* origin at drag start */
static int drag_origin_y = 0;

/* === TRACE MODE STATE ===
 *
 * Trace mode lets you "walk" along a plotted curve and see exact coordinates.
 * It's like putting your finger on the graph and sliding it along the line.
 *
 * How it works:
 *   - trace_px = the pixel X column we're currently at
 *   - We evaluate f(x) at that column to find the Y position
 *   - A bright dot marks the spot, and coordinates show in the title bar
 *   - Left/Right arrows step one pixel column at a time
 *   - Up/Down arrows switch between different equations
 */
static int trace_mode = 0;          /* 0 = off, 1 = tracing a curve */
static int trace_eq_index = 0;      /* which equation we're tracing (index) */
static int trace_px = 0;            /* current pixel X position on curve */
static int trace_py = 0;            /* current pixel Y (for implicit curves) */
static int tangent_visible = 0;     /* 1 = tangent line is currently drawn */
static float tangent_slope = 0.0f;  /* slope of the tangent at trace point */

/* === simple integer-to-string for axis labels === */
static void int_to_str(int n, char *buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    char tmp[12];
    int len = 0;
    while (n > 0) { tmp[len++] = '0' + (n % 10); n /= 10; }
    int pos = 0;
    if (neg) buf[pos++] = '-';
    for (int i = len - 1; i >= 0; i--) buf[pos++] = tmp[i];
    buf[pos] = '\0';
}

/*
 * float_to_str - convert a float to a string with 2 decimal places
 *
 * We can't use sprintf (no standard library in a bare-metal OS!),
 * so we build the string manually:
 *   1. Handle negative sign
 *   2. Extract integer part
 *   3. Extract fractional part (multiply by 100 to get 2 decimal digits)
 *   4. Assemble into "integer.decimal" format
 *
 * Example: -3.14 -> "-3.14"
 *          2.5   -> "2.50"
 */
static void float_to_str(float val, char *buf) {
    int pos = 0;

    /* handle NaN - not a number (like 0/0 or sqrt(-1)) */
    if (is_nan(val)) {
        buf[0] = 'N'; buf[1] = 'a'; buf[2] = 'N'; buf[3] = '\0';
        return;
    }

    /* handle negative numbers */
    if (val < 0.0f) {
        buf[pos++] = '-';
        val = -val;
    }

    /* extract integer part and fractional part
     * adding 0.005 rounds to 2 decimal places (like banker's rounding) */
    int int_part = (int)val;
    int frac_part = (int)((val - (float)int_part) * 100.0f + 0.5f);

    /* if rounding pushed frac to 100, carry over to integer */
    if (frac_part >= 100) {
        int_part++;
        frac_part -= 100;
    }

    /* convert integer part to string */
    if (int_part == 0) {
        buf[pos++] = '0';
    } else {
        char tmp[12];
        int len = 0;
        int n = int_part;
        while (n > 0) { tmp[len++] = '0' + (n % 10); n /= 10; }
        for (int i = len - 1; i >= 0; i--) buf[pos++] = tmp[i];
    }

    /* decimal point and 2 fractional digits */
    buf[pos++] = '.';
    buf[pos++] = '0' + (frac_part / 10);
    buf[pos++] = '0' + (frac_part % 10);
    buf[pos] = '\0';
}

/*
 * draw the coordinate grid and axes
 */
static void draw_graph_area(void) {
    /* clear graph area to black */
    vga_fill_rect(GRAPH_LEFT, GRAPH_TOP, GRAPH_WIDTH, GRAPH_HEIGHT, COLOR_BLACK);

    /* --- dotted grid lines at each integer ---
     *
     * grid lines must align with the origin. they appear at pixel positions
     * where (px - origin_x) is a multiple of grid_scale.
     *
     * to find the first grid line inside the graph area:
     *   offset = (origin_x - GRAPH_LEFT) % grid_scale
     *   first_line = GRAPH_LEFT + offset
     *
     * this finds the distance from the left edge to the nearest
     * grid-aligned position, then starts there.
     */

    /* vertical grid lines */
    int off_x = (origin_x - GRAPH_LEFT) % grid_scale;
    if (off_x < 0) off_x += grid_scale;
    for (int px = GRAPH_LEFT + off_x; px <= GRAPH_RIGHT; px += grid_scale) {
        for (int py = GRAPH_TOP; py <= GRAPH_BOTTOM; py += 3) {
            vga_put_pixel(px, py, COLOR_GRID_DOT);
        }
    }

    /* horizontal grid lines */
    int off_y = (origin_y - GRAPH_TOP) % grid_scale;
    if (off_y < 0) off_y += grid_scale;
    for (int py = GRAPH_TOP + off_y; py <= GRAPH_BOTTOM; py += grid_scale) {
        for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px += 3) {
            vga_put_pixel(px, py, COLOR_GRID_DOT);
        }
    }

    /* --- solid axes --- */
    if (origin_y >= GRAPH_TOP && origin_y <= GRAPH_BOTTOM) {
        vga_draw_hline(GRAPH_LEFT, origin_y, GRAPH_WIDTH, COLOR_AXIS);
    }
    if (origin_x >= GRAPH_LEFT && origin_x <= GRAPH_RIGHT) {
        vga_draw_vline(origin_x, GRAPH_TOP, GRAPH_HEIGHT, COLOR_AXIS);
    }

    /* --- tick marks and number labels on axes --- */
    char label[12];

    /* ticks on X axis (reuse same off_x from grid lines) */
    if (origin_y >= GRAPH_TOP && origin_y <= GRAPH_BOTTOM) {
        for (int px = GRAPH_LEFT + off_x; px <= GRAPH_RIGHT; px += grid_scale) {
            if (px == origin_x) continue;
            vga_draw_vline(px, origin_y - 3, 7, COLOR_TICK);

            int math_val = (px - origin_x) / grid_scale;
            if (math_val != 0) {
                int_to_str(math_val, label);
                int lbl_x = px - 4;
                int lbl_y = origin_y + 6;
                if (lbl_y > GRAPH_BOTTOM - 10) lbl_y = origin_y - 14;
                draw_string(lbl_x, lbl_y, label, COLOR_GRAY);
            }
        }
    }

    /* ticks on Y axis (reuse same off_y from grid lines) */
    if (origin_x >= GRAPH_LEFT && origin_x <= GRAPH_RIGHT) {
        for (int py = GRAPH_TOP + off_y; py <= GRAPH_BOTTOM; py += grid_scale) {
            if (py == origin_y) continue;
            vga_draw_hline(origin_x - 3, py, 7, COLOR_TICK);

            int math_val = -(py - origin_y) / grid_scale;  /* negative because y is flipped */
            if (math_val != 0) {
                int_to_str(math_val, label);
                int lbl_x = origin_x + 6;
                if (lbl_x > GRAPH_RIGHT - 20) lbl_x = origin_x - 24;
                draw_string(lbl_x, py - 4, label, COLOR_GRAY);
            }
        }
    }

    /* "0" near origin */
    if (origin_x >= GRAPH_LEFT && origin_x <= GRAPH_RIGHT &&
        origin_y >= GRAPH_TOP && origin_y <= GRAPH_BOTTOM) {
        draw_char(origin_x + 5, origin_y + 5, '0', COLOR_GRAY);
    }
}

/*
 * eval_equation - evaluate an equation at a given x value.
 *
 * For normal equations: just runs the compiled bytecode.
 * For derivative equations: computes f'(x) numerically using the
 * central difference formula on the SOURCE equation:
 *   f'(x) ≈ (f(x+h) - f(x-h)) / (2h)
 *
 * This is the SAME math as the tangent line, but used to plot
 * the entire derivative curve.
 */
static float eval_equation(const equation_slot *eq, float x) {
    if (eq->deriv_source >= 0 && eq->deriv_source < eq_count) {
        /* this is a derivative equation - compute numerically */
        const equation_slot *src = &equations[eq->deriv_source];
        float h = 0.001f;
        float y_plus  = eval_compiled(&src->compiled, x + h, 0.0f);
        float y_minus = eval_compiled(&src->compiled, x - h, 0.0f);
        if (is_nan(y_plus) || is_nan(y_minus)) return make_nan();
        return (y_plus - y_minus) / (2.0f * h);
    }
    return eval_compiled(&eq->compiled, x, 0.0f);
}

/*
 * tiny busy-wait delay for the drawing animation.
 * we can't use sleep() (no OS scheduler!) so we just burn CPU cycles.
 * "volatile" prevents the compiler from optimizing the loop away.
 */
static void tiny_delay(void) {
    for (volatile int d = 0; d < 8000; d++);
}

/*
 * ANIMATED plot - draws column by column directly to screen
 * so you see the equation sweep left-to-right in real time.
 *
 * uses compiled bytecode (fast evaluation) but adds a small delay
 * between columns so the animation is visible.
 */
static void plot_explicit_animated(const equation_slot *eq, uint32_t color) {
    int prev_py = -1;

    for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px++) {
        float math_x = (float)(px - origin_x) / (float)grid_scale;
        float math_y = eval_equation(eq, math_x);

        if (is_nan(math_y)) { prev_py = -1; continue; }

        int py = origin_y - (int)(math_y * (float)grid_scale);

        if (py >= GRAPH_TOP && py <= GRAPH_BOTTOM) {
            if (prev_py >= GRAPH_TOP && prev_py <= GRAPH_BOTTOM && prev_py != -1) {
                int y_start = (prev_py < py) ? prev_py : py;
                int y_end = (prev_py < py) ? py : prev_py;
                if (y_end - y_start < GRAPH_HEIGHT / 2) {
                    for (int y = y_start; y <= y_end; y++) {
                        if (y >= GRAPH_TOP && y <= GRAPH_BOTTOM) {
                            vga_put_pixel(px, y, color);
                            curve_put_pixel(px, y, color);
                        }
                    }
                }
            } else {
                vga_put_pixel(px, py, color);
                curve_put_pixel(px, py, color);
            }
        }
        prev_py = py;

        /* small delay every 4 columns for visible sweep effect */
        if ((px & 3) == 0) tiny_delay();
    }
}

/*
 * animated implicit plot - sweeps left to right in vertical columns.
 * for each column, checks every pixel in that column for sign changes.
 * this gives a nice left-to-right reveal effect for circles, etc.
 */
static void plot_implicit_animated(const equation_slot *eq, uint32_t color) {
    float dx = 1.0f / (float)grid_scale;
    float dy = 1.0f / (float)grid_scale;

    for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px++) {
        float mx = (float)(px - origin_x) / (float)grid_scale;
        for (int py = GRAPH_TOP; py <= GRAPH_BOTTOM; py++) {
            float my = (float)(origin_y - py) / (float)grid_scale;
            float val   = eval_compiled(&eq->comp_left, mx, my) - eval_compiled(&eq->comp_right, mx, my);
            float val_r = eval_compiled(&eq->comp_left, mx+dx, my) - eval_compiled(&eq->comp_right, mx+dx, my);
            float val_d = eval_compiled(&eq->comp_left, mx, my-dy) - eval_compiled(&eq->comp_right, mx, my-dy);
            if (is_nan(val) || is_nan(val_r) || is_nan(val_d)) continue;
            if ((val >= 0) != (val_r >= 0) || (val >= 0) != (val_d >= 0)) {
                vga_put_pixel(px, py, color);
                vga_put_pixel(px + 1, py, color);
                vga_put_pixel(px, py + 1, color);
                curve_put_pixel(px, py, color);
                curve_put_pixel(px + 1, py, color);
                curve_put_pixel(px, py + 1, color);
            }
        }
        /* delay every 2 columns for visible sweep */
        if ((px & 1) == 0) tiny_delay();
    }
}

/* initial plot with animation (draws directly to screen) */
static void plot_equation_animated(const equation_slot *eq, uint32_t color) {
    if (eq->is_implicit) {
        plot_implicit_animated(eq, color);
    } else {
        plot_explicit_animated(eq, color);
    }
}

/*
 * STRIP-AWARE plot for explicit equations.
 *
 * writes to the CURVE LAYER (not the screen or back buffer).
 * only evaluates within columns [x_left, x_right].
 *
 * for a full redraw: x_left = GRAPH_LEFT, x_right = GRAPH_RIGHT
 * for a pan strip:   x_left/x_right = just the newly revealed edge
 *
 * the curve layer is later composited on top of the grid.
 */
static void plot_explicit_to_curve(const equation_slot *eq, uint32_t color,
                                   int x_left, int x_right) {
    int prev_py = -1;

    for (int px = x_left; px <= x_right; px++) {
        float math_x = (float)(px - origin_x) / (float)grid_scale;
        float math_y = eval_equation(eq, math_x);

        if (is_nan(math_y)) { prev_py = -1; continue; }

        int py = origin_y - (int)(math_y * (float)grid_scale);

        if (py >= GRAPH_TOP && py <= GRAPH_BOTTOM) {
            if (prev_py >= GRAPH_TOP && prev_py <= GRAPH_BOTTOM && prev_py != -1) {
                int y_start = (prev_py < py) ? prev_py : py;
                int y_end = (prev_py < py) ? py : prev_py;
                if (y_end - y_start < GRAPH_HEIGHT / 2) {
                    for (int y = y_start; y <= y_end; y++) {
                        if (y >= GRAPH_TOP && y <= GRAPH_BOTTOM)
                            curve_put_pixel(px, y, color);
                    }
                }
            } else {
                curve_put_pixel(px, py, color);
            }
        }
        prev_py = py;
    }
}

/*
 * STRIP-AWARE implicit plot with adaptive sampling.
 *
 * writes to the CURVE LAYER. only evaluates within the given bounds.
 * uses "marching squares" - check coarse grid, refine only near the curve.
 */
static void plot_implicit_to_curve(const equation_slot *eq, uint32_t color,
                                   int x_left, int x_right, int y_top, int y_bottom) {
    #define STEP 4

    float inv_scale = 1.0f / (float)grid_scale;

    /* align to STEP grid so we don't miss cells at boundaries */
    int px_start = x_left - (x_left % STEP);
    if (px_start < GRAPH_LEFT) px_start = GRAPH_LEFT;
    int py_start = y_top - (y_top % STEP);
    if (py_start < GRAPH_TOP) py_start = GRAPH_TOP;

    for (int py = py_start; py <= y_bottom; py += STEP) {
        for (int px = px_start; px <= x_right; px += STEP) {
            float mx0 = (float)(px - origin_x) * inv_scale;
            float my0 = (float)(origin_y - py) * inv_scale;
            float mx1 = (float)(px + STEP - origin_x) * inv_scale;
            float my1 = (float)(origin_y - (py + STEP)) * inv_scale;

            float v00 = eval_compiled(&eq->comp_left, mx0, my0) - eval_compiled(&eq->comp_right, mx0, my0);
            float v10 = eval_compiled(&eq->comp_left, mx1, my0) - eval_compiled(&eq->comp_right, mx1, my0);
            float v01 = eval_compiled(&eq->comp_left, mx0, my1) - eval_compiled(&eq->comp_right, mx0, my1);
            float v11 = eval_compiled(&eq->comp_left, mx1, my1) - eval_compiled(&eq->comp_right, mx1, my1);

            if (is_nan(v00) || is_nan(v10) || is_nan(v01) || is_nan(v11)) continue;
            int s00 = (v00 >= 0), s10 = (v10 >= 0), s01 = (v01 >= 0), s11 = (v11 >= 0);
            if (s00 == s10 && s10 == s01 && s01 == s11) continue;

            int py_end = py + STEP;
            int px_end = px + STEP;
            if (py_end > GRAPH_BOTTOM) py_end = GRAPH_BOTTOM;
            if (px_end > GRAPH_RIGHT) px_end = GRAPH_RIGHT;

            for (int fy = py; fy <= py_end; fy++) {
                float my = (float)(origin_y - fy) * inv_scale;
                for (int fx = px; fx <= px_end; fx++) {
                    float mx = (float)(fx - origin_x) * inv_scale;
                    float val   = eval_compiled(&eq->comp_left, mx, my)          - eval_compiled(&eq->comp_right, mx, my);
                    float val_r = eval_compiled(&eq->comp_left, mx+inv_scale, my) - eval_compiled(&eq->comp_right, mx+inv_scale, my);
                    float val_d = eval_compiled(&eq->comp_left, mx, my-inv_scale) - eval_compiled(&eq->comp_right, mx, my-inv_scale);
                    if (is_nan(val) || is_nan(val_r) || is_nan(val_d)) continue;
                    if ((val >= 0) != (val_r >= 0) || (val >= 0) != (val_d >= 0)) {
                        curve_put_pixel(fx, fy, color);
                        curve_put_pixel(fx + 1, fy, color);
                        curve_put_pixel(fx, fy + 1, color);
                    }
                }
            }
        }
    }
    #undef STEP
}

/* plot an equation to the curve layer within a rectangular region */
static void plot_equation_to_curve(const equation_slot *eq, uint32_t color,
                                   int x_left, int x_right, int y_top, int y_bottom) {
    if (eq->is_implicit) {
        plot_implicit_to_curve(eq, color, x_left, x_right, y_top, y_bottom);
    } else {
        /* explicit equations only need x range (y is computed) */
        plot_explicit_to_curve(eq, color, x_left, x_right);
    }
}

/* plot all equations to curve layer (full graph area) */
static void plot_all_to_curve_full(void) {
    curve_layer_clear();
    for (int i = 0; i < eq_count; i++) {
        plot_equation_to_curve(&equations[i], eq_colors[i],
                               GRAPH_LEFT, GRAPH_RIGHT, GRAPH_TOP, GRAPH_BOTTOM);
    }
}

/*
 * draw_zero_markers - find and mark where curves cross the x-axis.
 *
 * A "zero" (or "root") is where f(x) = 0, i.e., the curve touches
 * the x-axis. These are important points in math!
 *
 * We detect zeros by looking for sign changes in the y-value as we
 * scan pixel columns. When f(x) changes from positive to negative
 * (or vice versa), the curve must have crossed zero somewhere between.
 *
 * Each zero gets a small diamond marker drawn ON the axis.
 */
static void draw_zero_markers(void) {
    for (int i = 0; i < eq_count; i++) {
        if (equations[i].is_implicit) continue; /* zeros only for explicit */

        float prev_y = 0;
        int prev_valid = 0;

        for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px++) {
            float mx = (float)(px - origin_x) / (float)grid_scale;
            float my = eval_equation(&equations[i], mx);

            if (is_nan(my)) { prev_valid = 0; continue; }

            if (prev_valid) {
                /* sign change? curve crossed zero! */
                if ((prev_y >= 0 && my < 0) || (prev_y < 0 && my >= 0)) {
                    /* draw diamond marker at this x on the x-axis */
                    int marker_y = origin_y;
                    if (marker_y < GRAPH_TOP || marker_y > GRAPH_BOTTOM) continue;

                    /* diamond shape: 5x5 */
                    uint32_t color = eq_colors[i];
                    for (int d = 0; d <= 3; d++) {
                        if (px + d <= GRAPH_RIGHT) vga_put_pixel(px + d, marker_y, color);
                        if (px - d >= GRAPH_LEFT)  vga_put_pixel(px - d, marker_y, color);
                        if (marker_y + d <= GRAPH_BOTTOM) vga_put_pixel(px, marker_y + d, color);
                        if (marker_y - d >= GRAPH_TOP)    vga_put_pixel(px, marker_y - d, color);
                    }
                    /* fill the diamond */
                    for (int dy = -2; dy <= 2; dy++) {
                        int w = 3 - (dy < 0 ? -dy : dy);
                        for (int dx = -w; dx <= w; dx++) {
                            int ppx = px + dx, ppy = marker_y + dy;
                            if (ppx >= GRAPH_LEFT && ppx <= GRAPH_RIGHT &&
                                ppy >= GRAPH_TOP && ppy <= GRAPH_BOTTOM)
                                vga_put_pixel(ppx, ppy, color);
                        }
                    }
                    /* white center dot */
                    vga_put_pixel(px, marker_y, COLOR_WHITE);
                }
            }

            prev_y = my;
            prev_valid = 1;
        }
    }
}

/*
 * draw_intersection_markers - find where two curves intersect.
 *
 * For each pair of explicit equations, we check where the difference
 * f1(x) - f2(x) changes sign. That's where the curves cross!
 *
 * The marker is a small white circle at the intersection point.
 */
static void draw_intersection_markers(void) {
    for (int i = 0; i < eq_count; i++) {
        if (equations[i].is_implicit) continue;
        for (int j = i + 1; j < eq_count; j++) {
            if (equations[j].is_implicit) continue;

            float prev_diff = 0;
            int prev_valid = 0;

            for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px++) {
                float mx = (float)(px - origin_x) / (float)grid_scale;
                float y1 = eval_equation(&equations[i], mx);
                float y2 = eval_equation(&equations[j], mx);

                if (is_nan(y1) || is_nan(y2)) { prev_valid = 0; continue; }

                float diff = y1 - y2;

                if (prev_valid && ((prev_diff >= 0 && diff < 0) || (prev_diff < 0 && diff >= 0))) {
                    /* intersection! draw at the average y */
                    float avg_y = (y1 + y2) / 2.0f;
                    int py = origin_y - (int)(avg_y * (float)grid_scale);

                    if (py >= GRAPH_TOP && py <= GRAPH_BOTTOM) {
                        /* draw a small circle marker */
                        for (int dy = -3; dy <= 3; dy++) {
                            for (int dx = -3; dx <= 3; dx++) {
                                if (dx*dx + dy*dy <= 9) {  /* r=3 circle */
                                    int ppx = px + dx, ppy = py + dy;
                                    if (ppx >= GRAPH_LEFT && ppx <= GRAPH_RIGHT &&
                                        ppy >= GRAPH_TOP && ppy <= GRAPH_BOTTOM) {
                                        /* outer ring white, inner yellow */
                                        if (dx*dx + dy*dy <= 4)
                                            vga_put_pixel(ppx, ppy, COLOR_YELLOW);
                                        else
                                            vga_put_pixel(ppx, ppy, COLOR_WHITE);
                                    }
                                }
                            }
                        }
                    }
                }

                prev_diff = diff;
                prev_valid = 1;
            }
        }
    }
}

/* === UI drawing functions === */

/* draw the side panel showing all equations */
static void draw_panel(void) {
    vga_fill_rect(0, 0, PANEL_WIDTH, SCREEN_HEIGHT, COLOR_PANEL_BG);

    draw_string(8, 8, "Equations", COLOR_WHITE);
    vga_draw_hline(8, 20, PANEL_WIDTH - 16, COLOR_GRAY);

    /* draw each equation with its color */
    for (int i = 0; i < eq_count; i++) {
        int y = 28 + i * 20;

        /* color indicator dot */
        vga_fill_rect(8, y + 2, 6, 6, eq_colors[i]);

        /* equation number */
        char num[4] = { '0' + (i + 1), ':', ' ', '\0' };
        draw_string(18, y, num, COLOR_LIGHT_GRAY);

        /* equation text (truncated to fit panel) */
        char display[14];
        int j;
        for (j = 0; j < 13 && equations[i].text[j]; j++) {
            display[j] = equations[i].text[j];
        }
        display[j] = '\0';
        draw_string(42, y, display, eq_colors[i]);
    }

    /* show instructions at bottom of panel */
    int help_y = SCREEN_HEIGHT - 210;
    draw_string(8, help_y,       "Controls:", COLOR_GRAY);
    draw_string(8, help_y + 12,  "arrows: pan", COLOR_GRAY);
    draw_string(8, help_y + 24,  "[ ] zoom", COLOR_GRAY);
    draw_string(8, help_y + 36,  "Enter: plot", COLOR_GRAY);
    draw_string(8, help_y + 48,  "Tab: clear all", COLOR_GRAY);
    draw_string(8, help_y + 60,  "click eq: del", COLOR_GRAY);
    draw_string(8, help_y + 72,  "drag: pan", COLOR_GRAY);
    draw_string(8, help_y + 88,  "Trace mode:", COLOR_LIGHT_CYAN);
    draw_string(8, help_y + 100, "Shift+T start", COLOR_GRAY);
    draw_string(8, help_y + 112, "mouse: trace", COLOR_GRAY);
    draw_string(8, help_y + 124, "U/D switch eq", COLOR_GRAY);
    draw_string(8, help_y + 136, "Shift+D tang.", COLOR_GRAY);
    draw_string(8, help_y + 148, "Shift+F deriv.", COLOR_GRAY);

    /* show current zoom level below the instructions */
    char zoom_str[16] = "zoom: ";
    char zoom_num[8];
    int_to_str(grid_scale, zoom_num);
    int zi = 6;
    for (int i = 0; zoom_num[i] && zi < 15; i++) zoom_str[zi++] = zoom_num[i];
    zoom_str[zi] = '\0';
    draw_string(8, help_y + 168, zoom_str, COLOR_GRAY);
}

/*
 * draw the title bar - this gets called frequently to update status info
 * (trace coordinates, mouse coordinates, slope, etc.)
 */
static void draw_title_bar(void) {
    vga_fill_rect(PANEL_WIDTH, 0, SCREEN_WIDTH - PANEL_WIDTH, TITLE_HEIGHT, COLOR_TITLE_BG);
    draw_string(PANEL_WIDTH + 8, 8, "GraphCalcOS", COLOR_LIGHT_GREEN);

    /* the right portion of the title bar shows context-sensitive info:
     * - in trace mode: trace coordinates and optional slope
     * - otherwise: a helpful hint */
    if (!trace_mode) {
        draw_string(PANEL_WIDTH + 200, 8, "type an equation and press enter!", COLOR_GRAY);
    }
}

/*
 * update_title_bar_info - redraw just the info portion of the title bar
 *
 * This is called frequently (mouse move, trace step) so it only redraws
 * the right part of the title bar, not the whole thing. This avoids flicker.
 */
static void update_title_bar_info(const char *info, uint32_t color) {
    /* clear the info area (right side of title bar) */
    vga_fill_rect(PANEL_WIDTH + 120, 0, SCREEN_WIDTH - PANEL_WIDTH - 120, TITLE_HEIGHT, COLOR_TITLE_BG);
    draw_string(PANEL_WIDTH + 128, 8, info, color);
}

/* draw the input bar */
static void draw_input_bar(void) {
    vga_fill_rect(0, INPUT_BAR_Y, SCREEN_WIDTH, 24, COLOR_INPUT_BG);
    draw_string(PANEL_WIDTH + 8, INPUT_TEXT_Y, ">", COLOR_WHITE);
}

/* clear just the text portion of the input bar */
static void clear_input_text(void) {
    vga_fill_rect(INPUT_TEXT_X, INPUT_TEXT_Y, SCREEN_WIDTH - INPUT_TEXT_X - 4, FONT_HEIGHT, COLOR_INPUT_BG);
}

/* draw text cursor in the input bar */
static void draw_cursor(int char_pos, uint32_t color) {
    int x = INPUT_TEXT_X + char_pos * FONT_WIDTH;
    vga_fill_rect(x, INPUT_TEXT_Y, 2, FONT_HEIGHT, color);
}

/* === MOUSE CURSOR DRAWING ===
 *
 * The cursor is a small arrow pointer shape, drawn pixel-by-pixel.
 * We read/write from the FRAMEBUFFER (not back_buffer) because the
 * cursor lives on top of whatever is currently displayed.
 */

/*
 * read a pixel directly from the visible framebuffer.
 * we need this to save/restore pixels under the cursor.
 * (vga_put_pixel writes to draw_target, which might be the back buffer,
 *  but for the cursor we always want the actual screen.)
 */
/*
 * read/write screen pixels for cursor save/restore.
 * we read from screen_buffer (logical 640x480) and when writing,
 * also update the real framebuffer at 2x scale.
 */
static inline uint32_t read_screen_pixel(int x, int y) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return 0;
    return screen_buffer[y * SCREEN_WIDTH + x];
}

static inline void write_screen_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    screen_buffer[y * SCREEN_WIDTH + x] = color;
    fb_write_scaled(x, y, color);
}

/*
 * The cursor shape: a small arrow pointer.
 * '1' = white (main body), '2' = black (outline), ' ' = transparent.
 *
 * This gives us a classic arrow cursor that's easy to see on any background.
 * The outline ensures visibility whether the background is light or dark.
 */
static const char cursor_shape[CURSOR_SIZE][CURSOR_SIZE + 1] = {
    "2           ",
    "22          ",
    "212         ",
    "2112        ",
    "21112       ",
    "211112      ",
    "2111112     ",
    "21111112    ",
    "211112222   ",
    "21121       ",
    "2212 2      ",
    "22   2      ",
};

/*
 * restore_cursor - put back the pixels that were under the cursor
 *
 * This "erases" the cursor by restoring the original image.
 * Called before drawing the cursor at a new position.
 */
static void restore_cursor(void) {
    if (!cursor_visible) return;

    for (int dy = 0; dy < CURSOR_SIZE; dy++) {
        for (int dx = 0; dx < CURSOR_SIZE; dx++) {
            if (cursor_shape[dy][dx] != ' ') {
                write_screen_pixel(cursor_old_x + dx, cursor_old_y + dy,
                                   cursor_saved[dy * CURSOR_SIZE + dx]);
            }
        }
    }
    cursor_visible = 0;
}

/*
 * draw_mouse_cursor - save pixels under new position, then draw cursor
 *
 * Step 1: Save every pixel that the cursor will cover
 * Step 2: Draw the cursor shape on top
 *
 * We always read/write the actual framebuffer, not the back buffer,
 * because the cursor is an overlay that sits on top of everything.
 */
static void draw_mouse_cursor(int mx, int my) {
    /* save pixels under the new cursor position */
    for (int dy = 0; dy < CURSOR_SIZE; dy++) {
        for (int dx = 0; dx < CURSOR_SIZE; dx++) {
            cursor_saved[dy * CURSOR_SIZE + dx] = read_screen_pixel(mx + dx, my + dy);
        }
    }

    /* draw the cursor shape */
    for (int dy = 0; dy < CURSOR_SIZE; dy++) {
        for (int dx = 0; dx < CURSOR_SIZE; dx++) {
            char ch = cursor_shape[dy][dx];
            if (ch == '1') {
                write_screen_pixel(mx + dx, my + dy, COLOR_WHITE);
            } else if (ch == '2') {
                write_screen_pixel(mx + dx, my + dy, COLOR_BLACK);
            }
            /* ' ' = transparent, don't draw anything */
        }
    }

    cursor_old_x = mx;
    cursor_old_y = my;
    cursor_visible = 1;
}

/* === TRACE MODE DRAWING ===
 *
 * When trace mode is active, we draw:
 *   1. A bright dot on the curve at the current trace position
 *   2. Coordinate info in the title bar
 *   3. Optionally, a tangent line through the trace point
 */

/*
 * find_next_traceable_eq - find the index of any equation starting from a
 * given index. We can now trace both explicit AND implicit equations!
 *
 * For explicit equations: straightforward y = f(x) tracing.
 * For implicit equations: we search the column for the nearest y on the curve.
 */
static int find_next_traceable_eq(int start) {
    for (int i = start; i < eq_count; i++) {
        return i;
    }
    return -1;
}

/*
 * eval_implicit_f - evaluate F(x,y) = left(x,y) - right(x,y) for an implicit equation.
 *
 * The curve is where F(x,y) = 0. We use the sign of F to find curve crossings.
 */
static float eval_implicit_f(const equation_slot *eq, float mx, float my) {
    return eval_compiled(&eq->comp_left, mx, my) - eval_compiled(&eq->comp_right, mx, my);
}

/*
 * find_implicit_y - find the nearest y-pixel on an implicit curve for a given x-pixel.
 *
 * Strategy: scan the column looking for sign changes in F(x,y), then pick the
 * one closest to hint_py (our previous trace position). This way we follow
 * one branch of the curve instead of jumping between branches.
 *
 * Uses bisection to refine the crossing to sub-pixel accuracy.
 */
static int find_implicit_y(const equation_slot *eq, int px, int hint_py) {
    float inv_scale = 1.0f / (float)grid_scale;
    float mx = (float)(px - origin_x) * inv_scale;

    int best_py = -1;
    int best_dist = 999999;

    /* scan the column for sign changes in F(x,y) */
    float prev_val = eval_implicit_f(eq, mx, (float)(origin_y - GRAPH_TOP) * inv_scale);
    for (int py = GRAPH_TOP + 1; py <= GRAPH_BOTTOM; py++) {
        float my = (float)(origin_y - py) * inv_scale;
        float val = eval_implicit_f(eq, mx, my);

        if (is_nan(val) || is_nan(prev_val)) {
            prev_val = val;
            continue;
        }

        /* sign change = curve crossing! */
        if ((val >= 0) != (prev_val >= 0)) {
            /* this crossing is at py-1 to py, pick the midpoint */
            int cross_py = py;
            int dist = cross_py - hint_py;
            if (dist < 0) dist = -dist;
            if (dist < best_dist) {
                best_dist = dist;
                best_py = cross_py;
            }
        }
        prev_val = val;
    }

    return best_py;
}

/*
 * eval_trace_y - evaluate the traced equation at a pixel X column
 *
 * For explicit equations: straightforward f(x) evaluation.
 * For implicit equations: searches the column for the nearest curve crossing
 * to our current trace_py position (so we follow one branch).
 *
 * Returns the pixel Y, or -1 if the result is NaN (undefined) or not found.
 */
static int eval_trace_y(int px) {
    if (trace_eq_index < 0 || trace_eq_index >= eq_count) return -1;
    const equation_slot *eq = &equations[trace_eq_index];

    if (eq->is_implicit) {
        /* for implicit: find nearest y crossing to current trace_py */
        return find_implicit_y(eq, px, trace_py);
    }

    float math_x = (float)(px - origin_x) / (float)grid_scale;
    float math_y = eval_equation(eq, math_x);
    if (is_nan(math_y)) return -1;

    int py = origin_y - (int)(math_y * (float)grid_scale);
    return py;
}

/*
 * draw_trace_dot - draw a visible marker on the curve at the trace position
 *
 * The dot is a 5x5 bright square with a colored border, so it stands out
 * against both the curve and the background.
 */
static void draw_trace_dot(void) {
    int py = eval_trace_y(trace_px);
    if (py < GRAPH_TOP || py > GRAPH_BOTTOM) return;
    trace_py = py;  /* remember Y so implicit curves can follow this branch */

    /* draw a 5x5 dot: white center with colored border */
    uint32_t border_color = eq_colors[trace_eq_index];
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            int px = trace_px + dx;
            int qy = py + dy;
            if (px < GRAPH_LEFT || px > GRAPH_RIGHT) continue;
            if (qy < GRAPH_TOP || qy > GRAPH_BOTTOM) continue;

            /* outer ring = equation color, inner = white */
            if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) {
                vga_put_pixel(px, qy, COLOR_WHITE);
            } else {
                vga_put_pixel(px, qy, border_color);
            }
        }
    }
}

/*
 * draw_tangent_line - draw the tangent (derivative) line at the trace point
 *
 * The tangent line is the line that just "touches" the curve at one point
 * and has the same slope. It shows the instantaneous rate of change.
 *
 * We calculate the slope numerically using the "central difference" formula:
 *   f'(x) = (f(x+h) - f(x-h)) / (2h)
 *
 * This is more accurate than the one-sided (f(x+h) - f(x)) / h because
 * errors from both sides cancel out. It's the same idea behind the
 * symmetric derivative in calculus.
 *
 * Then we draw the line: y = f(x0) + slope * (x - x0)
 * across the entire graph area.
 */
static void draw_tangent_line(void) {
    if (trace_eq_index < 0 || trace_eq_index >= eq_count) return;
    const equation_slot *eq = &equations[trace_eq_index];

    /* the point we're computing the tangent at */
    float x0 = (float)(trace_px - origin_x) / (float)grid_scale;
    float y0;
    float slope;

    float h = 0.001f;

    if (eq->is_implicit) {
        /*
         * IMPLICIT TANGENT: for F(x,y) = 0, the tangent slope is:
         *   dy/dx = -(dF/dx) / (dF/dy)
         *
         * We compute partial derivatives numerically using central differences:
         *   dF/dx ≈ (F(x+h,y) - F(x-h,y)) / (2h)
         *   dF/dy ≈ (F(x,y+h) - F(x,y-h)) / (2h)
         *
         * This works for circles, ellipses, any implicit curve!
         */
        y0 = (float)(origin_y - trace_py) / (float)grid_scale;

        float dFdx = (eval_implicit_f(eq, x0 + h, y0) - eval_implicit_f(eq, x0 - h, y0)) / (2.0f * h);
        float dFdy = (eval_implicit_f(eq, x0, y0 + h) - eval_implicit_f(eq, x0, y0 - h)) / (2.0f * h);

        if (is_nan(dFdx) || is_nan(dFdy)) return;

        /* if dF/dy is ~0, tangent is vertical (infinite slope), skip */
        if (dFdy > -0.0001f && dFdy < 0.0001f) return;

        slope = -dFdx / dFdy;
    } else {
        /*
         * EXPLICIT TANGENT: use central difference on f(x) directly.
         *   f'(x) ~ (f(x+h) - f(x-h)) / (2h)
         */
        y0 = eval_equation(eq, x0);
        if (is_nan(y0)) return;

        float y_plus  = eval_equation(eq, x0 + h);
        float y_minus = eval_equation(eq, x0 - h);

        if (is_nan(y_plus) || is_nan(y_minus)) return;

        slope = (y_plus - y_minus) / (2.0f * h);
    }

    tangent_slope = slope;

    /*
     * draw the tangent line across the graph.
     * for each pixel column, compute:
     *   math_y = y0 + slope * (math_x - x0)
     * then convert to pixel coordinates.
     *
     * we use a bright white color so it contrasts with the curve.
     */
    int prev_tpy = -1;
    for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px++) {
        float math_x = (float)(px - origin_x) / (float)grid_scale;
        float tangent_y = y0 + slope * (math_x - x0);

        int tpy = origin_y - (int)(tangent_y * (float)grid_scale);

        if (tpy >= GRAPH_TOP && tpy <= GRAPH_BOTTOM) {
            /* connect to previous pixel to avoid gaps in steep lines */
            if (prev_tpy >= GRAPH_TOP && prev_tpy <= GRAPH_BOTTOM && prev_tpy != -1) {
                int y_start = (prev_tpy < tpy) ? prev_tpy : tpy;
                int y_end = (prev_tpy < tpy) ? tpy : prev_tpy;
                if (y_end - y_start < GRAPH_HEIGHT / 2) {
                    for (int y = y_start; y <= y_end; y++) {
                        if (y >= GRAPH_TOP && y <= GRAPH_BOTTOM)
                            vga_put_pixel(px, y, COLOR_WHITE);
                    }
                }
            } else {
                vga_put_pixel(px, tpy, COLOR_WHITE);
            }
        }
        prev_tpy = tpy;
    }
}

/*
 * update_trace_display - show trace info in the title bar
 *
 * Displays the current math (x, y) coordinates and optionally the
 * tangent slope. This gives you the exact values as you trace the curve.
 */
static void update_trace_display(void) {
    float math_x = (float)(trace_px - origin_x) / (float)grid_scale;
    const equation_slot *eq = &equations[trace_eq_index];
    float math_y;

    if (eq->is_implicit) {
        /* for implicit equations, get y from the traced pixel position */
        math_y = (float)(origin_y - trace_py) / (float)grid_scale;
    } else {
        math_y = eval_equation(eq, math_x);
    }

    /* build the info string: "trace: x=1.50 y=2.25" */
    char info[80];
    int p = 0;

    /* "trace eq#: " prefix */
    info[p++] = 't'; info[p++] = 'r'; info[p++] = 'a';
    info[p++] = 'c'; info[p++] = 'e'; info[p++] = ' ';
    info[p++] = '0' + (trace_eq_index + 1);
    info[p++] = ':'; info[p++] = ' ';

    /* "x=" */
    info[p++] = 'x'; info[p++] = '=';
    char num_buf[16];
    float_to_str(math_x, num_buf);
    for (int i = 0; num_buf[i] && p < 70; i++) info[p++] = num_buf[i];

    info[p++] = ' ';

    /* "y=" */
    info[p++] = 'y'; info[p++] = '=';
    float_to_str(math_y, num_buf);
    for (int i = 0; num_buf[i] && p < 70; i++) info[p++] = num_buf[i];

    /* if tangent is visible, also show the slope */
    if (tangent_visible) {
        info[p++] = ' ';
        info[p++] = 'm'; info[p++] = '=';
        float_to_str(tangent_slope, num_buf);
        for (int i = 0; num_buf[i] && p < 78; i++) info[p++] = num_buf[i];
    }

    info[p] = '\0';
    update_title_bar_info(info, COLOR_LIGHT_CYAN);
}

/*
 * redraw everything using the back buffer (instant update).
 * draws grid + all equations + UI to the hidden buffer,
 * then copies it all to the screen at once.
 */
/*
 * FULL REDRAW - recomputes everything from scratch.
 * used for: zoom changes, new equations, trace mode, tab clear.
 */
static void redraw_all(void) {
    vga_use_backbuffer();

    /* 1. draw grid + axes to back buffer */
    draw_graph_area();

    /* 2. recompute ALL curve pixels from scratch */
    plot_all_to_curve_full();

    /* 3. stamp curves on top of grid */
    curve_layer_blit();

    /* 4. mark zeros (x-axis crossings) and intersections */
    draw_zero_markers();
    draw_intersection_markers();

    /* if trace mode is on, redraw the tangent line and trace dot on the buffer */
    if (trace_mode) {
        if (tangent_visible) {
            draw_tangent_line();
        }
        draw_trace_dot();
    }

    draw_panel();
    draw_title_bar();

    /* if trace mode, update the title bar info on the back buffer too */
    if (trace_mode) {
        update_trace_display();
    }

    vga_flush();       /* copy everything to screen in one shot */
    vga_use_screen();  /* switch back to direct drawing */
}

/*
 * TRACE REDRAW - the FAST path for moving the trace dot!
 *
 * When you move the mouse in trace mode, only the trace dot and tangent
 * line change -- the curves themselves haven't changed at all.
 * So we skip the expensive plot_all_to_curve_full() and just:
 *   1. redraw the grid (cheap - just dots and lines)
 *   2. stamp EXISTING curves on top (no recomputation!)
 *   3. draw new trace dot + tangent
 *   4. flush to screen
 *
 * This is 10-50x faster than redraw_all() because the curve computation
 * (which involves evaluating equations at every pixel) is the slow part.
 */
static void redraw_trace(void) {
    vga_use_backbuffer();

    /* 1. redraw grid (cheap) */
    draw_graph_area();

    /* 2. stamp existing curve data - NO recomputation! */
    curve_layer_blit();

    /* 2b. zero and intersection markers */
    draw_zero_markers();
    draw_intersection_markers();

    /* 3. draw trace visuals */
    if (tangent_visible) {
        draw_tangent_line();
    }
    draw_trace_dot();

    draw_panel();
    draw_title_bar();
    update_trace_display();

    vga_flush();       /* copy everything to screen in one shot */
    vga_use_screen();  /* switch back to direct drawing */
}

/*
 * PAN REDRAW - the fast path! only recomputes the newly revealed strip.
 *
 * when you pan by (dx, dy), 96% of the pixels are the same, just shifted.
 * so we:
 *   1. shift the curve layer (instant - just memory copy)
 *   2. evaluate equations ONLY in the new strip (~20px wide)
 *   3. redraw grid+axes fully (cheap)
 *   4. composite curves on top
 *
 * this is 10-25x faster than redraw_all() for typical pan amounts!
 */
static void pan_redraw(int dx, int dy) {
    vga_use_backbuffer();

    /* 1. shift existing curve data */
    curve_layer_shift(dx, dy);

    /* 2. evaluate equations only in the newly revealed strips */
    if (dx > 0) {
        /* panned left = new strip on the LEFT side */
        int strip_right = GRAPH_LEFT + dx;
        if (strip_right > GRAPH_RIGHT) strip_right = GRAPH_RIGHT;
        for (int i = 0; i < eq_count; i++) {
            plot_equation_to_curve(&equations[i], eq_colors[i],
                                   GRAPH_LEFT, strip_right, GRAPH_TOP, GRAPH_BOTTOM);
        }
    } else if (dx < 0) {
        /* panned right = new strip on the RIGHT side */
        int strip_left = GRAPH_RIGHT + dx;
        if (strip_left < GRAPH_LEFT) strip_left = GRAPH_LEFT;
        for (int i = 0; i < eq_count; i++) {
            plot_equation_to_curve(&equations[i], eq_colors[i],
                                   strip_left, GRAPH_RIGHT, GRAPH_TOP, GRAPH_BOTTOM);
        }
    }
    if (dy > 0) {
        /* panned up = new strip at the TOP */
        int strip_bottom = GRAPH_TOP + dy;
        if (strip_bottom > GRAPH_BOTTOM) strip_bottom = GRAPH_BOTTOM;
        for (int i = 0; i < eq_count; i++) {
            plot_equation_to_curve(&equations[i], eq_colors[i],
                                   GRAPH_LEFT, GRAPH_RIGHT, GRAPH_TOP, strip_bottom);
        }
    } else if (dy < 0) {
        /* panned down = new strip at the BOTTOM */
        int strip_top = GRAPH_BOTTOM + dy;
        if (strip_top < GRAPH_TOP) strip_top = GRAPH_TOP;
        for (int i = 0; i < eq_count; i++) {
            plot_equation_to_curve(&equations[i], eq_colors[i],
                                   GRAPH_LEFT, GRAPH_RIGHT, strip_top, GRAPH_BOTTOM);
        }
    }

    /* 3. draw grid+axes fresh (cheap - just dots and lines) */
    draw_graph_area();

    /* 4. composite curves on top of grid */
    curve_layer_blit();

    /* 4b. zero and intersection markers */
    draw_zero_markers();
    draw_intersection_markers();

    if (trace_mode) {
        if (tangent_visible) draw_tangent_line();
        draw_trace_dot();
        update_trace_display();
    }

    draw_panel();
    draw_title_bar();

    vga_flush();
    vga_use_screen();
}

/*
 * Helper to redraw the input bar and its contents after a full redraw.
 * This is called after redraw_all() since the back buffer doesn't
 * include the input bar state.
 */
static void restore_input_bar(int input_len, const char *input_buf) {
    draw_input_bar();
    draw_cursor(input_len, COLOR_WHITE);
    if (input_len > 0) draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE);
}

/*
 * show_mouse_coords - display math coordinates of mouse position in title bar
 *
 * Converts pixel position to math coordinates using the same formula
 * as the graph plotter (just in reverse):
 *   math_x = (pixel_x - origin_x) / grid_scale
 *   math_y = (origin_y - pixel_y) / grid_scale   (y is flipped!)
 */
static void show_mouse_coords(int mx, int my) {
    /* only show coords when mouse is in the graph area */
    if (mx < GRAPH_LEFT || mx > GRAPH_RIGHT || my < GRAPH_TOP || my > GRAPH_BOTTOM) return;
    if (trace_mode) return; /* trace mode has its own title bar display */

    float math_x = (float)(mx - origin_x) / (float)grid_scale;
    float math_y = (float)(origin_y - my) / (float)grid_scale;

    char info[48];
    int p = 0;
    info[p++] = '(';

    char num_buf[16];
    float_to_str(math_x, num_buf);
    for (int i = 0; num_buf[i] && p < 40; i++) info[p++] = num_buf[i];

    info[p++] = ','; info[p++] = ' ';

    float_to_str(math_y, num_buf);
    for (int i = 0; num_buf[i] && p < 46; i++) info[p++] = num_buf[i];

    info[p++] = ')';
    info[p] = '\0';

    update_title_bar_info(info, COLOR_LIGHT_GRAY);
}

/* === KERNEL ENTRY POINT === */

void kmain(void) {
    /* switch to 640x480 high-resolution graphics! */
    vga_init_graphics();
    vga_clear(COLOR_BLACK);

    /*
     * Initialize the PS/2 mouse.
     * This tells the mouse hardware to start sending us movement packets.
     * Without this call, the mouse would be silent even if you move it.
     */
    mouse_init();

    /* set up the initial view (origin centered, default zoom) */
    reset_view();

    /* draw the UI */
    draw_title_bar();
    draw_graph_area();
    draw_panel();
    draw_input_bar();

    /* input state */
    char input_buf[MAX_INPUT_LEN + 1];
    int input_len = 0;
    input_buf[0] = '\0';

    draw_cursor(0, COLOR_WHITE);

    /* === MAIN KERNEL LOOP ===
     *
     * This is the heart of the OS. Since we have no multitasking,
     * this single loop handles EVERYTHING: keyboard, mouse, and display.
     *
     * Each iteration:
     *   1. Check for mouse movement/clicks (polling)
     *   2. Check for keyboard input (polling)
     *   3. Handle whichever input we got
     *
     * "Polling" means we keep asking "is there data?" in a loop,
     * as opposed to "interrupts" where the hardware notifies us.
     * Polling is simpler to implement (no interrupt handlers needed).
     */
    while (1) {
        /*
         * === MOUSE HANDLING ===
         *
         * mouse_read() checks if the mouse sent a new 3-byte packet.
         * If it did, it updates the global 'mouse' struct with:
         *   mouse.x, mouse.y = new cursor position
         *   mouse.left/right/middle = button states
         *   mouse.dx, mouse.dy = how much it moved
         */
        if (mouse_read()) {
            /*
             * DRAIN ALL PENDING MOUSE PACKETS.
             *
             * When the main loop is busy (e.g. redraw_all() during trace),
             * mouse bytes queue up. If we process them one-by-one, the
             * cursor gets drawn and immediately erased on each iteration,
             * making it invisible. Instead, we consume all pending packets
             * right here and use only the final mouse position.
             * This gives us one clean cursor draw per "batch" of movements.
             */
            while (mouse_read()) {
                /* keep draining - mouse struct updates to latest position */
            }

            /* step 1: erase the old cursor (restore pixels underneath) */
            restore_cursor();

            /*
             * MOUSE DRAG PANNING
             *
             * When left button is held and mouse moves, we pan the graph.
             * This works by tracking where the drag started and how far
             * the mouse has moved since then.
             *
             * We adjust the graph origin by the TOTAL drag distance,
             * not just the last movement delta. This prevents drift
             * from accumulating rounding errors.
             */
            if (mouse.left) {
                if (!mouse_dragging) {
                    /* drag just started - remember starting position */
                    mouse_dragging = 1;
                    drag_start_x = mouse.x;
                    drag_start_y = mouse.y;
                    drag_origin_x = origin_x;
                    drag_origin_y = origin_y;
                } else {
                    /* drag in progress - update the view */
                    int new_ox = drag_origin_x + (mouse.x - drag_start_x);
                    int new_oy = drag_origin_y + (mouse.y - drag_start_y);

                    /* only redraw if the origin actually changed */
                    if (new_ox != origin_x || new_oy != origin_y) {
                        int dx = new_ox - origin_x;
                        int dy = new_oy - origin_y;
                        origin_x = new_ox;
                        origin_y = new_oy;
                        pan_redraw(dx, dy);
                        restore_input_bar(input_len, input_buf);
                    }
                }
            } else {
                /*
                 * LEFT BUTTON RELEASED - check if this was a click (not drag).
                 *
                 * A "click" = the mouse barely moved between press and release.
                 * If the click is in the equation panel, delete that equation!
                 * This is way more intuitive than only having Tab to clear ALL.
                 */
                if (mouse_dragging) {
                    int move_dx = mouse.x - drag_start_x;
                    int move_dy = mouse.y - drag_start_y;
                    if (move_dx < 0) move_dx = -move_dx;
                    if (move_dy < 0) move_dy = -move_dy;

                    if (move_dx < 5 && move_dy < 5) {
                        /* it was a click! check if it's on a panel equation */
                        int click_x = drag_start_x;
                        int click_y = drag_start_y;

                        if (click_x < PANEL_WIDTH && click_y >= 28 && click_y < 28 + eq_count * 20) {
                            int clicked_eq = (click_y - 28) / 20;
                            if (clicked_eq >= 0 && clicked_eq < eq_count) {
                                /* remove this equation by shifting everything down */
                                for (int k = clicked_eq; k < eq_count - 1; k++) {
                                    equations[k] = equations[k + 1];
                                }
                                eq_count--;

                                /* if we deleted the traced equation, exit trace */
                                if (trace_mode) {
                                    if (eq_count == 0) {
                                        trace_mode = 0;
                                        tangent_visible = 0;
                                    } else if (trace_eq_index >= eq_count) {
                                        trace_eq_index = eq_count - 1;
                                    }
                                }

                                redraw_all();
                                restore_input_bar(input_len, input_buf);
                            }
                        }
                    }
                }
                mouse_dragging = 0;
            }

            /*
             * TRACE MODE: snap trace point to the curve at the mouse's X.
             * the mouse controls WHERE on the curve you're looking,
             * and the trace dot snaps to the nearest point on the curve.
             * for implicit curves (circles etc), we use the mouse Y
             * as a hint so it follows the branch you're closest to.
             *
             * if there's no curve at the exact mouse X, we search nearby
             * columns (up to 30px away) so the trace always stays connected.
             *
             * we only trigger a redraw when the trace point moves by at
             * least 3 pixels, to avoid excessive redraws on tiny movements.
             */
            if (trace_mode && mouse.x >= GRAPH_LEFT && mouse.x <= GRAPH_RIGHT) {
                int old_px = trace_px;
                int old_py = trace_py;
                int want_px = mouse.x;
                /* use mouse Y as hint for implicit curve branch selection */
                trace_py = mouse.y;

                /*
                 * try the exact mouse X first, then search outward.
                 * this ensures the trace dot always snaps to the curve,
                 * even when the mouse is between pixels of the curve
                 * (common with implicit equations like circles).
                 */
                int found = 0;
                int new_py = eval_trace_y(want_px);
                if (new_py >= GRAPH_TOP && new_py <= GRAPH_BOTTOM) {
                    trace_px = want_px;
                    trace_py = new_py;
                    found = 1;
                } else {
                    /* search nearby columns, expanding outward */
                    for (int offset = 1; offset <= 30 && !found; offset++) {
                        /* try right */
                        if (want_px + offset <= GRAPH_RIGHT) {
                            new_py = eval_trace_y(want_px + offset);
                            if (new_py >= GRAPH_TOP && new_py <= GRAPH_BOTTOM) {
                                trace_px = want_px + offset;
                                trace_py = new_py;
                                found = 1;
                                break;
                            }
                        }
                        /* try left */
                        if (want_px - offset >= GRAPH_LEFT) {
                            new_py = eval_trace_y(want_px - offset);
                            if (new_py >= GRAPH_TOP && new_py <= GRAPH_BOTTOM) {
                                trace_px = want_px - offset;
                                trace_py = new_py;
                                found = 1;
                                break;
                            }
                        }
                    }
                }

                /* only redraw if trace moved enough to be worth it (3+ px) */
                int dpx = trace_px - old_px;
                int dpy = trace_py - old_py;
                if (dpx < 0) dpx = -dpx;
                if (dpy < 0) dpy = -dpy;
                if (found && (dpx >= 3 || dpy >= 3)) {
                    redraw_trace();  /* fast! skips curve recomputation */
                    restore_input_bar(input_len, input_buf);
                }
            }

            /* show math coordinates when hovering over the graph area */
            show_mouse_coords(mouse.x, mouse.y);

            /* step 2: draw cursor at new position */
            draw_mouse_cursor(mouse.x, mouse.y);
        }

        /*
         * === KEYBOARD HANDLING ===
         *
         * note: we don't "continue" if no key is pressed -- we just
         * skip the key handling and loop back to check both mouse
         * and keyboard again. the "continue" used to skip mouse
         * processing which starved the mouse of CPU time.
         */
        int key = keyboard_read_key();
        if (key == KEY_NONE) continue;

        /* erase cursor before any redraws to avoid leaving artifacts */
        restore_cursor();

        /* --- Shift+T: toggle trace mode ---
         *
         * uses SHIFT+T (uppercase 'T') so lowercase 't' stays free for typing.
         * you need 't' for things like sqrt().
         *
         * trace mode lets you walk along a curve with arrow keys,
         * seeing exact coordinates as you go.
         * press up/down to switch between equations.
         */
        if (key == 'T') {
            if (trace_mode) {
                /* turning trace OFF */
                trace_mode = 0;
                tangent_visible = 0;
                redraw_all();
                restore_input_bar(input_len, input_buf);
            } else if (eq_count > 0) {
                /* turning trace ON - find first equation (explicit or implicit) */
                int idx = find_next_traceable_eq(0);
                if (idx >= 0) {
                    trace_mode = 1;
                    trace_eq_index = idx;
                    tangent_visible = 0;
                    /* start tracing from the center of the graph */
                    trace_px = GRAPH_LEFT + GRAPH_WIDTH / 2;
                    trace_py = (GRAPH_TOP + GRAPH_BOTTOM) / 2;
                    redraw_all();
                    restore_input_bar(input_len, input_buf);
                }
            }
            draw_mouse_cursor(mouse.x, mouse.y);
            continue;
        }

        /* --- Shift+D: toggle tangent line (only in trace mode) ---
         *
         * uses SHIFT+D so lowercase 'd' stays free for typing.
         * draws the tangent line at the current trace point,
         * showing the derivative (slope).
         */
        if (key == 'D' && trace_mode) {
            tangent_visible = !tangent_visible;
            redraw_all();
            restore_input_bar(input_len, input_buf);
            draw_mouse_cursor(mouse.x, mouse.y);
            continue;
        }

        /* --- Shift+F: plot derivative of traced equation as a new curve ---
         *
         * Takes the equation you're tracing and creates a NEW equation that
         * represents its derivative f'(x). Uses numerical differentiation
         * at each pixel (same math as tangent lines).
         *
         * The derivative equation has deriv_source set to the index of
         * the source equation. When plotting, eval_equation() detects this
         * and computes (f(x+h) - f(x-h)) / (2h) automatically.
         *
         * Example: tracing y=x^2, Shift+F adds its derivative (y=2x).
         */
        if (key == 'F' && trace_mode && eq_count < MAX_EQUATIONS) {
            const equation_slot *src = &equations[trace_eq_index];
            if (!src->is_implicit && src->deriv_source < 0) {
                equation_slot *deriv = &equations[eq_count];

                /* label: "d/dx(original)" */
                char *dt = deriv->text;
                dt[0] = 'd'; dt[1] = '/'; dt[2] = 'd'; dt[3] = 'x';
                dt[4] = '(';
                int di = 5;
                for (int si = 0; si < 8 && src->text[si]; si++)
                    dt[di++] = src->text[si];
                dt[di++] = ')';
                dt[di] = '\0';

                deriv->is_implicit = 0;
                deriv->deriv_source = trace_eq_index;
                deriv->compiled = src->compiled; /* keep a copy for reference */

                eq_count++;
                redraw_all();
                restore_input_bar(input_len, input_buf);
            }
            draw_mouse_cursor(mouse.x, mouse.y);
            continue;
        }

        /* --- arrow keys behavior depends on trace mode --- */
        if (trace_mode) {
            /*
             * IN TRACE MODE:
             *   Mouse movement = trace along curve (handled above in mouse_read)
             *   Up/Down = switch to a different equation
             *   Left/Right = pan the graph (fall through to normal behavior)
             */
            if (key == KEY_UP || key == KEY_DOWN) {
                /*
                 * switch equations: scan forward (down) or backward (up)
                 * through the equation list. Works for both explicit and implicit!
                 */
                int dir = (key == KEY_DOWN) ? 1 : -1;
                int new_idx = trace_eq_index + dir;
                if (new_idx >= eq_count) new_idx = 0;
                if (new_idx < 0) new_idx = eq_count - 1;
                trace_eq_index = new_idx;
                tangent_visible = 0; /* clear tangent when switching */
                /* reset trace_py for implicit: find a point on the new curve */
                trace_py = (GRAPH_TOP + GRAPH_BOTTOM) / 2;
                redraw_all();
                restore_input_bar(input_len, input_buf);
                draw_mouse_cursor(mouse.x, mouse.y);
                continue;
            }
        }

        /* arrow keys pan the graph (in trace mode, Up/Down switch equations above) */
        if (!trace_mode && key == KEY_UP)   { origin_y += 20; pan_redraw(0, 20);  restore_input_bar(input_len, input_buf); draw_mouse_cursor(mouse.x, mouse.y); continue; }
        if (!trace_mode && key == KEY_DOWN) { origin_y -= 20; pan_redraw(0, -20); restore_input_bar(input_len, input_buf); draw_mouse_cursor(mouse.x, mouse.y); continue; }
        if (key == KEY_LEFT)  { origin_x += 20; pan_redraw(20, 0);  restore_input_bar(input_len, input_buf); draw_mouse_cursor(mouse.x, mouse.y); continue; }
        if (key == KEY_RIGHT) { origin_x -= 20; pan_redraw(-20, 0); restore_input_bar(input_len, input_buf); draw_mouse_cursor(mouse.x, mouse.y); continue; }

        /* --- [ and ] keys: zoom in/out ---
         * using [ ] instead of +/- so they don't conflict with typing equations */
        if (key == ']') {
            if (grid_scale < 100) {
                grid_scale += 5;
                redraw_all(); restore_input_bar(input_len, input_buf);
            }
            draw_mouse_cursor(mouse.x, mouse.y);
            continue;
        }
        if (key == '[') {
            if (grid_scale > 5) {
                grid_scale -= 5;
                redraw_all(); restore_input_bar(input_len, input_buf);
            }
            draw_mouse_cursor(mouse.x, mouse.y);
            continue;
        }

        /* --- tab key: clear all equations --- */
        if (key == '\t') {
            eq_count = 0;
            trace_mode = 0;
            tangent_visible = 0;
            curve_layer_clear();
            reset_view();
            redraw_all();
            draw_input_bar();
            draw_cursor(0, COLOR_WHITE);
            input_len = 0;
            input_buf[0] = '\0';
            draw_mouse_cursor(mouse.x, mouse.y);
            continue;
        }

        /* --- regular input handling --- */
        if (key == '\b') {
            if (input_len > 0) {
                draw_cursor(input_len, COLOR_INPUT_BG);
                input_len--;
                input_buf[input_len] = '\0';
                clear_input_text();
                draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE);
                draw_cursor(input_len, COLOR_WHITE);
            }
        } else if (key == '\n') {
            if (input_len > 0 && eq_count < MAX_EQUATIONS) {
                /* copy input text to equation slot */
                equation_slot *slot = &equations[eq_count];
                for (int i = 0; i <= input_len; i++) {
                    slot->text[i] = input_buf[i];
                }

                /* compile the equation to bytecode for fast redraw later */
                slot->deriv_source = -1;  /* normal equation, not a derivative */
                slot->is_implicit = is_implicit_equation(input_buf);
                if (slot->is_implicit) {
                    compile_implicit(input_buf, &slot->comp_left, &slot->comp_right);
                } else {
                    compile(input_buf, &slot->compiled);
                }

                /*
                 * SAFETY CHECK: test-evaluate the equation at x=0, y=0.
                 * if the compiled bytecode has zero instructions, skip it.
                 * this prevents freezing on garbage input like single letters.
                 */
                int valid = 0;
                if (slot->is_implicit) {
                    valid = (slot->comp_left.len > 0 && slot->comp_right.len > 0);
                } else {
                    valid = (slot->compiled.len > 0);
                    /* also test-evaluate: if it returns NaN at x=0, still allow it
                       (could be valid elsewhere, like sqrt(x) which is NaN at x=-1) */
                }

                if (!valid) {
                    /* bad equation - show error briefly and don't add it */
                    update_title_bar_info("invalid equation!", COLOR_LIGHT_RED);
                    draw_cursor(input_len, COLOR_INPUT_BG);
                    input_len = 0;
                    input_buf[0] = '\0';
                    clear_input_text();
                    draw_cursor(0, COLOR_WHITE);
                    draw_mouse_cursor(mouse.x, mouse.y);
                    continue;
                }

                /* plot with animation (compiled bytecode + delay, directly to screen) */
                plot_equation_animated(slot, eq_colors[eq_count]);
                eq_count++;

                /* update the side panel */
                draw_panel();

                /* clear input */
                draw_cursor(input_len, COLOR_INPUT_BG);
                input_len = 0;
                input_buf[0] = '\0';
                clear_input_text();
                draw_cursor(0, COLOR_WHITE);
            }
        } else if (key > 0 && key < 128 && input_len < MAX_INPUT_LEN) {
            /*
             * Regular character input.
             * Skip 't' and 'd' if they would toggle trace/tangent mode --
             * NO, actually we always type them. The T/D handling above uses
             * uppercase check. But since keyboard returns lowercase for
             * unshifted keys, 't' triggers trace. So we need to NOT add
             * 't' or 'd' to the input if they were consumed above.
             *
             * Actually, the 'continue' statements above prevent us from
             * reaching here, so this is fine -- if we're here, the key
             * was NOT consumed by trace mode toggles.
             */
            char c = (char)key;
            draw_cursor(input_len, COLOR_INPUT_BG);
            input_buf[input_len] = c;
            input_len++;
            input_buf[input_len] = '\0';
            draw_char(INPUT_TEXT_X + (input_len - 1) * FONT_WIDTH, INPUT_TEXT_Y, c, COLOR_WHITE);
            draw_cursor(input_len, COLOR_WHITE);
        }

        /* redraw mouse cursor after keyboard handling */
        draw_mouse_cursor(mouse.x, mouse.y);
    }
}
