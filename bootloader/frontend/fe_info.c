/*
 * Copyright (c) 2018 naehrwert
 * Copyright (c) 2018-2025 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <bdk.h>

#include "fe_info.h"
#include "../config.h"
#include "../hos/hos.h"
#include "../hos/pkg1.h"
#include <libs/fatfs/ff.h>

#pragma GCC push_options
#pragma GCC optimize ("Os")

void print_fuseinfo()
{
	u32 fuse_size = h_cfg.t210b01 ? 0x368 : 0x300;
	u32 fuse_address = h_cfg.t210b01 ? 0x7000F898 : 0x7000F900;

	gfx_clear_partial_grey(0x1B, 0, 1256);
	gfx_con_setpos(0, 0);

	gfx_printf("\nSKU:         %X - ", FUSE(FUSE_SKU_INFO));
	switch (h_cfg.devmode)
	{
	case FUSE_NX_HW_STATE_PROD:
		gfx_printf("Retail\n");
		break;
	case FUSE_NX_HW_STATE_DEV:
		gfx_printf("Dev\n");
		break;
	}
	gfx_printf("ID Sdram:    %d\n", fuse_read_dramid(true));
	gfx_printf("Fuses queim.: %d / 64\n", bit_count(fuse_read_odm(7)));
	gfx_printf("Chave segura:%08X%08X%08X%08X\n\n\n",
		byte_swap_32(FUSE(FUSE_PRIVATE_KEY0)), byte_swap_32(FUSE(FUSE_PRIVATE_KEY1)),
		byte_swap_32(FUSE(FUSE_PRIVATE_KEY2)), byte_swap_32(FUSE(FUSE_PRIVATE_KEY3)));

	gfx_printf("%kCache de fuses:\n\n%k", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);
	gfx_hexdump(fuse_address, (u8 *)fuse_address, fuse_size);

	btn_wait();
}

void print_mmc_info()
{
	gfx_clear_partial_grey(0x1B, 0, 1256);
	gfx_con_setpos(0, 0);

	static const u32 SECTORS_TO_MIB_COEFF = 11;

	if (emmc_initialize(false))
	{
		EPRINTF("Falha ao iniciar eMMC.");
		goto out;
	}
	else
	{
		u16 card_type;
		u32 speed = 0;

		gfx_printf("%kCID:%k\n", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);
		switch (emmc_storage.csd.mmca_vsn)
		{
		case 2: /* MMC v2.0 - v2.2 */
		case 3: /* MMC v3.1 - v3.3 */
		case 4: /* MMC v4 */
			gfx_printf(
				" Vendor ID:  %X\n"
				" OEM ID:     %02X\n"
				" Model:      %c%c%c%c%c%c\n"
				" Prd Rev:    %X\n"
				" S/N:        %04X\n"
				" Month/Year: %02d/%04d\n\n",
				emmc_storage.cid.manfid, emmc_storage.cid.oemid,
				emmc_storage.cid.prod_name[0], emmc_storage.cid.prod_name[1], emmc_storage.cid.prod_name[2],
				emmc_storage.cid.prod_name[3], emmc_storage.cid.prod_name[4],	emmc_storage.cid.prod_name[5],
				emmc_storage.cid.prv, emmc_storage.cid.serial, emmc_storage.cid.month, emmc_storage.cid.year);
			break;
		default:
			break;
		}

		if (emmc_storage.csd.structure == 0)
			EPRINTF("Estrutura CSD desconhecida.");
		else
		{
			gfx_printf("%kExtended CSD V1.%d:%k\n",
				TXT_CLR_CYAN_L, emmc_storage.ext_csd.ext_struct, TXT_CLR_DEFAULT);
			card_type = emmc_storage.ext_csd.card_type;
			char card_type_support[96];
			card_type_support[0] = 0;
			if (card_type & EXT_CSD_CARD_TYPE_HS_26)
			{
				strcat(card_type_support, "HS26");
				speed = (26 << 16) | 26;
			}
			if (card_type & EXT_CSD_CARD_TYPE_HS_52)
			{
				strcat(card_type_support, ", HS52");
				speed = (52 << 16) | 52;
			}
			if (card_type & EXT_CSD_CARD_TYPE_DDR_1_8V)
			{
				strcat(card_type_support, ", DDR52_1.8V");
				speed = (52 << 16) | 104;
			}
			if (card_type & EXT_CSD_CARD_TYPE_HS200_1_8V)
			{
				strcat(card_type_support, ", HS200_1.8V");
				speed = (200 << 16) | 200;
			}
			if (card_type & EXT_CSD_CARD_TYPE_HS400_1_8V)
			{
				strcat(card_type_support, ", HS400_1.8V");
				speed = (200 << 16) | 400;
			}

			gfx_printf(
				" Versao spec:   %02X\n"
				" Rev. estend.:  1.%d\n"
				" Versao disp.:  %d\n"
				" Classes cmd:   %02X\n"
				" Capacidade:    %s\n"
				" Taxa max.:     %d MB/s (%d MHz)\n"
				" Taxa atual:    %d MB/s\n"
				" Tipos suport.: ",
				emmc_storage.csd.mmca_vsn, emmc_storage.ext_csd.rev, emmc_storage.ext_csd.dev_version, emmc_storage.csd.cmdclass,
				emmc_storage.csd.capacity == (4096 * EMMC_BLOCKSIZE) ? "Alta" : "Baixa", speed & 0xFFFF, (speed >> 16) & 0xFFFF,
				emmc_storage.csd.busspeed);
			gfx_con.fntsz = 8;
			gfx_printf("%s", card_type_support);
			gfx_con.fntsz = 16;
			gfx_printf("\n\n", card_type_support);

			u32 boot_size = emmc_storage.ext_csd.boot_mult << 17;
			u32 rpmb_size = emmc_storage.ext_csd.rpmb_mult << 17;
			gfx_printf("%kParticoes eMMC:%k\n", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);
			gfx_printf(" 1: %kBOOT0      %k\n    Tam.: %5d KiB (Setores LBA: 0x%07X)\n", TXT_CLR_GREENISH, TXT_CLR_DEFAULT,
				boot_size / 1024, boot_size / EMMC_BLOCKSIZE);
			gfx_put_small_sep();
			gfx_printf(" 2: %kBOOT1      %k\n    Tam.: %5d KiB (Setores LBA: 0x%07X)\n", TXT_CLR_GREENISH, TXT_CLR_DEFAULT,
				boot_size / 1024, boot_size / EMMC_BLOCKSIZE);
			gfx_put_small_sep();
			gfx_printf(" 3: %kRPMB       %k\n    Tam.: %5d KiB (Setores LBA: 0x%07X)\n", TXT_CLR_GREENISH, TXT_CLR_DEFAULT,
				rpmb_size / 1024, rpmb_size / EMMC_BLOCKSIZE);
			gfx_put_small_sep();
			gfx_printf(" 0: %kGPP (USER) %k\n    Tam.: %5d MiB (Setores LBA: 0x%07X)\n\n", TXT_CLR_GREENISH, TXT_CLR_DEFAULT,
				emmc_storage.sec_cnt >> SECTORS_TO_MIB_COEFF, emmc_storage.sec_cnt);
			gfx_put_small_sep();
			gfx_printf("%kTabela particoes GPP (eMMC USER):%k\n", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);

			emmc_set_partition(EMMC_GPP);
			LIST_INIT(gpt);
			emmc_gpt_parse(&gpt);
			int gpp_idx = 0;
			LIST_FOREACH_ENTRY(emmc_part_t, part, &gpt, link)
			{
				gfx_printf(" %02d: %k%s%k\n     Tam.: % 5d MiB (Setores LBA 0x%07X)\n     Faixa LBA: %08X-%08X\n",
					gpp_idx++, TXT_CLR_GREENISH, part->name, TXT_CLR_DEFAULT, (part->lba_end - part->lba_start + 1) >> SECTORS_TO_MIB_COEFF,
					part->lba_end - part->lba_start + 1, part->lba_start, part->lba_end);
				gfx_put_small_sep();
			}
			emmc_gpt_free(&gpt);
		}
	}

