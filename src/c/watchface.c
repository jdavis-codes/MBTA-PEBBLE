#include <pebble.h>

// The phone sends a few more departures than can be shown so the row stays
// full as trains leave between refreshes.
#define NUM_TIMES 7
#define MIN_DEPARTURES 1
#define MAX_DEPARTURES 5
#define DEFAULT_DEPARTURES 3
#define REFRESH_MINUTES 2
#define PERSIST_KEY_DATA 1
#define PERSIST_KEY_SETTINGS 2
#define PERSIST_KEY_DIAG 3

// Diagnostics. A breadcrumb in persistent storage records what the app was
// doing, so that after a crash or a watch reset the next launch can report it
// to the phone (and to `pebble logs`). The live state goes out once a minute as
// a heartbeat log line.
#define DIAG_VERSION 1
#define DIAG_PERSIST_MINUTES 10
enum { STAGE_INIT, STAGE_RUNNING };

typedef struct {
  uint32_t launches;
  uint32_t started;
  uint32_t updated;
  uint16_t max_draw_ms;
  uint16_t heap_free;
  uint8_t stage;
  uint8_t detail;
  uint8_t clean_exit;
  uint8_t version;
} Diag;

#define TIME_HEIGHT 45
#define HEADER_HEIGHT 15
#define MIN_CELL_GAP 2
#define SUFFIX_GAP 2
// Phone connection dot in the top right corner: filled when connected, hollow when not.
#define BT_DOT_RADIUS 3
#define BT_DOT_MARGIN 3
// Fetch status dot, left of the connection dot: steady after a good fetch,
// pulsing while waiting for the phone, an X when the fetch failed.
#define FETCH_DOT_SPACING 11
#define FETCH_PULSE_MS 150
// The phone gives up on a hung fetch after 30s; allow for that plus delivery.
#define FETCH_TIMEOUT_MS 40000

// One section per departure row (header + times); must match ROWS in src/pkjs/config.js.
#define NUM_SECTIONS 3
// Includes the terminator; the settings page limits labels to 23 characters.
#define LABEL_SIZE 24

// How a row shows departures; must match MODE_* in src/pkjs/index.js.
enum { MODE_CLOCK, MODE_MINUTES };

// Per-departure status letters; must match FLAG_* in src/pkjs/index.js.
#define FLAG_NONE ' '
#define FLAG_CANCELLED 'X'
#define FLAG_DELAYED 'D'
// Gap between the diagonal lines hatched behind a delayed departure.
#define HATCH_SPACING 5

typedef enum { FETCH_OK, FETCH_PENDING, FETCH_FAILED } FetchState;

// A departure blinks while inside its section's warning range, speeding up
// from BLINK_SLOW_MS to BLINK_FAST_MS per toggle as the range end approaches.
#define BLINK_SLOW_MS 1000
#define BLINK_FAST_MS 125

typedef struct {
  int32_t times[NUM_SECTIONS][NUM_TIMES];
  // One destination letter per departure, or ' ' for none.
  char dest[NUM_SECTIONS][NUM_TIMES + 1];
  uint8_t mode[NUM_SECTIONS];
  // One FLAG_* per departure.
  char flags[NUM_SECTIONS][NUM_TIMES + 1];
} TransitData;

typedef struct {
  bool warn_on[NUM_SECTIONS];
  uint8_t warn_start_min[NUM_SECTIONS];
  uint8_t warn_end_min[NUM_SECTIONS];
  // New fields go at the end so older persisted settings still load.
  uint8_t departures;
  char labels[NUM_SECTIONS][LABEL_SIZE];
} Settings;

typedef struct {
  bool active;
  bool inverted;
  int64_t next_toggle_ms;
} BlinkState;

// A departure row's font plus its vertical metrics. text_dy/suffix_dy offset the
// text box from the row top to compensate for the fonts' built-in top padding.
typedef struct {
  const char *key;
  const char *suffix_key;
  int row_h;
  int text_dy;
  int suffix_dy;
} RowFontSpec;

static const RowFontSpec s_row_font_specs[] = {
  { FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_14_BOLD, 26, -4, 5 },
  { FONT_KEY_GOTHIC_18_BOLD, FONT_KEY_GOTHIC_14_BOLD, 20, -3, 1 },
  { FONT_KEY_GOTHIC_18,      FONT_KEY_GOTHIC_14,      20, -3, 1 },
  { FONT_KEY_GOTHIC_14_BOLD, FONT_KEY_GOTHIC_14_BOLD, 16, -2, -2 },
  { FONT_KEY_GOTHIC_14,      FONT_KEY_GOTHIC_14,      16, -2, -2 },
};
#define NUM_ROW_FONTS ((int)ARRAY_LENGTH(s_row_font_specs))

