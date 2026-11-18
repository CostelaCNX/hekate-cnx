/*
 * Wireless Joy-Con Bluetooth frontend for Nyx GUI.
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

static nova_bt_peer paired[2];
static bool boot_checked;
static u8 boot_status; /* 1 rails, 2 started, 3 no file, 4 disabled/Lite,
                        * 5 detection unavailable, 6 already active, 7 failure. */
static struct {
	u8 attempted, rail[2], record[2], hos[2], compatible, recognized[2], source;
} import_info;

static struct {
	lv_obj_t *win, *label;
	lv_task_t *task;
	lv_signal_func_t signal;
	u32 shown;
	char note[512];
} ui;

static int pairing_file(bool write);

static void refresh(void)
{
	if (!ui.label)
		return;
	const nova_bt_status *s = nova_bt_get_status();
	char text[3072], detail[512];
	s_printf(detail,
		"Relatórios: %d / %d | ID: %02X / %02X | HID: %d / %d | ACK: %d / %d | ERR: %02X / %02X\n"
		"BTN: %06X / %06X | XY: %d,%d / %d,%d | PSM: %04X / %04X | ETAPA: %d / %d",
		(int)s->pad[0].reports, (int)s->pad[1].reports,
		s->pad[0].report_id, s->pad[1].report_id,
		s->pad[0].hid_open, s->pad[1].hid_open,
		s->pad[0].mode_ack, s->pad[1].mode_ack,
		s->pad[0].last_error, s->pad[1].last_error,
		(unsigned)s->pad[0].buttons, (unsigned)s->pad[1].buttons,
		s->pad[0].x, s->pad[0].y, s->pad[1].x, s->pad[1].y,
		(unsigned)s->pad[0].last_psm, (unsigned)s->pad[1].last_psm,
		s->pad[0].stage, s->pad[1].stage);

	const char *states[] = {
		"Bluetooth desligado",
		"Iniciando Bluetooth...",
		"Bluetooth pronto",
		"Erro no Bluetooth"
	};
	const char *state = s->state == NOVA_BT_READY ?
		(s->pad[0].connected || s->pad[1].connected ?
			"Controle Joy-Con sem fio ativo" :
			"Rádio pronto - pressione um botão no Joy-Con para acordar") :
		states[s->state < 4 ? s->state : 3];

	s_printf(text,
		"Estado: %s\n%s\n\n"
		"Joy-Con Esquerdo (L): %s    |    Joy-Con Direito (R): %s\n"
		"Pacotes HCI RX / TX: %d / %d\n%s\n\n"
		"1. Com os Joy-Cons encaixados no console, toque em 'Ler dos trilhos' (salva automático no SD).\n"
		"2. Destaque os Joy-Cons, toque em 'Iniciar BT' e pressione um botão em cada Joy-Con.\n"
		"Com o pareamento salvo no SD, o Bluetooth inicia sozinho ao ligar sem Joy-Cons nos trilhos.",
		state, ui.note,
		s->pad[0].connected ? "Conectado" : "Sem sinal",
		s->pad[1].connected ? "Conectado" : "Sem sinal",
		(int)s->rx_packets, (int)s->tx_packets, detail);

	if (s->state == NOVA_BT_ERROR)
	{
		char error[64];
		s_printf(error, "\nCódigo de erro: %d", s->error);
		strcat(text, error);
	}
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

void gui_bluetooth_shutdown(void)
{
	boot_checked = true;
	nova_bt_stop();
	if (ui.task)
	{
		lv_task_del(ui.task);
		ui.task = NULL;
	}
	refresh();
}

bool gui_bluetooth_active(void)
{
	return nova_bt_get_status()->state == NOVA_BT_READY;
}

void gui_bluetooth_poll(void) { nova_bt_poll(); }
void gui_bluetooth_poll_fast(void) { nova_bt_poll_fast(); }
void gui_bluetooth_draw_poll(void) { nova_bt_draw_poll(); }

static lv_res_t import_pairing(lv_obj_t *button)
{
	(void)button;
	gui_bluetooth_shutdown();
	memset(paired, 0, sizeof(paired));
	memset(&import_info, 0, sizeof(import_info));
	import_info.attempted = 1;
	import_info.source = 1;

	if (n_cfg.jc_disable || fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG)
	{
		strcpy(ui.note, "É necessário um console com Joy-Cons destacáveis e driver de trilho ativo.");
		refresh();
		return LV_RES_OK;
	}

	bool hos[2] = {false, false}, recognized[2] = {false, false};
	jc_gamepad_rpt_t *p = jc_get_bt_pairing_info_ex(&hos[0], &hos[1], &recognized[0], &recognized[1]);
	if (p)
	{
		for (unsigned i = 0; i < 2; i++)
		{
			jc_bt_conn_t *b = i ? &p->bt_conn_r : &p->bt_conn_l;
			import_info.rail[i] = i ? p->conn_r : p->conn_l;
			import_info.hos[i] = hos[i];
			import_info.recognized[i] = recognized[i];
			unsigned present = 0;
			for (unsigned j = 0; j < 16; j++)
				present |= b->ltk[j];
			import_info.record[i] = present != 0;
			if (!recognized[i] || !import_info.rail[i] || !(b->type & (i ? 2 : 1)))
				continue;
			paired[i].valid = 1;
			memcpy(paired[i].mac, b->mac, 6);
			memcpy(paired[i].host, b->host_mac, 6);
			memcpy(paired[i].key, b->ltk, 16);
		}
	}

	int count = nova_bt_validate_peers(paired);
	import_info.compatible = count != 0;
	if (!count)
		memset(paired, 0, sizeof(paired));

	if (count)
	{
		int saved_sd = pairing_file(true);
		s_printf(ui.note, "Pareamentos lidos: %d / 2 (%s)",
			count, saved_sd ? "salvo no SD automaticamente" : "apenas na memória");
	}
	else
	{
		const char *reason =
			(!import_info.rail[0] || !import_info.rail[1]) ?
				"Conexão nos trilhos incompleta. Encaixe ambos os Joy-Cons e tente novamente." :
			(!import_info.record[0] || !import_info.record[1]) ?
				"Não foi possível ler os dados de pareamento dos trilhos." :
			(!import_info.recognized[0] || !import_info.recognized[1]) ?
				"Soma de verificação ou perfil de host não reconhecido." :
				"Os registros de pareamento não passaram na validação.";
		s_printf(ui.note, "Pareamentos lidos: 0 / 2\n%s", reason);
	}
	refresh();
	return LV_RES_OK;
}

static lv_res_t start(lv_obj_t *button)
{
	(void)button;
	if (nova_bt_get_status()->state == NOVA_BT_STARTING || gui_bluetooth_active())
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
	if (!nova_bt_start(paired))
	{
		lv_task_del(ui.task);
		ui.task = NULL;
	}
	refresh();
	return LV_RES_OK;
}

static void report_signed(char *out, int32_t value)
{
	s_printf(out, "%s%d", value < 0 ? "-" : "", (unsigned)(value < 0 ? 0u - (uint32_t)value : (uint32_t)value));
}

static bool save_report(void)
{
	char *buffer = malloc(8192);
	if (!buffer)
		return false;
	const nova_bt_status *s = nova_bt_get_status();
	s_printf(buffer, "hekate Bluetooth session report\n"
		"Metadata only: no addresses, pairing keys or packet payloads.\n"
		"HCI RX/TX %d/%d; ACL RX/TX %d/%d; state %d error %d; HOST=%d (0=unknown 1=match 2=mismatch)\n",
		(int)s->rx_packets, (int)s->tx_packets, (int)s->rx_acl, (int)s->tx_acl, s->state, s->error, s->host_identity);
	u32 captured = get_tmr_ms(), length = strlen(buffer);
	s_printf(buffer + length, "Capture_ms=%d; radio_image=ROM (no HCD loaded); chipset_hint=%s\n", captured,
		s->controller_manufacturer == 15 && s->controller_subversion == 0x2409 ? "BCM4356A3" : "unknown");
	length = strlen(buffer);
	s_printf(buffer + length, "Autostart=%d (1=rails 2=started 3=no-file 4=disabled 5=unknown 6=active 7=failed)\n", boot_status);
	length = strlen(buffer);
	s_printf(buffer + length, "Import: attempted=%d rail=%d/%d record=%d/%d Nintendo=%d/%d compatible=%d recognized=%d/%d source=%d (1=rails 2=SD)\n",
		import_info.attempted, import_info.rail[0], import_info.rail[1], import_info.record[0], import_info.record[1],
		import_info.hos[0], import_info.hos[1], import_info.compatible, import_info.recognized[0], import_info.recognized[1], import_info.source);
	length = strlen(buffer);
	s_printf(buffer + length, "Controller: HCI=%02X revision=%04X manufacturer=%04X subversion=%04X\n",
		s->controller_hci_version, s->controller_revision, s->controller_manufacturer, s->controller_subversion);
	length = strlen(buffer);
	s_printf(buffer + length, "RX bytes=%d polls=%d max_call_us=%d\n",
		(int)s->rx_bytes, (int)s->rx_polls, (int)s->rx_max_us);
	length = strlen(buffer);
	s_printf(buffer + length, "Poll total_max_us=%d fast_max_us=%d normal_max_us=%d gap_max_us=%d RX_budget_hits=%d\n",
		(int)s->poll_max_us, (int)s->poll_fast_max_us, (int)s->poll_normal_max_us, (int)s->poll_gap_max_us, (int)s->rx_budget_hits);
	length = strlen(buffer);
	for (unsigned side = 0; side < 2; side++)
	{
		const nova_bt_pad *p = &s->pad[side];
		s_printf(buffer + length, "side %d: HID=%d reports=%d ID=%02X ACK=%d ERR=%02X PSM=%04X STEP=%d AUTH=%02X SEEN=%d ENC=%02X SEEN=%d ENABLED=%d DISC=%02X ROLE=%02X BR_ONLY=%d MODE=%d SEEN=%d\n",
			side, p->hid_open, (int)p->reports, p->report_id, p->mode_ack, p->last_error, (unsigned)p->last_psm, p->stage, p->auth_error, p->auth_seen,
			p->encryption_error, p->encryption_seen, p->encrypted, p->disconnect_reason, p->role, p->br_only, p->radio_mode, p->mode_seen);
		length = strlen(buffer);
		for (unsigned k = 0; k < NOVA_BT_RF_METRICS; k++)
		{
			char value[16], low[16], high[16];
			if (p->rf.samples[k])
			{
				report_signed(value, p->rf.value[k]);
				report_signed(low, p->rf.minimum[k]);
				report_signed(high, p->rf.maximum[k]);
			}
			else
			{
				strcpy(value, "n/a");
				strcpy(low, "n/a");
				strcpy(high, "n/a");
			}
			s_printf(buffer + length, "  RF kind=%d samples=%d age_ms=%d last=%s min=%s max=%s error=%02X\n", k, (int)p->rf.samples[k],
				p->rf.samples[k] ? (u32)(captured - p->rf.updated[k]) : 0, value, low, high, p->rf.error[k]);
			length = strlen(buffer);
		}
	}
	for (unsigned i = 0; i < nova_bt_trace_count() && length + 80 < 8192; i++)
	{
		nova_bt_trace r;
		if (!nova_bt_trace_get(i, &r))
			break;
		s_printf(buffer + length, "%08d %02d %04X %04X %04X\n", (int)r.ms, r.kind, r.a, r.b, r.c);
		length = strlen(buffer);
	}
	bool was_mounted = sd_get_card_mounted();
	if (sd_mount() != 0)
	{
		free(buffer);
		return false;
	}
	int result = f_mkdir("bootloader/bluetooth");
	if (result == FR_EXIST)
		result = 0;
	FIL fp;
	u32 written = 0;
	if (!result)
	{
		result = f_open(&fp, "bootloader/bluetooth/last-session.txt", FA_WRITE | FA_CREATE_ALWAYS);
		if (!result)
		{
			result = f_write(&fp, buffer, length, &written);
			if (!result && written != length)
				result = 1;
			int closed = f_close(&fp);
			if (!result)
				result = closed;
		}
	}
	if (!was_mounted)
		sd_unmount();
	free(buffer);
	return result == 0;
}

static lv_res_t stop(lv_obj_t *button)
{
	(void)button;
	bool had_trace = nova_bt_trace_count() != 0 || import_info.attempted || boot_status;
	bool saved = had_trace && save_report();
	gui_bluetooth_shutdown();
	if (had_trace)
	{
		strcpy(ui.note, saved ?
			"Bluetooth parado. Relatório salvo em: bootloader/bluetooth/last-session.txt" :
			"Bluetooth parado.");
		refresh();
	}
	return LV_RES_OK;
}

static lv_res_t forget(lv_obj_t *button)
{
	stop(button);
	memset(paired, 0, sizeof(paired));
	strcpy(ui.note, "Chaves da sessão em memória removidas.");
	refresh();
	return LV_RES_OK;
}

static int pairing_file(bool write)
{
	static const char *path = "bootloader/bluetooth/joycon-pairing.bin";
	static const char *backup = "bootloader/bluetooth/joycon-pairing.bak";
	static const char *temp = "bootloader/bluetooth/joycon-pairing.tmp";
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
		result = f_mkdir("bootloader/bluetooth");
		if (result && result != FR_EXIST)
			goto unmount;
		result = f_open(&fp, temp, FA_WRITE | FA_CREATE_ALWAYS);
		if (result)
			goto unmount;
		result = f_write(&fp, bytes, sizeof(bytes), &transferred);
		int closed = f_close(&fp);
		if (result || closed || transferred != sizeof(bytes))
		{
			f_unlink(temp);
			goto unmount;
		}
		result = f_stat(path, NULL);
		bool had_old = result == FR_OK;
		if (result != FR_OK && result != FR_NO_FILE)
		{
			f_unlink(temp);
			goto unmount;
		}
		if (had_old)
		{
			result = f_unlink(backup);
			if (result != FR_OK && result != FR_NO_FILE)
			{
				f_unlink(temp);
				goto unmount;
			}
			if (f_rename(path, backup) != FR_OK)
			{
				f_unlink(temp);
				goto unmount;
			}
		}
		if (f_rename(temp, path) != FR_OK)
		{
			if (had_old)
				f_rename(backup, path);
			f_unlink(temp);
			goto unmount;
		}
		count = nova_bt_validate_peers(paired);
	}
	else
	{
		const char *paths[] = {path, backup};
		for (unsigned i = 0; i < 2 && !count; i++)
		{
			result = f_open(&fp, paths[i], FA_READ);
			if (result)
				continue;
			bool size_ok = f_size(&fp) == sizeof(bytes);
			result = size_ok ? f_read(&fp, bytes, sizeof(bytes), &transferred) : 1;
			int closed = f_close(&fp);
			if (!result && !closed && transferred == sizeof(bytes))
				count = nova_bt_pairing_decode(candidate, bytes, sizeof(bytes));
		}
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
					if (raw_bt[i].type & (i ? 2 : 1))
					{
						candidate[i].valid = 1;
						memcpy(candidate[i].mac, raw_bt[i].mac, 6);
						memcpy(candidate[i].host, raw_bt[i].host_mac, 6);
						memcpy(candidate[i].key, raw_bt[i].ltk, 16);
					}
				}
				count = nova_bt_validate_peers(candidate);
				if (!count)
					memset(candidate, 0, sizeof(candidate));
			}
		}
		if (count)
		{
			memcpy(paired, candidate, sizeof(paired));
			memset(&import_info, 0, sizeof(import_info));
			import_info.attempted = 1;
			import_info.compatible = 1;
			import_info.source = 2;
			for (unsigned i = 0; i < 2; i++)
				import_info.record[i] = import_info.recognized[i] = paired[i].valid;
		}
	}
