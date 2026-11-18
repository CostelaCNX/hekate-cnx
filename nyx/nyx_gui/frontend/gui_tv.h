#ifndef GUI_TV_H
#define GUI_TV_H

#include "gui.h"

lv_res_t gui_tv_open(lv_obj_t *btn);
void gui_tv_dirty(int y1, int y2);
void gui_tv_shutdown(void);
void gui_tv_auto_configure(void);
void gui_tv_resume_detection(void);
void gui_tv_screen_configure(void);
bool gui_tv_console_dark(void);

#endif
