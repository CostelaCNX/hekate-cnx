/*
 * usbpd.h - the USB-C port's PD controller (Rohm BM92T36, I2C_1 at 0x18)
 * past what bdk reads from it: its status and alert registers, its
 * commands, and the conversation the console has with its dock.
 *
 * From HWTest-RCM by Creased (Baptiste MOINE), as ported by Hekate Nova Pro
 * (Retrogendude).
 *
 * The register and command map and the handshake follow CTCaer's bm92txx.c
 * in the switchroot L4T kernel, which is what Linux on the Switch docks
 * with; HOS does the same from its usb sysmodule. The handshake changes the
 * port's state (a 15 V request, a data role swap, the dock's DisplayPort
 * mode, lights and hub), so it only runs when TV output is started.
 *
 * The USB 3 / DisplayPort switch (PI3USB30532) that sits between the SoC
 * and the port has no address on any I2C bus the SoC reaches (the census of
 * all six finds none), and neither L4T nor its device tree drive one: on
 * this board the PD controller configures it as part of the DisplayPort
 * pin assignment. So the switch is tested through that step, indirectly.
 */
#ifndef HWTEST_USBPD_H
#define HWTEST_USBPD_H

#include <utils/types.h>

/* ---- the controller, read only ---- */

/* STATUS1 */
#define PD_S1_FAULT_MASK    (3 << 0)
#define PD_S1_LASTCMD_SHIFT 4
#define PD_S1_LASTCMD_MASK  (7 << 4)
#define PD_S1_INSERT        BIT(7)
#define PD_S1_DR_SHIFT      8
#define PD_S1_DR_MASK       (3 << 8)
#define PD_S1_VSAFE         BIT(10)
#define PD_S1_CSIDE         BIT(11)   /* CC2 is the live side          */
#define PD_S1_CMD_BUSY      BIT(13)
#define PD_S1_SPDSNK        BIT(14)
/* STATUS2 */
#define PD_S2_VCONN_ON      BIT(9)
#define PD_S2_ACC_SHIFT     10
#define PD_S2_ACC_MASK      (3 << 10)
#define PD_S2_EM_CABLE      BIT(12)
#define PD_S2_OTG_INSERT    BIT(13)
/* DP_STATUS */
#define PD_DP_PIN_CFG_DONE  BIT(1)
#define PD_DP_SIGNAL_ON     BIT(7)
#define PD_DP_INSERT        BIT(14)
#define PD_DP_EN            BIT(15)
/* ALERT_STATUS */
#define PD_AL_CMD_DONE      BIT(2)
#define PD_AL_VDM_RECEIVED  BIT(11)
#define PD_AL_CONTRACT      BIT(12)

#define PD_ROLE_UFP   1
#define PD_ROLE_DFP   2

typedef struct {
    bool ok;                  /* the chip answered with its ids           */
    int  rc;                  /* the id reads' I2C result, when it did not */
    bool normal_mode;         /* answered in normal mode, not packet mode  */
    u16  fw_type, fw_rev, man_id, dev_id, caps;
    u16  alert, status1, status2, dp_status;
    u16  dp_unk1c, dp_unk1d; /* HOS reads them around DisplayPort       */
    u16  config1, config2, sys1, sys2;
    u32  pdo_n;
    u32  pdo_mv[7], pdo_ma[7];
    u32  cur_pdo, cur_rdo;    /* the contract's objects, raw, 0 if none  */
    bool hpd;                 /* the hot plug detect line, GPIO CC1      */
} pd_snapshot_t;

/* Reading the alert register may clear it, so while a handshake runs the
 * snapshot leaves it to the handshake (read_alert false). */
void pd_snapshot(pd_snapshot_t *s, bool read_alert);
/* Passive polling: STATUS1, source PDOs and idle device ID; no alerts/commands.
 * Nintendo dock signature follows this driver's existing _choose() policy.
 * Nonzero means a failed read or silent/unrecognized chip, not cable removal. */
int pd_dock_attached(bool *inserted, bool *official_dock);
const char *pd_lastcmd_name(u16 status1);

/* ---- what HOS does to the controller at boot ---- */

