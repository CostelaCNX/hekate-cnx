/*
 * dpout.h - the SoC's DisplayPort output to the dock: the AUX channel
 * (DPAUX1) and the sink's DPCD, then SOR1 and the second display head.
 *
 * The PD controller has already routed SOR1's lanes to the port and the
 * dock has raised HPD (the usbpd handshake). What follows is what the
 * display driver does next, on the SoC side: the AUX channel to the dock's
 * DisplayPort sink, then link training, then video. coreboot's Tegra210
 * driver and L4T's are the references; the Switch uses SOR1, DPAUX1 and
 * the second display controller (DC1), two lanes, as its device tree says.
 *
 * From HWTest-RCM by Creased (Baptiste MOINE), as ported by Hekate Nova Pro
 * (Retrogendude).
 */
#ifndef HWTEST_DPOUT_H
#define HWTEST_DPOUT_H

#include <utils/types.h>

typedef struct {
    bool hpd;                 /* as DPAUX1 sees the hot plug detect line  */
    bool answered;            /* the sink answered an AUX read            */
    u8   dpcd[16];            /* DPCD 000..00F                            */
    u8   sink_count;          /* DPCD 200                                 */
    u8   lane_status[3];      /* DPCD 202..204, as the last link left it  */
    u8   sink_oui[3], sink_id[7];      /* DPCD 400..402, 403..408, text   */
    u8   branch_oui[3], branch_id[7];  /* DPCD 500..502, 503..508, text   */
    bool edid_ok;             /* the TV's EDID came through the converter */
    u8   edid[128];
    char tv_maker[4], tv_name[14];
    char why[96];
} dpout_sink_t;

/* The DPCD, decoded */
#define DPCD_REV(s)             ((s)->dpcd[0])
#define DPCD_MAX_RATE(s)        ((s)->dpcd[1])   /* 06 1.62, 0A 2.7, 14 5.4 Gbps */
#define DPCD_MAX_LANES(s)       ((s)->dpcd[2] & 0x1F)
#define DPCD_ENHANCED(s)        (((s)->dpcd[2] & 0x80) != 0)
#define DPCD_TPS3(s)            (((s)->dpcd[2] & 0x40) != 0)
#define DPCD_DOWNSTREAM(s)      (((s)->dpcd[5] & 0x01) != 0)
#define DPCD_DOWNSTREAM_TYPE(s) (((s)->dpcd[5] >> 1) & 3) /* 0 DP, 1 VGA, 2 DVI/HDMI, 3 other */

/* The AUX channel up: the SOR partition, DPAUX1's clock and pads. 0 ok. */
int  dpout_aux_init(void);
void dpout_aux_end(void);              /* release this session's AUX clocks */
bool dpout_aux_ready(void);
bool dpout_hpd(void);                 /* DPAUX1's view of HPD, once up    */
int  dpout_dpcd_read(u32 addr, u8 *buf, u32 n);          /* 0 ok        */
int  dpout_dpcd_write(u32 addr, const u8 *buf, u32 n);   /* 0 ok        */
const char *dpout_aux_error(int r);

/* Stage one of the picture: what is on the other end of the AUX channel.
 * 0 ok, with the sink filled in; otherwise why. */
int  dpout_probe_sink(dpout_sink_t *s);
int  dpout_edid_read(u8 *edid, u32 size);                /* 0 ok        */
const char *dpout_rate_name(u8 rate);

/* Stages two and three: SOR1 up on PLLDP, the link trained on two lanes
 * against the sink's own status registers, then the second display head
 * (DC1) sending the frame at fb to the TV, 1280x720 at 60 Hz, the GUI's
 * own size. The picture stays until dpout_video_stop(). */
typedef struct {
    u8   lanes, rate;         /* as trained                                */
    char why[96];
} dpout_video_t;

int  dpout_video_start(dpout_video_t *v, const dpout_sink_t *s, u32 fb);
void dpout_video_stop(void);
bool dpout_video_on(void);

#endif
