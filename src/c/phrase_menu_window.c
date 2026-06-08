#include "phrase_menu_window.h"
#include "config.h"

static Window *s_window;
static MenuLayer *s_menu;
static void (*s_on_pick)(const char *phrase);

static uint16_t prv_num_rows(MenuLayer *menu, uint16_t section_index, void *ctx) {
  return config_preset_count();
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

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prv_num_rows,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
  });
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void prv_window_unload(Window *window) {
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
