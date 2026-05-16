/*
 * hekate Portuguese (Brazil) string table for Nyx/LVGL.
 */

#ifndef STRINGS_PTBR_H
#define STRINGS_PTBR_H

typedef enum
{
	NYX_STR_HEKATE,
	NYX_STR_HOME,
	NYX_STR_TOOLS,
	NYX_STR_CONSOLE_INFO,
	NYX_STR_OPTIONS,
	NYX_STR_POWER_OFF,
	NYX_STR_REBOOT,
	NYX_STR_RELOAD,
	NYX_STR_COUNT
} nyx_str_id_t;

const char *nyx_str_get(nyx_str_id_t id, const char *fallback);

#define MENU_STR(id, fallback) nyx_str_get((id), (fallback))

#endif
