#include "parrot_window.h"
#include "parrot_anim.h"

#define BUBBLE_HEIGHT 64
#define BUBBLE_MARGIN 8
#define STATE_LABEL_HEIGHT 22
// Used to size the parrot layer only if the IDLE bitmap somehow fails to load
// (e.g. resource corruption) -- matches gen_art.py's logical frame size.
#define FALLBACK_PARROT_SIZE 140

static Window *s_window;
static BitmapLayer *s_bg_layer;
static GBitmap *s_bg_bitmap;
static BitmapLayer *s_parrot_layer;
static GPoint s_parrot_origin;   // resting top-left of the parrot frame; set_offset nudges from here

static TextLayer *s_state_label;

static Layer *s_bubble_layer;
static TextLayer *s_bubble_text;
static char s_bubble_buf[256];

static ActionBarLayer *s_action_bar;
static GBitmap *s_icon_ask, *s_icon_talk, *s_icon_phrases;

static AppUiState s_state = UI_STATE_IDLE;
static void (*s_select_handler)(void);
static void (*s_up_handler)(void);
static void (*s_down_handler)(void);
static void (*s_back_handler)(void);

// --- Pose bitmaps: loaded in small per-state groups, never all 5 at once ----
//
// Each ~140x168px sprite costs ~1 byte/pixel resident (GBitmap is stored in the
// watch's native 8-bit color format regardless of the source PNG's complexity),
// and the full-screen jungle backdrop alone is ~45KB. Holding all 5 poses plus
// that backdrop resident at launch overflows emery's ~128KB app heap --
// `gbitmap_create_with_resource` then logs "PNG memory allocation failed" and
// hands back a bitmap that crashes the app the moment it's first drawn (i.e.
// exactly when a SPEAK_1/2 pose -- whichever failed -- gets shown). Loading only
// the 1-3 bitmaps relevant to the *current* UI state keeps peak resident pose
// memory well within budget.
typedef enum {
  POSE_GROUP_NONE,
  POSE_GROUP_IDLE,     // IDLE, BLINK, TILT -- UI_STATE_IDLE (cycling + touch reaction)
  POSE_GROUP_NEUTRAL,  // IDLE only -- listening/thinking/error (held, no fidgeting)
  POSE_GROUP_SPEAK,    // SPEAK_1, SPEAK_2 -- UI_STATE_SPEAKING
  POSE_GROUP_FLAP,     // IDLE, FLAP -- transient, only during an idle wing-flap
} PoseGroup;

#define POSE_GROUP_MAX_BITMAPS 3
static GBitmap *s_group_bitmaps[POSE_GROUP_MAX_BITMAPS];
static PoseGroup s_pose_group = POSE_GROUP_NONE;

static void prv_unload_pose_group(void) {
  for (int i = 0; i < POSE_GROUP_MAX_BITMAPS; i++) {
    gbitmap_destroy(s_group_bitmaps[i]);
    s_group_bitmaps[i] = NULL;
  }
  s_pose_group = POSE_GROUP_NONE;
}

static void prv_load_pose_group(PoseGroup group) {
  if (s_pose_group == group) {
    return;
  }
  prv_unload_pose_group();
  switch (group) {
    case POSE_GROUP_IDLE:
      s_group_bitmaps[0] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_IDLE);
      s_group_bitmaps[1] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_BLINK);
      s_group_bitmaps[2] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_TILT);
      break;
    case POSE_GROUP_NEUTRAL:
      s_group_bitmaps[0] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_IDLE);
      break;
    case POSE_GROUP_SPEAK:
      s_group_bitmaps[0] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_SPEAK_1);
      s_group_bitmaps[1] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_SPEAK_2);
      break;
    case POSE_GROUP_FLAP:
      s_group_bitmaps[0] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_IDLE);
      s_group_bitmaps[1] = gbitmap_create_with_resource(RESOURCE_ID_IMG_PARROT_FLAP);
      break;
    case POSE_GROUP_NONE:
      break;
  }
  s_pose_group = group;
}

static PoseGroup prv_pose_group_for_state(AppUiState state) {
  switch (state) {
    case UI_STATE_IDLE:     return POSE_GROUP_IDLE;
    case UI_STATE_SPEAKING: return POSE_GROUP_SPEAK;
    default:                return POSE_GROUP_NEUTRAL;  // listening/thinking/error
  }
}

