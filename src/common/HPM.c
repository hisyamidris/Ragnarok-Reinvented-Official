// Copyright (c) Hercules Dev Team, licensed under GNU GPL.
// See the LICENSE file

#define HERCULES_CORE

#include "config/core.h" // CONSOLE_INPUT
#include "HPM.h"

#include "common/cbasetypes.h"
#include "common/conf.h"
#include "common/console.h"
#include "common/core.h"
#include "common/db.h"
#include "common/malloc.h"
#include "common/mapindex.h"
#include "common/mmo.h"
#include "common/showmsg.h"
#include "common/socket.h"
#include "common/sql.h"
#include "common/strlib.h"
#include "common/sysinfo.h"
#include "common/timer.h"
#include "common/utils.h"
#include "common/nullpo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32
#	include <unistd.h>
#endif

struct malloc_interface iMalloc_HPM;
struct malloc_interface *HPMiMalloc;
struct HPM_interface HPM_s;
struct HPM_interface *HPM;

/**
 * (char*) data name -> (unsigned int) HPMDataCheck[] index
 **/
DBMap *datacheck_db;
int datacheck_version;
const struct s_HPMDataCheck *datacheck_data;

/**
 * Executes an event on all loaded plugins.
 *
 * @param type The event type to trigger.
 */
void hplugin_trigger_event(enum hp_event_types type)
{
	unsigned int i;
	for (i = 0; i < VECTOR_LENGTH(HPM->plugins); i++) {
		struct hplugin *plugin = VECTOR_INDEX(HPM->plugins, i);
		if (plugin->hpi->event[type] != NULL)
			plugin->hpi->event[type]();
	}
}

/**
 * Exports a symbol to the shared symbols list.
 *
 * @param value The symbol value.
 * @param name  The symbol name.
 */
void hplugin_export_symbol(void *value, const char *name)
{
	struct hpm_symbol *symbol = NULL;
	CREATE(symbol ,struct hpm_symbol, 1);
	symbol->name = name;
	symbol->ptr = value;
	VECTOR_ENSURE(HPM->symbols, 1, 1);
	VECTOR_PUSH(HPM->symbols, symbol);
}

/**
 * Imports a shared symbol.
 *
 * @param name The symbol name.
 * @param pID  The requesting plugin ID.
 * @return The symbol value.
 * @retval NULL if the symbol wasn't found.
 */
void *hplugin_import_symbol(char *name, unsigned int pID)
{
	int i;
	ARR_FIND(0, VECTOR_LENGTH(HPM->symbols), i, strcmp(VECTOR_INDEX(HPM->symbols, i)->name, name) == 0);

	if (i != VECTOR_LENGTH(HPM->symbols))
		return VECTOR_INDEX(HPM->symbols, i)->ptr;

	ShowError("HPM:get_symbol:%s: '"CL_WHITE"%s"CL_RESET"' not found!\n",HPM->pid2name(pID),name);
	return NULL;
}

bool hplugin_iscompatible(char* version) {
	unsigned int req_major = 0, req_minor = 0;

	if( version == NULL )
		return false;

	sscanf(version, "%u.%u", &req_major, &req_minor);

	return ( req_major == HPM->version[0] && req_minor <= HPM->version[1] ) ? true : false;
}

/**
 * Checks whether a plugin is currently loaded
 *
 * @param filename The plugin filename.
 * @retval true  if the plugin exists and is currently loaded.
 * @retval false otherwise.
 */
bool hplugin_exists(const char *filename)
{
	int i;
	for (i = 0; i < VECTOR_LENGTH(HPM->plugins); i++) {
		if (strcmpi(VECTOR_INDEX(HPM->plugins, i)->filename,filename) == 0)
			return true;
	}
	return false;
}

/**
 * Initializes the data structure for a new plugin and registers it.
 *
 * @return A (retained) pointer to the initialized data.
 */
struct hplugin *hplugin_create(void)
{
	struct hplugin *plugin = NULL;
	CREATE(plugin, struct hplugin, 1);
	plugin->idx = (int)VECTOR_LENGTH(HPM->plugins);
	plugin->filename = NULL;
	VECTOR_ENSURE(HPM->plugins, 1, 1);
	VECTOR_PUSH(HPM->plugins, plugin);
	return plugin;
}