out:
	emmc_end();

	btn_wait();
}

void print_sdcard_info()
{
	static const u32 SECTORS_TO_MIB_COEFF = 11;

	gfx_clear_partial_grey(0x1B, 0, 1256);
	gfx_con_setpos(0, 0);

	if (!sd_initialize(false))
	{
		gfx_printf("%kIdentificacao do cartao:%k\n", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);
		gfx_printf(
			" ID vendor:  %02x\n"
			" OEM ID:     %c%c\n"
			" Modelo:     %c%c%c%c%c\n"
			" HW rev:     %X\n"
			" FW rev:     %X\n"
			" S/N:        %08x\n"
			" Mes/Ano:    %02d/%04d\n\n",
			sd_storage.cid.manfid, (sd_storage.cid.oemid >> 8) & 0xFF, sd_storage.cid.oemid & 0xFF,
			sd_storage.cid.prod_name[0], sd_storage.cid.prod_name[1], sd_storage.cid.prod_name[2],
			sd_storage.cid.prod_name[3], sd_storage.cid.prod_name[4],
			sd_storage.cid.hwrev, sd_storage.cid.fwrev, sd_storage.cid.serial,
			sd_storage.cid.month, sd_storage.cid.year);

		u16 *sd_errors = sd_get_error_count();
		gfx_printf("%kDados especificos do cartao V%d.0:%k\n", TXT_CLR_CYAN_L, sd_storage.csd.structure + 1, TXT_CLR_DEFAULT);
		gfx_printf(
			" Classes cmd:    %02X\n"
			" Capacidade:     %d MiB\n"
			" Larg. barram.:  %d\n"
			" Taxa atual:     %d MB/s (%d MHz)\n"
			" Classe veloc.:  %d\n"
			" Grau UHS:       U%d\n"
			" Classe video:   V%d\n"
			" Classe app:     A%d\n"
			" Prot. escrita:  %d\n"
			" Erros SDMMC:    %d %d %d\n\n",
			sd_storage.csd.cmdclass, sd_storage.sec_cnt >> 11,
			sd_storage.ssr.bus_width, sd_storage.csd.busspeed, sd_storage.csd.busspeed * 2,
			sd_storage.ssr.speed_class, sd_storage.ssr.uhs_grade, sd_storage.ssr.video_class,
			sd_storage.ssr.app_class, sd_storage.csd.write_protect,
			sd_errors[0], sd_errors[1], sd_errors[2]); // SD_ERROR_INIT_FAIL, SD_ERROR_RW_FAIL, SD_ERROR_RW_RETRY.

		int res = f_mount(&sd_fs, "", 1);
		if (!res)
		{
			gfx_puts("Obtendo info do volume FAT...\n\n");
			gfx_printf("%kVolume %s encontrado:%k\n Livre:   %d MiB\n Cluster: %d KiB\n",
					TXT_CLR_CYAN_L, sd_fs.fs_type == FS_EXFAT ? "exFAT" : "FAT32", TXT_CLR_DEFAULT,
					sd_fs.free_clst * sd_fs.csize >> SECTORS_TO_MIB_COEFF, (sd_fs.csize > 1) ? (sd_fs.csize >> 1) : 512);
			f_unmount("");
		}
		else
		{
			EPRINTFARGS("Falha ao montar SD (erro FatFS %d).\n"
				"Verifique se existe uma particao FAT..", res);
		}

		sd_end();
	}
	else
	{
		EPRINTF("Falha ao iniciar cartao SD.");
		if (!sdmmc_get_sd_inserted())
			EPRINTF("Verifique se ele esta inserido.");
		else
			EPRINTF("Leitor SD nao esta bem encaixado!");
		sd_end();
	}

	btn_wait();
}