static GBitmap *prv_bitmap_for_pose(ParrotPose pose) {
  switch (s_pose_group) {
    case POSE_GROUP_IDLE:
      switch (pose) {
        case PARROT_POSE_IDLE:  return s_group_bitmaps[0];
        case PARROT_POSE_BLINK: return s_group_bitmaps[1];
        case PARROT_POSE_TILT:  return s_group_bitmaps[2];
        default: return NULL;
      }
    case POSE_GROUP_NEUTRAL:
      return (pose == PARROT_POSE_IDLE) ? s_group_bitmaps[0] : NULL;
    case POSE_GROUP_SPEAK:
      switch (pose) {
        case PARROT_POSE_SPEAK_1: return s_group_bitmaps[0];
        case PARROT_POSE_SPEAK_2: return s_group_bitmaps[1];
        default: return NULL;
      }
    case POSE_GROUP_FLAP:
      switch (pose) {
        case PARROT_POSE_IDLE: return s_group_bitmaps[0];
        case PARROT_POSE_FLAP: return s_group_bitmaps[1];
        default: return NULL;
      }
    case POSE_GROUP_NONE:
    default:
      return NULL;
  }
}

// --- Speech bubble (rounded rect drawn behind a transparent TextLayer) ------

static void bubble_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 10, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_round_rect(ctx, bounds, 10);
}

static void bubble_set_text(const char *text) {
  strncpy(s_bubble_buf, text, sizeof(s_bubble_buf) - 1);
  s_bubble_buf[sizeof(s_bubble_buf) - 1] = '\0';
  text_layer_set_text(s_bubble_text, s_bubble_buf);
  layer_set_hidden(s_bubble_layer, false);
}

void parrot_window_show_transcript(const char *text) {
  text_layer_set_text_color(s_bubble_text, GColorBlack);
  bubble_set_text(text);
}

void parrot_window_show_message(const char *text) {
  text_layer_set_text_color(s_bubble_text, GColorDarkGray);
  bubble_set_text(text);
}

void parrot_window_clear_bubble(void) {
  layer_set_hidden(s_bubble_layer, true);
  text_layer_set_text(s_bubble_text, "");
}

// --- Jungle backdrop (freed while busy to reclaim heap) ---------------------
//
// The full-screen backdrop is ~45KB resident -- the single biggest heap user.
// While Polly is busy (THINKING/SPEAKING/etc.) we free it and fall back to a
// solid green fill so the audio reassembly buffer has room to malloc; the heap,
// not AUDIO_MAX_BUFFER_BYTES, was the real ceiling on phrase length. It's
// restored on the return to idle, when the buffer is already freed again.

static void prv_release_backdrop(void) {
  if (!s_bg_bitmap) {
    return;
  }
  bitmap_layer_set_bitmap(s_bg_layer, NULL);
  bitmap_layer_set_background_color(s_bg_layer, GColorDarkGreen);
  gbitmap_destroy(s_bg_bitmap);
  s_bg_bitmap = NULL;
}

static void prv_restore_backdrop(void) {
  if (s_bg_bitmap || !s_bg_layer) {
    return;  // already loaded, or window not built yet
  }
  s_bg_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMG_JUNGLE_BG);
  if (s_bg_bitmap) {
    bitmap_layer_set_bitmap(s_bg_layer, s_bg_bitmap);
  }
}

// --- Pose / state -----------------------------------------------------------

void parrot_window_set_pose(ParrotPose pose) {
  GBitmap *bitmap = prv_bitmap_for_pose(pose);
  if (!s_parrot_layer || !bitmap) {
    return;
  }
  bitmap_layer_set_bitmap(s_parrot_layer, bitmap);
}

void parrot_window_set_offset(int16_t dx, int16_t dy) {
  if (!s_parrot_layer) {
    return;
  }
  GRect frame = layer_get_frame(bitmap_layer_get_layer(s_parrot_layer));
  frame.origin.x = s_parrot_origin.x + dx;
  frame.origin.y = s_parrot_origin.y + dy;
  layer_set_frame(bitmap_layer_get_layer(s_parrot_layer), frame);
}

void parrot_window_load_flap_group(void) {
  prv_load_pose_group(POSE_GROUP_FLAP);
  parrot_window_set_pose(PARROT_POSE_IDLE);
}

void parrot_window_load_idle_group(void) {
  prv_load_pose_group(POSE_GROUP_IDLE);
  parrot_window_set_pose(PARROT_POSE_IDLE);
}

