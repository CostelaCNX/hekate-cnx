/*
 * Cooperative Classic HID host. BTstack stays isolated from BDK headers.
 *
 * Ported from Hekate Nova Pro (Retrogendude). The ROM link profile follows
 * HWTest-RCM by Creased (Baptiste MOINE).
 */

#include <string.h>

#include "nova_bt.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_event.h"
#include "btstack_chipset.h"
#include "hci.h"
#include "hci_cmd.h"
#include "l2cap.h"
#include "classic/hid_host.h"

#define PROFILE_BR     1
#define PROFILE_POLICY 2
#define PROFILE_ACTIVE 4
#define PROFILE_QOS    8

static nova_bt_status status;
static nova_bt_peer peers[2];
static int initialized, failure;
static uint8_t host_match;
static uint32_t started, tx_started, rx_started;
static uint8_t tx[1030], rx[1030];
static unsigned tx_len, tx_pos, rx_len, rx_need, rx_header;
static uint8_t rx_type;
static void (*receive_packet)(uint8_t, uint8_t *, uint16_t);
static btstack_packet_callback_registration_t event_registration;
static uint8_t descriptors[1024];
static uint32_t draw_last, page_quiet;
static uint8_t draw_seen, draw_busy;

static struct {
	uint16_t cid, handle;
	uint8_t retries, sequence, mode_tries, profile, output[48];
	uint8_t stream_seen, nudged, recovering, simple;
	uint32_t next, mode_next, opened, last_input;
} link[2];

static const uint8_t *patch_data;
static uint32_t patch_size, patch_pos;
static uint8_t patch_minidriver;

