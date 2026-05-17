/*
 * CNX Loader — Extras tab
 * Controle de sysmodules e remoção de temas customizados
 * Copyright (c) 2026 CostelaBR
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <string.h>
#include <stdlib.h>

#include <bdk.h>

#include "gui.h"
#include "gui_extras.h"
#include <libs/fatfs/ff.h>

// ---------------------------------------------------------------------------
// Módulos — toggle via <TID>/flags/boot2.flag
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

// TIDs dos temas customizados em atmosphere/contents/
static const char *theme_tids[] = {
	"0100000000001000",
	"0100000000001013",
	"0100000000001007",
	"0100000000000811",
	"0100000000000039",
	NULL
};

// Ponteiros para os labels dos botões de módulo — atualizados após toggle
static lv_obj_t *mod_btn_labels[SYSMOD_COUNT];

// ---------------------------------------------------------------------------
// Lógica de boot2.flag
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

static void _refresh_btn_label(u32 idx, int active)
{
	if (!mod_btn_labels[idx])
		return;

	char label[64];
	lv_label_set_recolor(mod_btn_labels[idx], true);
	s_printf(label, "%s   #%s %s#",
		sysmodules[idx].name,
		active ? "00FF41" : "FF2200",
		active ? "LIG" : "DESL");
	lv_label_set_text(mod_btn_labels[idx], label);
}

// ---------------------------------------------------------------------------
// Remoção recursiva
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
			_refresh_btn_label(idx, now_active);
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

static lv_obj_t *_section_header(lv_obj_t *parent, u32 col_w, const char *title)
{
	lv_obj_t *lbl = lv_label_create(parent, NULL);
	lv_label_set_static_text(lbl, title);
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
// Tab principal
// ---------------------------------------------------------------------------

void create_tab_extras(lv_theme_t *th, lv_obj_t *parent)
{
	lv_page_set_scrl_layout(parent, LV_LAYOUT_PRETTY);

	// Ler estado dos módulos uma vez ao carregar a aba
	int mod_states[SYSMOD_COUNT];
	bool sd_ok = !sd_mount();
	for (u32 i = 0; i < SYSMOD_COUNT; i++)
		mod_states[i] = sd_ok ? _module_active(i) : -1;
	if (sd_ok)
		sd_unmount();

	u32 col_w = (LV_HOR_RES / 9) * 4;
	u32 btn_w = col_w - LV_DPI;

	// === Coluna esquerda ===
	lv_obj_t *h_left = _make_col(parent, col_w);
	lv_obj_t *anchor = _section_header(h_left, col_w, "Módulos do Sistema");

	for (u32 i = 0; i < SYSMOD_COUNT; i++)
	{
		lv_obj_t *btn = lv_btn_create(h_left, NULL);
		lv_btn_ext_t *ext = lv_obj_get_ext_attr(btn);
		ext->idx = i;
		lv_btn_set_action(btn, LV_BTN_ACTION_CLICK, _action_toggle_module);
		lv_obj_set_width(btn, btn_w);
		lv_obj_align(btn, anchor, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);

		lv_obj_t *lbl = lv_label_create(btn, NULL);
		lv_label_set_recolor(lbl, true);
		mod_btn_labels[i] = lbl;

		int active = mod_states[i];
		char label[64];
		if (active < 0)
			s_printf(label, "%s   #AAAAAA ---#", sysmodules[i].name);
		else
			s_printf(label, "%s   #%s %s#",
				sysmodules[i].name,
				active ? "00FF41" : "FF2200",
				active ? "LIG" : "DESL");
		lv_label_set_text(lbl, label);

		anchor = btn;
	}

	// === Coluna direita ===
	lv_obj_t *h_right = _make_col(parent, col_w);
	lv_obj_t *anchor2 = _section_header(h_right, col_w, "Temas Customizados");

	lv_obj_t *btn_themes = lv_btn_create(h_right, NULL);
	lv_btn_set_action(btn_themes, LV_BTN_ACTION_CLICK, _action_remove_themes);
	lv_obj_set_width(btn_themes, btn_w);
	lv_obj_align(btn_themes, anchor2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);

	lv_obj_t *lbl_themes = lv_label_create(btn_themes, NULL);
	lv_label_set_static_text(lbl_themes, "Remover Temas");

	// Lock positions after PRETTY has stabilized, then snap h_right top to h_left top.
	lv_obj_set_protect(h_left,  LV_PROTECT_POS);
	lv_obj_set_protect(h_right, LV_PROTECT_POS);
	lv_obj_set_y(h_right, lv_obj_get_y(h_left));
}