typedef struct {
  GFont font;
  GFont suffix_font;
  const RowFontSpec *spec;
} RowFont;

// One departure as displayed: main text plus an optional small suffix letter.
typedef struct {
  int32_t departure;
  char text[12];
  char suffix[2];
  char flag;
} Cell;

static Window *s_window;
static Layer *s_canvas;
static TransitData s_data;
static Settings s_settings;
static bool s_has_data;
static bool s_connected;
static FetchState s_fetch_state;
static AppTimer *s_fetch_timer;
static int s_fetch_ticks;
static time_t s_last_request;
static Diag s_diag;
// The previous run's breadcrumb as text, until the phone has received it.
static char s_prev_diag[80];
static bool s_prev_diag_pending;
static uint16_t s_max_draw_ms;
static char s_time_buffer[8];
static AppTimer *s_blink_timer;
static BlinkState s_blink[NUM_SECTIONS];

static GFont s_time_font;
static GFont s_header_font;
static RowFont s_row_fonts[ARRAY_LENGTH(s_row_font_specs)];

static void prv_update_time_string(struct tm *tick_time) {
  if (clock_is_24h_style()) {
    strftime(s_time_buffer, sizeof(s_time_buffer), "%H:%M", tick_time);
  } else {
    strftime(s_time_buffer, sizeof(s_time_buffer), "%I:%M", tick_time);
    if (s_time_buffer[0] == '0') {
      memmove(s_time_buffer, s_time_buffer + 1, strlen(s_time_buffer));
    }
  }
}

static void prv_format_clock(time_t t, char *buf, size_t size) {
  struct tm *tm = localtime(&t);
  if (clock_is_24h_style()) {
    strftime(buf, size, "%H:%M", tm);
  } else {
    strftime(buf, size, "%I:%M", tm);
    if (buf[0] == '0') {
      memmove(buf, buf + 1, strlen(buf));
    }
  }
}

// Collects up to max upcoming (not yet departed) times into out[]; returns the
// count. out_dest and out_flags are optional.
static int prv_upcoming(int section, time_t now, int32_t *out, char *out_dest, char *out_flags,
                        int max) {
  const int32_t *times = s_data.times[section];
  int n = 0;
  for (int i = 0; i < NUM_TIMES && n < max; i++) {
    if (times[i] > now) {
      out[n] = times[i];
      if (out_dest) {
        out_dest[n] = s_data.dest[section][i];
      }
      if (out_flags) {
        out_flags[n] = s_data.flags[section][i];
      }
      n++;
    }
  }
  return n;
}

static void prv_draw_text(GContext *ctx, const char *text, GFont font, GRect box,
                          GTextAlignment align) {
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeTrailingEllipsis, align, NULL);
}

static int prv_text_width(const char *text, GFont font);

// Copies label into out, shortening the part before " > " with an ellipsis until
// it fits in max_w so the direction stays visible.
static void prv_fit_label(const char *label, GFont font, int max_w, char *out, size_t size) {
  snprintf(out, size, "%s", label);
  const char *sep = strstr(label, " > ");
  if (!sep || prv_text_width(out, font) <= max_w) {
    return;
  }
  int station_len = sep - label;
  while (station_len > 1) {
    station_len--;
    while (station_len > 1 && label[station_len - 1] == ' ') {
      station_len--;
    }
    snprintf(out, size, "%.*s\xE2\x80\xA6%s", station_len, label, sep);
    if (prv_text_width(out, font) <= max_w) {
      return;
    }
  }
}

// Draws a header bar; a long left label is shortened to fit before the right text.
static void prv_draw_header(GContext *ctx, int y, int width, const char *left, const char *right) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, y, width, HEADER_HEIGHT), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  int left_w = width - 6;
  if (right && right[0]) {
    prv_draw_text(ctx, right, s_header_font, GRect(3, y - 3, width - 6, HEADER_HEIGHT + 4),
                  GTextAlignmentRight);
    left_w -= prv_text_width(right, s_header_font) + 4;
  }
  char label[LABEL_SIZE + 4];
  prv_fit_label(left, s_header_font, left_w, label, sizeof(label));
  prv_draw_text(ctx, label, s_header_font, GRect(3, y - 3, left_w, HEADER_HEIGHT + 4),
                GTextAlignmentLeft);
}

static void prv_format_wait(time_t t, time_t now, char *buf, size_t size) {
  int mins = (int)((t - now) / 60);
  if (mins < 1) {
    snprintf(buf, size, "now");
  } else if (mins < 60) {
    snprintf(buf, size, "%dm", mins);
  } else {
    snprintf(buf, size, "%dh%02dm", mins / 60, mins % 60);
  }
}

