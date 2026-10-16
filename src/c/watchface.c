#include <pebble.h>

#define NUM_TIMES 4
#define NUM_COLUMNS 3
#define REFRESH_MINUTES 2
#define PERSIST_KEY_DATA 1
#define PERSIST_KEY_SETTINGS 2

#define TIME_HEIGHT 45
#define HEADER_HEIGHT 15
#define SECTION_HEIGHT 41

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
} Settings;

typedef struct {
  bool active;
  bool inverted;
  int64_t next_toggle_ms;
} BlinkState;

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
static GFont s_value_font;
static GFont s_small_font;

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

static void prv_draw_empty(GContext *ctx, int y, int width) {
  graphics_context_set_text_color(ctx, GColorWhite);
  prv_draw_text(ctx, s_has_data ? "No trains" : "Loading...", s_value_font,
                GRect(0, y, width, 28), GTextAlignmentCenter);
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

// Fills the column's full value row, flush with the header bars above and below.
static void prv_draw_highlight(GContext *ctx, int col, int col_w, int section_y) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx,
                     GRect(col * col_w, section_y + HEADER_HEIGHT, col_w,
                           SECTION_HEIGHT - HEADER_HEIGHT),
                     0, GCornerNone);
}

static void prv_draw_commuter_section(GContext *ctx, int y, int width, const char *label,
                                      int section, time_t now) {
  const int32_t *times = prv_section_times(section);
  int32_t upcoming[NUM_COLUMNS];
  int n = prv_upcoming(times, NULL, now, upcoming, NULL, NUM_COLUMNS);

  char wait[16] = "";
  if (n > 0) {
    prv_format_wait(upcoming[0], now, wait, sizeof(wait));
  }
  prv_draw_header(ctx, y, width, label, wait);

  int values_y = y + HEADER_HEIGHT - 4;
  if (n == 0) {
    prv_draw_empty(ctx, values_y, width);
    return;
  }

  int64_t now_ms = prv_now_ms();
  int col_w = width / NUM_COLUMNS;
  for (int i = 0; i < n; i++) {
    char buf[8];
    prv_format_clock(upcoming[i], buf, sizeof(buf));
    bool inverted = prv_is_highlighted(section, upcoming[i], now_ms);
    if (inverted) {
      prv_draw_highlight(ctx, i, col_w, y);
    }
    graphics_context_set_text_color(ctx, inverted ? GColorBlack : GColorWhite);
    prv_draw_text(ctx, buf, s_value_font, GRect(i * col_w, values_y, col_w, 28),
                  GTextAlignmentCenter);
  }
}

static void prv_draw_central_section(GContext *ctx, int y, int width, time_t now) {
  int32_t upcoming[NUM_COLUMNS];
  char dest[NUM_COLUMNS];
  int n = prv_upcoming(s_data.central, s_data.central_dest, now, upcoming, dest, NUM_COLUMNS);

  prv_draw_header(ctx, y, width, "CENTRAL > SOUTH", "min");

  int values_y = y + HEADER_HEIGHT - 4;
  if (n == 0) {
    prv_draw_empty(ctx, values_y, width);
    return;
  }

  int64_t now_ms = prv_now_ms();
  int col_w = width / NUM_COLUMNS;
  for (int i = 0; i < n; i++) {
    int mins = (int)((upcoming[i] - now) / 60);
    char num[12];
    char letter[2] = { dest[i], '\0' };
    if (mins < 1) {
      snprintf(num, sizeof(num), "now");
    } else {
      snprintf(num, sizeof(num), "%d", mins);
    }

    bool inverted = prv_is_highlighted(SECTION_CENTRAL, upcoming[i], now_ms);
    if (inverted) {
      prv_draw_highlight(ctx, i, col_w, y);
    }
    graphics_context_set_text_color(ctx, inverted ? GColorBlack : GColorWhite);

    // Center the "<minutes><destination letter>" pair within its column.
    GRect col = GRect(i * col_w, values_y, col_w, 28);
    GSize num_size = graphics_text_layout_get_content_size(
        num, s_value_font, col, GTextOverflowModeWordWrap, GTextAlignmentLeft);
    GSize letter_size = graphics_text_layout_get_content_size(
        letter, s_small_font, col, GTextOverflowModeWordWrap, GTextAlignmentLeft);
    int gap = 2;
    int x = col.origin.x + (col_w - (num_size.w + gap + letter_size.w)) / 2;
    prv_draw_text(ctx, num, s_value_font, GRect(x, values_y, num_size.w + 4, 28),
                  GTextAlignmentLeft);
    prv_draw_text(ctx, letter, s_small_font,
                  GRect(x + num_size.w + gap, values_y + 9, letter_size.w + 4, 20),
                  GTextAlignmentLeft);
  }
}

static void prv_canvas_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int width = bounds.size.w;
  time_t now = time(NULL);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorWhite);
  prv_draw_text(ctx, s_time_buffer, s_time_font, GRect(0, 0, width, TIME_HEIGHT),
                GTextAlignmentCenter);

  int y = TIME_HEIGHT;
  prv_draw_commuter_section(ctx, y, width, "NORTH STA > OUT", SECTION_NORTH, now);
  y += SECTION_HEIGHT;
  prv_draw_commuter_section(ctx, y, width, "SALEM > IN", SECTION_SALEM, now);
  y += SECTION_HEIGHT;
  prv_draw_central_section(ctx, y, width, now);
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
      const int32_t *times = prv_section_times(section);
      for (int i = 0; i < NUM_TIMES; i++) {
        if (times[i] <= 0) {
          continue;
        }
        int64_t remaining = (int64_t)times[i] * 1000 - now_ms;
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
  s_value_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  s_small_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  memset(&s_data, 0, sizeof(s_data));
  if (persist_exists(PERSIST_KEY_DATA) &&
      persist_read_data(PERSIST_KEY_DATA, &s_data, sizeof(s_data)) == (int)sizeof(s_data)) {
    s_has_data = true;
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
