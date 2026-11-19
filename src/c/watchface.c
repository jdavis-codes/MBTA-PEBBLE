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
#define PERSIST_KEY_TO 4

// Diagnostics. A breadcrumb in persistent storage records what the app was
// doing, so that after a crash or a watch reset the next launch can report it
// to the phone (and to `pebble logs`). The live state goes out once a minute as
// a heartbeat log line.
#define DIAG_VERSION 3
#define DIAG_PERSIST_MINUTES 10
#define LAYOUT_TRIAL_S 600
// STAGE_SCROLL's detail is how far the alert had scrolled, in units of 8px.
enum { STAGE_INIT, STAGE_RUNNING, STAGE_SCROLL };

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
  // Set when a run died while an alert was scrolling: alerts then stay off
  // until the settings are saved again, so a bad scroll cannot reset-loop the watch.
  uint8_t alerts_blocked;
  // The layout the run was drawing, so a layout that crashes at launch can be undone.
  uint8_t layout;
} Diag;

// Includes the terminator; must match ALERT_MAX_CHARS + 1 in src/pkjs/index.js.
#define ALERT_SIZE 161
// A service alert scrolls across its row's label once a minute. The watch
// takes 50-160ms to redraw, so each frame is followed by an idle gap and the
// position comes from the clock, not from a frame count.
#define SCROLL_PX_PER_SEC 45
#define SCROLL_IDLE_MS 100
#define SCROLL_BREADCRUMB_MS 2000


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

// Bluebikes docks, in clusters of nearby docks. The classic layout shows them
// as small pies along the top left; the other layouts as stacks of bars.
// Must match NUM_BIKES and BIKE_LEVELS in src/pkjs/index.js.
#define NUM_CLUSTERS 3
#define CLUSTER_SIZE 3
#define NUM_BIKES (NUM_CLUSTERS * CLUSTER_SIZE)
#define BIKE_LEVELS 8
#define BIKE_NONE '-'
// Levels above BIKE_LEVELS are drawn as squares so a dock with nowhere to
// return a bike stands out: a hollow square for one free space, solid for none.
#define BIKE_ONE_FREE (BIKE_LEVELS + 1)
#define BIKE_FULL (BIKE_LEVELS + 2)
// A dock that is out of service or has been removed: crossed out.
#define BIKE_CLOSED (BIKE_LEVELS + 3)
// Up to this many e-bikes are shown as dots around a pie.
#define BIKE_MAX_EBIKES 8
// Centre of the first pie, and the gap between pies; the orbiting dots reach
// 6px out, so the row occupies y 0..INDICATOR_BOTTOM - 1.
#define BIKE_LEFT 6
#define BIKE_Y 6
#define BIKE_SPACING 13
// The pies move closer together once a dock beyond the sixth is in use.
#define BIKE_SPACING_TIGHT 11
#define BIKE_WIDE_SLOTS 6
// A dock as a bar: an outline that fills with bikes from the left. A full dock
// is solid and reaches back to the cluster's spine; a dock with one free
// space is solid with a single open cell at its right end.
#define BAR_W 10
#define BAR_H 5
#define BAR_PITCH 6
#define CLUSTER_BAR_X 3
#define CLUSTER_EBIKE_X 15
#define CLUSTER_W 17
#define CLUSTER_H (CLUSTER_SIZE * BAR_PITCH - 1)
#define INDICATOR_BOTTOM 13
// Where the digits sit inside the clock's text box.
#define TIME_GLYPH_TOP 11
#define TIME_GLYPH_HEIGHT 25
// What the REQUEST message asks the phone for; must match src/pkjs/index.js.
#define REQUEST_ALL 1
#define REQUEST_BIKES 2

// One section per departure row (header + times); must match ROWS in src/pkjs/config.js.
#define NUM_SECTIONS 3
// Includes the terminator; the settings page limits labels to 23 characters.
#define LABEL_SIZE 24

// The screen arrangements; must match the Layout options in src/pkjs/config.js.
enum { LAYOUT_CLASSIC, LAYOUT_BOARD, LAYOUT_DIAL, NUM_LAYOUTS };
// Includes the terminator; must match TO_MAX_CHARS + 1 in src/pkjs/index.js.
#define TO_SIZE 12

// The departure board: a strip with the clock and docks, a column bar, then
// every upcoming train from all rows in time order.
#define BOARD_ROWS 6
#define BOARD_TOP 24
#define BOARD_BAR_H 12
#define BOARD_ROW_H 22
#define BOARD_TO_X 46

// The hour dial: one ring per row, each train at the minute it departs.
#define DIAL_CX 72
#define DIAL_CY 84
// Offset of the time's text box above the centre, so its digits are centred.
#define DIAL_TIME_DY 21
#define DIAL_HAND_INNER 44
#define DIAL_HAND_OUTER 71
// Every DIAL_DOT_STEP-th pixel of a ring is lit.
#define DIAL_DOT_STEP 3
#define DIAL_HOUR_S 3600

// How a row shows departures; must match MODE_* in src/pkjs/index.js.
// MODE_UNSET is a station nobody has chosen yet. MODE_BUS shows minutes like
// MODE_MINUTES, and says "buses" where a train row says "trains".
enum { MODE_CLOCK, MODE_MINUTES, MODE_UNSET, MODE_BUS };

// Per-departure status letters; must match FLAG_* in src/pkjs/index.js.
#define FLAG_NONE ' '
#define FLAG_CANCELLED 'X'
#define FLAG_DELAYED 'D'
// Gap between the diagonal lines hatched behind a delayed departure.
#define HATCH_SPACING 5

typedef enum { FETCH_OK, FETCH_PENDING, FETCH_FAILED } FetchState;
// Trains and docks are fetched separately, so each has its own indicator.
typedef enum { FETCH_TRAINS, FETCH_BIKES, NUM_FETCHES } FetchKind;

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
  uint8_t layout;
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

// One line of the departure board.
typedef struct {
  int32_t departure;
  uint8_t section;
  // Index into s_data.times[section], to find the train's destination.
  uint8_t index;
  char flag;
  char time[8];
  char mins[6];
  uint8_t mins_w;
} BoardRow;

// One train on the hour dial.
typedef struct {
  GPoint at;
  // Where on the ring the train departs, before any nudge clear of the labels.
  int32_t angle;
  int32_t departure;
  char flag;
} DialMark;

static Window *s_window;
static Layer *s_canvas;
static TransitData s_data;
static Settings s_settings;
static bool s_has_data;
static bool s_connected;
static FetchState s_fetch_state[NUM_FETCHES];
static AppTimer *s_fetch_timer;
static int s_fetch_ticks[NUM_FETCHES];
static time_t s_last_request;
// Two characters per dock: the fill level as a letter from 'a' (0) to
// 'a' + BIKE_LEVELS, then BIKE_ONE_FREE and BIKE_FULL, or BIKE_NONE when the
// slot is unused; then the number of e-bikes as a digit.
static char s_bikes[2 * NUM_BIKES + 1] = "-0-0-0-0-0-0-0-0-0";
// Where each train is going, for the departure board.
static char s_to[NUM_SECTIONS][NUM_TIMES][TO_SIZE];
static Diag s_diag;
// The previous run's breadcrumb as text, until the phone has received it.
static char s_prev_diag[120];
static bool s_prev_diag_pending;
static uint16_t s_max_draw_ms;

