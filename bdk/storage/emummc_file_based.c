#include "emummc_file_based.h"
#include <string.h>
#include <stdlib.h>
#include <libs/fatfs/ff.h>
#include <gfx_utils.h>

static FIL  active_file;
static s32  active_file_idx; // -0xff: none, -1: boot0, -2: boot1, 0+: gpp
static char file_based_base_path[0x80];
static u32  file_part_sz_sct;
static u32  active_part;
static u32  file_based_base_path_len;

static int _emummc_storage_file_based_read_write_single(u32 sector, u32 num_sectors, void *buf, bool is_write)
{
#if FF_FS_READONLY == 1
	if (is_write)
		return FR_WRITE_PROTECTED;
#endif
	int res = f_lseek(&active_file, (u64)sector << 9);
	if (res != FR_OK)
		return res;

	if (is_write)
		res = f_write(&active_file, buf, (u64)num_sectors << 9, NULL);
	else
		res = f_read(&active_file, buf, (u64)num_sectors << 9, NULL);

	return res;
}

static int _emummc_storage_file_based_change_file(const char *name, s32 idx)
{
	if (active_file_idx == idx)
		return FR_OK;

	if (active_file_idx != -0xff) {
		f_close(&active_file);
		active_file_idx = -0xff;
	}

	strcpy(file_based_base_path + file_based_base_path_len, name);

#if FF_FS_READONLY == 1
	int res = f_open(&active_file, file_based_base_path, FA_READ);
#else
	int res = f_open(&active_file, file_based_base_path, FA_READ | FA_WRITE);
#endif
	if (res != FR_OK)
		return res;

	active_file_idx = idx;
	return FR_OK;
}

static int _emummc_storage_file_based_read_write(u32 sector, u32 num_sectors, void *buf, bool is_write)
{
#if FF_FS_READONLY == 1
	if (is_write)
		return 0;
#endif
	if (file_part_sz_sct == 0)
		return 0;

	int res = FR_OK;

	if (active_part == 1) {
		// BOOT0
		if (_emummc_storage_file_based_change_file("BOOT0", -1) != FR_OK)
			return 0;
		res = _emummc_storage_file_based_read_write_single(sector, num_sectors, buf, is_write);
		if (res != FR_OK)
			return 0;
		f_sync(&active_file);
		return 1;
	} else if (active_part == 2) {
		// BOOT1
		if (_emummc_storage_file_based_change_file("BOOT1", -2) != FR_OK)
			return 0;
		res = _emummc_storage_file_based_read_write_single(sector, num_sectors, buf, is_write);
		if (res != FR_OK)
			return 0;
		f_sync(&active_file);
		return 1;
	} else if (active_part == 0) {
		// GPP — may span multiple numbered files
		u32 scts_left = num_sectors;
		u32 cur_sct   = sector;

		while (scts_left) {
			u32 offset   = cur_sct % file_part_sz_sct;
			u32 sct_cnt  = MIN(file_part_sz_sct - offset, scts_left);
			u32 file_idx = cur_sct / file_part_sz_sct;

			if ((s32)file_idx != active_file_idx) {
				char name[3] = "";
				if (file_idx < 10)
					strcpy(name, "0");
				itoa(file_idx, name + strlen(name), 10);
				if (_emummc_storage_file_based_change_file(name, file_idx) != FR_OK)
					return 0;
			}

			res = _emummc_storage_file_based_read_write_single(
				offset, sct_cnt,
				buf + ((u64)(num_sectors - scts_left) << 9),
				is_write);
			if (res != FR_OK)
				return 0;

			cur_sct   += sct_cnt;
			scts_left -= sct_cnt;
		}
		f_sync(&active_file);
		return 1;
	}

	return 0;
}

int emummc_storage_file_base_set_partition(u32 partition)
{
	active_part = partition;
	return 1;
}

int emummc_storage_file_based_init(const char *path)
{
	strcpy(file_based_base_path, path);
	file_based_base_path_len = strlen(file_based_base_path);

	// probe the first GPP part file to determine split size
	strcat(file_based_base_path + file_based_base_path_len, "00");

	active_part = 0;
	FILINFO fi;
	if (f_stat(file_based_base_path, &fi) != FR_OK)
		return 0;

	file_part_sz_sct = fi.fsize ? fi.fsize >> 9 : 0;
	active_file_idx  = -0xff;
	return 1;
}

void emummc_storage_file_based_end()
{
	if (active_file_idx != -0xff)
		f_close(&active_file);

	active_file_idx = -0xff;
	file_based_base_path[0] = '\0';
	file_part_sz_sct = 0;
}

#if FF_FS_READONLY == 0
int emummc_storage_file_based_write(u32 sector, u32 num_sectors, void *buf)
{
	return _emummc_storage_file_based_read_write(sector, num_sectors, buf, true);
}
#endif

int emummc_storage_file_based_read(u32 sector, u32 num_sectors, void *buf)
{
	return _emummc_storage_file_based_read_write(sector, num_sectors, buf, false);
}

u32 emummc_storage_file_based_get_total_gpp_size(const char *path)
{
	u32 path_len = strlen(path);
	u32 total    = 0;
	char file_path[0x80];

	strcpy(file_path, path);
	strcpy(file_path + path_len, "00");

	FILINFO fi;
	u32 cur_idx = 0;
	int res = f_stat(file_path, &fi);

	while (res == FR_OK) {
		total += fi.fsize >> 9;
		cur_idx++;

		char name[3] = "0";
		if (cur_idx >= 10)
			name[0] = '\0';
		itoa(cur_idx, name + strlen(name), 10);
		strcpy(file_path + path_len, name);
		res = f_stat(file_path, &fi);
	}

	return total;
}
