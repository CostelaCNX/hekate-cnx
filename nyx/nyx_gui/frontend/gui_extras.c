/*
 * Extras tab for Nyx GUI - part of the hekate project.
 * Sysmodule toggles and custom theme removal.
 *
 * Copyright (c) 2026 CostelaBR
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdlib.h>

#include <bdk.h>

#include "gui.h"
#include "gui_extras.h"
#include "gui_tools_files.h"
#include <libs/fatfs/ff.h>

// ---------------------------------------------------------------------------
// Sysmodules - toggled via <TID>/flags/boot2.flag
// ---------------------------------------------------------------------------

#define MAX_TIDS 4

typedef struct {
	const char *name;
	const char *tids[MAX_TIDS];
} sysmod_t;

static const sysmod_t sysmodules[] = {
	{ "Mission Control",  { "010000000000bd00", NULL } },
	{ "Tesla",            { "420000000007E51A", NULL } },
	{ "sys-clk",          { "00FF0000636C6BFF", NULL } },
	{ "SaltyNX",          { "0000000000534C56", NULL } },
};

#define SYSMOD_COUNT (sizeof(sysmodules) / sizeof(sysmodules[0]))

// Custom theme TIDs under atmosphere/contents/
static const char *theme_tids[] = {
	"0100000000001000",
	"0100000000001013",
	"0100000000001007",
	"0100000000000811",
	"0100000000000039",
	NULL
};

// Module button labels - refreshed after a toggle.
static lv_obj_t *mod_btns[SYSMOD_COUNT];
static lv_obj_t *mod_btn_labels[SYSMOD_COUNT];
static lv_obj_t *mod_sw_knobs[SYSMOD_COUNT];

static lv_style_t mod_sw_track_on;
static lv_style_t mod_sw_track_off;
static lv_style_t mod_sw_track_ina;
static lv_style_t mod_sw_knob;

// ---------------------------------------------------------------------------
// boot2.flag logic
// ---------------------------------------------------------------------------

static void _flag_path(const char *tid, char *out)
{
	s_printf(out, "atmosphere/contents/%s/flags/boot2.flag", tid);
}

static int _tid_active(const char *tid)
{
	char path[256];
	_flag_path(tid, path);
	return f_stat(path, NULL) == FR_OK ? 1 : 0;
}

static int _module_active(u32 idx)
{
	for (int i = 0; i < MAX_TIDS && sysmodules[idx].tids[i]; i++)
		if (!_tid_active(sysmodules[idx].tids[i]))
			return 0;
	return 1;
}

static bool _tid_enable(const char *tid)
{
	char dir1[128], dir2[160], flag[256];
	s_printf(dir1, "atmosphere/contents/%s", tid);
	s_printf(dir2, "atmosphere/contents/%s/flags", tid);
	_flag_path(tid, flag);

	f_mkdir("atmosphere");
	f_mkdir("atmosphere/contents");
	f_mkdir(dir1);
	f_mkdir(dir2);

	FIL fp;
	if (f_open(&fp, flag, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
		return false;
	f_close(&fp);
	return true;
}

static bool _tid_disable(const char *tid)
{
	char path[256];
	_flag_path(tid, path);
	FRESULT res = f_unlink(path);
	return res == FR_OK || res == FR_NO_FILE;
}

static bool _module_toggle(u32 idx, bool *ok_out)
{
	bool want_active = !_module_active(idx);
	bool ok = true;

	for (int i = 0; i < MAX_TIDS && sysmodules[idx].tids[i]; i++)
		ok &= want_active ? _tid_enable(sysmodules[idx].tids[i])
		                  : _tid_disable(sysmodules[idx].tids[i]);

	*ok_out = ok;
	return want_active;
}

static void _init_module_switch_styles(lv_theme_t *th)
{
	static bool inited = false;
	if (inited)
		return;

	lv_style_copy(&mod_sw_track_on, &lv_style_plain);
	mod_sw_track_on.body.main_color = th->btn.rel->body.main_color;
	mod_sw_track_on.body.grad_color = mod_sw_track_on.body.main_color;
	mod_sw_track_on.body.radius = LV_RADIUS_CIRCLE;
	mod_sw_track_on.body.border.width = 0;
	mod_sw_track_on.body.shadow.color = LV_COLOR_HEX(0x000000);
	mod_sw_track_on.body.shadow.type = LV_SHADOW_BOTTOM;
	mod_sw_track_on.body.shadow.width = 4;

	lv_style_copy(&mod_sw_track_off, &mod_sw_track_on);
	mod_sw_track_off.body.main_color = LV_COLOR_HEX(0x3A3A3A);
	mod_sw_track_off.body.grad_color = mod_sw_track_off.body.main_color;
	mod_sw_track_off.body.border.width = 2;
	mod_sw_track_off.body.border.color = LV_COLOR_HEX(0x6A6A6A);

	lv_style_copy(&mod_sw_track_ina, &mod_sw_track_on);
	mod_sw_track_ina.body.main_color = LV_COLOR_HEX(0x444444);
	mod_sw_track_ina.body.grad_color = mod_sw_track_ina.body.main_color;
	mod_sw_track_ina.body.opa = LV_OPA_50;

	lv_style_copy(&mod_sw_knob, &lv_style_plain);
	mod_sw_knob.body.main_color = LV_COLOR_HEX(0xFFFFFF);
	mod_sw_knob.body.grad_color = mod_sw_knob.body.main_color;
	mod_sw_knob.body.radius = LV_RADIUS_CIRCLE;
	mod_sw_knob.body.border.width = 0;
	mod_sw_knob.body.shadow.color = LV_COLOR_HEX(0x000000);
	mod_sw_knob.body.shadow.type = LV_SHADOW_BOTTOM;
	mod_sw_knob.body.shadow.width = 5;

	inited = true;
}

static void _refresh_module_button(u32 idx, int active)
{
	if (!mod_btns[idx] || !mod_btn_labels[idx] || !mod_sw_knobs[idx])
		return;

	lv_label_set_static_text(mod_btn_labels[idx], sysmodules[idx].name);

	if (active < 0)
	{
		lv_btn_set_state(mod_btns[idx], LV_BTN_STATE_INA);
		lv_btn_set_style(mod_btns[idx], LV_BTN_STYLE_REL, &mod_sw_track_ina);
		lv_btn_set_style(mod_btns[idx], LV_BTN_STYLE_PR,  &mod_sw_track_ina);
		lv_obj_align(mod_sw_knobs[idx], mod_btns[idx], LV_ALIGN_IN_LEFT_MID, LV_DPI / 50, 0);
	}
	else
	{
		lv_btn_set_state(mod_btns[idx], LV_BTN_STATE_REL);
		lv_btn_set_style(mod_btns[idx], LV_BTN_STYLE_REL, active ? &mod_sw_track_on : &mod_sw_track_off);
		lv_btn_set_style(mod_btns[idx], LV_BTN_STYLE_PR,  active ? &mod_sw_track_on : &mod_sw_track_off);
		lv_obj_align(mod_sw_knobs[idx], mod_btns[idx],
			active ? LV_ALIGN_IN_RIGHT_MID : LV_ALIGN_IN_LEFT_MID,
			active ? -(LV_DPI / 50) : LV_DPI / 50, 0);
	}
}

// ---------------------------------------------------------------------------
// Recursive removal
// ---------------------------------------------------------------------------

static FRESULT _rm_recursive(const char *path)
{
	DIR dir;
	FILINFO fno;
	char sub[256];

	if (f_opendir(&dir, path) != FR_OK)
		return f_unlink(path);

	while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0)
	{
		s_printf(sub, "%s/%s", path, fno.fname);
		if (fno.fattrib & AM_DIR)
			_rm_recursive(sub);
		else
			f_unlink(sub);
	}
	f_closedir(&dir);
	return f_unlink(path);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static lv_res_t _action_toggle_module(lv_obj_t *btn)
{
	lv_btn_ext_t *ext = lv_obj_get_ext_attr(btn);
	u32 idx = ext->idx;
	if (idx >= SYSMOD_COUNT)
		return LV_RES_OK;

	if (!sd_mount())
	{
		bool ok;
		bool now_active = _module_toggle(idx, &ok);
		if (ok)
			_refresh_module_button(idx, now_active);
		sd_unmount();
	}
	return LV_RES_OK;
}

// Cleared when the user dismisses the mbox — prevents stacking popups.
static lv_obj_t *s_theme_mbox = NULL;

static lv_res_t _mbox_close(lv_obj_t *btnm, const char *txt)
{
	lv_obj_del(lv_obj_get_parent(btnm));
	s_theme_mbox = NULL;
	return LV_RES_INV;
}

static lv_res_t _action_remove_themes(lv_obj_t *btn)
{
	if (s_theme_mbox)
		return LV_RES_OK;

	static const char *mbox_btns[] = { "\251", "\222OK!", "\251", "" };
	s_theme_mbox = lv_mbox_create(lv_scr_act(), NULL);
	lv_mbox_set_recolor_text(s_theme_mbox, true);

	if (!sd_mount())
	{
		int found = 0, removed = 0;
		for (int i = 0; theme_tids[i]; i++)
		{
			char path[128];
			s_printf(path, "atmosphere/contents/%s", theme_tids[i]);
			if (f_stat(path, NULL) == FR_OK)
			{
				found++;
				if (_rm_recursive(path) == FR_OK)
					removed++;
			}
		}
		sd_unmount();

		if (found == 0)
			lv_mbox_set_text(s_theme_mbox, "#0055CC Remover Temas#\n\n#AAAAAA Nenhum tema encontrado.#");
		else if (removed == found)
		{
			char msg[128];
			s_printf(msg, "#0055CC Remover Temas#\n\n#96FF00 %d pasta(s) removida(s)!#", removed);
			lv_mbox_set_text(s_theme_mbox, msg);
		}
		else
		{
			char msg[128];
			s_printf(msg, "#0055CC Remover Temas#\n\n#FFDD00 %d de %d removida(s).#", removed, found);
			lv_mbox_set_text(s_theme_mbox, msg);
		}
	}
	else
	{
		lv_mbox_set_text(s_theme_mbox, "#FF0000 Erro:# Não foi possível montar o SD.");
	}

	lv_mbox_add_btns(s_theme_mbox, mbox_btns, _mbox_close);
	lv_obj_set_top(s_theme_mbox, true);
	return LV_RES_OK;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static lv_obj_t *_make_col(lv_obj_t *parent, u32 col_w)
{
	static lv_style_t col_style;
	lv_style_copy(&col_style, &lv_style_transp);
	col_style.body.padding.inner = 0;
	col_style.body.padding.hor   = LV_DPI - (LV_DPI / 4);
	col_style.body.padding.ver   = LV_DPI / 6;

	lv_obj_t *h = lv_cont_create(parent, NULL);
	lv_cont_set_style(h, &col_style);
	lv_cont_set_fit(h, false, true);
	lv_obj_set_width(h, col_w);
	lv_obj_set_click(h, false);
	lv_cont_set_layout(h, LV_LAYOUT_OFF);
	return h;
}

static lv_obj_t *_section_header(lv_obj_t *parent, u32 col_w, const char *title, lv_obj_t *anchor)
{
	lv_obj_t *lbl = lv_label_create(parent, NULL);
	lv_label_set_static_text(lbl, title);
	if (anchor)
		lv_obj_align(lbl, anchor, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	else
		lv_obj_set_pos(lbl, 0, 0);

	static lv_style_t line_style;
	lv_style_copy(&line_style, lv_theme_get_current()->line.decor);
	line_style.line.width = 1;

	u32 line_w = col_w - (LV_DPI - (LV_DPI / 4)) * 2 - LV_DPI;
	static lv_point_t pts[2] = {{0, 0}, {0, 0}};
	pts[1].x = line_w;

	lv_obj_t *line = lv_line_create(parent, NULL);
	lv_line_set_style(line, &line_style);
	lv_line_set_points(line, pts, 2);
	lv_obj_align(line, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 16);
	return line;
}

// ---------------------------------------------------------------------------
// Main tab
// ---------------------------------------------------------------------------

void create_tab_extras(lv_theme_t *th, lv_obj_t *parent)
{
	lv_page_set_scrl_layout(parent, LV_LAYOUT_PRETTY);
	_init_module_switch_styles(th);

	// Read module states once when the tab loads.
	int mod_states[SYSMOD_COUNT];
	bool sd_ok = !sd_mount();
	for (u32 i = 0; i < SYSMOD_COUNT; i++)
		mod_states[i] = sd_ok ? _module_active(i) : -1;
	if (sd_ok)
		sd_unmount();

	u32 col_w = (LV_HOR_RES / 9) * 4;
	u32 btn_w = col_w - LV_DPI;

	// === Left column ===
	lv_obj_t *h_left = _make_col(parent, col_w);
	lv_obj_t *anchor = _section_header(h_left, col_w, "Módulos do Sistema", NULL);

	for (u32 i = 0; i < SYSMOD_COUNT; i++)
	{
		lv_obj_t *row = lv_cont_create(h_left, NULL);
		lv_cont_set_style(row, &lv_style_transp);
		lv_cont_set_layout(row, LV_LAYOUT_OFF);
		lv_obj_set_click(row, false);
		lv_obj_set_size(row, btn_w, LV_DPI * 3 / 5);
		lv_obj_align(row, anchor, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);

		lv_obj_t *label_btn = lv_btn_create(row, NULL);
		lv_btn_set_layout(label_btn, LV_LAYOUT_OFF);
		lv_obj_set_click(label_btn, false);
		lv_obj_set_size(label_btn, LV_DPI * 29 / 10, LV_DPI * 3 / 5);
		lv_obj_align(label_btn, row, LV_ALIGN_IN_LEFT_MID, 0, 0);

		lv_obj_t *btn = lv_btn_create(row, NULL);
		lv_btn_ext_t *ext = lv_obj_get_ext_attr(btn);
		ext->idx = i;
		lv_btn_set_action(btn, LV_BTN_ACTION_CLICK, _action_toggle_module);
		lv_btn_set_layout(btn, LV_LAYOUT_OFF);
		lv_obj_set_size(btn, LV_DPI * 4 / 5, LV_DPI * 2 / 5);
		lv_obj_align(btn, label_btn, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 5, 0);
		mod_btns[i] = btn;

		lv_obj_t *lbl = lv_label_create(label_btn, NULL);
		mod_btn_labels[i] = lbl;
		lv_obj_align(lbl, label_btn, LV_ALIGN_IN_LEFT_MID, LV_DPI / 4, 0);

		lv_obj_t *sw_knob = lv_cont_create(btn, NULL);
		lv_obj_set_click(sw_knob, false);
		lv_cont_set_style(sw_knob, &mod_sw_knob);
		lv_obj_set_size(sw_knob, LV_DPI * 7 / 20, LV_DPI * 7 / 20);
		mod_sw_knobs[i] = sw_knob;

		_refresh_module_button(i, mod_states[i]);

		anchor = row;
	}

	// === Right column ===
	lv_obj_t *h_right = _make_col(parent, col_w);
	lv_obj_t *anchor2 = _section_header(h_right, col_w, "Temas Customizados", NULL);

	lv_obj_t *btn_themes = lv_btn_create(h_right, NULL);
	lv_btn_set_action(btn_themes, LV_BTN_ACTION_CLICK, _action_remove_themes);
	lv_obj_set_width(btn_themes, btn_w);
	lv_obj_align(btn_themes, anchor2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);

	lv_obj_t *lbl_themes = lv_label_create(btn_themes, NULL);
	lv_label_set_static_text(lbl_themes, "Remover Temas");

	// Files (SD file browser) - header anchored below the themes button.
	lv_obj_t *anchor3 = _section_header(h_right, col_w, "Arquivos", btn_themes);

	lv_obj_t *btn_files = lv_btn_create(h_right, NULL);
	lv_btn_set_action(btn_files, LV_BTN_ACTION_CLICK, create_win_files);
	lv_obj_set_width(btn_files, btn_w);
	lv_obj_align(btn_files, anchor3, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);

	lv_obj_t *lbl_files = lv_label_create(btn_files, NULL);
	lv_label_set_static_text(lbl_files, SYMBOL_SD"  Gerenciador de Arquivos");

	// Lock positions after PRETTY has stabilized, then snap h_right top to h_left top.
	lv_obj_set_protect(h_left,  LV_PROTECT_POS);
	lv_obj_set_protect(h_right, LV_PROTECT_POS);
	lv_obj_set_y(h_right, lv_obj_get_y(h_left));
}
