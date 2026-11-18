#ifndef GUI_BLUETOOTH_H
#define GUI_BLUETOOTH_H

#include "gui.h"
#include <input/joycon.h>

lv_res_t gui_bluetooth_open(lv_obj_t *button);
void gui_bluetooth_shutdown(void);
void gui_bluetooth_resume(void);
bool gui_bluetooth_active(void);
bool gui_bluetooth_using_radio(void);
void gui_bluetooth_poll(void);
void gui_bluetooth_poll_fast(void);
void gui_bluetooth_draw_poll(void);
void gui_bluetooth_autostart(void);
void gui_bluetooth_try_dock_autostart(void);
jc_gamepad_rpt_t *gui_bluetooth_merge(jc_gamepad_rpt_t *wired);

#endif
