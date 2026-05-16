#include "strings_ptbr.h"

static const char *const _nyx_str_ptbr[NYX_STR_COUNT] = {
	[NYX_STR_HEKATE]      = "hekate",
	[NYX_STR_HOME]        = "Início",
	[NYX_STR_TOOLS]       = "Ferramentas",
	[NYX_STR_CONSOLE_INFO]= "Info do console",
	[NYX_STR_OPTIONS]     = "Opções",
	[NYX_STR_POWER_OFF]   = "Desligar",
	[NYX_STR_REBOOT]      = "Reiniciar",
	[NYX_STR_RELOAD]      = "Recarregar",
};

const char *nyx_str_get(nyx_str_id_t id, const char *fallback)
{
	if ((id < NYX_STR_COUNT) && _nyx_str_ptbr[id])
		return _nyx_str_ptbr[id];

	return fallback;
}