void print_fuel_gauge_info()
{
	int value = 0;

	gfx_printf("%kInfo Fuel Gauge:\n%k", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);

	max17050_get_property(MAX17050_RepSOC, &value);
	gfx_printf("Carga agora:            %3d%\n", value >> 8);

	max17050_get_property(MAX17050_RepCap, &value);
	gfx_printf("Capacidade agora:       %4d mAh\n", value);

	max17050_get_property(MAX17050_FullCAP, &value);
	gfx_printf("Capacidade cheia:       %4d mAh\n", value);

	max17050_get_property(MAX17050_DesignCap, &value);
	gfx_printf("Capacidade projeto:     %4d mAh\n", value);

	max17050_get_property(MAX17050_Current, &value);
	gfx_printf("Corrente agora:         %d mA\n", value / 1000);

	max17050_get_property(MAX17050_AvgCurrent, &value);
	gfx_printf("Corrente media:         %d mA\n", value / 1000);

	max17050_get_property(MAX17050_VCELL, &value);
	gfx_printf("Tensao agora:           %4d mV\n", value);

	max17050_get_property(MAX17050_OCVInternal, &value);
	gfx_printf("Tensao circuito aberto: %4d mV\n", value);

	max17050_get_property(MAX17050_MinVolt, &value);
	gfx_printf("Tensao min atingida:    %4d mV\n", value);

	max17050_get_property(MAX17050_MaxVolt, &value);
	gfx_printf("Tensao max atingida:    %4d mV\n", value);

	max17050_get_property(MAX17050_V_empty, &value);
	gfx_printf("Tensao vazia projeto:   %4d mV\n", value);

	max17050_get_property(MAX17050_TEMP, &value);
	gfx_printf("Temperatura bateria:    %d.%d oC\n", value / 10,
			   (value >= 0 ? value : (~value)) % 10);
}

