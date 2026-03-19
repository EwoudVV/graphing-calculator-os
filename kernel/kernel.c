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
 */

#include <stdint.h>
#include "keyboard.h"
#include "font.h"        /* includes vga.h and ports.h */
#include "math_parser.h" /* equation parser and evaluator */

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

static char equations[MAX_EQUATIONS][MAX_EQ_LEN + 1];
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
 * plot an explicit equation (y = f(x) style)
 * walks pixel by pixel left to right, evaluates, draws.
 */
static void plot_explicit(const char *equation, uint32_t color) {
    int prev_py = -1;

    for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px++) {
        float math_x = (float)(px - origin_x) / (float)grid_scale;
        float math_y = evaluate(equation, math_x);

        if (is_nan(math_y)) { prev_py = -1; continue; }

        int py = origin_y - (int)(math_y * (float)grid_scale);

        if (py >= GRAPH_TOP && py <= GRAPH_BOTTOM) {
            if (prev_py >= GRAPH_TOP && prev_py <= GRAPH_BOTTOM && prev_py != -1) {
                /* connect to previous point to fill gaps in steep curves */
                int y_start = (prev_py < py) ? prev_py : py;
                int y_end = (prev_py < py) ? py : prev_py;
                /* limit the gap fill so discontinuities don't draw huge lines */
                if (y_end - y_start < GRAPH_HEIGHT / 2) {
                    for (int y = y_start; y <= y_end; y++) {
                        if (y >= GRAPH_TOP && y <= GRAPH_BOTTOM)
                            vga_put_pixel(px, y, color);
                    }
                }
            } else {
                vga_put_pixel(px, py, color);
            }
        }
        prev_py = py;
    }
}

/*
 * plot an implicit equation (F(x,y) = 0 style)
 *
 * this is the COOL algorithm for drawing circles and other curves
 * that aren't functions (multiple y values per x).
 *
 * the idea: evaluate F(x,y) = left_side - right_side at every pixel.
 * wherever F changes sign between adjacent pixels, the curve crosses
 * through there - so we draw a pixel.
 *
 * it's like finding where the ground goes from above sea level to below:
 * the coastline is where it crosses zero.
 */
static void plot_implicit(const char *equation, uint32_t color) {
    /* step size = 1 pixel in math coordinates */
    float dx = 1.0f / (float)grid_scale;
    float dy = 1.0f / (float)grid_scale;

    /* evaluate every pixel in the graph area */
    for (int py = GRAPH_TOP; py <= GRAPH_BOTTOM; py += 1) {
        float my = (float)(origin_y - py) / (float)grid_scale;

        for (int px = GRAPH_LEFT; px <= GRAPH_RIGHT; px += 1) {
            float mx = (float)(px - origin_x) / (float)grid_scale;

            /* evaluate F at this point and its neighbors */
            float val   = evaluate_implicit(equation, mx, my);
            float val_r = evaluate_implicit(equation, mx + dx, my);
            float val_d = evaluate_implicit(equation, mx, my - dy);

            if (is_nan(val) || is_nan(val_r) || is_nan(val_d)) continue;

            /* if the sign changes, the curve passes through here! */
            if ((val >= 0) != (val_r >= 0) || (val >= 0) != (val_d >= 0)) {
                vga_put_pixel(px, py, color);
                /* draw a slightly thicker line for visibility */
                vga_put_pixel(px + 1, py, color);
                vga_put_pixel(px, py + 1, color);
            }
        }
    }
}

/* plot an equation (auto-detects explicit vs implicit) */
static void plot_equation(const char *equation, uint32_t color) {
    if (is_implicit_equation(equation)) {
        plot_implicit(equation, color);
    } else {
        plot_explicit(equation, color);
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
        for (j = 0; j < 13 && equations[i][j]; j++) {
            display[j] = equations[i][j];
        }
        display[j] = '\0';
        draw_string(42, y, display, eq_colors[i]);
    }

    /* show instructions at bottom of panel */
    int help_y = SCREEN_HEIGHT - 120;
    draw_string(8, help_y,      "Controls:", COLOR_GRAY);
    draw_string(8, help_y + 14, "arrows pan", COLOR_GRAY);
    draw_string(8, help_y + 28, "[ ] zoom", COLOR_GRAY);
    draw_string(8, help_y + 42, "Enter: plot", COLOR_GRAY);
    draw_string(8, help_y + 56, "Tab: clear", COLOR_GRAY);

    /* show current zoom level */
    char zoom_str[16] = "zoom: ";
    char zoom_num[8];
    int_to_str(grid_scale, zoom_num);
    int zi = 6;
    for (int i = 0; zoom_num[i] && zi < 15; i++) zoom_str[zi++] = zoom_num[i];
    zoom_str[zi] = '\0';
    draw_string(8, help_y + 76, zoom_str, COLOR_GRAY);
}