AppUiState parrot_window_get_state(void) {
  return s_state;
}

static const char *label_for_state(AppUiState state) {
  switch (state) {
    case UI_STATE_LISTENING: return "Listening...";
    case UI_STATE_THINKING:  return "Thinking...";
    case UI_STATE_SPEAKING:  return "Speaking...";
    case UI_STATE_ERROR:     return "Hmm...";
    case UI_STATE_IDLE:
    default:                 return "Press SELECT to talk to Polly";
  }
}

void parrot_window_set_state(AppUiState state) {
  s_state = state;
  text_layer_set_text(s_state_label, label_for_state(state));

  if (state == UI_STATE_IDLE || state == UI_STATE_LISTENING) {
    parrot_window_clear_bubble();
  }

  // Reclaim the ~45KB backdrop while busy so the audio buffer can malloc;
  // restore it once we're idle again. Done before loading the pose group so the
  // freed heap is available for both the buffer and the pose bitmaps.
  if (state == UI_STATE_IDLE) {
    prv_restore_backdrop();
  } else {
    prv_release_backdrop();
  }

  // Swap in the pose bitmaps this state needs *before* parrot_anim's timers
  // start firing (they call parrot_window_set_pose right away in some cases).
  prv_load_pose_group(prv_pose_group_for_state(state));

  parrot_anim_set_state(state);
}

// --- Click handling ----------------------------------------------------------

void parrot_window_set_select_handler(void (*handler)(void)) {
  s_select_handler = handler;
}

void parrot_window_set_up_handler(void (*handler)(void)) {
  s_up_handler = handler;
}

void parrot_window_set_down_handler(void (*handler)(void)) {
  s_down_handler = handler;
}

void parrot_window_set_back_handler(void (*handler)(void)) {
  s_back_handler = handler;
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_select_handler) {
    s_select_handler();
  }
}

static void prv_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_up_handler) {
    s_up_handler();
  }
}

static void prv_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_down_handler) {
    s_down_handler();
  }
}

static void prv_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_back_handler) {
    s_back_handler();
  }
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, prv_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_down_click_handler);
  // Override the default BACK (which would exit the app) so the caller can
  // bounce back to idle from an error/busy state.
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click_handler);
}

// --- Touch handling (PBL_TOUCH -- emery has a real touchscreen) -------------

static bool point_in_layer_frame(Layer *layer, int16_t x, int16_t y) {
  GRect frame = layer_get_frame(layer);
  return x >= frame.origin.x && x < frame.origin.x + frame.size.w
      && y >= frame.origin.y && y < frame.origin.y + frame.size.h;
}

static void prv_touch_handler(const TouchEvent *event, void *context) {
  if (event->type != TouchEvent_Touchdown || s_state != UI_STATE_IDLE) {
    return;
  }
  if (point_in_layer_frame(bitmap_layer_get_layer(s_parrot_layer), event->x, event->y)) {
    parrot_anim_trigger_happy();
  }
}