/* Public BTstack commands, queued only when its command buffer is available. */
static void profile_run(void)
{
	for (unsigned i = 0; i < 2; i++)
	{
		if (!link[i].profile || link[i].handle == HCI_CON_HANDLE_INVALID || !hci_connection_for_handle(link[i].handle))
			continue;
		if (!hci_can_send_command_packet_now())
			return;

		uint8_t flag;
		int result;
		if (link[i].profile & PROFILE_BR)
		{
			flag = PROFILE_BR;
			result = hci_send_cmd(&hci_change_connection_packet_type, link[i].handle, hci_usable_acl_packet_types());
		}
		else if (link[i].profile & PROFILE_POLICY)
		{
			flag = PROFILE_POLICY;
			result = hci_send_cmd(&hci_write_link_policy_settings, link[i].handle, LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
		}
		else if (link[i].profile & PROFILE_ACTIVE)
		{
			flag = PROFILE_ACTIVE;
			result = gap_sniff_mode_exit(link[i].handle);
		}
		else
		{
			flag = PROFILE_QOS;
			result = gap_qos_set(link[i].handle, HCI_SERVICE_TYPE_GUARANTEED, 8000, 0, 2500, 0xffffffffu);
		}
		if (!result)
			link[i].profile &= ~flag;
		return;
	}
}

static int side_for(const uint8_t *addr)
{
	for (unsigned i = 0; i < 2; i++)
		if (peers[i].valid && !memcmp(addr, peers[i].mac, 6))
			return i;
	return -1;
}

static int side_handle(uint16_t handle)
{
	hci_connection_t *conn = hci_connection_for_handle(handle);
	if (conn)
		return side_for(conn->address);
	for (unsigned i = 0; i < 2; i++)
		if (link[i].handle == handle)
			return i;
	return -1;
}

static int side_cid(uint16_t cid)
{
	for (unsigned i = 0; i < 2; i++)
		if (cid && link[i].cid == cid)
			return i;
	return -1;
}

static void timer_set(btstack_timer_source_t *t, uint32_t ms)
{
	t->timeout = nova_bt_now() + ms + 1;
}

static void noop(void) {}

static const btstack_run_loop_t run_loop = {
	.init = btstack_run_loop_base_init,
	.add_data_source = btstack_run_loop_base_add_data_source,
	.remove_data_source = btstack_run_loop_base_remove_data_source,
	.enable_data_source_callbacks = btstack_run_loop_base_enable_data_source_callbacks,
	.disable_data_source_callbacks = btstack_run_loop_base_disable_data_source_callbacks,
	.set_timer = timer_set,
	.add_timer = btstack_run_loop_base_add_timer,
	.remove_timer = btstack_run_loop_base_remove_timer,
	.execute = noop,
	.dump_timer = btstack_run_loop_base_dump_timer,
	.get_time_ms = nova_bt_now,
	.poll_data_sources_from_irq = noop,
	.execute_on_main_thread = btstack_run_loop_base_add_callback,
	.trigger_exit = noop
};

static void transport_init(const void *config) { (void)config; }
static int transport_open(void) { return 0; }
static int transport_close(void) { tx_len = rx_len = rx_need = 0; return 0; }
static void transport_register(void (*cb)(uint8_t, uint8_t *, uint16_t)) { receive_packet = cb; }
static int transport_ready(uint8_t type) { (void)type; return tx_len == 0; }

static int transport_send(uint8_t type, uint8_t *packet, int size)
{
	if (tx_len || size < 0 || (unsigned)size + 1 > sizeof(tx))
		return -1;
	tx[0] = type;
	memcpy(tx + 1, packet, size);
	tx_len = size + 1;
	tx_pos = 0;
	tx_started = nova_bt_now();
	return 0;
}

static const hci_transport_t transport = {
	.name = "UART-D H4",
	.init = transport_init,
	.open = transport_open,
	.close = transport_close,
	.register_packet_handler = transport_register,
	.can_send_packet_now = transport_ready,
	.send_packet = transport_send
};

/* Broadcom Write_BD_ADDR. RAM only. */
static void bcm_address(bd_addr_t addr, uint8_t *cmd)
{
	cmd[0] = 1;
	cmd[1] = 0xfc;
	cmd[2] = 6;
	reverse_bd_addr(addr, cmd + 3);
}

int nova_bt_patchram_valid(const uint8_t *d, uint32_t n)
{
	if (!d || n < 3)
		return 0;
	unsigned records = 0;
	for (uint32_t at = 0; at < n; records++)
	{
		if (n - at < 3 || d[at + 1] != 0xfc)
			return 0;
		uint32_t len = 3u + d[at + 2];
		if (len > n - at)
			return 0;
		at += len;
	}
	return records > 1;
}

void nova_bt_set_patchram(const uint8_t *d, uint32_t n)
{
	int valid = nova_bt_patchram_valid(d, n);
	patch_data = valid ? d : NULL;
	patch_size = valid ? n : 0;
}

static void bcm_init(const void *config)
{
	(void)config;
	patch_pos = 0;
	patch_minidriver = patch_size != 0;
}

/* Horizon pushes a patchram over HCI; the bare ROM drops links seconds after
 * encryption. The image is user-supplied, never embedded here. */
static btstack_chipset_result_t bcm_next(uint8_t *cmd)
{
	if (!patch_size)
		return BTSTACK_CHIPSET_NO_INIT_SCRIPT;

	if (patch_minidriver)
	{
		/* Download_Minidriver: the ROM accepts Write_RAM only after this. */
		patch_minidriver = 0;
		cmd[0] = 0x2e;
		cmd[1] = 0xfc;
		cmd[2] = 0;
		status.patchram = 1;
		return BTSTACK_CHIPSET_VALID_COMMAND;
	}

	if (patch_pos >= patch_size)
	{
		if (status.patchram == 1)
			status.patchram = 2;
		return BTSTACK_CHIPSET_DONE;
	}

	unsigned len = 3u + patch_data[patch_pos + 2];
	memcpy(cmd, patch_data + patch_pos, len);
	patch_pos += len;
	return BTSTACK_CHIPSET_VALID_COMMAND;
}

static const btstack_chipset_t chipset = {
	.name = "CYW4356",
	.init = bcm_init,
	.next_command = bcm_next,
	.set_bd_addr_command = bcm_address
};

static int known_connection(bd_addr_t addr, hci_link_type_t type)
{
	return type == HCI_LINK_TYPE_ACL && side_for(addr) >= 0;
}

static int key_get(bd_addr_t addr, link_key_t key, link_key_type_t *type)
{
	int i = side_for(addr);
	if (i < 0)
		return 0;
	memcpy(key, peers[i].key, 16);
	*type = UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192;
	return 1;
}

static void key_put(bd_addr_t addr, link_key_t key, link_key_type_t type) { (void)addr; (void)key; (void)type; }
static void key_addr(bd_addr_t addr) { (void)addr; }
static int key_iter(btstack_link_key_iterator_t *it) { (void)it; return 0; }
static int key_next(btstack_link_key_iterator_t *it, bd_addr_t addr, link_key_t key, link_key_type_t *type)
{
	(void)it; (void)addr; (void)key; (void)type;
	return 0;
}
static void key_done(btstack_link_key_iterator_t *it) { (void)it; }

/* Keys come only from the imported pairing; nothing is ever written back. */
static const btstack_link_key_db_t keys = {
	noop, key_addr, noop, key_get, key_put, key_addr, key_iter, key_next, key_done
};

static void clear_pad(unsigned i)
{
	status.pad[i].connected = 0;
	status.pad[i].buttons = status.pad[i].pending_buttons = 0;
	status.pad[i].x = status.pad[i].y = 2048;
}

/* Recover one silent HID stream without restarting the other Joy-Con or
 * touching the saved pairing. Times are per connection and wrap-safe. */
static void recover_stream(unsigned side, uint32_t now)
{
	nova_bt_pad *pad = &status.pad[side];
	if (!pad->hid_open || !pad->encrypted || link[side].recovering || link[side].simple)
		return;

	uint32_t idle = now - (link[side].stream_seen ? link[side].last_input : link[side].opened);
	if (idle >= (link[side].stream_seen ? 4000u : 8000u))
	{
		if (link[side].handle != HCI_CON_HANDLE_INVALID && !gap_disconnect(link[side].handle))
		{
			link[side].recovering = 1;
			clear_pad(side);
		}
	}
	else if (link[side].stream_seen && idle >= 1000 && !link[side].nudged)
	{
		/* Reuse the padded 0x03/0x30 report command and existing retry path. */
		link[side].nudged = 1;
		link[side].mode_tries = 0;
		link[side].mode_next = now ? now : 1;
		pad->mode_ack = 0;
	}
}

static void hid_event(uint8_t *p)
{
	bd_addr_t addr;
	int i;
	uint16_t cid;

	switch (hci_event_hid_meta_get_subevent_code(p))
	{
	case HID_SUBEVENT_INCOMING_CONNECTION:
		hid_subevent_incoming_connection_get_address(p, addr);
		cid = hid_subevent_incoming_connection_get_hid_cid(p);
		i = side_for(addr);
		if (i >= 0 && !hid_subevent_incoming_connection_get_status(p))
		{
			link[i].cid = cid;
			hid_host_accept_connection(cid, HID_PROTOCOL_MODE_REPORT);
		}
		else
			hid_host_decline_connection(cid);
		break;

	case HID_SUBEVENT_CONNECTION_OPENED:
		hid_subevent_connection_opened_get_bd_addr(p, addr);
		i = side_for(addr);
		if (i < 0)
			break;
		if (hid_subevent_connection_opened_get_status(p))
		{
			status.pad[i].hid_open = 0;
			link[i].cid = 0;
			link[i].next = nova_bt_now() + (link[i].retries >= 2 ? 20000 : 4000);
			page_quiet = nova_bt_now() + 3000;
			clear_pad(i);
		}
		else
		{
			status.pad[i].hid_open = 1;
			status.pad[i].mode_ack = 0;
			link[i].cid = hid_subevent_connection_opened_get_hid_cid(p);
			link[i].retries = 0;
			link[i].opened = nova_bt_now();
			link[i].last_input = 0;
			link[i].stream_seen = link[i].nudged = link[i].recovering = link[i].simple = 0;
			link[i].mode_tries = 0;
			link[i].mode_next = nova_bt_now() + 150;
		}
		break;

	case HID_SUBEVENT_REPORT:
		i = side_cid(hid_subevent_report_get_hid_cid(p));
		if (i < 0)
			break;
		if (nova_bt_decode(&status.pad[i], i, hid_subevent_report_get_report(p),
				hid_subevent_report_get_report_len(p), nova_bt_now()))
		{
			if (status.pad[i].report_id == 0x30)
			{
				link[i].last_input = nova_bt_now();
				link[i].stream_seen = 1;
				link[i].nudged = link[i].simple = 0;
			}
			else if (status.pad[i].report_id == 0x3f)
				link[i].simple = 1;
		}
		break;

	case HID_SUBEVENT_CONNECTION_CLOSED:
		i = side_cid(hid_subevent_connection_closed_get_hid_cid(p));
		if (i < 0)
			break;
		clear_pad(i);
		status.pad[i].hid_open = 0;
		link[i].cid = 0;
		link[i].next = nova_bt_now() + (link[i].recovering ? 1000 : 5000);
		link[i].recovering = 0;
		break;

	default:
		break;
	}
}

static void event(uint8_t type, uint16_t channel, uint8_t *p, uint16_t size)
{
	(void)channel;
	if (type != HCI_EVENT_PACKET || size < 2)
		return;

	bd_addr_t addr;
	int i;

	switch (hci_event_packet_get_type(p))
	{
	case HCI_EVENT_COMMAND_COMPLETE:
		/* Verify the controller took the console's address. */
		if (size >= 12 && little_endian_read_16(p, 3) == HCI_OPCODE_HCI_READ_BD_ADDR && !p[5])
		{
			reverse_bd_addr(p + 6, addr);
			host_match = !memcmp(addr, peers[peers[0].valid ? 0 : 1].host, 6);
			if (!host_match)
				failure = NOVA_BT_ERR_HOST;
		}
		break;

	case HCI_EVENT_CONNECTION_COMPLETE:
		if (size < 13)
			break;
		hci_event_connection_complete_get_bd_addr(p, addr);
		i = side_for(addr);
		if (i < 0 || hci_event_connection_complete_get_status(p))
			break;
		link[i].handle = hci_event_connection_complete_get_connection_handle(p);
		link[i].profile = PROFILE_BR | PROFILE_QOS;
		break;

	case HCI_EVENT_MODE_CHANGE:
		if (size < 8 || p[2])
			break;
		i = side_handle(little_endian_read_16(p, 3));
		/* Joy-Cons request sniff; keep the link active for input latency. */
		if (i >= 0 && p[5] == 2)
			link[i].profile |= PROFILE_POLICY | PROFILE_ACTIVE;
		break;

	case HCI_EVENT_ENCRYPTION_CHANGE:
	case HCI_EVENT_ENCRYPTION_CHANGE_V2:
		if (size < 6)
			break;
		i = side_handle(little_endian_read_16(p, 3));
		if (i >= 0)
			status.pad[i].encrypted = p[2] ? 0 : p[5];
		break;

	case HCI_EVENT_DISCONNECTION_COMPLETE:
		if (size < 6 || p[2])
			break;
		i = side_handle(little_endian_read_16(p, 3));
		if (i < 0)
			break;
		status.pad[i].encrypted = 0;
		status.pad[i].hid_open = 0;
		link[i].profile = 0;
		clear_pad(i);
		break;

	case BTSTACK_EVENT_STATE:
		if (btstack_event_state_get_state(p) == HCI_STATE_WORKING)
		{
			if (host_match)
				status.state = NOVA_BT_READY;
			else
				failure = NOVA_BT_ERR_HOST;
		}
		break;

	case HCI_EVENT_HARDWARE_ERROR:
		failure = NOVA_BT_ERR_HW;
		break;

	/* Never create new pairings: only the keys imported from the rails. */
	case HCI_EVENT_PIN_CODE_REQUEST:
		hci_event_pin_code_request_get_bd_addr(p, addr);
		gap_pin_code_negative(addr);
		break;
	case HCI_EVENT_USER_CONFIRMATION_REQUEST:
		hci_event_user_confirmation_request_get_bd_addr(p, addr);
		gap_ssp_confirmation_negative(addr);
		break;
	case HCI_EVENT_USER_PASSKEY_REQUEST:
		hci_event_user_passkey_request_get_bd_addr(p, addr);
		gap_ssp_passkey_negative(addr);
		break;

	case HCI_EVENT_HID_META:
		if (size >= 3)
			hid_event(p);
		break;

	default:
		break;
	}

	if (status.state == NOVA_BT_READY)
		profile_run();
}

static void input_byte(uint8_t value)
{
	if (!rx_type)
	{
		if (value == 0 || value == 0x31 || value == 0x34)
			return;
		rx_type = value;
		rx_len = 0;
		rx_started = nova_bt_now();
		switch (value)
		{
		case 4: rx_header = 2; break;
		case 2: rx_header = 4; break;
		case 3: rx_header = 3; break;
		case 7: rx_header = 0; break;
		default:
			failure = NOVA_BT_ERR_RX;
			return;
		}
		rx_need = rx_header ? rx_header : 63;
		return;
	}

	if (rx_len >= sizeof(rx))
	{
		failure = NOVA_BT_ERR_RX;
		return;
	}
	rx[rx_len++] = value;
	if (rx_header && rx_len == rx_header)
	{
		unsigned payload = rx_type == 4 ? rx[1] : rx_type == 2 ? (rx[2] | (rx[3] << 8)) : rx[2];
		rx_need = rx_header + payload;
		if (rx_need > sizeof(rx))
		{
			failure = NOVA_BT_ERR_RX;
			return;
		}
	}
	if (rx_len == rx_need)
	{
		uint8_t type = rx_type;
		unsigned len = rx_len;
		rx_type = 0;
		rx_len = rx_need = 0;
		if (type == 2 || type == 4)
			receive_packet(type, rx, len);
	}
}

const nova_bt_status *nova_bt_get_status(void)
{
	return &status;
}

uint32_t nova_bt_take_buttons(unsigned side)
{
	if (side >= 2)
		return 0;
	nova_bt_pad *p = &status.pad[side];
	uint32_t now = nova_bt_now(), buttons = 0;
	if (p->connected && (uint32_t)(now - p->updated) <= 500)
	{
		buttons = p->buttons;
		uint32_t pending = p->pending_buttons;
		for (unsigned bit = 0; pending; bit++, pending >>= 1)
			if ((pending & 1) && (uint32_t)(now - p->pressed_ms[bit]) <= NOVA_BT_PRESS_LATCH_MS)
				buttons |= 1u << bit;
	}
	p->pending_buttons = 0;
	return buttons;
}

void nova_bt_stop(void)
{
	if (initialized)
	{
		/* Power is removed first; callbacks cannot outlive the stack pools. */
		nova_bt_radio_close();
		hid_host_deinit();
		l2cap_deinit();
		hci_close();
		btstack_memory_deinit();
		initialized = 0;
	}
	memset(peers, 0, sizeof(peers));
	memset(link, 0, sizeof(link));
	draw_seen = draw_busy = 0;
	page_quiet = 0;
	tx_len = rx_len = rx_need = rx_type = 0;
	receive_packet = NULL;
	for (unsigned i = 0; i < 2; i++)
	{
		clear_pad(i);
		status.pad[i].hid_open = 0;
		status.pad[i].encrypted = 0;
	}
	status.state = NOVA_BT_OFF;
}

int nova_bt_start(const nova_bt_peer p[2])
{
	nova_bt_stop();
	memset(&status, 0, sizeof(status));
	for (unsigned i = 0; i < 2; i++)
		link[i].handle = HCI_CON_HANDLE_INVALID;

	if (!nova_bt_validate_peers(p))
	{
		status.state = NOVA_BT_ERROR;
		status.error = NOVA_BT_ERR_PEERS;
		return 0;
	}
	if (!nova_bt_radio_open())
	{
		status.state = NOVA_BT_ERROR;
		status.error = NOVA_BT_ERR_RADIO;
		return 0;
	}

	memcpy(peers, p, sizeof(peers));
	failure = 0;
	host_match = 0;
	started = nova_bt_now();

	btstack_memory_init();
	btstack_run_loop_init(&run_loop);
	hci_init(&transport, 0);
	initialized = 1;
	hci_set_chipset(&chipset);
	hci_set_link_key_db(&keys);
	hci_set_bd_addr(peers[peers[0].valid ? 0 : 1].host);
	gap_set_security_level(LEVEL_2);
	gap_ssp_set_auto_accept(0);

	/* A woken Joy-Con pages the console itself; while the controller is
	 * paging it cannot answer. Keep outgoing pages short and rare, and scan
	 * interlaced so the peer's page train is caught on the first interval. */
	gap_set_page_timeout(0x0800);
	gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
	gap_set_page_scan_activity(0x0200, 0x0024);

	/* HWTest-RCM ROM compatibility profile: no EDR packets; initially accept
	 * the Joy-Con's sniff request and role changes. */
	hci_enable_acl_packet_types(ACL_PACKET_TYPES_BR);
	gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
	hci_set_master_slave_policy(HCI_ROLE_MASTER);

	gap_register_classic_connection_filter(known_connection);
	gap_discoverable_control(0);
	gap_connectable_control(1);
	l2cap_init();
	hid_host_init(descriptors, sizeof(descriptors));
	hid_host_register_packet_handler(event);
	event_registration.callback = event;
	hci_add_event_handler(&event_registration);

	status.state = NOVA_BT_STARTING;
	hci_power_control(HCI_POWER_ON);
	return 1;
}

static void request_full_reports(unsigned i, uint32_t now)
{
	/* Keep the output storage alive until BTstack sends it. No rumble. */
	static const uint8_t neutral[] = { 0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40 };
	if (!link[i].mode_tries)
	{
		link[i].output[0] = link[i].sequence & 15;
		memcpy(link[i].output + 1, neutral, 8);
		link[i].output[9] = 3;
		link[i].output[10] = 0x30;
	}
	if (!hid_host_send_report(link[i].cid, 1, link[i].output, sizeof(link[i].output)))
		link[i].mode_tries++;
	link[i].mode_next = now + 1000;
}

static void poll_budget(uint32_t max_us, uint32_t quiet_us)
{
	if (!initialized)
		return;

	/* UART flow control restarts take >300us. The foreground drains bursts
	 * with a quiet period; drawing uses a short budget without waiting for a
	 * UART restart. An empty line returns at once. */
	uint32_t t0 = nova_bt_now_us(), last = t0;
	unsigned bytes = 0;
	int rx_yielded = 0;
	for (unsigned n = 0; n < 2048 && !failure;)
	{
		int value = nova_bt_radio_read();
		if (value == -2)
		{
			failure = NOVA_BT_ERR_UART;
			break;
		}
		if (value >= 0)
		{
			input_byte(value);
			bytes++;
			n++;
			if (n == 2048)
				rx_yielded = 1;
		}
		uint32_t us = nova_bt_now_us();
		if (value >= 0)
			last = us;
		else if (!bytes || (uint32_t)(us - last) >= quiet_us)
			break;
		if ((uint32_t)(us - t0) >= max_us)
		{
			rx_yielded = 1;
			break;
		}
	}

	if (!failure && tx_len)
	{
		for (unsigned n = 0; n < 32 && tx_pos < tx_len; n++)
		{
			if (!nova_bt_radio_write(tx[tx_pos]))
				break;
			tx_pos++;
		}
		if (tx_pos == tx_len)
		{
			tx_len = 0;
			uint8_t sent[] = { HCI_EVENT_TRANSPORT_PACKET_SENT, 0 };
			receive_packet(HCI_EVENT_PACKET, sent, sizeof(sent));
		}
		else if ((uint32_t)(nova_bt_now() - tx_started) > 2000)
			failure = NOVA_BT_ERR_TX;
	}

	if (!failure)
	{
		btstack_run_loop_base_execute_callbacks();
		btstack_run_loop_base_poll_data_sources();
		btstack_run_loop_base_process_timers(nova_bt_now());
	}
	if (!failure && status.state == NOVA_BT_READY)
		profile_run();

	uint32_t now = nova_bt_now();
	if (rx_type && (uint32_t)(now - rx_started) > 2000)
		failure = NOVA_BT_ERR_RX;
	/* 115200 baud moves ~11 bytes/ms; allow the patchram twice that time. */
	if (status.state == NOVA_BT_STARTING && (uint32_t)(now - started) > 15000 + patch_size / 5)
		failure = NOVA_BT_ERR_TIMEOUT;

	int pending = 0;
	for (unsigned i = 0; i < 2; i++)
		if (link[i].cid && !status.pad[i].hid_open)
			pending = 1;

	for (unsigned i = 0; i < 2 && !failure && status.state == NOVA_BT_READY; i++)
	{
		if (status.pad[i].connected && (uint32_t)(now - status.pad[i].updated) > 500)
			clear_pad(i);

		/* Do not mistake queued UART data or a partial H4 frame for silence. */
		if (!rx_yielded && !rx_type)
			recover_stream(i, now);

		if (!pending && peers[i].valid && !link[i].cid &&
			(int32_t)(now - link[i].next) >= 0 && (int32_t)(now - page_quiet) >= 0)
		{
			if (link[i].retries < 3)
				link[i].retries++;
			link[i].next = now + 20000;
			if (hid_host_connect(peers[i].mac, HID_PROTOCOL_MODE_REPORT, &link[i].cid))
				link[i].cid = 0;
			else
				pending = 1;
		}

		if (!link[i].recovering && link[i].cid && link[i].mode_next && !status.pad[i].mode_ack &&
			(int32_t)(now - link[i].mode_next) >= 0 && link[i].mode_tries < 3)
			request_full_reports(i, now);
	}

	if (failure)
	{
		int code = failure;
		nova_bt_stop();
		status.state = NOVA_BT_ERROR;
		status.error = code;
	}
}

/* Drain long bursts once in the foreground, not for every display rectangle. */
void nova_bt_poll(void)
{
	poll_budget(8000, 2000);
}

void nova_bt_poll_fast(void)
{
	poll_budget(250, 0);
}

void nova_bt_draw_poll(void)
{
	if (!initialized || draw_busy)
		return;
	uint32_t now = nova_bt_now_us();
	if (draw_seen && (uint32_t)(now - draw_last) < 2000)
		return;
	draw_seen = 1;
	draw_last = now;
	draw_busy = 1;
	poll_budget(250, 0);
	draw_busy = 0;
}