static char s_alerts[NUM_SECTIONS][ALERT_SIZE];
// Pixel width of each alert character, and their total.
static uint8_t s_alert_cw[NUM_SECTIONS][ALERT_SIZE];
static int s_alert_w[NUM_SECTIONS];
// When the current scroll pass started, or 0 when none is running.
static int64_t s_scroll_start_ms;
static int64_t s_scroll_breadcrumb_ms;
static AppTimer *s_scroll_timer;
static uint16_t s_scroll_frames;

// What each row shows, rebuilt only when the minute, the data or the settings
// change, so scroll frames redraw without re-measuring text.
static Cell s_cells[NUM_SECTIONS][MAX_DEPARTURES];
static int s_counts[NUM_SECTIONS];
static char s_wait[NUM_SECTIONS][16];
static bool s_layout_stale = true;
static char s_time_buffer[8];
static AppTimer *s_blink_timer;
static BlinkState s_blink[NUM_SECTIONS];

static BoardRow s_board[BOARD_ROWS];
static int s_board_count;
static const uint8_t s_dial_r[NUM_SECTIONS] = { 68, 58, 48 };
static DialMark s_marks[NUM_SECTIONS][NUM_TIMES];
static int s_mark_count[NUM_SECTIONS];
// Minutes until each ring's next running train, or -1 when there is none.
static int16_t s_dial_mins[NUM_SECTIONS];
static int16_t s_dial_label_w[NUM_SECTIONS];
static int32_t s_dial_now_angle;
// 1 when the ring labels sit below the centre, -1 when above. They are always
// on the far side from the hand, so they change sides twice an hour.
static int8_t s_dial_label_side = 1;

static GFont s_time_font;
static GFont s_time_font_small;
static GFont s_time_font_dial;
static GFont s_board_font;
static GFont s_board_to_font;
static GFont s_header_font;
static RowFont s_row_fonts[ARRAY_LENGTH(s_row_font_specs)];
static const RowFont *s_row_font;

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

// Pixels scrolled in the current pass.
static int prv_scroll_px(int64_t now_ms) {
  return (int)((now_ms - s_scroll_start_ms) * SCROLL_PX_PER_SEC / 1000);
}

// The alert enters at the right edge of the label area and leaves at the left.
static bool prv_alert_scrolling(int section, int area_w, int64_t now_ms) {
  return s_scroll_start_ms && s_alerts[section][0] &&
         prv_scroll_px(now_ms) < area_w + s_alert_w[section];
}

// Draws a header bar; a long left label is shortened to fit before the right
// text. A row with a service alert shows it scrolling through the label area,
// and "!" before the label between passes.
static void prv_draw_header(GContext *ctx, int y, int width, int section, const char *right,
                            int64_t now_ms) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, y, width, HEADER_HEIGHT), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  int left_w = width - 6;
  if (right && right[0]) {
    prv_draw_text(ctx, right, s_header_font, GRect(3, y - 3, width - 6, HEADER_HEIGHT + 4),
                  GTextAlignmentRight);
    left_w -= prv_text_width(right, s_header_font) + 4;
  }
  GRect text_box = GRect(3, y - 3, left_w, HEADER_HEIGHT + 4);

  if (prv_alert_scrolling(section, left_w, now_ms)) {
    // Only whole characters that lie fully inside the label area are drawn,
    // as one short string: nothing is ever drawn partly off-screen.
    const char *alert = s_alerts[section];
    const uint8_t *cw = s_alert_cw[section];
    int x = left_w - prv_scroll_px(now_ms);
    int i = 0;
    while (alert[i] && x < 0) {
      x += cw[i++];
    }
    static char visible[48];
    int n = 0;
    int visible_w = 0;
    while (alert[i + n] && n < (int)sizeof(visible) - 1 &&
           x + visible_w + cw[i + n] <= left_w) {
      visible[n] = alert[i + n];
      visible_w += cw[i + n];
      n++;
    }
    visible[n] = '\0';
    if (n > 0) {
      text_box.origin.x += x;
      text_box.size.w = visible_w + 8;
      prv_draw_text(ctx, visible, s_header_font, text_box, GTextAlignmentLeft);
    }
    return;
  }

  if (s_alerts[section][0]) {
    int mark_w = prv_text_width("!", s_header_font) + 3;
    prv_draw_text(ctx, "!", s_header_font, text_box, GTextAlignmentLeft);
    text_box.origin.x += mark_w;
    text_box.size.w -= mark_w;
  }
  static char label[LABEL_SIZE + 4];
  prv_fit_label(s_settings.labels[section], s_header_font, text_box.size.w, label, sizeof(label));
  prv_draw_text(ctx, label, s_header_font, text_box, GTextAlignmentLeft);
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
  bool minutes = s_data.mode[section] == MODE_MINUTES || s_data.mode[section] == MODE_BUS;
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
static void prv_draw_no_more(GContext *ctx, GRect space, const RowFont *rf, bool any_trains,
                             uint8_t mode) {
  static const char *const s_some[] = { "No more trains", "No more", "-" };
  static const char *const s_none[] = { "No trains" };
  static const char *const s_some_bus[] = { "No more buses", "No more", "-" };
  static const char *const s_none_bus[] = { "No buses" };
  bool unset = mode == MODE_UNSET;
  bool bus = mode == MODE_BUS;
  const char *const *texts = any_trains ? (bus ? s_some_bus : s_some) : (bus ? s_none_bus : s_none);
  int num_texts = any_trains ? (int)ARRAY_LENGTH(s_some) : (int)ARRAY_LENGTH(s_none);
  if (!s_has_data) {
    static const char *const s_loading[] = { "Loading..." };
    texts = s_loading;
    num_texts = 1;
  } else if (unset) {
    // No station has been chosen for this row yet.
    static const char *const s_unset[] = { "Set up on phone", "Not set", "-" };
    texts = s_unset;
    num_texts = (int)ARRAY_LENGTH(s_unset);
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
    prv_draw_no_more(ctx, GRect(x, row_y, width - x, spec->row_h), rf, n > 0,
                     s_data.mode[section]);
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
  s_diag.layout = s_settings.layout;
  persist_write_data(PERSIST_KEY_DIAG, &s_diag, sizeof(s_diag));
}

// Reads the previous run's breadcrumb and starts this run's.
static void prv_diag_init(void) {
  Diag prev;
  uint32_t launches = 0;
  bool blocked = false;
  if (persist_read_data(PERSIST_KEY_DIAG, &prev, sizeof(prev)) == (int)sizeof(prev) &&
      prev.version == DIAG_VERSION) {
    launches = prev.launches;
    blocked = prev.alerts_blocked || (!prev.clean_exit && prev.stage == STAGE_SCROLL);
    // A run in one of the newer layouts that died within minutes of starting
    // goes back to the classic layout, so it cannot reset-loop the watch.
    bool layout_reset = !prev.clean_exit && prev.layout != LAYOUT_CLASSIC &&
                        prev.updated - prev.started < LAYOUT_TRIAL_S &&
                        s_settings.layout != LAYOUT_CLASSIC;
    if (layout_reset) {
      s_settings.layout = LAYOUT_CLASSIC;
      persist_write_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));
    }
    snprintf(s_prev_diag, sizeof(s_prev_diag),
             "prev %s stage=%d.%d ran=%lds draw=%dms heap=%d launch=%lu layout=%d%s%s",
             prev.clean_exit ? "clean" : "UNCLEAN", prev.stage, prev.detail,
             (long)(prev.updated - prev.started), prev.max_draw_ms, prev.heap_free,
             (unsigned long)prev.launches, prev.layout, blocked ? " ALERTS-BLOCKED" : "",
             layout_reset ? " LAYOUT-RESET" : "");
    s_prev_diag_pending = true;
    APP_LOG(APP_LOG_LEVEL_INFO, "%s", s_prev_diag);
  }
  s_diag = (Diag) {
    .launches = launches + 1,
    .started = (uint32_t)time(NULL),
    .version = DIAG_VERSION,
    .alerts_blocked = blocked,
  };
  prv_breadcrumb(STAGE_INIT, 0);
}