bool hplugins_addpacket(unsigned short cmd, unsigned short length, void (*receive) (int fd), unsigned int point, unsigned int pluginID)
{
	struct HPluginPacket *packet;
	int i;

	if (point >= hpPHP_MAX) {
		ShowError("HPM->addPacket:%s: unknown point '%u' specified for packet 0x%04x (len %d)\n",HPM->pid2name(pluginID),point,cmd,length);
		return false;
	}

	for (i = 0; i < VECTOR_LENGTH(HPM->packets[point]); i++) {
		if (VECTOR_INDEX(HPM->packets[point], i).cmd == cmd ) {
			ShowError("HPM->addPacket:%s: can't add packet 0x%04x, already in use by '%s'!",
					HPM->pid2name(pluginID), cmd, HPM->pid2name(VECTOR_INDEX(HPM->packets[point], i).pluginID));
			return false;
		}
	}

	VECTOR_ENSURE(HPM->packets[point], 1, 1);
	VECTOR_PUSHZEROED(HPM->packets[point]);
	packet = &VECTOR_LAST(HPM->packets[point]);

	packet->pluginID = pluginID;
	packet->cmd = cmd;
	packet->len = length;
	packet->receive = receive;

	return true;
}

void hplugins_grabHPData(struct HPDataOperationStorage *ret, enum HPluginDataTypes type, void *ptr)
{
	/* record address */
	switch (type) {
		/* core-handled */
		case HPDT_SESSION:
			ret->HPDataSRCPtr = (void**)(&((struct socket_data *)ptr)->hdata);
			ret->hdatac = &((struct socket_data *)ptr)->hdatac;
			break;
		/* goes to sub */
		default:
			if (HPM->grabHPDataSub) {
				if (HPM->grabHPDataSub(ret,type,ptr))
					return;
				ShowError("HPM:HPM:grabHPData failed, unknown type %d!\n",type);
			} else {
				ShowError("HPM:grabHPData failed, type %d needs sub-handler!\n",type);
			}
			ret->HPDataSRCPtr = NULL;
			ret->hdatac = NULL;
			return;
	}
}

void hplugins_addToHPData(enum HPluginDataTypes type, unsigned int pluginID, void *ptr, void *data, unsigned int index, bool autofree)
{
	struct HPluginData *HPData, **HPDataSRC;
	struct HPDataOperationStorage action;
	unsigned int i, max;

	HPM->grabHPData(&action,type,ptr);

	if (action.hdatac == NULL) { /* woo it failed! */
		ShowError("HPM:addToHPData:%s: failed, type %d (%u|%u)\n",HPM->pid2name(pluginID),type,pluginID,index);
		return;
	}

	/* flag */
	HPDataSRC = *(action.HPDataSRCPtr);
	max = *(action.hdatac);

	/* duplicate check */
	for (i = 0; i < max; i++) {
		if (HPDataSRC[i]->pluginID == pluginID && HPDataSRC[i]->type == index) {
			ShowError("HPM:addToHPData:%s: error! attempting to insert duplicate struct of id %u and index %u\n",HPM->pid2name(pluginID),pluginID,index);
			return;
		}
	}

	/* HPluginData is always same size, probably better to use the ERS (with reasonable chunk size e.g. 10/25/50) */
	CREATE(HPData, struct HPluginData, 1);

	/* input */
	HPData->pluginID = pluginID;
	HPData->type = index;
	HPData->flag.free = autofree ? 1 : 0;
	HPData->data = data;

	/* resize */
	*(action.hdatac) += 1;
	RECREATE(*(action.HPDataSRCPtr),struct HPluginData *,*(action.hdatac));

	/* RECREATE modified the address */
	HPDataSRC = *(action.HPDataSRCPtr);
	HPDataSRC[*(action.hdatac) - 1] = HPData;
}

void *hplugins_getFromHPData(enum HPluginDataTypes type, unsigned int pluginID, void *ptr, unsigned int index)
{
	struct HPDataOperationStorage action;
	struct HPluginData **HPDataSRC;
	unsigned int i, max;

	HPM->grabHPData(&action,type,ptr);

	if (action.hdatac == NULL) { /* woo it failed! */
		ShowError("HPM:getFromHPData:%s: failed, type %d (%u|%u)\n",HPM->pid2name(pluginID),type,pluginID,index);
		return NULL;
	}

	/* flag */
	HPDataSRC = *(action.HPDataSRCPtr);
	max = *(action.hdatac);

	for (i = 0; i < max; i++) {
		if (HPDataSRC[i]->pluginID == pluginID && HPDataSRC[i]->type == index)
			return HPDataSRC[i]->data;
	}

	return NULL;
}