static int64_t prv_now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return (int64_t)sec * 1000 + ms;
}

// Gets a section's warning range in ms before departure; false if disabled or empty.
static bool prv_warning_range(int section, int64_t *lo_ms, int64_t *hi_ms) {
  if (!s_settings.warn_on[section]) {
    return false;
  }
  int a = s_settings.warn_start_min[section];
  int b = s_settings.warn_end_min[section];
  *lo_ms = (int64_t)(a < b ? a : b) * 60000;
  *hi_ms = (int64_t)(a < b ? b : a) * 60000;
  return *hi_ms > *lo_ms;
}

static bool prv_in_blink_window(int section, int32_t departure, int64_t now_ms) {
  int64_t lo, hi;
  if (departure <= 0 || !prv_warning_range(section, &lo, &hi)) {
    return false;
  }
  int64_t remaining = (int64_t)departure * 1000 - now_ms;
  return remaining > lo && remaining <= hi;
}

static bool prv_is_highlighted(int section, int32_t departure, int64_t now_ms) {
  return s_blink[section].inverted && prv_in_blink_window(section, departure, now_ms);
}

static int prv_departures(void) {
  int n = s_settings.departures;
  return n < MIN_DEPARTURES ? MIN_DEPARTURES : n > MAX_DEPARTURES ? MAX_DEPARTURES : n;
}

// Builds the visible cells for a section; returns how many there are.
static int prv_build_cells(int section, time_t now, Cell *cells) {
  int32_t upcoming[MAX_DEPARTURES];
  char dest[MAX_DEPARTURES];
  char flags[MAX_DEPARTURES];
  bool minutes = s_data.mode[section] == MODE_MINUTES;
  int n = prv_upcoming(section, now, upcoming, dest, flags, prv_departures());
  for (int i = 0; i < n; i++) {
    Cell *cell = &cells[i];
    cell->departure = upcoming[i];
    cell->flag = flags[i];
    cell->suffix[0] = '\0';
    if (minutes) {
      int mins = (int)((upcoming[i] - now) / 60);
      if (mins < 1) {
        snprintf(cell->text, sizeof(cell->text), "now");
      } else {
        snprintf(cell->text, sizeof(cell->text), "%d", mins);
      }
    } else {
      prv_format_clock(upcoming[i], cell->text, sizeof(cell->text));
    }
    cell->suffix[0] = dest[i] == ' ' ? '\0' : dest[i];
    cell->suffix[1] = '\0';
  }
  return n;
}

static int prv_text_width(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 200, 40),
                                               GTextOverflowModeWordWrap,
                                               GTextAlignmentLeft).w;
}

static int prv_cell_width(const Cell *cell, const RowFont *rf, int *text_w) {
  *text_w = prv_text_width(cell->text, rf->font);
  if (!cell->suffix[0]) {
    return *text_w;
  }
  return *text_w + SUFFIX_GAP + prv_text_width(cell->suffix, rf->suffix_font);
}

// The row is a timetable with one equal-width column per departure shown, so
// times stay put as trains run out. Returns the left edge of a column.
static int prv_column_x(int column, int width) {
  return column * width / prv_departures();
}

// Picks the largest font allowed for the departure count that fits every
// departure inside its column.
static const RowFont *prv_pick_row_font(Cell cells[][MAX_DEPARTURES], const int *counts,
                                        int width) {
  int count = prv_departures();
  int first = count <= 3 ? 0 : count == 4 ? 1 : 3;
  for (int f = first; f < NUM_ROW_FONTS; f++) {
    bool fits = true;
    for (int section = 0; section < NUM_SECTIONS && fits; section++) {
      for (int i = 0; i < counts[section] && fits; i++) {
        int text_w;
        int column_w = prv_column_x(i + 1, width) - prv_column_x(i, width);
        fits = prv_cell_width(&cells[section][i], &s_row_fonts[f], &text_w) + MIN_CELL_GAP
               <= column_w;
      }
    }
    if (fits) {
      return &s_row_fonts[f];
    }
  }
  return &s_row_fonts[NUM_ROW_FONTS - 1];
}