unmount:
	if (!mounted)
		sd_unmount();
done:
	memset(bytes, 0, sizeof(bytes));
	memset(candidate, 0, sizeof(candidate));
	return count;
}

static lv_res_t store_pairing(lv_obj_t *button)
{
	lv_btn_ext_t *ext = lv_obj_get_ext_attr(button);
	bool write = ext && ext->idx != 0;
	if (nova_bt_get_status()->state == NOVA_BT_STARTING || gui_bluetooth_active())
	{
		strcpy(ui.note, "Pare o Bluetooth antes de salvar ou carregar o pareamento.");
		refresh();
		return LV_RES_OK;
	}
	int count = pairing_file(write);
	const char *message = write ?
		(count ? "Pareamento salvo no SD (bootloader/bluetooth/joycon-pairing.bin)." :
		         "Não foi possível salvar. Leia um pareamento válido primeiro.") :
		(count ? "Pareamento carregado do cartão SD." :
		         "Nenhum arquivo de pareamento válido encontrado no SD.");
	s_printf(ui.note, "%s\nPareamentos válidos na sessão: %d / 2", message, nova_bt_validate_peers(paired));
	refresh();
	return LV_RES_OK;
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

	static const char *titles[6] = {
		SYMBOL_DOWNLOAD" Ler dos trilhos",
		SYMBOL_POWER" Iniciar BT",
		SYMBOL_REBOOT" Parar BT",
		SYMBOL_SAVE" Salvar no SD",
		SYMBOL_SD" Carregar do SD",
		SYMBOL_TRASH" Limpar sessão"
	};
	static const lv_action_t actions[6] = {
		import_pairing, start, stop, store_pairing, store_pairing, forget
	};

	for (unsigned i = 0; i < 6; i++)
	{
		lv_obj_t *btn = lv_btn_create(scrl, NULL);
		lv_btn_ext_t *ext = lv_obj_get_ext_attr(btn);
		ext->idx = (i == 3) ? 1 : 0;
		lv_obj_set_size(btn, 376, 56);
		lv_obj_set_pos(btn, 36 + (i % 3) * 394, 16 + (i / 3) * 68);
		lv_btn_set_action(btn, LV_BTN_ACTION_CLICK, actions[i]);
		lv_obj_t *lbl = lv_label_create(btn, NULL);
		lv_label_set_static_text(lbl, titles[i]);
	}

	static lv_point_t line_pts[] = { {0, 0}, {1164, 0} };
	lv_obj_t *line = lv_line_create(scrl, NULL);
	lv_line_set_points(line, line_pts, 2);
	lv_line_set_style(line, th->line.decor);
	lv_obj_set_pos(line, 36, 162);

	ui.label = lv_label_create(scrl, NULL);
	lv_label_set_long_mode(ui.label, LV_LABEL_LONG_BREAK);
	lv_obj_set_style(ui.label, th->label.prim);
	lv_obj_set_width(ui.label, 1164);
	lv_obj_set_pos(ui.label, 36, 178);

	refresh();
	return LV_RES_OK;
}