void hplugins_removeFromHPData(enum HPluginDataTypes type, unsigned int pluginID, void *ptr, unsigned int index)
{
	struct HPDataOperationStorage action;
	struct HPluginData **HPDataSRC;
	unsigned int i, max;

	HPM->grabHPData(&action,type,ptr);

	if (action.hdatac == NULL) { /* woo it failed! */
		ShowError("HPM:removeFromHPData:%s: failed, type %d (%u|%u)\n",HPM->pid2name(pluginID),type,pluginID,index);
		return;
	}

	/* flag */
	HPDataSRC = *(action.HPDataSRCPtr);
	max = *(action.hdatac);

	for (i = 0; i < max; i++) {
		if (HPDataSRC[i]->pluginID == pluginID && HPDataSRC[i]->type == index)
			break;
	}

	if (i != max) {
		unsigned int cursor;

		aFree(HPDataSRC[i]->data);/* when its removed we delete it regardless of autofree */
		aFree(HPDataSRC[i]);
		HPDataSRC[i] = NULL;

		for (i = 0, cursor = 0; i < max; i++) {
			if (HPDataSRC[i] == NULL)
				continue;
			if (i != cursor)
				HPDataSRC[cursor] = HPDataSRC[i];
			cursor++;
		}
		*(action.hdatac) = cursor;
	}
}

/* TODO: add ability for tracking using pID for the upcoming runtime load/unload support. */
bool HPM_AddHook(enum HPluginHookType type, const char *target, void *hook, unsigned int pID)
{
	if (!HPM->hooking) {
		ShowError("HPM:AddHook Fail! '%s' tried to hook to '%s' but HPMHooking is disabled!\n",HPM->pid2name(pID),target);
		return false;
	}
	/* search if target is a known hook point within 'common' */
	/* if not check if a sub-hooking list is available (from the server) and run it by */
	if (HPM->addhook_sub && HPM->addhook_sub(type,target,hook,pID))
		return true;

	ShowError("HPM:AddHook: unknown Hooking Point '%s'!\n",target);

	return false;
}

void HPM_HookStop(const char *func, unsigned int pID)
{
	/* track? */
	HPM->force_return = true;
}

bool HPM_HookStopped (void)
{
	return HPM->force_return;
}

/**
 * Adds a plugin-defined command-line argument.
 *
 * @param pluginID  the current plugin's ID.
 * @param name      the command line argument's name, including the leading '--'.
 * @param has_param whether the command line argument expects to be followed by a value.
 * @param func      the triggered function.
 * @param help      the help string to be displayed by '--help', if any.
 * @return the success status.
 */
bool hpm_add_arg(unsigned int pluginID, char *name, bool has_param, CmdlineExecFunc func, const char *help)
{
	int i;

	if (!name || strlen(name) < 3 || name[0] != '-' || name[1] != '-') {
		ShowError("HPM:add_arg:%s invalid argument name: arguments must begin with '--' (from %s)\n", name, HPM->pid2name(pluginID));
		return false;
	}

	ARR_FIND(0, cmdline->args_data_count, i, strcmp(cmdline->args_data[i].name, name) == 0);

       if (i < cmdline->args_data_count) {
               ShowError("HPM:add_arg:%s duplicate! (from %s)\n",name,HPM->pid2name(pluginID));
               return false;
       }

       return cmdline->arg_add(pluginID, name, '\0', func, help, has_param ? CMDLINE_OPT_PARAM : CMDLINE_OPT_NORMAL);
}

bool hplugins_addconf(unsigned int pluginID, enum HPluginConfType type, char *name, void (*func) (const char *val))
{
	struct HPConfListenStorage *conf;
	unsigned int i;

	if (type >= HPCT_MAX) {
		ShowError("HPM->addConf:%s: unknown point '%u' specified for config '%s'\n",HPM->pid2name(pluginID),type,name);
		return false;
	}

	for (i = 0; i < HPM->confsc[type]; i++) {
		if (!strcmpi(name,HPM->confs[type][i].key)) {
			ShowError("HPM->addConf:%s: duplicate '%s', already in use by '%s'!",HPM->pid2name(pluginID),name,HPM->pid2name(HPM->confs[type][i].pluginID));
			return false;
		}
	}

	RECREATE(HPM->confs[type], struct HPConfListenStorage, ++HPM->confsc[type]);
	conf = &HPM->confs[type][HPM->confsc[type] - 1];

	conf->pluginID = pluginID;
	safestrncpy(conf->key, name, HPM_ADDCONF_LENGTH);
	conf->func = func;

	return true;
}