// Fills the columns left empty once trains run out with the longest message
// that fits, in the row font or the largest smaller one.
static void prv_draw_no_more(GContext *ctx, GRect space, const RowFont *rf, bool any_trains) {
  static const char *const s_some[] = { "No more trains", "No more", "-" };
  static const char *const s_none[] = { "No trains" };
  const char *const *texts = any_trains ? s_some : s_none;
  int num_texts = any_trains ? (int)ARRAY_LENGTH(s_some) : (int)ARRAY_LENGTH(s_none);
  if (!s_has_data) {
    static const char *const s_loading[] = { "Loading..." };
    texts = s_loading;
    num_texts = 1;
  }
  const char *text = texts[num_texts - 1];
  const RowFont *font = &s_row_fonts[NUM_ROW_FONTS - 1];
  bool found = false;
  for (int t = 0; t < num_texts && !found; t++) {
    for (const RowFont *f = rf; f < s_row_fonts + NUM_ROW_FONTS && !found; f++) {
      if (prv_text_width(texts[t], f->font) + 2 * MIN_CELL_GAP <= space.size.w) {
        text = texts[t];
        font = f;
        found = true;
      }
    }
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  // A smaller font is centred vertically in the taller row.
  int y = space.origin.y + (space.size.h - font->spec->row_h) / 2 + font->spec->text_dy;
  prv_draw_text(ctx, text, font->font, GRect(space.origin.x, y, space.size.w, font->spec->row_h + 8),
                GTextAlignmentCenter);
}

static GRect prv_shift(GRect r, int dx, int dy) {
  r.origin.x += dx;
  r.origin.y += dy;
  return r;
}

static void prv_draw_cell_text(GContext *ctx, const Cell *cell, const RowFont *rf, GRect text_box,
                               GRect suffix_box) {
  prv_draw_text(ctx, cell->text, rf->font, text_box, GTextAlignmentLeft);
  if (cell->suffix[0]) {
    prv_draw_text(ctx, cell->suffix, rf->suffix_font, suffix_box, GTextAlignmentLeft);
  }
}

// Fills a rect with "/" diagonal lines, clipped to it.
static void prv_draw_hatch(GContext *ctx, GRect r, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  int h = r.size.h;
  for (int off = HATCH_SPACING - h; off < r.size.w; off += HATCH_SPACING) {
    // The line runs from (off, bottom) up to (off + h - 1, top).
    int x0 = off, y0 = h - 1, x1 = off + h - 1, y1 = 0;
    if (x0 < 0) {
      y0 += x0;
      x0 = 0;
    }
    if (x1 > r.size.w - 1) {
      y1 += x1 - (r.size.w - 1);
      x1 = r.size.w - 1;
    }
    if (y0 >= y1) {
      graphics_draw_line(ctx, GPoint(r.origin.x + x0, r.origin.y + y0),
                         GPoint(r.origin.x + x1, r.origin.y + y1));
    }
  }
}

// Draws a row of cells, one per column; a blinking cell fills its whole column.
static void prv_draw_row(GContext *ctx, int row_y, int width, int section, const Cell *cells,
                         int n, const RowFont *rf, int64_t now_ms) {
  const RowFontSpec *spec = rf->spec;
  for (int i = 0; i < n; i++) {
    int x = prv_column_x(i, width);
    int slot = prv_column_x(i + 1, width) - x;
    int text_w;
    int cell_w = prv_cell_width(&cells[i], rf, &text_w);
    bool cancelled = cells[i].flag == FLAG_CANCELLED;
    bool delayed = cells[i].flag == FLAG_DELAYED;
    bool inverted = !cancelled && prv_is_highlighted(section, cells[i].departure, now_ms);
    GColor fg = inverted ? GColorBlack : GColorWhite;
    GColor bg = inverted ? GColorWhite : GColorBlack;
    GRect slot_rect = GRect(x, row_y, slot, spec->row_h);
    if (inverted) {
      graphics_context_set_fill_color(ctx, bg);
      graphics_fill_rect(ctx, slot_rect, 0, GCornerNone);
    }
    int tx = x + (slot - cell_w) / 2;
    GRect text_box = GRect(tx, row_y + spec->text_dy, text_w + 4, spec->row_h + 8);
    GRect suffix_box = GRect(tx + text_w + SUFFIX_GAP, row_y + spec->suffix_dy, 20, 20);
    if (delayed) {
      prv_draw_hatch(ctx, slot_rect, fg);
      // Outline the text in the background colour so it stays readable.
      graphics_context_set_text_color(ctx, bg);
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          if (dx || dy) {
            prv_draw_cell_text(ctx, &cells[i], rf, prv_shift(text_box, dx, dy),
                               prv_shift(suffix_box, dx, dy));
          }
        }
      }
    }
    graphics_context_set_text_color(ctx, fg);
    prv_draw_cell_text(ctx, &cells[i], rf, text_box, suffix_box);
    if (cancelled) {
      // Cross the departure out, corner to corner of its text.
      int top = row_y + 3;
      int bottom = row_y + spec->row_h - 4;
      // A thin line edged in the background colour, so it cuts visibly through the digits.
      for (int pass = 0; pass < 2; pass++) {
        graphics_context_set_stroke_color(ctx, pass == 0 ? bg : fg);
        graphics_context_set_stroke_width(ctx, pass == 0 ? 3 : 1);
        graphics_draw_line(ctx, GPoint(tx, top), GPoint(tx + cell_w, bottom));
        graphics_draw_line(ctx, GPoint(tx, bottom), GPoint(tx + cell_w, top));
      }
    }
  }
  if (n < prv_departures()) {
    int x = prv_column_x(n, width);
    prv_draw_no_more(ctx, GRect(x, row_y, width - x, spec->row_h), rf, n > 0);
  }
}

