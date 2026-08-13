#include "phrase_menu_window.h"
#include "config.h"
#include "touch_nav.h"

static Window *s_window;
static MenuLayer *s_menu;
static void (*s_on_pick)(const char *phrase);

static uint16_t prv_num_rows(MenuLayer *menu, uint16_t section_index, void *ctx) {
  return config_preset_count();
}

// Stated explicitly (rather than left to the default) so touch hit-testing and
// drawing agree on where a row starts and ends.
#define ROW_HEIGHT 44

static int16_t prv_cell_height(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
  return ROW_HEIGHT;
}

static void prv_draw_row(GContext *gctx, const Layer *cell_layer, MenuIndex *cell_index, void *ctx) {
  menu_cell_basic_draw(gctx, cell_layer, config_preset_phrase(cell_index->row), NULL, NULL);
}

static void prv_select(MenuLayer *menu, MenuIndex *cell_index, void *ctx) {
  // config_preset_phrase() points into config's static storage, so it stays
  // valid after we pop this window. Pop first, then speak on the parrot window.
  const char *phrase = config_preset_phrase(cell_index->row);
  void (*pick)(const char *) = s_on_pick;
  window_stack_remove(s_window, true);
  if (pick) {
    pick(phrase);
  }
}

// --- Touch -------------------------------------------------------------------
//
// The system touch bridge (app_touch_navigation_enable) faults inside firmware
// on 4.33.1, so touch is handled here from the raw stream — see touch_nav.h.

// Keeps the selection on a visible row after a drag or glide, so SELECT never
// acts on a row that scrolled out of sight.
static void prv_touch_settled(void) {
  if (!s_menu) { return; }
  GRect frame = layer_get_frame(menu_layer_get_layer(s_menu));
  int row = touch_nav_row_at(s_menu, GPoint(frame.size.w / 2, frame.origin.y + frame.size.h / 2),
                             prv_num_rows(s_menu, 0, NULL), prv_cell_height);
  if (row >= 0) {
    menu_layer_set_selected_index(s_menu, MenuIndex(0, (uint16_t)row), MenuRowAlignNone, false);
    layer_mark_dirty(menu_layer_get_layer(s_menu));   // no scroll means no repaint otherwise
  }
}

bool phrase_menu_window_is_top(void) {
  return s_window && window_stack_get_top_window() == s_window;
}

void phrase_menu_window_handle_touch(const TouchEvent *event) {
  if (!s_menu) { return; }
  GPoint point;
  switch (touch_nav_feed(menu_layer_get_scroll_layer(s_menu), event, &point)) {
    case TOUCH_NAV_TAP: {
      int row = touch_nav_row_at(s_menu, point, prv_num_rows(s_menu, 0, NULL), prv_cell_height);
      if (row < 0) { return; }
      MenuIndex index = MenuIndex(0, (uint16_t)row);
      menu_layer_set_selected_index(s_menu, index, MenuRowAlignNone, false);
      prv_select(s_menu, &index, NULL);
      break;
    }
    case TOUCH_NAV_BACK:
      window_stack_remove(s_window, true);   // back to the parrot
      break;
    default:
      break;
  }
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_num_rows,
    .get_cell_height = prv_cell_height,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));

  window_set_touch_bridge_disabled(window, true);
  touch_nav_set_settled_handler(prv_touch_settled);
}

static void prv_window_unload(Window *window) {
  touch_nav_reset();
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  window_destroy(window);
  s_window = NULL;
}

void phrase_menu_window_push(void (*on_pick)(const char *phrase)) {
  if (s_window || config_preset_count() == 0) {
    return;
  }
  s_on_pick = on_pick;
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
}
