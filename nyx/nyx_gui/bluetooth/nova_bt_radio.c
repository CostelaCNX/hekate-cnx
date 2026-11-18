/* Nova Bluetooth UART-D platform. Hardware sequence follows the existing
 * HWTest radio probe; PH0 (OLED panel IRQ) is never touched. */
#include "nova_bt.h"
#include <soc/clock.h>
#include <soc/gpio.h>
#include <soc/pinmux.h>
#include <soc/uart.h>
#include <soc/t210.h>
#include <soc/timer.h>
#include <utils/util.h>
#include <soc/i2c.h>
#include <power/max7762x.h>
#include <power/max77620.h>

#define U ((volatile uart_t *)(UART_BASE+0x300))
#define HMASK (BIT(1)|BIT(3)|BIT(4)|BIT(5)|BIT(7))
/* Same RX trigger encoding as the existing HWTest UART-D radio probe. */
#define RX_TRIGGER (2u<<6) /* HWTest GUI host: 8-byte receive watermark. */
static const u32 pads[]={0x1b8,0x1c0,0x1c4,0x1c8,PINMUX_AUX_AP_WAKE_NFC,
    PINMUX_AUX_GPIO_PK3-12,PINMUX_AUX_GPIO_PK3-8,PINMUX_AUX_GPIO_PK3-4,
    PINMUX_AUX_UARTX_TX(UART_D),PINMUX_AUX_UARTX_RX(UART_D),
    PINMUX_AUX_UARTX_RTS(UART_D),PINMUX_AUX_UARTX_CTS(UART_D)};