static uint16_t prv_cap16(int32_t v) {
  return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v;
}

// Records what the app is doing. Written to flash, so only called on a change
// of stage and every DIAG_PERSIST_MINUTES.
static void prv_breadcrumb(uint8_t stage, uint8_t detail) {
  s_diag.stage = stage;
  s_diag.detail = detail;
  s_diag.updated = (uint32_t)time(NULL);
  if (s_max_draw_ms > s_diag.max_draw_ms) {
    s_diag.max_draw_ms = s_max_draw_ms;
  }
  s_diag.heap_free = prv_cap16((int32_t)heap_bytes_free());
  persist_write_data(PERSIST_KEY_DIAG, &s_diag, sizeof(s_diag));
}

// Reads the previous run's breadcrumb and starts this run's.
static void prv_diag_init(void) {
  Diag prev;
  uint32_t launches = 0;
  if (persist_read_data(PERSIST_KEY_DIAG, &prev, sizeof(prev)) == (int)sizeof(prev) &&
      prev.version == DIAG_VERSION) {
    launches = prev.launches;
    snprintf(s_prev_diag, sizeof(s_prev_diag),
             "prev %s stage=%d.%d ran=%lds draw=%dms heap=%d launch=%lu",
             prev.clean_exit ? "clean" : "UNCLEAN", prev.stage, prev.detail,
             (long)(prev.updated - prev.started), prev.max_draw_ms, prev.heap_free,
             (unsigned long)prev.launches);
    s_prev_diag_pending = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "%s", s_prev_diag);
  }
  s_diag = (Diag) {
    .launches = launches + 1,
    .started = (uint32_t)time(NULL),
    .version = DIAG_VERSION,
  };
  prv_breadcrumb(STAGE_INIT, 0);
}

static void prv_canvas_draw(Layer *layer, GContext *ctx);

// Times each redraw; the slowest since the last heartbeat is reported.
static void prv_canvas_update(Layer *layer, GContext *ctx) {
  int64_t start_ms = prv_now_ms();
  prv_canvas_draw(layer, ctx);
  uint16_t took = prv_cap16((int32_t)(prv_now_ms() - start_ms));
  if (took > s_max_draw_ms) {
    s_max_draw_ms = took;
  }
}

static void prv_canvas_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int width = bounds.size.w;
  time_t now = time(NULL);
  int64_t now_ms = prv_now_ms();

  // Static: the app stack is only about 2KB, and the text drawing below needs most of it.
  static Cell cells[NUM_SECTIONS][MAX_DEPARTURES];
  static int counts[NUM_SECTIONS];
  for (int section = 0; section < NUM_SECTIONS; section++) {
    counts[section] = prv_build_cells(section, now, cells[section]);
  }
  const RowFont *rf = prv_pick_row_font(cells, counts, width);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Sections stack from the bottom; the clock is centred in whatever space remains.
  int section_h = HEADER_HEIGHT + rf->spec->row_h;
  int sections_top = bounds.size.h - NUM_SECTIONS * section_h;
  graphics_context_set_text_color(ctx, GColorWhite);
  prv_draw_text(ctx, s_time_buffer, s_time_font,
                GRect(0, (sections_top - TIME_HEIGHT) / 2, width, TIME_HEIGHT),
                GTextAlignmentCenter);

  GPoint dot = GPoint(width - BT_DOT_MARGIN - BT_DOT_RADIUS - 1, BT_DOT_MARGIN + BT_DOT_RADIUS);
  if (s_connected) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, dot, BT_DOT_RADIUS);
  } else {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_circle(ctx, dot, BT_DOT_RADIUS);
  }

  dot.x -= FETCH_DOT_SPACING;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  if (s_fetch_state == FETCH_FAILED) {
    int r = BT_DOT_RADIUS;
    graphics_draw_line(ctx, GPoint(dot.x - r, dot.y - r), GPoint(dot.x + r, dot.y + r));
    graphics_draw_line(ctx, GPoint(dot.x - r, dot.y + r), GPoint(dot.x + r, dot.y - r));
  } else if (s_fetch_state == FETCH_PENDING) {
    // Radius runs 0..3..0 so the dot grows and shrinks.
    int phase = s_fetch_ticks % (2 * BT_DOT_RADIUS);
    int r = phase <= BT_DOT_RADIUS ? phase : 2 * BT_DOT_RADIUS - phase;
    if (r > 0) {
      graphics_fill_circle(ctx, dot, r);
    } else {
      graphics_draw_pixel(ctx, dot);
    }
  } else {
    graphics_fill_circle(ctx, dot, BT_DOT_RADIUS);
  }

  for (int section = 0; section < NUM_SECTIONS; section++) {
    int y = sections_top + section * section_h;
    char right[16] = "";
    // The header counts down to the next train that is actually running.
    for (int i = 0; i < counts[section]; i++) {
      if (cells[section][i].flag != FLAG_CANCELLED) {
        prv_format_wait(cells[section][i].departure, now, right, sizeof(right));
        break;
      }
    }
    prv_draw_header(ctx, y, width, s_settings.labels[section], right);
    prv_draw_row(ctx, y + HEADER_HEIGHT, width, section, cells[section], counts[section], rf,
                 now_ms);
  }
}