struct hplugin *hplugin_load(const char* filename) {
	struct hplugin *plugin;
	struct hplugin_info *info;
	struct HPMi_interface **HPMi;
	bool anyEvent = false;
	void **import_symbol_ref;
	int *HPMDataCheckVer;
	unsigned int *HPMDataCheckLen;
	struct s_HPMDataCheck *HPMDataCheck;
	const char *(*HPMLoadEvent)(int server_type);

	if( HPM->exists(filename) ) {
		ShowWarning("HPM:plugin_load: attempting to load duplicate '"CL_WHITE"%s"CL_RESET"', skipping...\n", filename);
		return NULL;
	}

	plugin = HPM->create();

	if (!(plugin->dll = plugin_open(filename))) {
		char buf[1024];
		ShowFatalError("HPM:plugin_load: failed to load '"CL_WHITE"%s"CL_RESET"' (error: %s)!\n", filename, plugin_geterror(buf));
		exit(EXIT_FAILURE);
	}

	if( !( info = plugin_import(plugin->dll, "pinfo",struct hplugin_info*) ) ) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'plugin_info' for '"CL_WHITE"%s"CL_RESET"'!\n", filename);
		exit(EXIT_FAILURE);
	}

	if( !(info->type & SERVER_TYPE) ) {
		HPM->unload(plugin);
		return NULL;
	}

	if( !HPM->iscompatible(info->req_version) ) {
		ShowFatalError("HPM:plugin_load: '"CL_WHITE"%s"CL_RESET"' incompatible version '%s' -> '%s'!\n", filename, info->req_version, HPM_VERSION);
		exit(EXIT_FAILURE);
	}

	plugin->info = info;
	plugin->filename = aStrdup(filename);

	if( !( import_symbol_ref = plugin_import(plugin->dll, "import_symbol",void **) ) ) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'import_symbol' for '"CL_WHITE"%s"CL_RESET"'!\n", filename);
		exit(EXIT_FAILURE);
	}

	*import_symbol_ref = HPM->import_symbol;

	if( !( HPMi = plugin_import(plugin->dll, "HPMi",struct HPMi_interface **) ) ) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'HPMi' for '"CL_WHITE"%s"CL_RESET"'!\n", filename);
		exit(EXIT_FAILURE);
	}

	if( !( *HPMi = plugin_import(plugin->dll, "HPMi_s",struct HPMi_interface *) ) ) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'HPMi_s' for '"CL_WHITE"%s"CL_RESET"'!\n", filename);
		exit(EXIT_FAILURE);
	}
	plugin->hpi = *HPMi;

	if( ( plugin->hpi->event[HPET_INIT] = plugin_import(plugin->dll, "plugin_init",void (*)(void)) ) )
		anyEvent = true;

	if( ( plugin->hpi->event[HPET_FINAL] = plugin_import(plugin->dll, "plugin_final",void (*)(void)) ) )
		anyEvent = true;

	if( ( plugin->hpi->event[HPET_READY] = plugin_import(plugin->dll, "server_online",void (*)(void)) ) )
		anyEvent = true;

	if( ( plugin->hpi->event[HPET_POST_FINAL] = plugin_import(plugin->dll, "server_post_final",void (*)(void)) ) )
		anyEvent = true;

	if( ( plugin->hpi->event[HPET_PRE_INIT] = plugin_import(plugin->dll, "server_preinit",void (*)(void)) ) )
		anyEvent = true;

	if( !anyEvent ) {
		ShowWarning("HPM:plugin_load: no events found for '"CL_WHITE"%s"CL_RESET"'!\n", filename);
		exit(EXIT_FAILURE);
	}

	if (!(HPMLoadEvent = plugin_import(plugin->dll, "HPM_shared_symbols", const char *(*)(int)))) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'HPM_shared_symbols' for '"CL_WHITE"%s"CL_RESET"', most likely not including HPMDataCheck.h!\n", filename);
		exit(EXIT_FAILURE);
	}
	{
		const char *failure = HPMLoadEvent(SERVER_TYPE);
		if (failure) {
			ShowFatalError("HPM:plugin_load: failed to import symbol '%s' into '"CL_WHITE"%s"CL_RESET"'.\n", failure, filename);
			exit(EXIT_FAILURE);
		}
	}

	if( !( HPMDataCheckLen = plugin_import(plugin->dll, "HPMDataCheckLen", unsigned int *) ) ) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'HPMDataCheckLen' for '"CL_WHITE"%s"CL_RESET"', most likely not including HPMDataCheck.h!\n", filename);
		exit(EXIT_FAILURE);
	}

	if( !( HPMDataCheckVer = plugin_import(plugin->dll, "HPMDataCheckVer", int *) ) ) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'HPMDataCheckVer' for '"CL_WHITE"%s"CL_RESET"', most likely an outdated plugin!\n", filename);
		exit(EXIT_FAILURE);
	}

	if( !( HPMDataCheck = plugin_import(plugin->dll, "HPMDataCheck", struct s_HPMDataCheck *) ) ) {
		ShowFatalError("HPM:plugin_load: failed to retrieve 'HPMDataCheck' for '"CL_WHITE"%s"CL_RESET"', most likely not including HPMDataCheck.h!\n", filename);
		exit(EXIT_FAILURE);
	}

	// TODO: Remove the HPM->DataCheck != NULL check once login and char support is complete
	if (HPM->DataCheck != NULL && !HPM->DataCheck(HPMDataCheck,*HPMDataCheckLen,*HPMDataCheckVer,plugin->info->name)) {
		ShowFatalError("HPM:plugin_load: '"CL_WHITE"%s"CL_RESET"' failed DataCheck, out of sync from the core (recompile plugin)!\n", filename);
		exit(EXIT_FAILURE);
	}

	/* id */
	plugin->hpi->pid                = plugin->idx;
	/* core */
