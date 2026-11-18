#ifndef NOVA_BT_H
#define NOVA_BT_H
#include <stdint.h>
/* Fixed-width ABI: BDK and BTstack use different definitions of bool. */
typedef struct {
    uint8_t valid, mac[6], host[6], key[16]; /* key in HCI wire order */
} nova_bt_peer;
#define NOVA_BT_RF_METRICS 5
enum {NOVA_RF_RSSI,NOVA_RF_QUALITY,NOVA_RF_TX_CURRENT,NOVA_RF_TX_MAX,NOVA_RF_FAILED_CONTACTS};
typedef struct {
    int32_t value[5],minimum[5],maximum[5];
    uint32_t samples[5],updated[5];
    uint8_t error[5];
} nova_bt_rf;
typedef struct {
    uint32_t buttons, updated;
    /* Rising edges await one UI read; each expires independently. */
    uint32_t pending_buttons, pressed_ms[24];
    uint16_t x, y;
    uint8_t connected, battery;
    uint8_t report_id, mode_ack, hid_open, last_error;
    uint32_t reports;
    uint16_t last_psm;
    uint8_t stage, auth_error;
    uint8_t auth_seen, encryption_seen, encryption_error, encrypted;
    uint8_t disconnect_reason, role; /* role: 0 central, 1 peripheral, 0xff unknown */
    uint8_t br_only, radio_mode, mode_seen;
    uint32_t buttons_seen, first_report_ms;
    uint32_t full_reports,last_full_ms,max_full_gap_ms,full_gaps_50ms,full_gaps_250ms;
    uint8_t full_gap_valid;
    uint32_t report_restarts,recoveries;
    nova_bt_rf rf;
} nova_bt_pad;
enum { NOVA_BT_OFF, NOVA_BT_STARTING, NOVA_BT_READY, NOVA_BT_ERROR };
typedef struct {
    int state, error;
    uint8_t host_identity; /* 0 unknown, 1 controller matches imported host, 2 mismatch */
    uint8_t controller_hci_version;
    uint16_t controller_manufacturer, controller_revision, controller_subversion;
    uint32_t rx_packets, tx_packets;
    uint32_t rx_acl, tx_acl;
    uint32_t rx_bytes, rx_polls, rx_max_us;
    uint32_t poll_max_us, poll_fast_max_us, poll_normal_max_us, poll_gap_max_us;
    uint32_t rx_budget_hits;
    uint32_t draw_yields,rf_queries,rf_bad_replies;
    nova_bt_pad pad[2];
} nova_bt_status;
typedef struct {uint32_t ms;uint16_t kind,a,b,c;} nova_bt_trace;
unsigned nova_bt_trace_count(void);
int nova_bt_trace_get(unsigned index,nova_bt_trace *record);
/* Metadata only: never put addresses, link keys or packet payloads here. */
void nova_bt_trace_reset(void);
void nova_bt_trace_add(uint16_t kind,uint16_t a,uint16_t b,uint16_t c);
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
/* Pure protocol helpers are exercised on the host as well as on ARM. */
int nova_bt_decode(nova_bt_pad *pad, unsigned side, const uint8_t *report, unsigned len, uint32_t now);
int nova_bt_validate_peers(const nova_bt_peer peers[2]);
#define NOVA_BT_PAIRING_SIZE 72
/* Private SD format, independent of C structure padding. CRC detects damage;
 * this file contains plain pairing keys and is never included in reports. */
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