static void prv_blink_timer_cb(void *context);

// Advances every section's blink state, then schedules the timer for the next
// toggle or for the moment the next departure enters a warning range.
static void prv_update_blink(void) {
  int64_t now_ms = prv_now_ms();
  // Waits longer than a minute are picked up by the minute tick instead.
  const int64_t horizon_ms = now_ms + 61000;
  int64_t next_wake = horizon_ms;
  bool changed = false;

  for (int section = 0; section < NUM_SECTIONS; section++) {
    BlinkState *blink = &s_blink[section];
    bool was_inverted = blink->inverted;
    int64_t lo, hi;
    int64_t nearest_in_range = -1;
    int64_t until_start = -1;

    if (prv_warning_range(section, &lo, &hi)) {
      // Only departures that are actually on screen can blink.
      int32_t visible[MAX_DEPARTURES];
      char flags[MAX_DEPARTURES];
      int n = prv_upcoming(section, (time_t)(now_ms / 1000), visible, NULL, flags,
                           prv_departures());
      for (int i = 0; i < n; i++) {
        if (flags[i] == FLAG_CANCELLED) {
          continue;
        }
        int64_t remaining = (int64_t)visible[i] * 1000 - now_ms;
        if (remaining > lo && remaining <= hi) {
          if (nearest_in_range < 0 || remaining < nearest_in_range) {
            nearest_in_range = remaining;
          }
        } else if (remaining > hi) {
          int64_t wait = remaining - hi;
          if (until_start < 0 || wait < until_start) {
            until_start = wait;
          }
        }
      }
    }

    if (nearest_in_range >= 0) {
      if (!blink->active || now_ms >= blink->next_toggle_ms) {
        blink->inverted = blink->active ? !blink->inverted : true;
        blink->active = true;
        int64_t elapsed = hi - nearest_in_range;
        int64_t interval = BLINK_SLOW_MS - elapsed * (BLINK_SLOW_MS - BLINK_FAST_MS) / (hi - lo);
        blink->next_toggle_ms = now_ms + interval;
      }
      if (blink->next_toggle_ms < next_wake) {
        next_wake = blink->next_toggle_ms;
      }
    } else {
      blink->active = false;
      blink->inverted = false;
      if (until_start >= 0 && now_ms + until_start + 1 < next_wake) {
        next_wake = now_ms + until_start + 1;
      }
    }
    changed |= blink->inverted != was_inverted;
  }

  if (s_blink_timer) {
    app_timer_cancel(s_blink_timer);
    s_blink_timer = NULL;
  }
  if (next_wake < horizon_ms) {
    int64_t delay = next_wake - now_ms;
    s_blink_timer = app_timer_register(delay > 1 ? (uint32_t)delay : 1, prv_blink_timer_cb, NULL);
  }
  if (changed && s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void prv_blink_timer_cb(void *context) {
  s_blink_timer = NULL;
  prv_update_blink();
}

static void prv_default_settings(void) {
  s_settings = (Settings) {
    .warn_on = { true, false, false },
    .warn_start_min = { 30, 15, 10 },
    .warn_end_min = { 25, 10, 5 },
    .departures = DEFAULT_DEPARTURES,
    .labels = { "NORTH STA > OUT", "SALEM > IN", "CENTRAL > SOUTH" },
  };
}

static void prv_load_settings(void) {
  prv_default_settings();
  persist_read_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));
  for (int section = 0; section < NUM_SECTIONS; section++) {
    s_settings.labels[section][LABEL_SIZE - 1] = '\0';
  }
}

