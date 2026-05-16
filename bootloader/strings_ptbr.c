#include "strings_ptbr.h"

static const char *const _menu_str_ptbr[MENU_STR_COUNT] = {
	[MENU_STR_HEKATE]      = "hekate",
	[MENU_STR_LAUNCH]      = "Iniciar",
	[MENU_STR_TOOLS]       = "Ferramentas",
	[MENU_STR_CONSOLE_INFO]= "Info do console",
	[MENU_STR_RELOAD]      = "Recarregar",
	[MENU_STR_LOAD_GUI]    = "Abrir interface",
	[MENU_STR_REBOOT_OFW]  = "Reiniciar (OFW)",
	[MENU_STR_REBOOT_RCM]  = "Reiniciar (RCM)",
	[MENU_STR_POWER_OFF]   = "Desligar",
	[MENU_STR_ABOUT]       = "Sobre",
};

const char *menu_str_get(menu_str_id_t id, const char *fallback)
{
	if ((id < MENU_STR_COUNT) && _menu_str_ptbr[id])
		return _menu_str_ptbr[id];

	return fallback;
}
