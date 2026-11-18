/*
 * Dock / USB-C DisplayPort TV output service for Nyx GUI.
 *
 * Based on Hekate Nova Pro (Retrogendude). The dock and video drivers in
 * bdk/display/tv come from HWTest-RCM by Creased (Baptiste MOINE).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <bdk.h>
#include <display/tv/dpout.h>
#include <display/tv/usbpd.h>
#include <power/max77620.h>
#include <string.h>

#include "gui_tv.h"
#include "gui_bluetooth.h"
#include "../config.h"

#define TV_RUN_LIMIT_MS 45000

extern bpmp_freq_t bpmp_fid_current;

static struct {
	lv_obj_t *win, *status;
	lv_task_t *task;
	lv_signal_func_t signal;
	dock_t dock;
	pd_reset_t pd_rst;
	dpout_sink_t sink;
	dpout_video_t video;
	pd_snapshot_t snap;
	void *allocation;
	u32 *fb;
	u32 started;
	bpmp_freq_t clock;
	bool clock_saved, running, resetting_pd, pd_reset_tried, mirrored, rails_saved;
	bool pad_saved, hpd_gpio, hpd_oe;
	bool auto_attempted, auto_suspended, resume_arm;
	bool console_dark, hardware_exit;
	u32 auto_since, auto_poll_at, auto_samples, auto_errors;
	u32 hpd_mux;
	u8 rails[2];
	int dirty_first, dirty_last;
	char message[384];
} tv;

static lv_res_t _cancel(lv_obj_t *btn);
static lv_res_t _begin(void);
static void _tick(void *param);

bool gui_tv_console_dark(void)
{
	return tv.console_dark;
}

void gui_tv_screen_configure(void)
{
	bool dark = tv.mirrored && !n_cfg.tv_keep_screen;
	if (dark && !tv.console_dark)
	{
		display_backlight_brightness(0, 150);
		tv.console_dark = true;
	}
	else if (!dark && tv.console_dark)
	{
		if (!tv.hardware_exit)
		{
			int level = (int)h_cfg.backlight - 20;
			if (level < 10) level = 10;
			if (level > 200) level = 200;
			display_backlight_brightness(level, 150);
		}
		tv.console_dark = false;
	}
}

static bool _auto_enabled(void)
{
	return n_cfg.tv_auto == 1 && !tv.auto_suspended && fuse_read_hw_type() != FUSE_NX_HW_TYPE_HOAG;
}

static void _task_ensure(void)
{
	if (!tv.task)
		tv.task = lv_task_create(_tick, 40, LV_TASK_PRIO_MID, NULL);
}

static void _auto_poll(void)
{
	u32 now = get_tmr_ms();
	u32 interval = tv.auto_errors >= 3 ? 5000 : tv.auto_errors ? 2000 : 250;
	if (!_auto_enabled() || (u32)(now - tv.auto_since) < 500 ||
		(u32)(now - tv.auto_poll_at) < interval)
		return;
	tv.auto_poll_at = now;

	bool inserted, dock;
	bpmp_freq_t clock = bpmp_fid_current;
	bpmp_clk_rate_set(BPMP_CLK_NORMAL);
	int r = pd_dock_attached(&inserted, &dock);
	bpmp_clk_rate_set(clock);
	if (r)
	{
		if (tv.auto_errors < 3) tv.auto_errors++;
		tv.auto_samples = 0; // An I2C error is not an unplug event.
		// A silent controller after a cold boot needs the HOS reset sequence.
		if (tv.auto_errors >= 2 && !tv.pd_reset_tried)
		{
			tv.pd_reset_tried = true;
			_begin();
		}
		return;
	}
	tv.auto_errors = 0;
	if (!inserted)
	{
		tv.auto_attempted = false;
		tv.auto_samples = 0;
		return;
	}
	if (!dock || tv.auto_attempted)
	{
		tv.auto_samples = 0;
		return;
	}
	if (++tv.auto_samples >= 2)
	{
		tv.auto_samples = 0;
		tv.auto_attempted = true; // One automatic attempt per physical insertion.
		_begin();
	}
}

void gui_tv_auto_configure(void)
{
	tv.auto_suspended = false;
	tv.resume_arm = false;
	tv.auto_attempted = false;
	tv.auto_samples = 0;
	tv.auto_errors = 0;
	tv.auto_since = tv.auto_poll_at = get_tmr_ms();
	if (_auto_enabled())
		_task_ensure();
	if (!_auto_enabled() && !tv.running && !tv.mirrored && !tv.win && tv.task)
	{
		lv_task_del(tv.task);
		tv.task = NULL;
	}
}

void gui_tv_resume_detection(void)
{
	if (!tv.auto_suspended)
		return;
	tv.auto_suspended = false;
	if (tv.resume_arm)
		tv.auto_attempted = false;
	tv.resume_arm = false;
	tv.auto_samples = 0;
	tv.auto_errors = 0;
	tv.auto_since = tv.auto_poll_at = get_tmr_ms();
	if (_auto_enabled())
		_task_ensure();
}

static void _show(const char *text)
{
	if (text != tv.message)
	{
		strncpy(tv.message, text, sizeof(tv.message) - 1);
		tv.message[sizeof(tv.message) - 1] = 0;
	}
	if (tv.status)
		lv_label_set_text(tv.status, tv.message);
}

static void _stop(void)
{
	// Stop DMA before freeing its framebuffer, including partial starts.
	tv.mirrored = false;
	gui_tv_screen_configure();
	if (tv.clock_saved)
		bpmp_clk_rate_set(BPMP_CLK_NORMAL);
	dpout_video_stop();
	dock_stop(&tv.dock);
	dpout_aux_end();
	if (tv.pad_saved)
	{
		PINMUX_AUX(0x19C) = tv.hpd_mux;
		gpio_config(GPIO_PORT_CC, GPIO_PIN_1, tv.hpd_gpio);
		gpio_output_enable(GPIO_PORT_CC, GPIO_PIN_1, tv.hpd_oe);
		tv.pad_saved = false;
	}
	if (tv.rails_saved)
	{
		// LDO7 also feeds the Bluetooth radio; leave it while BT owns it.
		if (!gui_bluetooth_using_radio())
			i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO7_CFG, tv.rails[0]);
		i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO8_CFG, tv.rails[1]);
		tv.rails_saved = false;
	}
	if (tv.clock_saved)
	{
		bpmp_clk_rate_set(tv.clock);
		tv.clock_saved = false;
	}
	tv.running = false;
	tv.resetting_pd = false;
	if (tv.allocation)
		free(tv.allocation);
	tv.allocation = NULL;
	tv.fb = NULL;
	if (!tv.win && tv.task && !_auto_enabled())
	{
		lv_task_del(tv.task);
		tv.task = NULL;
	}
}

static void _fail(const char *text)
{
	_stop();
	_show(text);
}

void gui_tv_shutdown(void)
{
	bool was_active = tv.running || tv.mirrored || tv.clock_saved;
	tv.resume_arm |= was_active && n_cfg.tv_auto == 1;
	tv.auto_suspended = true;
	_stop();
	if (tv.task)
	{
		lv_task_del(tv.task);
		tv.task = NULL;
	}
	if (was_active)
		_show("Saída de TV encerrada.");
}

// Strong override of the BDK teardown hook; no SD access after sd_end().
void hw_display_external_end(void)
{
	gui_bluetooth_shutdown();
	tv.hardware_exit = true;
	gui_tv_shutdown();
	tv.hardware_exit = false;
}

void gui_tv_dirty(int y1, int y2)
{
	if (!tv.mirrored)
		return;
	if (y1 < 0) y1 = 0;
	if (y2 >= 720) y2 = 719;
	if (y1 < tv.dirty_first) tv.dirty_first = y1;
	if (y2 > tv.dirty_last) tv.dirty_last = y2;
}

static void _mirror(void)
{
	// This task runs outside LVGL's refresh, after all frame chunks.
	if (!dpout_hpd())
	{
		_fail("TV desconectada. Saída encerrada. Inicie novamente após conectar na dock.");
		return;
	}
	if (tv.dirty_last >= tv.dirty_first)
	{
		u32 first = tv.dirty_first * 1280;
		u32 count = (tv.dirty_last - tv.dirty_first + 1) * 1280;
		memcpy(tv.fb + first, (u32 *)NYX_FB2_ADDRESS + first, count * 4);
		bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLEAN_PHY, false);
		tv.dirty_first = 720;
		tv.dirty_last = -1;
	}
}

static void _video_start(void)
{
	int r = dpout_aux_init();
	if (!r)
		r = dpout_probe_sink(&tv.sink);
	if (r)
	{
		s_printf(tv.message, "A TV não respondeu pela dock (%s).", tv.sink.why);
		_fail(tv.message);
		return;
	}

	memcpy(tv.fb, (void *)NYX_FB2_ADDRESS, NYX_FB_SZ);
	bpmp_mmu_maintenance(BPMP_MMU_MAINT_CLEAN_PHY, false);
	if (dpout_video_start(&tv.video, &tv.sink, (u32)tv.fb))
	{
		s_printf(tv.message, "Falha ao ligar o vídeo na TV (%s).", tv.video.why);
		_fail(tv.message);
		return;
	}

	tv.running = false;
	tv.mirrored = true;
	tv.dirty_first = 0;
	tv.dirty_last = 719;
	// Only PD negotiation needs the base clock. Restore UI performance.
	bpmp_clk_rate_set(tv.clock);
	gui_tv_screen_configure();
	gui_bluetooth_try_dock_autostart();

	s_printf(tv.message,
		"Imagem ativa na TV: 1280x720 a 60 Hz (%s %s).\n\n"
		"Feche esta janela para continuar usando o Nyx.\n"
		"Use 'Parar TV' ou pressione VOL- para encerrar a saída de TV.",
		tv.sink.tv_maker, tv.sink.tv_name);
	_show(tv.message);
}

static void _tick(void *param)
{
	(void)param;
	if ((tv.running || tv.mirrored) && (btn_read_vol() & BTN_VOL_DOWN))
	{
		_cancel(NULL);
		return;
	}
	if (tv.mirrored)
	{
		_mirror();
		return;
	}
	if (!tv.running)
	{
		_auto_poll();
		return;
	}
	if ((u32)(get_tmr_ms() - tv.started) >= TV_RUN_LIMIT_MS)
	{
		_fail("Tempo esgotado na negociação com a dock. Verifique a fonte e o cabo HDMI e tente novamente.");
		return;
	}

	if (tv.resetting_pd)
	{
		pd_reset_step(&tv.pd_rst);
		if (tv.pd_rst.state == PDR_FAILED)
		{
			s_printf(tv.message, "Falha ao reiniciar o controlador USB-PD (%s).", tv.pd_rst.why);
			_fail(tv.message);
			return;
		}
		if (tv.pd_rst.state == PDR_DONE)
		{
			tv.resetting_pd = false;
			pd_snapshot(&tv.snap, false);
			if (!(tv.snap.status1 & PD_S1_INSERT) && !tv.win)
			{
				_stop();
				tv.auto_attempted = false;
				return;
			}
			dock_start(&tv.dock);
		}
		return;
	}

	dock_step(&tv.dock);
	if (tv.dock.state == DOCK_FAILED)
	{
		if (!tv.pd_reset_tried)
		{
			tv.pd_reset_tried = true;
			tv.resetting_pd = true;
			pd_reset_start(&tv.pd_rst);
			return;
		}
		s_printf(tv.message, "A dock recusou a conexão (%s).", tv.dock.fail);
		_fail(tv.message);
		return;
	}
	if (tv.dock.state != DOCK_DONE)
		return;
	if (!tv.dock.dp_on || (!tv.dock.hpd && !(tv.dock.dp & PD_DP_SIGNAL_ON)))
	{
		_fail("Sem sinal de TV após negociação. Verifique a dock e o cabo HDMI.");
		return;
	}
	_video_start();
}

static lv_res_t _begin(void)
{
	_task_ensure();
	if (!tv.task)
	{
		_show("Não foi possível iniciar o serviço de TV. Feche a janela e tente novamente.");
		return LV_RES_OK;
	}
	if (tv.running || tv.mirrored)
		return LV_RES_OK;
	if (fuse_read_hw_type() == FUSE_NX_HW_TYPE_HOAG)
	{
		_show("O Switch Lite não possui suporte para saída de TV.");
		return LV_RES_OK;
	}

	tv.auto_attempted = true;
	tv.started = get_tmr_ms();
	memset(&tv.sink, 0, sizeof(tv.sink));
	memset(&tv.video, 0, sizeof(tv.video));
	tv.clock = bpmp_fid_current;
	tv.clock_saved = true;
	bpmp_clk_rate_set(BPMP_CLK_NORMAL);

	// Save the HPD pad before pd_snapshot() switches it to a GPIO input.
	tv.hpd_mux = PINMUX_AUX(0x19C);
	tv.hpd_gpio = (GPIO(0x700) & GPIO_PIN_1) != 0;
	tv.hpd_oe = (GPIO(0x710) & GPIO_PIN_1) != 0;
	tv.pad_saved = true;
	pd_snapshot(&tv.snap, false);

	if (i2c_recv_buf_small(&tv.rails[0], 1, I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO7_CFG) ||
		i2c_recv_buf_small(&tv.rails[1], 1, I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_LDO8_CFG))
	{
		_fail("Não foi possível ler os reguladores de vídeo. Inicialização da TV cancelada.");
		return LV_RES_OK;
	}
	tv.rails_saved = true;

	tv.allocation = malloc(NYX_FB_SZ + 4095);
	if (!tv.allocation)
	{
		_fail("Memória insuficiente para o framebuffer da TV.");
		return LV_RES_OK;
	}
	tv.fb = (u32 *)ALIGN((u32)tv.allocation, 4096);
	tv.running = true;

	if (!tv.snap.ok)
	{
		tv.resetting_pd = true;
		pd_reset_start(&tv.pd_rst);
		_show("Controlador USB-PD sem resposta; reiniciando...");
		return LV_RES_OK;
	}
	dock_start(&tv.dock);
	_show("Conectando à dock... Use 'Parar TV' ou pressione VOL- para cancelar.");
	return LV_RES_OK;
}

static lv_res_t _tv_start_action(lv_obj_t *btn)
{
	(void)btn;
	tv.pd_reset_tried = false;
	return _begin();
}

static lv_res_t _cancel(lv_obj_t *btn)
{
	(void)btn;
	tv.auto_attempted = true;
	tv.auto_samples = 0;
	if (!tv.running && !tv.mirrored)
	{
		_show("A saída de TV está desligada. Toque em 'Iniciar TV' para conectar à dock.");
		return LV_RES_OK;
	}
	_fail("Saída de TV parada.");
	return LV_RES_OK;
}

static lv_res_t _tv_auto_toggle(lv_obj_t *btn)
{
	n_cfg.tv_auto = !n_cfg.tv_auto;
	lv_btn_set_state(btn, n_cfg.tv_auto ? LV_BTN_STATE_TGL_REL : LV_BTN_STATE_REL);
	nyx_generic_onoff_toggle(btn);
	gui_tv_auto_configure();
	create_nyx_config_entry(false);
	return LV_RES_OK;
}

static lv_res_t _tv_keep_screen_toggle(lv_obj_t *btn)
{
	n_cfg.tv_keep_screen = !n_cfg.tv_keep_screen;
	lv_btn_set_state(btn, n_cfg.tv_keep_screen ? LV_BTN_STATE_TGL_REL : LV_BTN_STATE_REL);
	nyx_generic_onoff_toggle(btn);
	gui_tv_screen_configure();
	create_nyx_config_entry(false);
	return LV_RES_OK;
}

static lv_res_t _signal(lv_obj_t *obj, lv_signal_t sign, void *param)
{
	lv_signal_func_t previous = tv.signal;
	if (sign == LV_SIGNAL_CLEANUP)
	{
		// The session outlives its settings window; never retain dead widgets.
		tv.win = NULL;
		tv.status = NULL;
		if (!tv.running && !tv.mirrored && !_auto_enabled() && tv.task)
		{
			lv_task_del(tv.task);
			tv.task = NULL;
		}
	}
	return previous(obj, sign, param);
}

lv_res_t gui_tv_open(lv_obj_t *btn)
{
	(void)btn;
	if (tv.win)
		return LV_RES_OK;

	lv_theme_t *th = lv_theme_get_current();
	tv.win = nyx_create_standard_window(SYMBOL_USB" Saída TV USB-C / Dock", NULL);
	tv.signal = lv_obj_get_signal_func(tv.win);
	lv_obj_set_signal_func(tv.win, _signal);

	lv_win_add_btn(tv.win, NULL, SYMBOL_POWER" Iniciar TV", _tv_start_action);
	lv_win_add_btn(tv.win, NULL, SYMBOL_REBOOT" Parar TV", _cancel);
	lv_win_add_btn(tv.win, NULL, SYMBOL_SHUFFLE" Joy-Con BT", gui_bluetooth_open);

	lv_obj_t *page = lv_win_get_content(tv.win);
	lv_page_set_scrl_layout(page, LV_LAYOUT_OFF);
	lv_obj_t *scrl = lv_page_get_scrl(page);

	lv_obj_t *btn_auto = lv_btn_create(scrl, NULL);
	nyx_create_onoff_button(th, scrl, btn_auto, SYMBOL_REFRESH" Detecção auto da dock", _tv_auto_toggle, false);
	lv_obj_set_width(btn_auto, 560);
	lv_obj_set_pos(btn_auto, 36, 20);
	if (n_cfg.tv_auto)
		lv_btn_set_state(btn_auto, LV_BTN_STATE_TGL_REL);
	nyx_generic_onoff_toggle(btn_auto);

	lv_obj_t *btn_screen = lv_btn_create(scrl, NULL);
	nyx_create_onoff_button(th, scrl, btn_screen, SYMBOL_BRIGHTNESS" Manter tela do console", _tv_keep_screen_toggle, false);
	lv_obj_set_width(btn_screen, 560);
	lv_obj_set_pos(btn_screen, 640, 20);
	if (n_cfg.tv_keep_screen)
		lv_btn_set_state(btn_screen, LV_BTN_STATE_TGL_REL);
	nyx_generic_onoff_toggle(btn_screen);

	static lv_point_t line_pts[] = { {0, 0}, {1164, 0} };
	lv_obj_t *line = lv_line_create(scrl, NULL);
	lv_line_set_points(line, line_pts, 2);
	lv_line_set_style(line, th->line.decor);
	lv_obj_set_pos(line, 36, 102);

	tv.status = lv_label_create(scrl, NULL);
	lv_label_set_long_mode(tv.status, LV_LABEL_LONG_BREAK);
	lv_obj_set_style(tv.status, th->label.prim);
	lv_obj_set_width(tv.status, 1164);
	lv_obj_set_pos(tv.status, 36, 124);

	if (!tv.message[0])
		_show("Conecte a dock oficial à fonte de energia e à TV via HDMI.\n"
		      "Com a detecção automática ligada, basta encaixar o console na dock.\n\n"
		      "Para iniciar manualmente, toque em 'Iniciar TV' e coloque o console na dock em até 45 segundos.\n"
		      "Pressione VOL- a qualquer momento para encerrar a saída de TV e restaurar a tela do console.");
	else
		_show(tv.message);

	_task_ensure();
	if (!tv.task)
		_show("Não foi possível iniciar o serviço de TV. Feche a janela e tente novamente.");

	return LV_RES_OK;
}