// Clay may send integers of varying widths (or numeric strings).
static int32_t prv_tuple_int(const Tuple *t) {
  switch (t->type) {
    case TUPLE_CSTRING:
      return atoi(t->value->cstring);
    case TUPLE_INT:
      return t->length == 1 ? t->value->int8 : t->length == 2 ? t->value->int16 : t->value->int32;
    case TUPLE_UINT:
      return t->length == 1 ? t->value->uint8 : t->length == 2 ? t->value->uint16
                                                                : (int32_t)t->value->uint32;
    default:
      return 0;
  }
}

static uint8_t prv_clamp_minutes(int32_t v) {
  return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static bool prv_read_settings(DictionaryIterator *iter) {
  bool changed = false;
  for (int section = 0; section < NUM_SECTIONS; section++) {
    Tuple *t;
    if ((t = dict_find(iter, MESSAGE_KEY_WARN_ON + section))) {
      s_settings.warn_on[section] = prv_tuple_int(t) != 0;
      changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_WARN_START + section))) {
      s_settings.warn_start_min[section] = prv_clamp_minutes(prv_tuple_int(t));
      changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_WARN_END + section))) {
      s_settings.warn_end_min[section] = prv_clamp_minutes(prv_tuple_int(t));
      changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_LABEL + section)) && t->type == TUPLE_CSTRING) {
      strncpy(s_settings.labels[section], t->value->cstring, LABEL_SIZE - 1);
      s_settings.labels[section][LABEL_SIZE - 1] = '\0';
      changed = true;
    }
  }
  Tuple *t = dict_find(iter, MESSAGE_KEY_DEPARTURES);
  if (t) {
    int32_t v = prv_tuple_int(t);
    s_settings.departures = v < MIN_DEPARTURES ? MIN_DEPARTURES
                          : v > MAX_DEPARTURES ? MAX_DEPARTURES : (uint8_t)v;
    changed = true;
  }
  if (changed) {
    persist_write_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));
  }
  return changed;
}

static void prv_fetch_timer_cb(void *context);

static void prv_set_fetch_state(FetchState state) {
  if (s_fetch_timer) {
    app_timer_cancel(s_fetch_timer);
    s_fetch_timer = NULL;
  }
  s_fetch_state = state;
  s_fetch_ticks = 0;
  if (state == FETCH_PENDING) {
    s_fetch_timer = app_timer_register(FETCH_PULSE_MS, prv_fetch_timer_cb, NULL);
  }
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

// Animates the pulse, and fails the fetch if the phone never answers.
static void prv_fetch_timer_cb(void *context) {
  s_fetch_timer = NULL;
  if (++s_fetch_ticks * FETCH_PULSE_MS >= FETCH_TIMEOUT_MS) {
    prv_set_fetch_state(FETCH_FAILED);
    return;
  }
  s_fetch_timer = app_timer_register(FETCH_PULSE_MS, prv_fetch_timer_cb, NULL);
  layer_mark_dirty(s_canvas);
}

static void prv_request_update(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    prv_set_fetch_state(FETCH_FAILED);
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
  prv_set_fetch_state(app_message_outbox_send() == APP_MSG_OK ? FETCH_PENDING : FETCH_FAILED);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed reason=%d", (int)reason);
  // Only a failed departure request fails the fetch, not a failed diagnostic.
  if (dict_find(iter, MESSAGE_KEY_REQUEST)) {
    prv_set_fetch_state(FETCH_FAILED);
  }
}

static void prv_outbox_sent(DictionaryIterator *iter, void *context) {
  if (dict_find(iter, MESSAGE_KEY_DIAG)) {
    s_prev_diag_pending = false;
  }
}

// Sends the previous run's breadcrumb to the phone, which logs and keeps it.
static void prv_send_prev_diag(void) {
  DictionaryIterator *iter;
  if (!s_prev_diag_pending || app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_cstring(iter, MESSAGE_KEY_DIAG, s_prev_diag);
  app_message_outbox_send();
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  bool changed = false;
  for (int section = 0; section < NUM_SECTIONS; section++) {
    Tuple *t;
    bool got_times = false;
    for (int i = 0; i < NUM_TIMES; i++) {
      if ((t = dict_find(iter, MESSAGE_KEY_TIMES + section * NUM_TIMES + i))) {
        s_data.times[section][i] = prv_tuple_int(t);
        got_times = true;
        changed = true;
      }
    }
    // Branch letters belong to the times they arrived with; new times without
    // letters mean the row has none.
    if (got_times) {
      memset(s_data.dest[section], ' ', NUM_TIMES);
      s_data.dest[section][NUM_TIMES] = '\0';
    }
    if ((t = dict_find(iter, MESSAGE_KEY_DEST + section)) && t->type == TUPLE_CSTRING) {
      char *dest = s_data.dest[section];
      memset(dest, ' ', NUM_TIMES);
      size_t len = strlen(t->value->cstring);
      memcpy(dest, t->value->cstring, len < NUM_TIMES ? len : NUM_TIMES);
      dest[NUM_TIMES] = '\0';
      changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_FLAGS + section)) && t->type == TUPLE_CSTRING) {
      char *flags = s_data.flags[section];
      memset(flags, FLAG_NONE, NUM_TIMES);
      size_t len = strlen(t->value->cstring);
      memcpy(flags, t->value->cstring, len < NUM_TIMES ? len : NUM_TIMES);
      flags[NUM_TIMES] = '\0';
      changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_MODE + section))) {
      s_data.mode[section] = prv_tuple_int(t) == MODE_MINUTES ? MODE_MINUTES : MODE_CLOCK;
      changed = true;
    }
  }

  if (changed) {
    s_has_data = true;
    persist_write_data(PERSIST_KEY_DATA, &s_data, sizeof(s_data));
  }

  // Every fetch result from the phone says how many rows failed to load.
  Tuple *failed = dict_find(iter, MESSAGE_KEY_FETCH_FAILED);
  if (failed) {
    prv_set_fetch_state(prv_tuple_int(failed) > 0 ? FETCH_FAILED : FETCH_OK);
  }

  // The phone is reachable right now, so report the previous run if still owed.
  prv_send_prev_diag();

  bool settings_changed = prv_read_settings(iter);
  if (changed || settings_changed) {
    layer_mark_dirty(s_canvas);
    prv_update_blink();
  }
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_update_time_string(tick_time);
  layer_mark_dirty(s_canvas);
  prv_update_blink();
  // The handler also runs whenever the phone sets the watch's clock, so the
  // time since the last request is checked too.
  time_t now = time(NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "hb up=%lds heap=%d draw=%dms fetch=%d bt=%d",
          (long)(now - (time_t)s_diag.started), (int)heap_bytes_free(), s_max_draw_ms,
          (int)s_fetch_state, (int)s_connected);
  if (s_diag.stage == STAGE_INIT || tick_time->tm_min % DIAG_PERSIST_MINUTES == 0) {
    prv_breadcrumb(STAGE_RUNNING, 0);
  }
  s_max_draw_ms = 0;
  if (tick_time->tm_min % REFRESH_MINUTES == 0 && now - s_last_request >= 60) {
    s_last_request = now;
    prv_request_update();
  }
}