// Fills the departure board: the next trains from every row, soonest first.
static void prv_build_board(time_t now) {
  int next[NUM_SECTIONS] = { 0 };
  s_board_count = 0;
  while (s_board_count < BOARD_ROWS) {
    int best = -1;
    for (int section = 0; section < NUM_SECTIONS; section++) {
      const int32_t *times = s_data.times[section];
      while (next[section] < NUM_TIMES && times[next[section]] <= now) {
        next[section]++;
      }
      if (next[section] < NUM_TIMES &&
          (best < 0 || times[next[section]] < s_data.times[best][next[best]])) {
        best = section;
      }
    }
    if (best < 0) {
      break;
    }
    BoardRow *row = &s_board[s_board_count++];
    row->section = (uint8_t)best;
    row->index = (uint8_t)next[best];
    row->departure = s_data.times[best][next[best]];
    row->flag = s_data.flags[best][next[best]];
    next[best]++;
    prv_format_clock(row->departure, row->time, sizeof(row->time));
    int mins = (int)((row->departure - now) / 60);
    if (mins < 1) {
      snprintf(row->mins, sizeof(row->mins), "now");
    } else {
      snprintf(row->mins, sizeof(row->mins), "%d", mins > 999 ? 999 : mins);
    }
    row->mins_w = (uint8_t)prv_text_width(row->mins, s_board_font);
  }
}

// The dial's ring labels: block digits in the style of the clock, small
// enough to fit between two rings (the smallest system clock font is too tall).
#define SMALL_DIGIT_W 6
#define SMALL_DIGIT_H 9
#define SMALL_DIGIT_GAP 1

// Draws a number with its top left at (x, y), or two dashes when it is
// negative. Returns its width; with a NULL context it only measures.
static int prv_small_number(GContext *ctx, int value, int x, int y) {
  // Bars of a seven-segment digit: top, upper right, lower right, bottom,
  // lower left, upper left, middle.
  static const GRect s_bars[7] = {
    { { 0, 0 }, { 6, 2 } }, { { 4, 0 }, { 2, 5 } }, { { 4, 4 }, { 2, 5 } }, { { 0, 7 }, { 6, 2 } },
    { { 0, 4 }, { 2, 5 } }, { { 0, 0 }, { 2, 5 } }, { { 0, 4 }, { 6, 1 } },
  };
  static const uint8_t s_digits[10] = {
    0x3F, 0x00, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
  };
  // The clock font's 1: a stem with a flag at the top and a full-width foot.
  static const GRect s_one[3] = {
    { { 2, 0 }, { 2, 9 } }, { { 0, 0 }, { 2, 2 } }, { { 0, 7 }, { 6, 2 } },
  };
  int count = value < 0 ? 2 : value >= 100 ? 3 : value >= 10 ? 2 : 1;
  int width = count * (SMALL_DIGIT_W + SMALL_DIGIT_GAP) - SMALL_DIGIT_GAP;
  if (!ctx) {
    return width;
  }
  int divisor = count == 3 ? 100 : count == 2 ? 10 : 1;
  for (int i = 0; i < count; i++) {
    int dx = x + i * (SMALL_DIGIT_W + SMALL_DIGIT_GAP);
    int digit = value < 0 ? -1 : value / divisor % 10;
    divisor /= 10;
    if (digit == 1) {
      for (int b = 0; b < 3; b++) {
        graphics_fill_rect(ctx, GRect(dx + s_one[b].origin.x, y + s_one[b].origin.y,
                                      s_one[b].size.w, s_one[b].size.h), 0, GCornerNone);
      }
      continue;
    }
    // A dash is the middle bar alone.
    uint8_t mask = digit < 0 ? 0x40 : s_digits[digit];
    for (int b = 0; b < 7; b++) {
      if (mask & (1 << b)) {
        graphics_fill_rect(ctx, GRect(dx + s_bars[b].origin.x, y + s_bars[b].origin.y,
                                      s_bars[b].size.w, s_bars[b].size.h), 0, GCornerNone);
      }
    }
  }
  return width;
}

static GPoint prv_polar(int r, int32_t angle) {
  return GPoint(DIAL_CX + r * sin_lookup(angle) / TRIG_MAX_RATIO,
                DIAL_CY - r * cos_lookup(angle) / TRIG_MAX_RATIO);
}

// Places the trains of the next hour on the dial and writes each ring's label.
static void prv_build_dial(time_t now) {
  struct tm *tm = localtime(&now);
  int now_s = tm->tm_min * 60 + tm->tm_sec;
  s_dial_now_angle = TRIG_MAX_ANGLE * now_s / DIAL_HOUR_S;
  // The hand is in the lower half from quarter past until quarter to.
  int side = now_s >= DIAL_HOUR_S / 4 && now_s < 3 * DIAL_HOUR_S / 4 ? -1 : 1;
  s_dial_label_side = (int8_t)side;
  for (int section = 0; section < NUM_SECTIONS; section++) {
    const int32_t *times = s_data.times[section];
    int r = s_dial_r[section];

    // The label is the minutes until the next train that is actually running.
    s_dial_mins[section] = -1;
    for (int i = 0; i < NUM_TIMES; i++) {
      if (times[i] > now && s_data.flags[section][i] != FLAG_CANCELLED) {
        int32_t mins = (times[i] - (int32_t)now) / 60;
        s_dial_mins[section] = (int16_t)(mins > 999 ? 999 : mins);
        break;
      }
    }
    s_dial_label_w[section] = (int16_t)prv_small_number(NULL, s_dial_mins[section], 0, 0);

    // A train that would sit under the label is moved to just beside it.
    int half = s_dial_label_w[section] / 2 + 9;
    int n = 0;
    for (int i = 0; i < NUM_TIMES; i++) {
      int32_t wait = times[i] - (int32_t)now;
      if (times[i] <= now || wait >= DIAL_HOUR_S) {
        continue;
      }
      int32_t angle = TRIG_MAX_ANGLE * ((now_s + wait) % DIAL_HOUR_S) / DIAL_HOUR_S;
      GPoint at = prv_polar(r, angle);
      int dx = at.x - DIAL_CX;
      if ((at.y - DIAL_CY) * side > r - 12 && dx > -half && dx < half) {
        dx = dx >= 0 ? half : -half;
        int dy = 0;
        while ((dy + 1) * (dy + 1) <= r * r - dx * dx) {
          dy++;
        }
        at = GPoint(DIAL_CX + dx, DIAL_CY + side * dy);
      }
      s_marks[section][n++] = (DialMark) { .at = at, .angle = angle, .departure = times[i],
                                           .flag = s_data.flags[section][i] };
    }
    s_mark_count[section] = n;
  }
}