/* The usb sysmodule's start-up, at every boot of a console with a battery
 * (read from its 17.0.0 build): a 30 ms pulse on PV5, 100 ms, the
 * interrupt line's fall, the device id polled every 10 ms until it answers
 * (10 s at most), SYS_RESET, then the DisplayPort alerts on and the
 * over-current protection on. Only run when the controller does not answer. */
typedef enum {
    PDR_IDLE,
    PDR_PULSE,            /* PV5 high                                     */
    PDR_SETTLE,           /* PV5 low again, HOS's 100 ms                   */
    PDR_WAIT_IRQ,         /* PK4 low                                       */
    PDR_WAIT_ID,          /* the device id back                            */
    PDR_SYS_RESET,        /* the command, then 100 ms as L4T waits         */
    PDR_CONFIG,           /* the ids, DP alerts, over-current protection   */
    PDR_DONE,
    PDR_FAILED,
} pd_reset_state_t;

typedef struct {
    pd_reset_state_t state;
    u32  since, started;      /* ms                                       */
    pd_reset_state_t failed_at;     /* the step, when PDR_FAILED          */
    char why[96];
} pd_reset_t;

void pd_reset_start(pd_reset_t *r);
void pd_reset_step(pd_reset_t *r);      /* every 40 ms or so              */
const char *pd_reset_state_name(pd_reset_state_t s);

/* ---- the dock handshake ---- */

typedef enum {
    DOCK_IDLE,
    DOCK_WAIT_PLUG,       /* a cable                                      */
    DOCK_RDO_SENT,        /* asked for a power profile                    */
    DOCK_PS_RDY_SENT,     /* told it the sink is ready                    */
    DOCK_DR_SWAP_SENT,    /* asked to become the data host                */
    DOCK_DISC_ID_SENT,    /* who is on the other end                      */
    DOCK_DISC_ID_ACC,
    DOCK_NIN_SENT,        /* a Nintendo dock: its own mode                */
    DOCK_NIN_ACC,
    DOCK_SVID_SENT,       /* anything else: does it do DisplayPort        */
    DOCK_SVID_ACC,
    DOCK_MODE_SENT,
    DOCK_MODE_ACC,
    DOCK_DP_ENTER_SENT,   /* DisplayPort alt mode                         */
    DOCK_DP_CFG_SENT,     /* lanes and pins, and hot plug detect from here */
    DOCK_DP_HPD_WAIT,     /* a moment for the TV's hot plug detect         */
    DOCK_QUERY_SENT,      /* the dock's state                             */
    DOCK_QUERY_ACC,
    DOCK_LED_SENT,        /* its light                                    */
    DOCK_LED_ACC,
    DOCK_HUB_SENT,        /* its USB hub                                  */
    DOCK_HUB_ACC,
    DOCK_DONE,
    DOCK_FAILED,
} dock_state_t;

typedef struct {
    dock_state_t state;
    u32  since;               /* when the state was entered, ms           */
    u32  tries;
    u16  alerts;              /* alert bits seen and not yet consumed     */
    u16  s1, s2, dp;          /* the last status registers                */
    /* what was found */
    bool plugged, plug_seen;
    u32  pdo_n, pdo_mv[7], pdo_ma[7];
    u32  pick;                /* the profile asked for, 1-based           */
    bool dock_power;          /* the dock's 5 V 0.5 A then 15 V offer      */
    u32  req_ma;              /* asked for                                */
    u32  got_mv, got_ma;      /* the contract                             */
    u32  role;                /* PD_ROLE_*, once settled                  */
    u16  vid, pid;
    u32  vdo_type;
    bool nin_dock, nin_charger;
    bool nin_mode;            /* the dock's own mode entered              */
    bool dp_on;               /* DisplayPort mode asked for: left on stop */
    u8   dp_caps[4];          /* the DisplayPort capabilities VDO         */
    u32  lanes;               /* 2 or 4, as configured                    */
    u16  dp_status;
    bool hpd;
    u8   dock_state[4];
    bool dock_old_fw;
    bool led_lit, hub_up;
    dock_state_t failed_state; /* the step, when DOCK_FAILED              */
    const char *failed_at;    /* its name                                 */
    char fail[96];            /* and why                                  */
} dock_t;

void dock_start(dock_t *d);
void dock_step(dock_t *d);    /* every 40 ms or so                       */
void dock_stop(dock_t *d);    /* light off, DisplayPort mode left        */
const char *dock_state_name(dock_state_t s);

#endif