static struct { int active;u32 pm[12],h[3],k[2],i,source,reset;u8 rails[2],ame,c32; } saved;
static unsigned tx_room;
/* Masked GPIO aliases do not overwrite neighbouring display/TV pins. */
static void masked(u32 alias,u32 mask,u32 value)
{ GPIO(alias)=(mask<<8)|(value&mask); (void)GPIO(alias); }
uint32_t nova_bt_now(void){return get_tmr_ms();}
uint32_t nova_bt_now_us(void){return get_tmr_us();}
static void pmic_restore(u32 reg,u8 mask,u8 value)
{u8 current=i2c_recv_byte(I2C_5,MAX77620_I2C_ADDR,reg);i2c_send_byte(I2C_5,MAX77620_I2C_ADDR,reg,(current&~mask)|(value&mask));}
int nova_bt_radio_open(void)
{
    if(saved.active)return 0;
    /* UART-D belongs exclusively to this service. Never reset another owner. */
    if(CLOCK(CLK_RST_CONTROLLER_CLK_OUT_ENB_U)&BIT(CLK_U_UARTD))return 0;
    saved.source=CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_UARTD);
    saved.reset=CLOCK(CLK_RST_CONTROLLER_RST_DEVICES_U)&BIT(CLK_U_UARTD);
    for(unsigned j=0;j<12;j++)saved.pm[j]=PINMUX_AUX(pads[j]);
    saved.h[0]=GPIO(0x10c);saved.h[1]=GPIO(0x11c);saved.h[2]=GPIO(0x12c);
    saved.k[0]=GPIO(0x208);saved.k[1]=GPIO(0x218);saved.i=GPIO(0x200);
    saved.active=1;tx_room=0;
    /* Same supplies/reference clock as HWTest-RCM's GUI Bluetooth host.
     * Retain voltage settings; restore only the control bits we own. */
    saved.rails[0]=i2c_recv_byte(I2C_5,MAX77620_I2C_ADDR,MAX77620_REG_LDO1_CFG);
    saved.rails[1]=i2c_recv_byte(I2C_5,MAX77620_I2C_ADDR,MAX77620_REG_LDO7_CFG);
    max7762x_regulator_enable(REGULATOR_LDO1,true);msleep(5);
    max7762x_regulator_enable(REGULATOR_LDO7,true);msleep(5);
    saved.ame=i2c_recv_byte(I2C_5,MAX77620_I2C_ADDR,MAX77620_REG_AME_GPIO);
    saved.c32=i2c_recv_byte(I2C_5,MAX77620_I2C_ADDR,MAX77620_REG_CNFG1_32K);
    i2c_send_byte(I2C_5,MAX77620_I2C_ADDR,MAX77620_REG_AME_GPIO,saved.ame|BIT(4));
    i2c_send_byte(I2C_5,MAX77620_I2C_ADDR,MAX77620_REG_CNFG1_32K,saved.c32|MAX77620_CNFG1_32K_OUT0_EN);msleep(10);
    PINMUX_AUX(0x1c8)=PINMUX_INPUT_ENABLE|PINMUX_TRISTATE;
    masked(0x18c,BIT(5),BIT(5));masked(0x19c,BIT(5),0);
    PINMUX_AUX(0x1c0)=PINMUX_INPUT_ENABLE|PINMUX_PULL_DOWN;
    PINMUX_AUX(PINMUX_AUX_AP_WAKE_NFC)=PINMUX_INPUT_ENABLE;
    PINMUX_AUX(0x1c4)=PINMUX_INPUT_ENABLE|PINMUX_PULL_DOWN;
    PINMUX_AUX(0x1b8)=PINMUX_INPUT_ENABLE|PINMUX_PULL_DOWN;
    masked(0x1ac,HMASK&~BIT(5),0);
    masked(0x18c,HMASK,HMASK);masked(0x19c,HMASK&~BIT(5),HMASK&~BIT(5));
    for(unsigned j=5;j<8;j++)PINMUX_AUX(pads[j])=2|PINMUX_INPUT_ENABLE|(j<7?PINMUX_PULL_DOWN:0);
    masked(0x288,7,7);masked(0x298,7,0);
    PINMUX_AUX(pads[8])=0;PINMUX_AUX(pads[9])=PINMUX_INPUT_ENABLE|PINMUX_TRISTATE|PINMUX_PULL_DOWN;
    PINMUX_AUX(pads[10])=0;PINMUX_AUX(pads[11])=PINMUX_INPUT_ENABLE|PINMUX_TRISTATE|PINMUX_PULL_UP;
    masked(0x280,0xf0,0);
    clock_enable_uart(UART_D);
    u32 divisor=clock_uart_use_src_div(UART_D,115200)?(8*115200+408000000)/(16*115200):1;
    U->UART_IER_DLAB=0;U->UART_LCR=UART_LCR_DLAB|UART_LCR_WORD_LENGTH_8;
    U->UART_THR_DLAB=divisor&255;U->UART_IER_DLAB=divisor>>8;U->UART_LCR=UART_LCR_WORD_LENGTH_8;
    U->UART_MCR=0;U->UART_IIR_FCR=RX_TRIGGER|UART_IIR_FCR_EN_FIFO|UART_IIR_FCR_RX_CLR|UART_IIR_FCR_TX_CLR;
    /* Both REG_ON low for 20 ms; no port-H writes during the 200 ms POR. */
    msleep(20);masked(0x1ac,BIT(4),BIT(4));msleep(200);
    /* Match HWTest-RCM _wl_reg_on_restore after the shared cold reset.
     * Keep the companion block out of reset while BT owns the radio; close
     * restores the exact previous GPIO state. No Wi-Fi stack is started. */
    masked(0x1ac,BIT(1),BIT(1));
    PINMUX_AUX(pads[9])=PINMUX_INPUT_ENABLE|PINMUX_TRISTATE|PINMUX_PULL_UP;
    U->UART_IIR_FCR=RX_TRIGGER|UART_IIR_FCR_EN_FIFO|UART_IIR_FCR_RX_CLR|UART_IIR_FCR_TX_CLR;
    (void)U->UART_LSR;
    /* Hardware RTS throttles RX while Nyx draws a frame; CTS throttles TX.
     * No busy wait for CTS, and no per-byte RX trigger for a polled host. */
    U->UART_MCR=UART_MCR_RTS_EN|UART_MCR_CTS_EN;
    return 1;
}
void nova_bt_radio_close(void)
{
    if(!saved.active)return;
    U->UART_MCR=0;U->UART_IER_DLAB=0;
    masked(0x1ac,BIT(4),0); /* stop the controller before host state is discarded */
    clock_disable_uart(UART_D);
    CLOCK(CLK_RST_CONTROLLER_CLK_SOURCE_UARTD)=saved.source;
    if(!saved.reset)CLOCK(CLK_RST_CONTROLLER_RST_DEV_U_CLR)=BIT(CLK_U_UARTD);
    masked(0x19c,HMASK,0);
    masked(0x1ac,HMASK,saved.h[2]);masked(0x18c,HMASK,saved.h[0]);masked(0x19c,HMASK,saved.h[1]);
    masked(0x288,7,saved.k[0]);masked(0x298,7,saved.k[1]);masked(0x280,0xf0,saved.i);
    for(unsigned j=0;j<12;j++)PINMUX_AUX(pads[j])=saved.pm[j];
    pmic_restore(MAX77620_REG_LDO1_CFG,0xc0,saved.rails[0]);
    pmic_restore(MAX77620_REG_LDO7_CFG,0xc0,saved.rails[1]);
    pmic_restore(MAX77620_REG_AME_GPIO,BIT(4),saved.ame);
    pmic_restore(MAX77620_REG_CNFG1_32K,MAX77620_CNFG1_32K_OUT0_EN,saved.c32);
    saved.active=0;tx_room=0;
}
int nova_bt_radio_read(void)
{
    if(!saved.active)return -1;
    u32 flags=U->UART_LSR;
    if(flags&0x1e)return -2;
    return flags&UART_LSR_RDR?(int)(U->UART_THR_DLAB&255):-1;
}
int nova_bt_radio_write(uint8_t value)
{
    if(!saved.active||!(U->UART_MSR&BIT(4)))return 0;
    /* A THRE sample permits a conservative 16-byte burst with the default
     * Tegra TX trigger. Checking THRE after every byte wastes FIFO space
     * and can make long HCI commands time out on a polled host. */
    if(!tx_room){if(!(U->UART_LSR&UART_LSR_THRE))return 0;tx_room=16;}
    U->UART_THR_DLAB=value;tx_room--;return 1;
}
