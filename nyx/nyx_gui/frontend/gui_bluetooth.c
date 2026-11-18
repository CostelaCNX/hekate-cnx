/*
 * Wireless Joy-Con Bluetooth frontend for Nyx GUI.
 *
 * Based on Hekate Nova Pro (Retrogendude).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <bdk.h>
#include <string.h>
#include <libs/fatfs/ff.h>

#include "gui_bluetooth.h"
#include "../bluetooth/nova_bt.h"
#include "../config.h"

#define BT_DIR          "bootloader/bluetooth"
#define BT_PAIRING      BT_DIR "/joycon-pairing.bin"
#define BT_PAIRING_BAK  BT_DIR "/joycon-pairing.bak"
#define BT_PAIRING_TMP  BT_DIR "/joycon-pairing.tmp"
#define BT_PATCHRAM     BT_DIR "/BCM4356A3.hcd"

static nova_bt_peer paired[2];
static bool boot_checked;
static bool manual_stop;
static u8 wired_conn_mask;
static u8 rail_checked_mask;
static u8 pending_rail_mask;
static u32 pending_rail_since;

static struct {
	lv_obj_t *win, *label;
	lv_task_t *task, *watch_task;
	lv_signal_func_t signal;
	u32 shown;
	char note[256];
} ui;

static int pairing_file(bool write);
static lv_res_t start(lv_obj_t *button);
static void watch_tick(void *unused);

static bool _unsupported(void)
{
	return n_cfg.jc_disable || fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG;
}

static void _notify(const char *msg, u32 duration_ms)
{
	lv_obj_t *top = lv_layer_top();
	if (!top)
		return;
	lv_obj_t *mbox = lv_mbox_create(top, NULL);
	if (!mbox)
		return;
	lv_mbox_set_recolor_text(mbox, true);
	lv_mbox_set_text(mbox, msg);
	lv_obj_set_width(mbox, LV_DPI * 5);
	lv_obj_set_top(mbox, true);
	lv_obj_align(mbox, NULL, LV_ALIGN_IN_TOP_LEFT, 20, 16);
	lv_mbox_start_auto_close(mbox, duration_ms);
}

static u8 _paired_mask(void)
{
	return (paired[0].valid ? 1 : 0) | (paired[1].valid ? 2 : 0);
}

static const char *_error_text(int error)
{
	switch (error)
	{
	case NOVA_BT_ERR_PEERS:
		return "Nenhum pareamento válido. Encaixe os Joy-Cons nos trilhos.";
	case NOVA_BT_ERR_RADIO:
		return "Rádio Bluetooth indisponível.";
	case NOVA_BT_ERR_TIMEOUT:
		return "O rádio não terminou de iniciar.";
	case NOVA_BT_ERR_HOST:
		return "Os Joy-Cons estão pareados com outro console.";
	default:
		return "Falha de comunicação com o rádio.";
	}
}

static void refresh(void)
{
	if (!ui.label)
		return;

	const nova_bt_status *s = nova_bt_get_status();
	const char *state;
	switch (s->state)
	{
	case NOVA_BT_STARTING:
		state = "Iniciando Bluetooth...";
		break;
	case NOVA_BT_READY:
		state = s->pad[0].connected || s->pad[1].connected ?
			"Joy-Con sem fio ativo" : "Pronto - pressione um botão no Joy-Con";
		break;
	case NOVA_BT_ERROR:
		state = _error_text(s->error);
		break;
	default:
		state = "Bluetooth desligado";
		break;
	}

	char text[1024];
	s_printf(text,
		"Estado: %s\n%s\n\n"
		"Joy-Con Esquerdo: %s    |    Joy-Con Direito: %s\n"
		"Pareamentos: %d / 2    |    Firmware do rádio: %s\n\n"
		"Encaixe os Joy-Cons uma vez para salvar o pareamento no SD.\n"
		"Ao destacá-los, o Bluetooth inicia sozinho; pressione um botão em cada Joy-Con.\n"
		"Para conexão estável, coloque o firmware em " BT_PATCHRAM ".",
		state, ui.note,
		s->pad[0].connected ? "Conectado" : "Sem sinal",
		s->pad[1].connected ? "Conectado" : "Sem sinal",
		nova_bt_validate_peers(paired),
		s->patchram == 2 ? "carregado" : s->state == NOVA_BT_STARTING ? "carregando" : "ROM");
	lv_label_set_text(ui.label, text);
}

static void tick(void *unused)
{
	(void)unused;
	nova_bt_poll_fast();
	if ((u32)(get_tmr_ms() - ui.shown) >= 500)
	{
		ui.shown = get_tmr_ms();
		refresh();
	}
}

static void _stop_radio(void)
{
	nova_bt_stop();
	if (ui.task)
	{
		lv_task_del(ui.task);
		ui.task = NULL;
	}
	refresh();
}

void gui_bluetooth_shutdown(void)
{
	boot_checked = true;
	if (ui.watch_task)
	{
		lv_task_del(ui.watch_task);
		ui.watch_task = NULL;
	}
	_stop_radio();
}

void gui_bluetooth_resume(void)
{
	if (_unsupported())
		return;
	manual_stop = false;
	if (!ui.watch_task)
		ui.watch_task = lv_task_create(watch_tick, 500, LV_TASK_PRIO_LOW, NULL);
}

bool gui_bluetooth_active(void)
{
	return nova_bt_get_status()->state == NOVA_BT_READY;
}

bool gui_bluetooth_using_radio(void)
{
	int st = nova_bt_get_status()->state;
	return st == NOVA_BT_STARTING || st == NOVA_BT_READY;
}

void gui_bluetooth_poll(void) { nova_bt_poll(); }
void gui_bluetooth_poll_fast(void) { nova_bt_poll_fast(); }
void gui_bluetooth_draw_poll(void) { nova_bt_draw_poll(); }

static int _import_from_rails(bool automatic)
{
	if (_unsupported())
	{
		if (!automatic)
		{
			strcpy(ui.note, "É necessário um console com Joy-Cons destacáveis e driver de trilho ativo.");
			refresh();
		}
		return 0;
	}

	nova_bt_peer prev[2], candidate[2];
	memcpy(prev, paired, sizeof(prev));
	memset(candidate, 0, sizeof(candidate));
	_stop_radio();

	bool hos[2] = { false, false }, recognized[2] = { false, false };
	const u8 *new_host = NULL;
	jc_gamepad_rpt_t *p = jc_get_bt_pairing_info_ex(&hos[0], &hos[1], &recognized[0], &recognized[1]);
	if (p)
	{
		for (unsigned i = 0; i < 2; i++)
		{
			jc_bt_conn_t *b = i ? &p->bt_conn_r : &p->bt_conn_l;
			bool on_rail = i ? p->conn_r : p->conn_l;
			if (!recognized[i] || !on_rail || !(b->type & (i ? 2 : 1)))
				continue;
			candidate[i].valid = 1;
			memcpy(candidate[i].mac, b->mac, 6);
			memcpy(candidate[i].host, b->host_mac, 6);
			memcpy(candidate[i].key, b->ltk, 16);
			if (!new_host)
				new_host = candidate[i].host;
		}
	}

	// A half seen alone keeps the other half's record if both share a host.
	if (new_host)
	{
		for (unsigned i = 0; i < 2; i++)
			if (!candidate[i].valid && prev[i].valid && !memcmp(prev[i].host, new_host, 6))
				candidate[i] = prev[i];
	}

	int count = nova_bt_validate_peers(candidate);
	if (count)
	{
		bool changed = memcmp(candidate, prev, sizeof(prev)) != 0;
		memcpy(paired, candidate, sizeof(paired));
		int saved_sd = pairing_file(true);
		manual_stop = false;
		s_printf(ui.note, "Pareamentos lidos: %d / 2 (%s)",
			count, saved_sd ? "salvo no SD" : "apenas na memória");
		if (automatic && changed)
		{
			char toast[96];
			s_printf(toast, SYMBOL_SHUFFLE"  #96FF00 Pareamento Joy-Con salvo! (%d/2)#", count);
			_notify(toast, 3500);
		}
	}
	else if (!automatic)
	{
		memset(paired, 0, sizeof(paired));
		strcpy(ui.note, "Pareamentos lidos: 0 / 2\n"
			"Encaixe os dois Joy-Cons pareados com este console e tente novamente.");
	}
	memset(candidate, 0, sizeof(candidate));
	memset(prev, 0, sizeof(prev));
	refresh();
	return count;
}

static lv_res_t import_pairing(lv_obj_t *button)
{
	(void)button;
	_import_from_rails(false);
	return LV_RES_OK;
}

static void _patchram_prepare(void)
{
	static u8 *image;
	static u32 size;
	static bool checked;

	if (!checked)
	{
		checked = true;
		bool mounted = sd_get_card_mounted();
		if (!sd_mount())
		{
			image = sd_file_read(BT_PATCHRAM, &size);
			if (image && !nova_bt_patchram_valid(image, size))
			{
				free(image);
				image = NULL;
			}
			if (!image)
				size = 0;
			if (!mounted)
				sd_unmount();
		}
	}
	nova_bt_set_patchram(image, size);
}

static lv_res_t start(lv_obj_t *button)
{
	if (button)
		manual_stop = false;
	if (gui_bluetooth_using_radio())
		return LV_RES_OK;
	if (!nova_bt_validate_peers(paired))
		pairing_file(false);
	if (!ui.task)
		ui.task = lv_task_create(tick, 2, LV_TASK_PRIO_HIGH, NULL);
	if (!ui.task)
	{
		strcpy(ui.note, "Não foi possível iniciar a tarefa do Bluetooth.");
		refresh();
		return LV_RES_OK;
	}
	_patchram_prepare();
	if (!nova_bt_start(paired))
	{
		lv_task_del(ui.task);
		ui.task = NULL;
	}
	refresh();
	return LV_RES_OK;
}

static lv_res_t stop(lv_obj_t *button)
{
	if (button)
		manual_stop = true;
	_stop_radio();
	strcpy(ui.note, "Bluetooth parado.");
	refresh();
	return LV_RES_OK;
}

static int pairing_file(bool write)
{
	u8 bytes[NOVA_BT_PAIRING_SIZE];
	nova_bt_peer candidate[2];
	int count = 0;
	memset(bytes, 0, sizeof(bytes));
	memset(candidate, 0, sizeof(candidate));
	bool mounted = sd_get_card_mounted();
	if (write && !nova_bt_pairing_encode(bytes, paired))
		goto done;
	if (sd_mount() != 0)
		goto done;

	FIL fp;
	u32 transferred = 0;
	int result;
	if (write)
	{
		// Write a temp file and rotate, so a power cut never loses both copies.
		result = f_mkdir(BT_DIR);
		if (result && result != FR_EXIST)
			goto unmount;
		if (f_open(&fp, BT_PAIRING_TMP, FA_WRITE | FA_CREATE_ALWAYS))
			goto unmount;
		result = f_write(&fp, bytes, sizeof(bytes), &transferred);
		int closed = f_close(&fp);
		if (result || closed || transferred != sizeof(bytes))
		{
			f_unlink(BT_PAIRING_TMP);
			goto unmount;
		}

		result = f_stat(BT_PAIRING, NULL);
		bool had_old = result == FR_OK;
		if (result != FR_OK && result != FR_NO_FILE)
		{
			f_unlink(BT_PAIRING_TMP);
			goto unmount;
		}
		if (had_old)
		{
			result = f_unlink(BT_PAIRING_BAK);
			if ((result != FR_OK && result != FR_NO_FILE) || f_rename(BT_PAIRING, BT_PAIRING_BAK) != FR_OK)
			{
				f_unlink(BT_PAIRING_TMP);
				goto unmount;
			}
		}
		if (f_rename(BT_PAIRING_TMP, BT_PAIRING) != FR_OK)
		{
			if (had_old)
				f_rename(BT_PAIRING_BAK, BT_PAIRING);
			f_unlink(BT_PAIRING_TMP);
			goto unmount;
		}
		count = nova_bt_validate_peers(paired);
	}
	else
	{
		static const char *paths[] = { BT_PAIRING, BT_PAIRING_BAK };
		for (unsigned i = 0; i < 2 && !count; i++)
		{
			if (f_open(&fp, paths[i], FA_READ))
				continue;
			bool size_ok = f_size(&fp) == sizeof(bytes);
			result = size_ok ? f_read(&fp, bytes, sizeof(bytes), &transferred) : 1;
			int closed = f_close(&fp);
			if (!result && !closed && transferred == sizeof(bytes))
				count = nova_bt_pairing_decode(candidate, bytes, sizeof(bytes));
		}

		// L4T keeps the same records after its own Joy-Con pairing dump.
		if (!count && !f_open(&fp, "switchroot/joycon_mac.bin", FA_READ))
		{
			jc_bt_conn_t raw_bt[2];
			memset(raw_bt, 0, sizeof(raw_bt));
			bool size_ok = f_size(&fp) == sizeof(raw_bt);
			result = size_ok ? f_read(&fp, raw_bt, sizeof(raw_bt), &transferred) : 1;
			int closed = f_close(&fp);
			if (!result && !closed && transferred == sizeof(raw_bt))
			{
				for (unsigned i = 0; i < 2; i++)
				{
					if (!(raw_bt[i].type & (i ? 2 : 1)))
						continue;
					candidate[i].valid = 1;
					memcpy(candidate[i].mac, raw_bt[i].mac, 6);
					memcpy(candidate[i].host, raw_bt[i].host_mac, 6);
					memcpy(candidate[i].key, raw_bt[i].ltk, 16);
				}
				count = nova_bt_validate_peers(candidate);
			}
			memset(raw_bt, 0, sizeof(raw_bt));
		}
		if (count)
			memcpy(paired, candidate, sizeof(paired));
	}

unmount:
	if (!mounted)
		sd_unmount();
done:
	memset(bytes, 0, sizeof(bytes));
	memset(candidate, 0, sizeof(candidate));
	return count;
}

static lv_res_t signal(lv_obj_t *obj, lv_signal_t sign, void *param)
{
	lv_signal_func_t previous = ui.signal;
	if (sign == LV_SIGNAL_CLEANUP)
	{
		ui.win = NULL;
		ui.label = NULL;
	}
	return previous(obj, sign, param);
}

lv_res_t gui_bluetooth_open(lv_obj_t *button)
{
	(void)button;
	if (ui.win)
		return LV_RES_OK;

	lv_theme_t *th = lv_theme_get_current();
	ui.win = nyx_create_standard_window(SYMBOL_SHUFFLE" Joy-Con Sem Fio (Bluetooth)", NULL);
	ui.signal = lv_obj_get_signal_func(ui.win);
	lv_obj_set_signal_func(ui.win, signal);

	lv_obj_t *page = lv_win_get_content(ui.win);
	lv_page_set_scrl_layout(page, LV_LAYOUT_OFF);
	lv_obj_t *scrl = lv_page_get_scrl(page);

	static const char *titles[3] = {
		SYMBOL_DOWNLOAD" Ler dos trilhos",
		SYMBOL_POWER" Iniciar BT",
		SYMBOL_REBOOT" Parar BT"
	};
	static const lv_action_t actions[3] = { import_pairing, start, stop };

	for (unsigned i = 0; i < 3; i++)
	{
		lv_obj_t *btn = lv_btn_create(scrl, NULL);
		lv_obj_set_size(btn, 376, 56);
		lv_obj_set_pos(btn, 36 + i * 394, 16);
		lv_btn_set_action(btn, LV_BTN_ACTION_CLICK, actions[i]);
		lv_obj_t *lbl = lv_label_create(btn, NULL);
		lv_label_set_static_text(lbl, titles[i]);
	}

	static lv_point_t line_pts[] = { {0, 0}, {1164, 0} };
	lv_obj_t *line = lv_line_create(scrl, NULL);
	lv_line_set_points(line, line_pts, 2);
	lv_line_set_style(line, th->line.decor);
	lv_obj_set_pos(line, 36, 94);

	ui.label = lv_label_create(scrl, NULL);
	lv_label_set_long_mode(ui.label, LV_LABEL_LONG_BREAK);
	lv_obj_set_style(ui.label, th->label.prim);
	lv_obj_set_width(ui.label, 1164);
	lv_obj_set_pos(ui.label, 36, 110);

	refresh();
	return LV_RES_OK;
}

static void watch_tick(void *unused)
{
	(void)unused;
	if (_unsupported())
		return;

	int attached = jc_get_attached_mask();
	if (attached < 0)
		return;

	// Import once per insertion, after the rail link has settled.
	rail_checked_mask &= (u8)attached;
	u8 saved_mask = _paired_mask();
	u8 uncached_wired = wired_conn_mask & (u8)attached & ~saved_mask & ~rail_checked_mask;
	if (uncached_wired)
	{
		if (pending_rail_mask != uncached_wired)
		{
			pending_rail_mask = uncached_wired;
			pending_rail_since = get_tmr_ms();
			return;
		}
		if ((u32)(get_tmr_ms() - pending_rail_since) >= 450)
		{
			rail_checked_mask |= uncached_wired;
			pending_rail_mask = 0;
			_import_from_rails(true);
		}
		return;
	}
	pending_rail_mask = 0;

	// Radio off while every paired half is on its rail.
	if (saved_mask && !(saved_mask & ~(u8)attached))
	{
		if (gui_bluetooth_using_radio())
			_stop_radio();
		return;
	}

	if (!manual_stop && saved_mask && nova_bt_get_status()->state == NOVA_BT_OFF)
		start(NULL);
}

void gui_bluetooth_autostart(void)
{
	if (boot_checked)
		return;
	boot_checked = true;
	if (_unsupported())
		return;
	if (!nova_bt_validate_peers(paired))
		pairing_file(false);
	if (!ui.watch_task)
		ui.watch_task = lv_task_create(watch_tick, 500, LV_TASK_PRIO_LOW, NULL);

	int attached = jc_get_attached_mask();
	u8 saved_mask = _paired_mask();
	if (attached < 0 || !saved_mask || !(saved_mask & ~(u8)attached) || gui_bluetooth_using_radio())
		return;

	s_printf(ui.note, "Pareamento carregado do SD (%d / 2). Bluetooth iniciado automaticamente.",
		nova_bt_validate_peers(paired));
	start(NULL);
}

void gui_bluetooth_try_dock_autostart(void)
{
	if (_unsupported())
		return;
	if (!ui.watch_task)
		ui.watch_task = lv_task_create(watch_tick, 500, LV_TASK_PRIO_LOW, NULL);

	int attached = jc_get_attached_mask();
	if (attached < 0)
		return;
	if (!nova_bt_validate_peers(paired))
		pairing_file(false);
	u8 saved_mask = _paired_mask();
	if (!saved_mask)
	{
		if (attached == 0)
			_notify(SYMBOL_SHUFFLE"  #FFDD00 Joy-Con sem pareamento no SD#\n"
			        "Encaixe os Joy-Cons no console uma vez\n"
			        "para salvar o Bluetooth automaticamente.", 6500);
		return;
	}
	if (!(saved_mask & ~(u8)attached))
		return;

	manual_stop = false;
	if (!gui_bluetooth_using_radio())
	{
		s_printf(ui.note, "Console na dock (%d / 2). Bluetooth iniciado automaticamente.",
			nova_bt_validate_peers(paired));
		start(NULL);
	}
	const nova_bt_status *s = nova_bt_get_status();
	if (!s->pad[0].connected && !s->pad[1].connected)
		_notify(SYMBOL_SHUFFLE"  #96FF00 Bluetooth ativo# - pressione um botão nos Joy-Cons", 4000);
}

jc_gamepad_rpt_t *gui_bluetooth_merge(jc_gamepad_rpt_t *wired)
{
	static jc_gamepad_rpt_t merged;
	wired_conn_mask = wired ? ((wired->conn_l ? 1 : 0) | (wired->conn_r ? 2 : 0)) : 0;
	const nova_bt_status *s = nova_bt_get_status();
	if (!s->pad[0].connected && !s->pad[1].connected)
		return wired;

	if (wired)
		memcpy(&merged, wired, sizeof(merged));
	else
	{
		memset(&merged, 0, sizeof(merged));
		merged.lstick_x = merged.lstick_y = merged.rstick_x = merged.rstick_y = 0x800;
	}

	// A half on its rail always wins over the same half over the air.
	for (unsigned i = 0; i < 2; i++)
	{
		const nova_bt_pad *p = &s->pad[i];
		u32 buttons = nova_bt_take_buttons(i);
		if (!p->connected || (u32)(get_tmr_ms() - p->updated) > 500)
			continue;
		if (i ? merged.conn_r : merged.conn_l)
			continue;

		u32 mask = i ? 0x0016ff : 0xff2900;
		merged.buttons = (merged.buttons & ~mask) | buttons;
		if (i)
		{
			merged.conn_r = true;
			merged.rstick_x = p->x;
			merged.rstick_y = p->y;
			merged.batt_info_r = p->battery;
		}
		else
		{
			merged.conn_l = true;
			merged.lstick_x = p->x;
			merged.lstick_y = p->y;
			merged.batt_info_l = p->battery;
		}
	}
	return &merged;
}