#ifdef CONSOLE_INPUT
	plugin->hpi->addCPCommand       = console->input->addCommand;
#endif // CONSOLE_INPUT
	plugin->hpi->addPacket          = hplugins_addpacket;
	plugin->hpi->addToHPData        = hplugins_addToHPData;
	plugin->hpi->getFromHPData      = hplugins_getFromHPData;
	plugin->hpi->removeFromHPData   = hplugins_removeFromHPData;
	plugin->hpi->AddHook            = HPM_AddHook;
	plugin->hpi->HookStop           = HPM_HookStop;
	plugin->hpi->HookStopped        = HPM_HookStopped;
	plugin->hpi->addArg             = hpm_add_arg;
	plugin->hpi->addConf            = hplugins_addconf;
	/* server specific */
	if( HPM->load_sub )
		HPM->load_sub(plugin);

	ShowStatus("HPM: Loaded plugin '"CL_WHITE"%s"CL_RESET"' (%s).\n", plugin->info->name, plugin->info->version);

	return plugin;
}

/**
 * Unloads and unregisters a plugin.
 *
 * @param plugin The plugin data.
 */
void hplugin_unload(struct hplugin* plugin)
{
	int i;
	if (plugin->filename)
		aFree(plugin->filename);
	if (plugin->dll)
		plugin_close(plugin->dll);
	/* TODO: for manual packet unload */
	/* - Go through known packets and unlink any belonging to the plugin being removed */
	ARR_FIND(0, VECTOR_LENGTH(HPM->plugins), i, VECTOR_INDEX(HPM->plugins, i)->idx == plugin->idx);
	if (i != VECTOR_LENGTH(HPM->plugins)) {
		VECTOR_ERASE(HPM->plugins, i);
	}
	aFree(plugin);
}

/**
 * Adds a plugin requested from the command line to the auto-load list.
 */
CMDLINEARG(loadplugin)
{
	RECREATE(HPM->cmdline_plugins, char *, ++HPM->cmdline_plugins_count);
	HPM->cmdline_plugins[HPM->cmdline_plugins_count-1] = aStrdup(params);
	return true;
}

/**
 * Reads the plugin configuration and loads the plugins as necessary.
 */