void gui_bluetooth_autostart(void)
{
	if (boot_checked)
		return;
	boot_checked = true;
	if (n_cfg.jc_disable || fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG)
	{
		boot_status = 4;
		return;
	}
	int attached = jc_get_attached_mask();
	if (attached < 0)
	{
		boot_status = 5;
		return;
	}
	if (attached)
	{
		boot_status = 1;
		return;
	}
	const nova_bt_status *s = nova_bt_get_status();
	if (s->state == NOVA_BT_STARTING || s->state == NOVA_BT_READY)
	{
		boot_status = 6;
		return;
	}
	if (!pairing_file(false))
	{
		boot_status = 3;
		return;
	}
	/* Recheck after SD I/O: a newly attached controller always wins. */
	attached = jc_get_attached_mask();
	if (attached)
	{
		memset(paired, 0, sizeof(paired));
		memset(&import_info, 0, sizeof(import_info));
		boot_status = attached < 0 ? 5 : 1;
		return;
	}
	s_printf(ui.note, "Pareamento carregado do SD (%d / 2). Bluetooth iniciado automaticamente.",
		nova_bt_validate_peers(paired));
	start(NULL);
	s = nova_bt_get_status();
	boot_status = (s->state == NOVA_BT_STARTING || s->state == NOVA_BT_READY) ? 2 : 7;
}

void gui_bluetooth_try_dock_autostart(void)
{
	if (n_cfg.jc_disable || fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG)
		return;
	const nova_bt_status *s = nova_bt_get_status();
	if (s->state == NOVA_BT_STARTING || s->state == NOVA_BT_READY)
		return;
	if (jc_get_attached_mask() != 0)
		return;
	if (!nova_bt_validate_peers(paired) && !pairing_file(false))
		return;
	s_printf(ui.note, "Console na dock sem Joy-Con nos trilhos (%d / 2). Bluetooth iniciado automaticamente.",
		nova_bt_validate_peers(paired));
	start(NULL);
}

jc_gamepad_rpt_t *gui_bluetooth_merge(jc_gamepad_rpt_t *wired)
{
	static jc_gamepad_rpt_t merged;
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
	for (unsigned i = 0; i < 2; i++)
	{
		const nova_bt_pad *p = &s->pad[i];
		u32 buttons = nova_bt_take_buttons(i);
		if (!p->connected || (u32)(get_tmr_ms() - p->updated) > 500)
			continue;
		if (i ? !merged.conn_r : !merged.conn_l)
		{
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
	}
	return &merged;
}
