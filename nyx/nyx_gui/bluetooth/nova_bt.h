/*
 * Wireless Joy-Con Classic HID host for Nyx.
 *
 * Ported from Hekate Nova Pro (Retrogendude). The UART-D radio bring-up
 * follows HWTest-RCM by Creased (Baptiste MOINE). Built on BTstack
 * (BlueKitchen GmbH), see libs/btstack/LICENSE.
 */

#ifndef NOVA_BT_H
#define NOVA_BT_H

#include <stdint.h>

/* Fixed-width ABI: BDK and BTstack use different definitions of bool. */
typedef struct {
	uint8_t valid, mac[6], host[6], key[16]; /* key in HCI wire order */
} nova_bt_peer;

typedef struct {
	uint32_t buttons, updated;
	/* Rising edges await one UI read; each expires independently. */
	uint32_t pending_buttons, pressed_ms[24];
	uint16_t x, y;
	uint8_t connected, battery;
	uint8_t report_id, mode_ack, hid_open, encrypted;
} nova_bt_pad;

enum { NOVA_BT_OFF, NOVA_BT_STARTING, NOVA_BT_READY, NOVA_BT_ERROR };

enum {
	NOVA_BT_ERR_PEERS = 1,
	NOVA_BT_ERR_RADIO,
	NOVA_BT_ERR_TIMEOUT,
	NOVA_BT_ERR_UART,
	NOVA_BT_ERR_RX,
	NOVA_BT_ERR_TX,
	NOVA_BT_ERR_HW = 8,
	NOVA_BT_ERR_HOST
};

typedef struct {
	int state, error;
	uint8_t patchram; /* 0 ROM only, 1 download in progress, 2 launched */
	nova_bt_pad pad[2];
} nova_bt_status;

/* Optional Broadcom .hcd patchram, sent after the first HCI_Reset. The buffer
 * must stay valid until nova_bt_stop(); NULL keeps the ROM image. */
void nova_bt_set_patchram(const uint8_t *hcd, uint32_t size);
int nova_bt_patchram_valid(const uint8_t *hcd, uint32_t size);

int nova_bt_start(const nova_bt_peer peers[2]);
void nova_bt_stop(void);
void nova_bt_poll(void);
void nova_bt_poll_fast(void); /* No quiet wait; short RX budget while drawing. */
void nova_bt_draw_poll(void); /* Rate-limited cooperative service, no LVGL work. */
const nova_bt_status *nova_bt_get_status(void);

/* Consume once per side per UI read, including when wired input wins.
 * Held buttons remain held; released short presses survive for at most 100 ms. */
#define NOVA_BT_PRESS_LATCH_MS 100
uint32_t nova_bt_take_buttons(unsigned side);

int nova_bt_decode(nova_bt_pad *pad, unsigned side, const uint8_t *report, unsigned len, uint32_t now);
int nova_bt_validate_peers(const nova_bt_peer peers[2]);

#define NOVA_BT_PAIRING_SIZE 72
/* Private SD format, independent of C structure padding. CRC detects damage;
 * this file contains plain pairing keys. */
int nova_bt_pairing_encode(uint8_t bytes[NOVA_BT_PAIRING_SIZE], const nova_bt_peer peers[2]);
int nova_bt_pairing_decode(nova_bt_peer peers[2], const uint8_t *bytes, unsigned size);

/* Radio operations never wait for a byte. Startup alone has a bounded POR. */
uint32_t nova_bt_now(void);
uint32_t nova_bt_now_us(void);
int nova_bt_radio_open(void);
void nova_bt_radio_close(void);
int nova_bt_radio_read(void); /* -1 empty, -2 UART fault */
int nova_bt_radio_write(uint8_t value); /* 0 backpressure */

#endif