void hplugins_config_read(void) {
	config_t plugins_conf;
	config_setting_t *plist = NULL;
	const char *config_filename = "conf/plugins.conf"; // FIXME hardcoded name
	FILE *fp;
	int i;

	/* yes its ugly, its temporary and will be gone as soon as the new inter-server.conf is set */
	if( (fp = fopen("conf/import/plugins.conf","r")) ) {
		config_filename = "conf/import/plugins.conf";
		fclose(fp);
	}

	if (libconfig->read_file(&plugins_conf, config_filename))
		return;

	plist = libconfig->lookup(&plugins_conf, "plugins_list");
	for (i = 0; i < HPM->cmdline_plugins_count; i++) {
		config_setting_t *entry = libconfig->setting_add(plist, NULL, CONFIG_TYPE_STRING);
		config_setting_set_string(entry, HPM->cmdline_plugins[i]);
	}

	if (plist != NULL) {
		int length = libconfig->setting_length(plist);
		char filename[60];
		char hooking_plugin_name[32];
		const char *plugin_name_suffix = "";
		if (SERVER_TYPE == SERVER_TYPE_LOGIN)
			plugin_name_suffix = "_login";
		else if (SERVER_TYPE == SERVER_TYPE_CHAR)
			plugin_name_suffix = "_char";
		else if (SERVER_TYPE == SERVER_TYPE_MAP)
			plugin_name_suffix = "_map";
		snprintf(hooking_plugin_name, sizeof(hooking_plugin_name), "HPMHooking%s", plugin_name_suffix);

		for (i = 0; i < length; i++) {
			const char *plugin_name = libconfig->setting_get_string_elem(plist,i);
			if (strcmpi(plugin_name, "HPMHooking") == 0 || strcmpi(plugin_name, hooking_plugin_name) == 0) { //must load it first
				struct hplugin *plugin;
				snprintf(filename, 60, "plugins/%s%s", hooking_plugin_name, DLL_EXT);
				if ((plugin = HPM->load(filename))) {
					const char * (*func)(bool *fr);
					bool (*addhook_sub) (enum HPluginHookType type, const char *target, void *hook, unsigned int pID);
					if ((func = plugin_import(plugin->dll, "Hooked",const char * (*)(bool *))) != NULL
					 && (addhook_sub = plugin_import(plugin->dll, "HPM_Plugin_AddHook",bool (*)(enum HPluginHookType, const char *, void *, unsigned int))) != NULL) {
						const char *failed = func(&HPM->force_return);
						if (failed) {
							ShowError("HPM: failed to retrieve '%s' for '"CL_WHITE"%s"CL_RESET"'!\n", failed, plugin_name);
						} else {
							HPM->hooking = true;
							HPM->addhook_sub = addhook_sub;
						}
					}
				}
				break;
			}
		}
		for (i = 0; i < length; i++) {
			if (strncmpi(libconfig->setting_get_string_elem(plist,i),"HPMHooking", 10) == 0) // Already loaded, skip
				continue;
			snprintf(filename, 60, "plugins/%s%s", libconfig->setting_get_string_elem(plist,i), DLL_EXT);
			HPM->load(filename);
		}
	}
	libconfig->destroy(&plugins_conf);

	if (VECTOR_LENGTH(HPM->plugins))
		ShowStatus("HPM: There are '"CL_WHITE"%"PRIuS""CL_RESET"' plugins loaded, type '"CL_WHITE"plugins"CL_RESET"' to list them\n", VECTOR_LENGTH(HPM->plugins));
}

/**
 * Console command: plugins
 *
 * Shows a list of loaded plugins.
 *
 * @see CPCMD()
 */
CPCMD(plugins)
{
	int i;

	if (VECTOR_LENGTH(HPM->plugins) == 0) {
		ShowInfo("HPC: there are no plugins loaded\n");
		return;
	}

	ShowInfo("HPC: There are '"CL_WHITE"%"PRIuS""CL_RESET"' plugins loaded\n", VECTOR_LENGTH(HPM->plugins));

	for(i = 0; i < VECTOR_LENGTH(HPM->plugins); i++) {
		struct hplugin *plugin = VECTOR_INDEX(HPM->plugins, i);
		ShowInfo("HPC: - '"CL_WHITE"%s"CL_RESET"' (%s)\n", plugin->info->name, plugin->filename);
	}
}

/**
 * Parses a packet through the registered plugin.
 *
 * @param fd The connection fd.
 * @param point The packet hooking point.
 * @retval 0 unknown packet
 * @retval 1 OK
 * @retval 2 incomplete packet
 */
unsigned char hplugins_parse_packets(int fd, enum HPluginPacketHookingPoints point)
{
	struct HPluginPacket *packet = NULL;
	int i;
	int16 length;

	ARR_FIND(0, VECTOR_LENGTH(HPM->packets[point]), i, VECTOR_INDEX(HPM->packets[point], i).cmd == RFIFOW(fd,0));

	if (i == VECTOR_LENGTH(HPM->packets[point]))
		return 0;

	packet = &VECTOR_INDEX(HPM->packets[point], i);
	length = packet->len;
	if (length == -1)
		length = RFIFOW(fd, 2);

	if (length > (int)RFIFOREST(fd))
		return 2;

	packet->receive(fd);
	RFIFOSKIP(fd, length);
	return 1;
}