/* draw the title bar */
static void draw_title_bar(void) {
    vga_fill_rect(PANEL_WIDTH, 0, SCREEN_WIDTH - PANEL_WIDTH, TITLE_HEIGHT, COLOR_TITLE_BG);
    draw_string(PANEL_WIDTH + 8, 8, "GraphCalcOS", COLOR_LIGHT_GREEN);

    draw_string(PANEL_WIDTH + 200, 8, "type an equation and press enter!", COLOR_GRAY);
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

/* draw cursor */
static void draw_cursor(int char_pos, uint32_t color) {
    int x = INPUT_TEXT_X + char_pos * FONT_WIDTH;
    vga_fill_rect(x, INPUT_TEXT_Y, 2, FONT_HEIGHT, color);
}

/* redraw everything: grid, all equations, panel */
static void redraw_all(void) {
    draw_graph_area();
    for (int i = 0; i < eq_count; i++) {
        plot_equation(equations[i], eq_colors[i]);
    }
    draw_panel();
    draw_title_bar();
}

/* === KERNEL ENTRY POINT === */

void kmain(void) {
    /* switch to 640x480 high-resolution graphics! */
    vga_init_graphics();
    vga_clear(COLOR_BLACK);

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

    /* === MAIN KERNEL LOOP === */
    while (1) {
        int key = keyboard_read_key();
        if (key == KEY_NONE) continue;

        /* --- arrow keys: pan the graph --- */
        if (key == KEY_UP)    { origin_y += 20; redraw_all(); draw_input_bar(); draw_cursor(input_len, COLOR_WHITE); if (input_len > 0) draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE); continue; }
        if (key == KEY_DOWN)  { origin_y -= 20; redraw_all(); draw_input_bar(); draw_cursor(input_len, COLOR_WHITE); if (input_len > 0) draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE); continue; }
        if (key == KEY_LEFT)  { origin_x += 20; redraw_all(); draw_input_bar(); draw_cursor(input_len, COLOR_WHITE); if (input_len > 0) draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE); continue; }
        if (key == KEY_RIGHT) { origin_x -= 20; redraw_all(); draw_input_bar(); draw_cursor(input_len, COLOR_WHITE); if (input_len > 0) draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE); continue; }

        /* --- [ and ] keys: zoom in/out ---
         * using [ ] instead of +/- so they don't conflict with typing equations */
        if (key == ']') {
            if (grid_scale < 100) {
                grid_scale += 5;
                redraw_all(); draw_input_bar(); draw_cursor(input_len, COLOR_WHITE);
                if (input_len > 0) draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE);
            }
            continue;
        }
        if (key == '[') {
            if (grid_scale > 5) {
                grid_scale -= 5;
                redraw_all(); draw_input_bar(); draw_cursor(input_len, COLOR_WHITE);
                if (input_len > 0) draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE);
            }
            continue;
        }

        /* --- tab key: clear all equations --- */
        if (key == '\t') {
            eq_count = 0;
            reset_view();
            redraw_all();
            draw_input_bar();
            draw_cursor(0, COLOR_WHITE);
            input_len = 0;
            input_buf[0] = '\0';
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
                /* copy input to equation list */
                for (int i = 0; i <= input_len; i++) {
                    equations[eq_count][i] = input_buf[i];
                }

                /* plot the new equation */
                plot_equation(equations[eq_count], eq_colors[eq_count]);
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
            /* regular character */
            char c = (char)key;
            draw_cursor(input_len, COLOR_INPUT_BG);
            input_buf[input_len] = c;
            input_len++;
            input_buf[input_len] = '\0';
            draw_char(INPUT_TEXT_X + (input_len - 1) * FONT_WIDTH, INPUT_TEXT_Y, c, COLOR_WHITE);
            draw_cursor(input_len, COLOR_WHITE);
        }
    }
}