// Rebuilds what each row shows and picks the row font, if anything changed.
static void prv_update_layout(int width) {
  if (!s_layout_stale && s_row_font) {
    return;
  }
  time_t now = time(NULL);
  for (int section = 0; section < NUM_SECTIONS; section++) {
    s_counts[section] = prv_build_cells(section, now, s_cells[section]);
    // The header counts down to the next train that is actually running.
    s_wait[section][0] = '\0';
    for (int i = 0; i < s_counts[section]; i++) {
      if (s_cells[section][i].flag != FLAG_CANCELLED) {
        prv_format_wait(s_cells[section][i].departure, now, s_wait[section],
                        sizeof(s_wait[section]));
        break;
      }
    }
  }
  s_row_font = prv_pick_row_font(s_cells, s_counts, width);
  if (s_settings.layout == LAYOUT_BOARD) {
    prv_build_board(now);
  } else if (s_settings.layout == LAYOUT_DIAL) {
    prv_build_dial(now);
  }
  s_layout_stale = false;
}

// Redraws after a change to what the rows show.
static void prv_content_changed(void) {
  s_layout_stale = true;
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void prv_scroll_timer_cb(void *context) {
  s_scroll_timer = NULL;
  layer_mark_dirty(s_canvas);
}

// Called at the end of every redraw. While a pass is running it leaves a
// breadcrumb every couple of seconds and schedules the next frame only after
// an idle gap, so the watch is never drawing back to back.
static void prv_scroll_after_draw(int width, int64_t now_ms) {
  if (!s_scroll_start_ms) {
    return;
  }
  s_scroll_frames++;
  bool running = false;
  for (int section = 0; section < NUM_SECTIONS; section++) {
    running |= prv_alert_scrolling(section, width, now_ms);
  }
  if (!running) {
    APP_LOG(APP_LOG_LEVEL_INFO, "scroll done frames=%d draw=%dms", s_scroll_frames,
            s_max_draw_ms);
    s_scroll_start_ms = 0;
    prv_breadcrumb(STAGE_RUNNING, 0);
  } else if (now_ms - s_scroll_breadcrumb_ms >= SCROLL_BREADCRUMB_MS) {
    s_scroll_breadcrumb_ms = now_ms;
    int px = prv_scroll_px(now_ms) / 8;
    prv_breadcrumb(STAGE_SCROLL, px > 255 ? 255 : (uint8_t)px);
  }
  // One more frame after the pass ends brings the label back.
  if (!s_scroll_timer) {
    s_scroll_timer = app_timer_register(SCROLL_IDLE_MS, prv_scroll_timer_cb, NULL);
  }
}

// Starts a scroll pass if there is an alert and none is running.
static void prv_start_scroll(void) {
  if (s_scroll_start_ms || s_diag.alerts_blocked || s_settings.layout != LAYOUT_CLASSIC) {
    return;
  }
  for (int section = 0; section < NUM_SECTIONS; section++) {
    if (s_alerts[section][0]) {
      s_scroll_start_ms = prv_now_ms();
      s_scroll_breadcrumb_ms = s_scroll_start_ms;
      s_scroll_frames = 0;
      prv_breadcrumb(STAGE_SCROLL, 0);
      layer_mark_dirty(s_canvas);
      return;
    }
  }
}

static void prv_canvas_draw(Layer *layer, GContext *ctx);

// Times each redraw; the slowest since the last heartbeat is reported.
static void prv_canvas_update(Layer *layer, GContext *ctx) {
  int64_t start_ms = prv_now_ms();
  prv_canvas_draw(layer, ctx);
  int64_t end_ms = prv_now_ms();
  uint16_t took = prv_cap16((int32_t)(end_ms - start_ms));
  if (took > s_max_draw_ms) {
    s_max_draw_ms = took;
  }
  prv_scroll_after_draw(layer_get_bounds(layer).size.w, end_ms);
}

static bool prv_has_bikes(void) {
  for (int i = 0; i < NUM_BIKES; i++) {
    if (s_bikes[2 * i] != BIKE_NONE) {
      return true;
    }
  }
  return false;
}

// One fetch indicator: a steady dot, a pulsing dot while fetching, an X on failure.
static void prv_draw_fetch_dot(GContext *ctx, GPoint dot, FetchKind kind) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  if (s_fetch_state[kind] == FETCH_FAILED) {
    int r = BT_DOT_RADIUS;
    graphics_draw_line(ctx, GPoint(dot.x - r, dot.y - r), GPoint(dot.x + r, dot.y + r));
    graphics_draw_line(ctx, GPoint(dot.x - r, dot.y + r), GPoint(dot.x + r, dot.y - r));
  } else if (s_fetch_state[kind] == FETCH_PENDING) {
    // Radius runs 0..3..0 so the dot grows and shrinks.
    int phase = s_fetch_ticks[kind] % (2 * BT_DOT_RADIUS);
    int r = phase <= BT_DOT_RADIUS ? phase : 2 * BT_DOT_RADIUS - phase;
    if (r > 0) {
      graphics_fill_circle(ctx, dot, r);
    } else {
      graphics_draw_pixel(ctx, dot);
    }
  } else {
    graphics_fill_circle(ctx, dot, BT_DOT_RADIUS);
  }
}

// Draws the dock pies. Each keeps its slot's position so docks are recognised
// by where they sit. One dot per available e-bike orbits the pie, starting at
// 12 o'clock and going clockwise.
static void prv_draw_bikes(GContext *ctx) {
  static const GPoint s_orbit[BIKE_MAX_EBIKES] = {
    { 0, -5 }, { 4, -4 }, { 5, 0 }, { 4, 4 }, { 0, 5 }, { -4, 4 }, { -5, 0 }, { -4, -4 },
  };
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  int spacing = BIKE_SPACING;
  for (int i = BIKE_WIDE_SLOTS; i < NUM_BIKES; i++) {
    if (s_bikes[2 * i] != BIKE_NONE) {
      spacing = BIKE_SPACING_TIGHT;
    }
  }
  for (int i = 0; i < NUM_BIKES; i++) {
    int level = s_bikes[2 * i] - 'a';
    int ebikes = s_bikes[2 * i + 1] - '0';
    if (level < 0 || level > BIKE_CLOSED) {
      continue;
    }
    GPoint centre = GPoint(BIKE_LEFT + i * spacing, BIKE_Y);
    if (level == BIKE_CLOSED) {
      // A ring with an X through it.
      int r = BT_DOT_RADIUS;
      graphics_draw_circle(ctx, centre, r);
      graphics_draw_line(ctx, GPoint(centre.x - r + 1, centre.y - r + 1),
                         GPoint(centre.x + r - 1, centre.y + r - 1));
      graphics_draw_line(ctx, GPoint(centre.x - r + 1, centre.y + r - 1),
                         GPoint(centre.x + r - 1, centre.y - r + 1));
      continue;
    }
    GRect square = GRect(centre.x - BT_DOT_RADIUS, centre.y - BT_DOT_RADIUS,
                         2 * BT_DOT_RADIUS + 1, 2 * BT_DOT_RADIUS + 1);
    if (level == BIKE_FULL) {
      graphics_fill_rect(ctx, square, 0, GCornerNone);
    } else if (level == BIKE_ONE_FREE) {
      graphics_draw_rect(ctx, square);
    } else {
      graphics_draw_circle(ctx, centre, BT_DOT_RADIUS);
    }
    if (level > BIKE_LEVELS) {
      // Squares have no pie inside.
    } else if (level == BIKE_LEVELS) {
      graphics_fill_circle(ctx, centre, BT_DOT_RADIUS);
    } else if (level > 0) {
      // A sector clockwise from 12 o'clock.
      GRect box = GRect(centre.x - BT_DOT_RADIUS, centre.y - BT_DOT_RADIUS,
                        2 * BT_DOT_RADIUS + 1, 2 * BT_DOT_RADIUS + 1);
      graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, BT_DOT_RADIUS + 1, 0,
                           TRIG_MAX_ANGLE * level / BIKE_LEVELS);
    }
    for (int e = 0; e < ebikes && e < BIKE_MAX_EBIKES; e++) {
      // A 2x2 dot that grows away from the pie.
      GPoint o = s_orbit[e];
      graphics_fill_rect(ctx, GRect(centre.x + o.x - (o.x < 0 ? 1 : 0),
                                    centre.y + o.y - (o.y < 0 ? 1 : 0), 2, 2), 0, GCornerNone);
    }
  }
}