/**
 * Retrieves a plugin name by ID.
 *
 * @param pid The plugin identifier.
 * @return The plugin name.
 * @retval "core" if the plugin ID belongs to the Hercules core.
 * @retval "UnknownPlugin" if the plugin wasn't found.
 */
char *hplugins_id2name(unsigned int pid)
{
	int i;

	if (pid == HPM_PID_CORE)
		return "core";

	for (i = 0; i < VECTOR_LENGTH(HPM->plugins); i++) {
		struct hplugin *plugin = VECTOR_INDEX(HPM->plugins, i);
		if (plugin->idx == pid)
			return plugin->info->name;
	}

	return "UnknownPlugin";
}

/**
 * Returns a retained permanent pointer to a source filename, for memory-manager reporting use.
 *
 * The returned pointer is safe to be used as filename in the memory manager
 * functions, and it will be available during and after the memory manager
 * shutdown (for memory leak reporting purposes).
 *
 * @param file The string/filename to retain
 * @return A retained copy of the source string.
 */
const char *HPM_file2ptr(const char *file)
{
	int i;

	ARR_FIND(0, HPM->filenames.count, i, HPM->filenames.data[i].addr == file);
	if (i != HPM->filenames.count) {
		return HPM->filenames.data[i].name;
	}

	/* we handle this memory outside of the server's memory manager because we need it to exist after the memory manager goes down */
	HPM->filenames.data = realloc(HPM->filenames.data, (++HPM->filenames.count)*sizeof(struct HPMFileNameCache));

	HPM->filenames.data[i].addr = file;
	HPM->filenames.data[i].name = strdup(file);

	return HPM->filenames.data[i].name;
}
void* HPM_mmalloc(size_t size, const char *file, int line, const char *func) {
	return iMalloc->malloc(size,HPM_file2ptr(file),line,func);
}
void* HPM_calloc(size_t num, size_t size, const char *file, int line, const char *func) {
	return iMalloc->calloc(num,size,HPM_file2ptr(file),line,func);
}
void* HPM_realloc(void *p, size_t size, const char *file, int line, const char *func) {
	return iMalloc->realloc(p,size,HPM_file2ptr(file),line,func);
}
void* HPM_reallocz(void *p, size_t size, const char *file, int line, const char *func) {
	return iMalloc->reallocz(p,size,HPM_file2ptr(file),line,func);
}
char* HPM_astrdup(const char *p, const char *file, int line, const char *func) {
	return iMalloc->astrdup(p,HPM_file2ptr(file),line,func);
}

bool hplugins_parse_conf(const char *w1, const char *w2, enum HPluginConfType point) {
	unsigned int i;

	/* exists? */
	for(i = 0; i < HPM->confsc[point]; i++) {
		if( !strcmpi(w1,HPM->confs[point][i].key) )
			break;
	}

	/* trigger and we're set! */
	if( i != HPM->confsc[point] ) {
		HPM->confs[point][i].func(w2);
		return true;
	}

	return false;
}

/**
 * Called by HPM->DataCheck on a plugins incoming data, ensures data structs in use are matching!
 **/
bool HPM_DataCheck(struct s_HPMDataCheck *src, unsigned int size, int version, char *name) {
	unsigned int i, j;

	if (version != datacheck_version) {
		ShowError("HPMDataCheck:%s: DataCheck API version mismatch %d != %d\n", name, datacheck_version, version);
		return false;
	}

	for (i = 0; i < size; i++) {
		if (!(src[i].type&SERVER_TYPE))
			continue;

		if (!strdb_exists(datacheck_db, src[i].name)) {
			ShowError("HPMDataCheck:%s: '%s' was not found\n",name,src[i].name);
			return false;
		} else {
			j = strdb_uiget(datacheck_db, src[i].name);/* not double lookup; exists sets cache to found data */
			if (src[i].size != datacheck_data[j].size) {
				ShowWarning("HPMDataCheck:%s: '%s' size mismatch %u != %u\n",name,src[i].name,src[i].size,datacheck_data[j].size);
				return false;
			}
		}
	}

	return true;
}

void HPM_datacheck_init(const struct s_HPMDataCheck *src, unsigned int length, int version) {
	unsigned int i;

	datacheck_version = version;
	datacheck_data = src;

	/**
	 * Populates datacheck_db for easy lookup later on
	 **/
	datacheck_db = strdb_alloc(DB_OPT_BASE,0);

	for(i = 0; i < length; i++) {
		strdb_uiput(datacheck_db, src[i].name, i);
	}
}

void HPM_datacheck_final(void) {
	db_destroy(datacheck_db);
}

