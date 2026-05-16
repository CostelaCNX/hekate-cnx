/*
 * hekate Portuguese (Brazil) string table.
 *
 * This file starts the bootloader i18n surface. Existing literal strings will be
 * moved here incrementally so behavior can stay stable while the fork evolves.
 */

#ifndef STRINGS_PTBR_H
#define STRINGS_PTBR_H

typedef enum
{
	MENU_STR_HEKATE,
	MENU_STR_LAUNCH,
	MENU_STR_TOOLS,
	MENU_STR_CONSOLE_INFO,
	MENU_STR_RELOAD,
	MENU_STR_LOAD_GUI,
	MENU_STR_REBOOT_OFW,
	MENU_STR_REBOOT_RCM,
	MENU_STR_POWER_OFF,
	MENU_STR_ABOUT,
	MENU_STR_COUNT
} menu_str_id_t;

const char *menu_str_get(menu_str_id_t id, const char *fallback);

#define MENU_STR(id, fallback) menu_str_get((id), (fallback))

#endif