void print_battery_charger_info()
{
	int value = 0;

	gfx_printf("%k\n\nInfo carregador bateria:\n%k", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);

	bq24193_get_property(BQ24193_InputCurrentLimit, &value);
	gfx_printf("Limite corrente ent.: %4d mA\n", value);

	bq24193_get_property(BQ24193_SystemMinimumVoltage, &value);
	gfx_printf("Limite tensao sist.:  %4d mV\n", value);

	bq24193_get_property(BQ24193_FastChargeCurrentLimit, &value);
	gfx_printf("Limite corrente carga:%4d mA\n", value);

	bq24193_get_property(BQ24193_ChargeVoltageLimit, &value);
	gfx_printf("Limite tensao carga:  %4d mV\n", value);

	bq24193_get_property(BQ24193_ChargeStatus, &value);
	gfx_printf("Status de carga:      ");
	switch (value)
	{
	case 0:
		gfx_printf("Nao carregando\n");
		break;
	case 1:
		gfx_printf("Pre-carga\n");
		break;
	case 2:
		gfx_printf("Carga rapida\n");
		break;
	case 3:
		gfx_printf("Carga encerrada\n");
		break;
	default:
		gfx_printf("Desconhecido (%d)\n", value);
		break;
	}
	bq24193_get_property(BQ24193_TempStatus, &value);
	gfx_printf("Status temperatura:   ");
	switch (value)
	{
	case 0:
		gfx_printf("Normal\n");
		break;
	case 2:
		gfx_printf("Morna\n");
		break;
	case 3:
		gfx_printf("Fresca\n");
		break;
	case 5:
		gfx_printf("Fria\n");
		break;
	case 6:
		gfx_printf("Quente\n");
		break;
	default:
		gfx_printf("Desconhecido (%d)\n", value);
		break;
	}
}

void print_battery_info()
{
	gfx_clear_partial_grey(0x1B, 0, 1256);
	gfx_con_setpos(0, 0);

	print_fuel_gauge_info();

	print_battery_charger_info();

	u8 *buf = (u8 *)malloc(0x100 * 2);

	gfx_printf("%k\n\nRegs. Fuel Gauge da bateria:\n%k", TXT_CLR_CYAN_L, TXT_CLR_DEFAULT);

	for (int i = 0; i < 0x200; i += 2)
	{
		i2c_recv_buf_small(buf + i, 2, I2C_1, MAXIM17050_I2C_ADDR, i >> 1);
		usleep(2500);
	}

	gfx_hexdump(0, (u8 *)buf, 0x200);

	btn_wait();
}

#pragma GCC pop_options