void hpm_init(void) {
	int i;
	datacheck_db = NULL;
	datacheck_data = NULL;
	datacheck_version = 0;

	VECTOR_INIT(HPM->plugins);
	VECTOR_INIT(HPM->symbols);

	HPM->off = false;

	memcpy(&iMalloc_HPM, iMalloc, sizeof(struct malloc_interface));
	HPMiMalloc = &iMalloc_HPM;
	HPMiMalloc->malloc = HPM_mmalloc;
	HPMiMalloc->calloc = HPM_calloc;
	HPMiMalloc->realloc = HPM_realloc;
	HPMiMalloc->reallocz = HPM_reallocz;
	HPMiMalloc->astrdup = HPM_astrdup;

	sscanf(HPM_VERSION, "%u.%u", &HPM->version[0], &HPM->version[1]);

	if( HPM->version[0] == 0 && HPM->version[1] == 0 ) {
		ShowError("HPM:init:failed to retrieve HPM version!!\n");
		return;
	}

	for (i = 0; i < hpPHP_MAX; i++) {
		VECTOR_INIT(HPM->packets[i]);
	}

#ifdef CONSOLE_INPUT
	console->input->addCommand("plugins",CPCMD_A(plugins));
#endif
	return;
}

/**
 * Releases the retained filenames cache.
 */
void hpm_memdown(void)
{
	/* this memory is handled outside of the server's memory manager and
	 * thus cleared after memory manager goes down */
	if (HPM->filenames.count) {
		int i;
		for (i = 0; i < HPM->filenames.count; i++) {
			free(HPM->filenames.data[i].name);
		}
		free(HPM->filenames.data);
		HPM->filenames.data = NULL;
		HPM->filenames.count = 0;
	}
}

void hpm_final(void)
{
	int i;

	HPM->off = true;

	while (VECTOR_LENGTH(HPM->plugins)) {
		HPM->unload(VECTOR_LAST(HPM->plugins));
	}
	VECTOR_CLEAR(HPM->plugins);

	while (VECTOR_LENGTH(HPM->symbols)) {
		aFree(VECTOR_POP(HPM->symbols));
	}
	VECTOR_CLEAR(HPM->symbols);

	for (i = 0; i < hpPHP_MAX; i++) {
		VECTOR_CLEAR(HPM->packets[i]);
	}

	for( i = 0; i < HPCT_MAX; i++ ) {
		if( HPM->confsc[i] )
			aFree(HPM->confs[i]);
	}
	if (HPM->cmdline_plugins) {
		int j;
		for (j = 0; j < HPM->cmdline_plugins_count; j++)
			aFree(HPM->cmdline_plugins[j]);
		aFree(HPM->cmdline_plugins);
		HPM->cmdline_plugins = NULL;
		HPM->cmdline_plugins_count = 0;
	}

	/* HPM->fnames is cleared after the memory manager goes down */
	iMalloc->post_shutdown = hpm_memdown;

	return;
}
void hpm_defaults(void) {
	unsigned int i;
	HPM = &HPM_s;

	memset(&HPM->filenames, 0, sizeof(HPM->filenames));
	HPM->force_return = false;
	HPM->hooking = false;
	/* */
	for(i = 0; i < HPCT_MAX; i++) {
		HPM->confs[i] = NULL;
		HPM->confsc[i] = 0;
	}
	HPM->cmdline_plugins = NULL;
	HPM->cmdline_plugins_count = 0;
	/* */
	HPM->init = hpm_init;
	HPM->final = hpm_final;

	HPM->create = hplugin_create;
	HPM->load = hplugin_load;
	HPM->unload = hplugin_unload;
	HPM->event = hplugin_trigger_event;
	HPM->exists = hplugin_exists;
	HPM->iscompatible = hplugin_iscompatible;
	HPM->import_symbol = hplugin_import_symbol;
	HPM->share = hplugin_export_symbol;
	HPM->config_read = hplugins_config_read;
	HPM->pid2name = hplugins_id2name;
	HPM->parse_packets = hplugins_parse_packets;
	HPM->load_sub = NULL;
	HPM->addhook_sub = NULL;
	HPM->grabHPData = hplugins_grabHPData;
	HPM->grabHPDataSub = NULL;
	HPM->parseConf = hplugins_parse_conf;
	HPM->DataCheck = HPM_DataCheck;
	HPM->datacheck_init = HPM_datacheck_init;
	HPM->datacheck_final = HPM_datacheck_final;
}
