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

#define TIME_HEIGHT 45
#define HEADER_HEIGHT 15
#define MIN_CELL_GAP 2
#define SUFFIX_GAP 2

// Index order must match the stations in src/pkjs/config.js.
enum { SECTION_NORTH, SECTION_SALEM, SECTION_CENTRAL, NUM_SECTIONS };

// A departure blinks while inside its section's warning range, speeding up
// from BLINK_SLOW_MS to BLINK_FAST_MS per toggle as the range end approaches.
#define BLINK_SLOW_MS 1000
#define BLINK_FAST_MS 125

typedef struct {
  int32_t north[NUM_TIMES];
  int32_t salem[NUM_TIMES];
  int32_t central[NUM_TIMES];
  char central_dest[NUM_TIMES + 1];
} TransitData;

typedef struct {
  bool warn_on[NUM_SECTIONS];
  uint8_t warn_start_min[NUM_SECTIONS];
  uint8_t warn_end_min[NUM_SECTIONS];
  // New fields go at the end so older persisted settings still load.
  uint8_t departures;
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
} Cell;

static Window *s_window;
static Layer *s_canvas;
static TransitData s_data;
static Settings s_settings;
static bool s_has_data;
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

// Collects up to max upcoming (not yet departed) times into out[]; returns the count.
static int prv_upcoming(const int32_t *times, const char *dest, time_t now,
                        int32_t *out, char *out_dest, int max) {
  int n = 0;
  for (int i = 0; i < NUM_TIMES && n < max; i++) {
    if (times[i] > now) {
      out[n] = times[i];
      if (out_dest) {
        out_dest[n] = dest ? dest[i] : ' ';
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

static void prv_draw_header(GContext *ctx, int y, int width, const char *left, const char *right) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, y, width, HEADER_HEIGHT), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  prv_draw_text(ctx, left, s_header_font, GRect(3, y - 3, width - 6, HEADER_HEIGHT + 4),
                GTextAlignmentLeft);
  if (right && right[0]) {
    prv_draw_text(ctx, right, s_header_font, GRect(3, y - 3, width - 6, HEADER_HEIGHT + 4),
                  GTextAlignmentRight);
  }
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

static const int32_t *prv_section_times(int section) {
  switch (section) {
    case SECTION_NORTH: return s_data.north;
    case SECTION_SALEM: return s_data.salem;
    default: return s_data.central;
  }
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
  bool central = section == SECTION_CENTRAL;
  int n = prv_upcoming(prv_section_times(section), central ? s_data.central_dest : NULL, now,
                       upcoming, dest, prv_departures());
  for (int i = 0; i < n; i++) {
    Cell *cell = &cells[i];
    cell->departure = upcoming[i];
    cell->suffix[0] = '\0';
    if (central) {
      int mins = (int)((upcoming[i] - now) / 60);
      if (mins < 1) {
        snprintf(cell->text, sizeof(cell->text), "now");
      } else {
        snprintf(cell->text, sizeof(cell->text), "%d", mins);
      }
      cell->suffix[0] = dest[i] == ' ' ? '\0' : dest[i];
      cell->suffix[1] = '\0';
    } else {
      prv_format_clock(upcoming[i], cell->text, sizeof(cell->text));
    }
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

// Picks the largest font allowed for the departure count that fits every row on one line.
static const RowFont *prv_pick_row_font(Cell cells[][MAX_DEPARTURES], const int *counts,
                                        int width) {
  int count = prv_departures();
  int first = count <= 3 ? 0 : count == 4 ? 1 : 3;
  for (int f = first; f < NUM_ROW_FONTS; f++) {
    bool fits = true;
    for (int section = 0; section < NUM_SECTIONS && fits; section++) {
      int total = counts[section] * MIN_CELL_GAP;
      for (int i = 0; i < counts[section]; i++) {
        int text_w;
        total += prv_cell_width(&cells[section][i], &s_row_fonts[f], &text_w);
      }
      fits = total <= width;
    }
    if (fits) {
      return &s_row_fonts[f];
    }
  }
  return &s_row_fonts[NUM_ROW_FONTS - 1];
}

// Draws a row of cells. Each cell gets a slot sized to its content plus an even
// share of the spare width; a blinking cell fills its whole slot.
static void prv_draw_row(GContext *ctx, int row_y, int width, int section, const Cell *cells,
                         int n, const RowFont *rf, int64_t now_ms) {
  const RowFontSpec *spec = rf->spec;
  if (n == 0) {
    graphics_context_set_text_color(ctx, GColorWhite);
    prv_draw_text(ctx, s_has_data ? "No trains" : "Loading...", rf->font,
                  GRect(0, row_y + spec->text_dy, width, spec->row_h + 8), GTextAlignmentCenter);
    return;
  }

  int widths[MAX_DEPARTURES];
  int text_ws[MAX_DEPARTURES];
  int total = 0;
  for (int i = 0; i < n; i++) {
    widths[i] = prv_cell_width(&cells[i], rf, &text_ws[i]);
    total += widths[i];
  }
  int spare = width - total;
  if (spare < 0) {
    spare = 0;
  }

  int x = 0;
  for (int i = 0; i < n; i++) {
    int slot = widths[i] + spare / n + (i < spare % n ? 1 : 0);
    if (i == n - 1) {
      slot = width - x;
    }
    bool inverted = prv_is_highlighted(section, cells[i].departure, now_ms);
    if (inverted) {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_rect(ctx, GRect(x, row_y, slot, spec->row_h), 0, GCornerNone);
    }
    graphics_context_set_text_color(ctx, inverted ? GColorBlack : GColorWhite);

    int tx = x + (slot - widths[i]) / 2;
    prv_draw_text(ctx, cells[i].text, rf->font,
                  GRect(tx, row_y + spec->text_dy, text_ws[i] + 4, spec->row_h + 8),
                  GTextAlignmentLeft);
    if (cells[i].suffix[0]) {
      prv_draw_text(ctx, cells[i].suffix, rf->suffix_font,
                    GRect(tx + text_ws[i] + SUFFIX_GAP, row_y + spec->suffix_dy, 20, 20),
                    GTextAlignmentLeft);
    }
    x += slot;
  }
}

static void prv_canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int width = bounds.size.w;
  time_t now = time(NULL);
  int64_t now_ms = prv_now_ms();

  static const char *const labels[NUM_SECTIONS] = {
    "NORTH STA > OUT", "SALEM > IN", "CENTRAL > SOUTH",
  };
  Cell cells[NUM_SECTIONS][MAX_DEPARTURES];
  int counts[NUM_SECTIONS];
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

  for (int section = 0; section < NUM_SECTIONS; section++) {
    int y = sections_top + section * section_h;
    char right[16] = "";
    if (counts[section] > 0) {
      prv_format_wait(cells[section][0].departure, now, right, sizeof(right));
    }
    prv_draw_header(ctx, y, width, labels[section], right);
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
      int n = prv_upcoming(prv_section_times(section), NULL, (time_t)(now_ms / 1000), visible,
                           NULL, prv_departures());
      for (int i = 0; i < n; i++) {
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
  };
}

static void prv_load_settings(void) {
  prv_default_settings();
  persist_read_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));
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

static void prv_request_update(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_REQUEST, 1);
  app_message_outbox_send();
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
  bool changed = false;
  for (int i = 0; i < NUM_TIMES; i++) {
    Tuple *t;
    if ((t = dict_find(iter, MESSAGE_KEY_N_TIMES + i))) {
      s_data.north[i] = t->value->int32;
      changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_S_TIMES + i))) {
      s_data.salem[i] = t->value->int32;
      changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_C_TIMES + i))) {
      s_data.central[i] = t->value->int32;
      changed = true;
    }
  }
  Tuple *dest = dict_find(iter, MESSAGE_KEY_C_DEST);
  if (dest) {
    memset(s_data.central_dest, ' ', NUM_TIMES);
    size_t len = strlen(dest->value->cstring);
    memcpy(s_data.central_dest, dest->value->cstring, len < NUM_TIMES ? len : NUM_TIMES);
    s_data.central_dest[NUM_TIMES] = '\0';
    changed = true;
  }

  if (changed) {
    s_has_data = true;
    persist_write_data(PERSIST_KEY_DATA, &s_data, sizeof(s_data));
  }

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
  if (tick_time->tm_min % REFRESH_MINUTES == 0) {
    prv_request_update();
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
  s_data.central_dest[NUM_TIMES] = '\0';

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
  app_message_open(512, 64);

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  prv_update_blink();
}

static void prv_deinit(void) {
  if (s_blink_timer) {
    app_timer_cancel(s_blink_timer);
  }
  tick_timer_service_unsubscribe();
  app_message_deregister_callbacks();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