// The phone connection dot, then the trains' and the docks' fetch dots, each
// a step further along.
static void prv_draw_status(GContext *ctx, GPoint dot, int dx, int dy) {
  if (s_connected) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, dot, BT_DOT_RADIUS);
  } else {
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_circle(ctx, dot, BT_DOT_RADIUS);
  }
  dot = GPoint(dot.x + dx, dot.y + dy);
  prv_draw_fetch_dot(ctx, dot, FETCH_TRAINS);
  // The docks' dot only appears when there are docks, or their fetch failed.
  if (prv_has_bikes() || s_fetch_state[FETCH_BIKES] != FETCH_OK) {
    dot = GPoint(dot.x + dx, dot.y + dy);
    prv_draw_fetch_dot(ctx, dot, FETCH_BIKES);
  }
}

// Draws one cluster of docks as a stack of bars joined by a spine on the
// left. Each dock keeps its place in its stack.
static void prv_draw_cluster(GContext *ctx, int c, int cx, int y) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  {
    int first = -1;
    int last = -1;
    for (int i = 0; i < CLUSTER_SIZE; i++) {
      int slot = c * CLUSTER_SIZE + i;
      int level = s_bikes[2 * slot] - 'a';
      if (level < 0 || level > BIKE_CLOSED) {
        continue;
      }
      first = first < 0 ? i : first;
      last = i;
      int by = y + i * BAR_PITCH;
      int bx = cx + CLUSTER_BAR_X;
      if (level == BIKE_CLOSED) {
        // An X in place of the bar: a bar this short has no room for an X
        // through its outline.
        int x0 = bx + (BAR_W - BAR_H) / 2;
        graphics_draw_line(ctx, GPoint(x0, by), GPoint(x0 + BAR_H - 1, by + BAR_H - 1));
        graphics_draw_line(ctx, GPoint(x0, by + BAR_H - 1), GPoint(x0 + BAR_H - 1, by));
      } else if (level == BIKE_FULL) {
        graphics_fill_rect(ctx, GRect(cx + 1, by, CLUSTER_BAR_X - 1 + BAR_W, BAR_H), 0, GCornerNone);
      } else if (level == BIKE_ONE_FREE) {
        graphics_fill_rect(ctx, GRect(bx, by, BAR_W - 5, BAR_H), 0, GCornerNone);
        graphics_draw_rect(ctx, GRect(bx + BAR_W - 4, by, 4, BAR_H));
      } else {
        graphics_draw_rect(ctx, GRect(bx, by, BAR_W, BAR_H));
        if (level > 0) {
          // Always some fill when there is a bike, and always a clear gap
          // when there are spaces, so neither can pass for empty or full.
          int fill = (BAR_W - 2) * level / BIKE_LEVELS;
          fill = fill < 2 ? 2 : fill > BAR_W - 6 ? BAR_W - 6 : fill;
          graphics_fill_rect(ctx, GRect(bx + 1, by + 1, fill, BAR_H - 2), 0, GCornerNone);
        }
      }
      if (s_bikes[2 * slot + 1] > '0') {
        graphics_fill_rect(ctx, GRect(cx + CLUSTER_EBIKE_X, by + 1, 2, BAR_H - 2), 0, GCornerNone);
      }
    }
    if (first >= 0) {
      graphics_draw_line(ctx, GPoint(cx, y + first * BAR_PITCH),
                         GPoint(cx, y + last * BAR_PITCH + BAR_H - 1));
    }
  }
}

// The clusters side by side.
static void prv_draw_clusters(GContext *ctx, int x, int y, int pitch) {
  for (int c = 0; c < NUM_CLUSTERS; c++) {
    prv_draw_cluster(ctx, c, x + c * pitch, y);
  }
}

static void prv_draw_classic(GContext *ctx, GRect bounds, int64_t now_ms) {
  int width = bounds.size.w;
  const RowFont *rf = s_row_font;
  // Sections stack from the bottom; the clock is centred in whatever space remains.
  int section_h = HEADER_HEIGHT + rf->spec->row_h;
  int sections_top = bounds.size.h - NUM_SECTIONS * section_h;
  graphics_context_set_text_color(ctx, GColorWhite);
  // The digits are centred between the indicator row and the sections.
  int digits_top = INDICATOR_BOTTOM + (sections_top - INDICATOR_BOTTOM - TIME_GLYPH_HEIGHT) / 2;
  prv_draw_text(ctx, s_time_buffer, s_time_font,
                GRect(0, digits_top - TIME_GLYPH_TOP, width, TIME_HEIGHT),
                GTextAlignmentCenter);

  prv_draw_status(ctx, GPoint(width - BT_DOT_MARGIN - BT_DOT_RADIUS - 1,
                              BT_DOT_MARGIN + BT_DOT_RADIUS), -FETCH_DOT_SPACING, 0);
  prv_draw_bikes(ctx);

  for (int section = 0; section < NUM_SECTIONS; section++) {
    int y = sections_top + section * section_h;
    prv_draw_header(ctx, y, width, section, s_wait[section], now_ms);
    prv_draw_row(ctx, y + HEADER_HEIGHT, width, section, s_cells[section], s_counts[section],
                 rf, now_ms);
  }
}

// One board line's text, shifted by (dx, dy) for the outline behind a delayed train.
static void prv_draw_board_text(GContext *ctx, const BoardRow *row, int y, int width, int dx,
                                int dy) {
  bool cancelled = row->flag == FLAG_CANCELLED;
  prv_draw_text(ctx, row->time, s_board_font, GRect(2 + dx, y - 2 + dy, BOARD_TO_X, 24),
                GTextAlignmentLeft);
  int to_x = BOARD_TO_X;
  int right_w = cancelled ? 30 : row->mins_w;
  if (s_alerts[row->section][0]) {
    prv_draw_text(ctx, "!", s_header_font, GRect(to_x + dx, y + 1 + dy, 8, 20),
                  GTextAlignmentLeft);
    to_x += 6;
  }
  prv_draw_text(ctx, s_to[row->section][row->index], s_board_to_font,
                GRect(to_x + dx, y + 1 + dy, width - 3 - right_w - 4 - to_x, 20),
                GTextAlignmentLeft);
  if (cancelled) {
    prv_draw_text(ctx, "CANC", s_header_font, GRect(width - 43 + dx, y + 1 + dy, 40, 20),
                  GTextAlignmentRight);
  } else {
    prv_draw_text(ctx, row->mins, s_board_font, GRect(width - 43 + dx, y - 2 + dy, 40, 24),
                  GTextAlignmentRight);
  }
}