static void prv_connection_handler(bool connected) {
  // The phone fetches by itself when the app starts or the watch reconnects.
  if (connected && !s_connected) {
    prv_set_fetch_state(FETCH_PENDING);
  }
  s_connected = connected;
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(window_layer));
  layer_set_update_proc(s_canvas, prv_canvas_update);
  layer_add_child(window_layer, s_canvas);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void prv_init(void) {
  prv_diag_init();
  prv_load_settings();

  s_time_font = fonts_get_system_font(FONT_KEY_LECO_36_BOLD_NUMBERS);
  s_header_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  for (int f = 0; f < NUM_ROW_FONTS; f++) {
    s_row_fonts[f] = (RowFont) {
      .font = fonts_get_system_font(s_row_font_specs[f].key),
      .suffix_font = fonts_get_system_font(s_row_font_specs[f].suffix_key),
      .spec = &s_row_font_specs[f],
    };
  }

  memset(&s_data, 0, sizeof(s_data));
  if (persist_exists(PERSIST_KEY_DATA) &&
      persist_read_data(PERSIST_KEY_DATA, &s_data, sizeof(s_data)) == (int)sizeof(s_data)) {
    s_has_data = true;
  } else {
    // Saved data from an older layout may have been partially read; discard it.
    memset(&s_data, 0, sizeof(s_data));
  }
  for (int section = 0; section < NUM_SECTIONS; section++) {
    s_data.dest[section][NUM_TIMES] = '\0';
    s_data.flags[section][NUM_TIMES] = '\0';
  }

  time_t now = time(NULL);
  prv_update_time_string(localtime(&now));

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_open(512, 128);

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_connection_handler,
  });
  s_fetch_state = FETCH_FAILED;
  prv_connection_handler(connection_service_peek_pebble_app_connection());
  prv_update_blink();
}

static void prv_deinit(void) {
  s_diag.clean_exit = 1;
  prv_breadcrumb(s_diag.stage, s_diag.detail);
  if (s_blink_timer) {
    app_timer_cancel(s_blink_timer);
  }
  if (s_fetch_timer) {
    app_timer_cancel(s_fetch_timer);
  }
  tick_timer_service_unsubscribe();
  connection_service_unsubscribe();
  app_message_deregister_callbacks();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