// --- Window lifecycle --------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_unobstructed_bounds(window_layer);
  // Reserve the right edge for the action bar; lay the parrot/label/bubble out
  // in the remaining width so nothing hides behind the button icons.
  int16_t content_w = bounds.size.w - ACTION_BAR_WIDTH;

  // Load just the pose group UI_STATE_IDLE needs (IDLE/BLINK/TILT) -- the
  // full set of 5 plus the backdrop below would overflow the app heap at
  // launch. parrot_window_set_state() swaps in SPEAK_1/2 (and frees this
  // group) once speaking actually starts.
  prv_load_pose_group(POSE_GROUP_IDLE);

  // Full-screen jungle backdrop, drawn first so everything else sits on top of it.
  s_bg_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMG_JUNGLE_BG);
  if (s_bg_bitmap) {
    GRect bgb = gbitmap_get_bounds(s_bg_bitmap);
    APP_LOG(APP_LOG_LEVEL_INFO, "BG loaded: %dx%d fmt=%d", bgb.size.w, bgb.size.h,
            gbitmap_get_format(s_bg_bitmap));
  } else {
    APP_LOG(APP_LOG_LEVEL_ERROR, "BG bitmap FAILED to load");
  }
  s_bg_layer = bitmap_layer_create(layer_get_bounds(window_layer));
  bitmap_layer_set_bitmap(s_bg_layer, s_bg_bitmap);
  layer_add_child(window_layer, bitmap_layer_get_layer(s_bg_layer));

  s_state_label = text_layer_create(GRect(0, 0, content_w, STATE_LABEL_HEIGHT));
  text_layer_set_font(s_state_label, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_state_label, GTextAlignmentCenter);
  text_layer_set_background_color(s_state_label, GColorClear);
  text_layer_set_text_color(s_state_label, GColorWhite);  // reads over the darkened top of the backdrop
  layer_add_child(window_layer, text_layer_get_layer(s_state_label));

  GBitmap *idle_bitmap = prv_bitmap_for_pose(PARROT_POSE_IDLE);
  GRect frame = idle_bitmap ? gbitmap_get_bounds(idle_bitmap)
                            : GRect(0, 0, FALLBACK_PARROT_SIZE, FALLBACK_PARROT_SIZE);
  frame.origin.x = (content_w - frame.size.w) / 2;
  frame.origin.y = STATE_LABEL_HEIGHT +
      (bounds.size.h - STATE_LABEL_HEIGHT - frame.size.h) / 2;
  s_parrot_origin = frame.origin;
  s_parrot_layer = bitmap_layer_create(frame);
  bitmap_layer_set_compositing_mode(s_parrot_layer, GCompOpSet);
  if (idle_bitmap) {
    bitmap_layer_set_bitmap(s_parrot_layer, idle_bitmap);
  }
  layer_add_child(window_layer, bitmap_layer_get_layer(s_parrot_layer));

  GRect bubble_frame = GRect(BUBBLE_MARGIN,
                             bounds.size.h - BUBBLE_HEIGHT - BUBBLE_MARGIN,
                             content_w - 2 * BUBBLE_MARGIN,
                             BUBBLE_HEIGHT);
  s_bubble_layer = layer_create(bubble_frame);
  layer_set_update_proc(s_bubble_layer, bubble_layer_update_proc);
  layer_set_hidden(s_bubble_layer, true);
  layer_add_child(window_layer, s_bubble_layer);

  s_bubble_text = text_layer_create(GRect(6, 4, bubble_frame.size.w - 12,
                                          bubble_frame.size.h - 8));
  text_layer_set_font(s_bubble_text, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_bubble_text, GTextAlignmentCenter);
  text_layer_set_background_color(s_bubble_text, GColorClear);
  text_layer_set_overflow_mode(s_bubble_text, GTextOverflowModeTrailingEllipsis);
  layer_add_child(s_bubble_layer, text_layer_get_layer(s_bubble_text));

  // Action bar: icons telling the user what UP / SELECT / DOWN do.
  s_icon_ask = gbitmap_create_with_resource(RESOURCE_ID_IMG_ICON_ASK);
  s_icon_talk = gbitmap_create_with_resource(RESOURCE_ID_IMG_ICON_TALK);
  s_icon_phrases = gbitmap_create_with_resource(RESOURCE_ID_IMG_ICON_PHRASES);
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, GColorBlack);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP, s_icon_ask);       // ask AI
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_talk);  // dictate
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN, s_icon_phrases); // quick phrases
  action_bar_layer_set_click_config_provider(s_action_bar, prv_click_config_provider);
  action_bar_layer_add_to_window(s_action_bar, window);

  parrot_anim_init();
  parrot_window_set_state(UI_STATE_IDLE);

  touch_service_subscribe(prv_touch_handler, NULL);
}

static void prv_window_unload(Window *window) {
  touch_service_unsubscribe();
  parrot_anim_deinit();

  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_icon_ask);
  gbitmap_destroy(s_icon_talk);
  gbitmap_destroy(s_icon_phrases);
  s_action_bar = NULL;
  s_icon_ask = s_icon_talk = s_icon_phrases = NULL;

  text_layer_destroy(s_bubble_text);
  layer_destroy(s_bubble_layer);
  text_layer_destroy(s_state_label);
  bitmap_layer_destroy(s_parrot_layer);
  bitmap_layer_destroy(s_bg_layer);
  gbitmap_destroy(s_bg_bitmap);
  s_bg_bitmap = NULL;

  prv_unload_pose_group();
}

void parrot_window_push(void) {
  if (s_window) {
    return;
  }
  s_window = window_create();
  // The click config provider is registered via the action bar in prv_window_load
  // (action_bar_layer_add_to_window), so it isn't set directly on the window here.
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}

void parrot_window_destroy(void) {
  if (!s_window) {
    return;
  }
  window_destroy(s_window);
  s_window = NULL;
}