// The strip shared by the newer layouts' top edge: clock, docks, status dots.
static void prv_draw_board(GContext *ctx, GRect bounds, int64_t now_ms) {
  int width = bounds.size.w;
  graphics_context_set_text_color(ctx, GColorWhite);
  prv_draw_text(ctx, s_time_buffer, s_time_font_small, GRect(2, -5, 72, 32), GTextAlignmentLeft);
  prv_draw_clusters(ctx, 75, 3, 20);
  prv_draw_status(ctx, GPoint(width - 5, 20), 0, -8);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(0, BOARD_TOP, width, BOARD_BAR_H), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  GRect bar = GRect(3, BOARD_TOP - 4, width - 6, 18);
  prv_draw_text(ctx, "TIME", s_header_font, bar, GTextAlignmentLeft);
  prv_draw_text(ctx, "MIN", s_header_font, bar, GTextAlignmentRight);
  bar.origin.x = BOARD_TO_X;
  prv_draw_text(ctx, "TO", s_header_font, bar, GTextAlignmentLeft);

  int rows_top = BOARD_TOP + BOARD_BAR_H;
  if (s_board_count == 0) {
    graphics_context_set_text_color(ctx, GColorWhite);
    bool unset = true;
    for (int section = 0; section < NUM_SECTIONS; section++) {
      unset &= s_data.mode[section] == MODE_UNSET;
    }
    prv_draw_text(ctx, !s_has_data ? "Loading..." : unset ? "Set up on phone" : "No departures",
                  s_board_font,
                  GRect(0, rows_top + 40, width, 24), GTextAlignmentCenter);
    return;
  }
  for (int i = 0; i < s_board_count; i++) {
    const BoardRow *row = &s_board[i];
    int y = rows_top + i * BOARD_ROW_H;
    bool cancelled = row->flag == FLAG_CANCELLED;
    bool inverted = !cancelled && prv_is_highlighted(row->section, row->departure, now_ms);
    GColor fg = inverted ? GColorBlack : GColorWhite;
    GColor bg = inverted ? GColorWhite : GColorBlack;
    GRect line = GRect(0, y, width, BOARD_ROW_H - 1);
    if (inverted) {
      graphics_context_set_fill_color(ctx, bg);
      graphics_fill_rect(ctx, line, 0, GCornerNone);
    }
    if (row->flag == FLAG_DELAYED) {
      prv_draw_hatch(ctx, line, fg);
      // Outline the text in the background colour so it stays readable.
      graphics_context_set_text_color(ctx, bg);
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          if (dx || dy) {
            prv_draw_board_text(ctx, row, y, width, dx, dy);
          }
        }
      }
    }
    graphics_context_set_text_color(ctx, fg);
    prv_draw_board_text(ctx, row, y, width, 0, 0);
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 1);
    if (cancelled) {
      graphics_draw_line(ctx, GPoint(2, y + 11), GPoint(width - 40, y + 11));
    }
    if (!inverted) {
      // Starts one pixel in so the dots sit the same distance from both edges.
      for (int x = 1; x < width; x += 3) {
        graphics_draw_pixel(ctx, GPoint(x, y + BOARD_ROW_H - 1));
      }
    }
  }
}

// Draws a stretch of a ring clockwise from one angle to another, as wide as
// the ring itself plus `extra` pixels on each side.
static void prv_draw_ring_arc(GContext *ctx, int r, int extra, int32_t from, int32_t to) {
  int outer = r + extra;
  GRect box = GRect(DIAL_CX - outer, DIAL_CY - outer, 2 * outer + 1, 2 * outer + 1);
  int thickness = 2 * extra + 1;
  if (to < from) {
    graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, thickness, from, TRIG_MAX_ANGLE);
    from = 0;
  }
  graphics_fill_radial(ctx, box, GOvalScaleModeFitCircle, thickness, from, to);
}

// Draws a ring as evenly spaced dots. The circle is walked one pixel at a
// time (the midpoint method) and every DIAL_DOT_STEP-th pixel is lit, mirrored
// into all eight octants, so the dots never bunch or leave uneven gaps the way
// dots placed by angle do.
static void prv_draw_dotted_ring(GContext *ctx, int r) {
  int x = 0;
  int y = r;
  int d = 1 - r;
  for (int i = 0; x <= y; i++) {
    if (i % DIAL_DOT_STEP == 0) {
      graphics_draw_pixel(ctx, GPoint(DIAL_CX + x, DIAL_CY - y));
      graphics_draw_pixel(ctx, GPoint(DIAL_CX - x, DIAL_CY - y));
      graphics_draw_pixel(ctx, GPoint(DIAL_CX + x, DIAL_CY + y));
      graphics_draw_pixel(ctx, GPoint(DIAL_CX - x, DIAL_CY + y));
      graphics_draw_pixel(ctx, GPoint(DIAL_CX + y, DIAL_CY - x));
      graphics_draw_pixel(ctx, GPoint(DIAL_CX - y, DIAL_CY - x));
      graphics_draw_pixel(ctx, GPoint(DIAL_CX + y, DIAL_CY + x));
      graphics_draw_pixel(ctx, GPoint(DIAL_CX - y, DIAL_CY + x));
    }
    x++;
    if (d < 0) {
      d += 2 * x + 1;
    } else {
      y--;
      d += 2 * (x - y) + 1;
    }
  }
}

static void prv_draw_dial(GContext *ctx, int64_t now_ms) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  for (int section = 0; section < NUM_SECTIONS; section++) {
    int r = s_dial_r[section];
    prv_draw_dotted_ring(ctx, r);
    // Time to leave. A bold bar runs from the hand to where the hand will be
    // when the warning range ends, so it shrinks to nothing as the hand closes
    // in; it pulses between bold and thin with the train's marker. A thin line carries on from
    // there to the train.
    int64_t lo, hi;
    if (!prv_warning_range(section, &lo, &hi)) {
      continue;
    }
    for (int i = 0; i < s_mark_count[section]; i++) {
      const DialMark *mark = &s_marks[section][i];
      if (mark->flag == FLAG_CANCELLED ||
          !prv_in_blink_window(section, mark->departure, now_ms)) {
        continue;
      }
      int32_t lead = TRIG_MAX_ANGLE * (int32_t)(lo / 1000 % DIAL_HOUR_S) / DIAL_HOUR_S;
      int32_t end = (mark->angle - lead + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;
      prv_draw_ring_arc(ctx, r, 0, end, mark->angle);
      int32_t span = (end - s_dial_now_angle + TRIG_MAX_ANGLE) % TRIG_MAX_ANGLE;
      // The hand's angle is a few seconds old, so a bar about to end can
      // come out as nearly a full turn; that one is already over.
      if (span > 0 && span <= TRIG_MAX_ANGLE / 2) {
        // The bar blinks between bold and thin; it never drops back to dots.
        prv_draw_ring_arc(ctx, r, s_blink[section].inverted ? 1 : 0, s_dial_now_angle, end);
      }
    }
  }

  // The hand points at the current minute, with a tick where it crosses each ring.
  graphics_draw_line(ctx, prv_polar(DIAL_HAND_INNER, s_dial_now_angle),
                     prv_polar(DIAL_HAND_OUTER, s_dial_now_angle));
  for (int section = 0; section < NUM_SECTIONS; section++) {
    GPoint at = prv_polar(s_dial_r[section], s_dial_now_angle);
    graphics_fill_rect(ctx, GRect(at.x - 1, at.y - 1, 3, 3), 0, GCornerNone);
  }

  for (int section = 0; section < NUM_SECTIONS; section++) {
    for (int i = 0; i < s_mark_count[section]; i++) {
      const DialMark *mark = &s_marks[section][i];
      GPoint at = mark->at;
      if (mark->flag == FLAG_CANCELLED) {
        // A crossed-out circle.
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, at, 5);
        graphics_draw_circle(ctx, at, 5);
        graphics_draw_line(ctx, GPoint(at.x - 3, at.y - 3), GPoint(at.x + 3, at.y + 3));
        graphics_draw_line(ctx, GPoint(at.x - 3, at.y + 3), GPoint(at.x + 3, at.y - 3));
      } else if (prv_is_highlighted(section, mark->departure, now_ms)) {
        // Time to leave: the marker blinks between a dot and a ringed dot.
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, at, 6);
        graphics_draw_circle(ctx, at, 6);
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_circle(ctx, at, 3);
      } else if (mark->flag == FLAG_DELAYED) {
        // A hollow circle.
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, at, 4);
        graphics_draw_circle(ctx, at, 4);
      } else {
        graphics_context_set_fill_color(ctx, GColorBlack);
        graphics_fill_circle(ctx, at, 5);
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_circle(ctx, at, 4);
      }
    }
  }

  // Minutes until each ring's next departure, in a gap cut into the ring
  // straight below or above the centre.
  for (int section = 0; section < NUM_SECTIONS; section++) {
    int y = DIAL_CY + s_dial_label_side * s_dial_r[section];
    int w = s_dial_label_w[section];
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(DIAL_CX - w / 2 - 3, y - 5, w + 6, 11), 0, GCornerNone);
    graphics_context_set_fill_color(ctx, GColorWhite);
    prv_small_number(ctx, s_dial_mins[section], DIAL_CX - w / 2, y - SMALL_DIGIT_H / 2);
  }
  graphics_context_set_text_color(ctx, GColorWhite);

  // The time fills the middle of the rings. The screen's corners, which the
  // rings leave free, hold one cluster each and the status dots.
  prv_draw_text(ctx, s_time_buffer, s_time_font_dial,
                GRect(0, DIAL_CY - DIAL_TIME_DY, 2 * DIAL_CX, 40), GTextAlignmentCenter);
  // They keep the same distance from the screen edge as the outer ring does.
  int inset = DIAL_CX - s_dial_r[0];
  int right = 2 * DIAL_CX - CLUSTER_W - inset;
  int bottom = 2 * DIAL_CY - CLUSTER_H - inset;
  prv_draw_cluster(ctx, 0, inset, inset);
  prv_draw_cluster(ctx, 1, right, inset);
  prv_draw_cluster(ctx, 2, inset, bottom);
  prv_draw_status(ctx, GPoint(2 * DIAL_CX - inset - BT_DOT_RADIUS - 1,
                              2 * DIAL_CY - inset - BT_DOT_RADIUS - 1), 0, -8);
}

static void prv_canvas_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int64_t now_ms = prv_now_ms();
  // The cells are static: the app stack is only about 2KB, and the text
  // drawing below needs most of it.
  prv_update_layout(bounds.size.w);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  if (s_settings.layout == LAYOUT_BOARD) {
    prv_draw_board(ctx, bounds, now_ms);
  } else if (s_settings.layout == LAYOUT_DIAL) {
    prv_draw_dial(ctx, now_ms);
  } else {
    prv_draw_classic(ctx, bounds, now_ms);
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
      int32_t visible[NUM_TIMES];
      char flags[NUM_TIMES];
      int n = prv_upcoming(section, (time_t)(now_ms / 1000), visible, NULL, flags,
                           s_settings.layout == LAYOUT_CLASSIC ? prv_departures() : NUM_TIMES);
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
    .warn_on = { false, false, false },
    .warn_start_min = { 10, 10, 10 },
    .warn_end_min = { 5, 5, 5 },
    .departures = DEFAULT_DEPARTURES,
    .labels = { "", "", "" },
  };
}

static void prv_load_settings(void) {
  prv_default_settings();
  persist_read_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));
  for (int section = 0; section < NUM_SECTIONS; section++) {
    s_settings.labels[section][LABEL_SIZE - 1] = '\0';
  }
  if (s_settings.layout >= NUM_LAYOUTS) {
    s_settings.layout = LAYOUT_CLASSIC;
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
  if ((t = dict_find(iter, MESSAGE_KEY_LAYOUT))) {
    int32_t v = prv_tuple_int(t);
    s_settings.layout = v < 0 || v >= NUM_LAYOUTS ? LAYOUT_CLASSIC : (uint8_t)v;
    changed = true;
  }
  if (changed) {
    // Saving the settings is how the user turns alerts back on after a block.
    if (s_diag.alerts_blocked) {
      s_diag.alerts_blocked = 0;
      prv_breadcrumb(s_diag.stage, s_diag.detail);
      APP_LOG(APP_LOG_LEVEL_INFO, "alerts unblocked");
    }
    persist_write_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(s_settings));
  }
  return changed;
}

static void prv_fetch_timer_cb(void *context);

static void prv_set_fetch_state(FetchKind kind, FetchState state) {
  s_fetch_state[kind] = state;
  s_fetch_ticks[kind] = 0;
  bool pending = false;
  for (int i = 0; i < NUM_FETCHES; i++) {
    pending |= s_fetch_state[i] == FETCH_PENDING;
  }
  // One timer pulses whichever indicators are waiting.
  if (pending && !s_fetch_timer) {
    s_fetch_timer = app_timer_register(FETCH_PULSE_MS, prv_fetch_timer_cb, NULL);
  } else if (!pending && s_fetch_timer) {
    app_timer_cancel(s_fetch_timer);
    s_fetch_timer = NULL;
  }
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

// Animates the pulse, and fails a fetch if the phone never answers.
static void prv_fetch_timer_cb(void *context) {
  s_fetch_timer = NULL;
  bool pending = false;
  for (int i = 0; i < NUM_FETCHES; i++) {
    if (s_fetch_state[i] != FETCH_PENDING) {
      continue;
    }
    if (++s_fetch_ticks[i] * FETCH_PULSE_MS >= FETCH_TIMEOUT_MS) {
      s_fetch_state[i] = FETCH_FAILED;
    } else {
      pending = true;
    }
  }
  if (pending) {
    s_fetch_timer = app_timer_register(FETCH_PULSE_MS, prv_fetch_timer_cb, NULL);
  }
  layer_mark_dirty(s_canvas);
}

// Applies a request's progress to the indicators it covers.
static void prv_set_request_state(uint8_t what, FetchState state) {
  if (what == REQUEST_ALL) {
    prv_set_fetch_state(FETCH_TRAINS, state);
  }
  if (what == REQUEST_BIKES || prv_has_bikes()) {
    prv_set_fetch_state(FETCH_BIKES, state);
  }
}

// Asks the phone for everything, or only the docks. A full request also
// fetches the docks when there are any.
static void prv_request_update(uint8_t what) {
  DictionaryIterator *iter;
  bool ok = app_message_outbox_begin(&iter) == APP_MSG_OK;
  if (ok) {
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST, what);
    ok = app_message_outbox_send() == APP_MSG_OK;
  }
  prv_set_request_state(what, ok ? FETCH_PENDING : FETCH_FAILED);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed reason=%d", (int)reason);
  // Only a failed data request fails a fetch, not a failed diagnostic.
  Tuple *request = dict_find(iter, MESSAGE_KEY_REQUEST);
  if (request) {
    prv_set_request_state(request->value->uint8, FETCH_FAILED);
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
  bool alert_changed = false;
  bool to_changed = false;
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
      memset(s_to[section], 0, sizeof(s_to[section]));
      to_changed = true;
    }
    // Destination names, one per time, separated by '|'.
    if ((t = dict_find(iter, MESSAGE_KEY_TO + section)) && t->type == TUPLE_CSTRING) {
      const char *p = t->value->cstring;
      memset(s_to[section], 0, sizeof(s_to[section]));
      for (int i = 0; i < NUM_TIMES && *p; i++) {
        int n = 0;
        while (*p && *p != '|') {
          if (n < TO_SIZE - 1) {
            s_to[section][i][n++] = *p;
          }
          p++;
        }
        if (*p == '|') {
          p++;
        }
      }
      to_changed = true;
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
    if ((t = dict_find(iter, MESSAGE_KEY_ALERT + section)) && t->type == TUPLE_CSTRING &&
        strncmp(s_alerts[section], t->value->cstring, ALERT_SIZE - 1) != 0) {
      strncpy(s_alerts[section], t->value->cstring, ALERT_SIZE - 1);
      s_alerts[section][ALERT_SIZE - 1] = '\0';
      s_alert_w[section] = 0;
      for (int i = 0; s_alerts[section][i]; i++) {
        char ch[2] = { s_alerts[section][i], '\0' };
        // A lone space measures as empty, so measure it between two letters.
        int w = ch[0] == ' ' ? prv_text_width("n n", s_header_font) -
                               prv_text_width("nn", s_header_font)
                             : prv_text_width(ch, s_header_font);
        s_alert_cw[section][i] = (uint8_t)w;
        s_alert_w[section] += w;
      }
      APP_LOG(APP_LOG_LEVEL_INFO, "alert row=%d len=%d w=%d blocked=%d", section,
              (int)strlen(s_alerts[section]), s_alert_w[section], s_diag.alerts_blocked);
      alert_changed = true;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_MODE + section))) {
      int32_t mode = prv_tuple_int(t);
      s_data.mode[section] = mode == MODE_MINUTES || mode == MODE_UNSET || mode == MODE_BUS
                             ? (uint8_t)mode : MODE_CLOCK;
      changed = true;
    }
  }

  if (changed) {
    s_has_data = true;
    persist_write_data(PERSIST_KEY_DATA, &s_data, sizeof(s_data));
  }
  if (to_changed) {
    persist_write_data(PERSIST_KEY_TO, s_to, sizeof(s_to));
  }

  Tuple *bikes = dict_find(iter, MESSAGE_KEY_BIKES);
  if (bikes && bikes->type == TUPLE_CSTRING &&
      strlen(bikes->value->cstring) == 2 * NUM_BIKES &&
      strncmp(s_bikes, bikes->value->cstring, 2 * NUM_BIKES) != 0) {
    memcpy(s_bikes, bikes->value->cstring, 2 * NUM_BIKES);
    layer_mark_dirty(s_canvas);
  }

  // Every fetch result from the phone says how many rows failed to load.
  Tuple *failed = dict_find(iter, MESSAGE_KEY_FETCH_FAILED);
  if (failed) {
    prv_set_fetch_state(FETCH_TRAINS, prv_tuple_int(failed) > 0 ? FETCH_FAILED : FETCH_OK);
  }
  Tuple *bike_failed = dict_find(iter, MESSAGE_KEY_BIKE_FAILED);
  if (bike_failed) {
    prv_set_fetch_state(FETCH_BIKES, prv_tuple_int(bike_failed) > 0 ? FETCH_FAILED : FETCH_OK);
  }

  // The phone is reachable right now, so report the previous run if still owed.
  prv_send_prev_diag();

  bool settings_changed = prv_read_settings(iter);
  if (changed || settings_changed || alert_changed || to_changed) {
    prv_content_changed();
    prv_update_blink();
  }
  if (alert_changed) {
    // A changed alert restarts the pass from the beginning.
    s_scroll_start_ms = 0;
    prv_start_scroll();
  }
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  prv_update_time_string(tick_time);
  prv_content_changed();
  prv_update_blink();
  prv_start_scroll();
  // The handler also runs whenever the phone sets the watch's clock, so the
  // time since the last request is checked too.
  time_t now = time(NULL);
  APP_LOG(APP_LOG_LEVEL_INFO, "hb up=%lds heap=%d draw=%dms fetch=%d bikes=%d bt=%d",
          (long)(now - (time_t)s_diag.started), (int)heap_bytes_free(), s_max_draw_ms,
          (int)s_fetch_state[FETCH_TRAINS], (int)s_fetch_state[FETCH_BIKES], (int)s_connected);
  if (s_diag.stage == STAGE_INIT ||
      (s_diag.stage == STAGE_RUNNING && tick_time->tm_min % DIAG_PERSIST_MINUTES == 0)) {
    prv_breadcrumb(STAGE_RUNNING, 0);
  }
  s_max_draw_ms = 0;
  // Trains refresh every REFRESH_MINUTES; docks every minute in between.
  if (now - s_last_request >= 50) {
    if (tick_time->tm_min % REFRESH_MINUTES == 0) {
      s_last_request = now;
      prv_request_update(REQUEST_ALL);
    } else if (prv_has_bikes()) {
      s_last_request = now;
      prv_request_update(REQUEST_BIKES);
    }
  }
}

static void prv_connection_handler(bool connected) {
  // The phone fetches by itself when the app starts or the watch reconnects.
  if (connected && !s_connected) {
    prv_set_request_state(REQUEST_ALL, FETCH_PENDING);
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
  // The settings come first: the breadcrumb records the layout in use.
  prv_load_settings();
  prv_diag_init();

  s_time_font = fonts_get_system_font(FONT_KEY_LECO_36_BOLD_NUMBERS);
  s_header_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_time_font_small = fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM);
  s_time_font_dial = fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS);
  s_board_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_board_to_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
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
  if (persist_read_data(PERSIST_KEY_TO, s_to, sizeof(s_to)) != (int)sizeof(s_to)) {
    memset(s_to, 0, sizeof(s_to));
  }
  for (int section = 0; section < NUM_SECTIONS; section++) {
    for (int i = 0; i < NUM_TIMES; i++) {
      s_to[section][i][TO_SIZE - 1] = '\0';
    }
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
  // Three full-length alerts on top of the departures and their destination
  // names need about 1200 bytes.
  app_message_open(1500, 128);

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_connection_handler,
  });
  s_fetch_state[FETCH_TRAINS] = FETCH_FAILED;
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
  if (s_scroll_timer) {
    app_timer_cancel(s_scroll_timer);
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
