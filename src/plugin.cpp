/*
 * TeamSpeak 3 demo plugin
 *
 * Copyright (c) 2008-2014 TeamSpeak Systems GmbH
 */

#ifdef _WIN32
// Static FFmpeg link: no DELAYLOAD pragmas. The shared-build pragmas
// emitted LNK4229 "directive ignored" warnings on every link because
// FFmpeg's symbols are baked into the plugin DLL directly. Kept the
// LoadLibraryEx fallback below in case someone bootstraps a shared-
// FFmpeg build flavour in the future — the calls are no-ops on the
// static build (the modules don't exist on disk, LoadLibrary fails
// silently, FFmpeg's already mapped from the .lib).
#pragma warning (disable : 4100)  /* Disable Unreferenced parameter warning */
#include <windows.h>
#include <string>

BOOL WINAPI DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD fdwReason, _In_ LPVOID lpvReserved)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		WCHAR path[MAX_PATH];
		if (GetModuleFileNameW(hinstDLL, path, MAX_PATH)) {
			WCHAR* lastSlash = wcsrchr(path, L'\\');
			if (lastSlash) {
				*lastSlash = L'\0';
				std::wstring dir = std::wstring(path) + L"\\rp_soundboard\\";
				LoadLibraryExW((dir + L"avutil-60.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
				LoadLibraryExW((dir + L"swresample-6.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
				LoadLibraryExW((dir + L"avcodec-62.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
				LoadLibraryExW((dir + L"avformat-62.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
				LoadLibraryExW((dir + L"avfilter-11.dll").c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
			}
		}
	}
	return TRUE;
}
#endif

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <map>

#include "plugin.h"
#include "main.h"
#include "buildinfo.h"
#include "ts3log.h"

/*static*/ struct TS3Functions ts3Functions;


#ifdef _WIN32
#define _strcpy(dest, destSize, src) strcpy_s(dest, destSize, src)
#define snprintf sprintf_s
#else
#define _strcpy(dest, destSize, src) { strncpy(dest, src, destSize-1); (dest)[destSize-1] = '\0'; }
#endif

#define PLUGIN_API_VERSION 26




static char* pluginID = NULL;
static char g_ts3ConfigPath[PATH_BUFSIZE] = {0};

/* See plugin.h: master gate for rpsb_debug.log writers. Default off so a
 * fresh install never touches the disk; ConfigModel writes the saved
 * value into this on plugin init and on every settings toggle. */
int g_rpsbLogsEnabled = 0;
int g_rpsbPreviewOnly = 0;
int g_rpsbExtremeLogging = 0;

const char *getTs3ConfigPath()
{
	if (g_ts3ConfigPath[0] == '\0')
	{
		if (ts3Functions.getConfigPath)
		{
			ts3Functions.getConfigPath(g_ts3ConfigPath, PATH_BUFSIZE);
			size_t len = strlen(g_ts3ConfigPath);
			if (len > 0 && g_ts3ConfigPath[len-1] != '/' && g_ts3ConfigPath[len-1] != '\\' && len < PATH_BUFSIZE - 1)
			{
#ifdef _WIN32
				g_ts3ConfigPath[len] = '\\';
#else
				g_ts3ConfigPath[len] = '/';
#endif
				g_ts3ConfigPath[len+1] = '\0';
			}
		}
#ifdef _WIN32
		if (g_ts3ConfigPath[0] == '\0')
		{
			char appdata[MAX_PATH];
			if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH))
				snprintf(g_ts3ConfigPath, PATH_BUFSIZE, "%s\\TS3Client\\", appdata);
		}
#elif defined(__APPLE__)
		if (g_ts3ConfigPath[0] == '\0')
		{
			const char *home = getenv("HOME");
			if (home)
				snprintf(g_ts3ConfigPath, PATH_BUFSIZE, "%s/Library/Application Support/TeamSpeak 3/", home);
		}
#else
		if (g_ts3ConfigPath[0] == '\0')
		{
			const char *home = getenv("HOME");
			if (home)
				snprintf(g_ts3ConfigPath, PATH_BUFSIZE, "%s/.ts3client/", home);
		}
#endif
	}
	return g_ts3ConfigPath;
}

#ifdef _WIN32
/* Helper function to convert wchar_T to Utf-8 encoded strings on Windows */
static int wcharToUtf8(const wchar_t* str, char** result) {
	int outlen = WideCharToMultiByte(CP_UTF8, 0, str, -1, 0, 0, 0, 0);
	*result = (char*)malloc(outlen);
	if(WideCharToMultiByte(CP_UTF8, 0, str, -1, *result, outlen, 0, 0) == 0) {
		*result = NULL;
		return -1;
	}
	return 0;
}
#endif

/*********************************** Required functions ************************************/
/*
 * If any of these required functions is not implemented, TS3 will refuse to load the plugin
 */


extern "C"
{

/* Unique name identifying this plugin */
const char* ts3plugin_name() {
	return buildinfo_getPluginName();
}

/* Plugin version */
const char* ts3plugin_version() {
    return buildinfo_getPluginVersion();
}

/* Plugin API version. Must be the same as the clients API major version, else the plugin fails to load. */
int ts3plugin_apiVersion() {
    return PLUGIN_API_VERSION;
}

/* Plugin author */
const char* ts3plugin_author() {
	return buildinfo_getPluginAuthor();
}

/* Plugin description */
const char* ts3plugin_description() {
	return buildinfo_getPluginDescription();
}

/* Set TeamSpeak 3 callback functions */
void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    ts3Functions = funcs;
}

/*
 * Custom code called right after loading the plugin. Returns 0 on success, 1 on failure.
 * If the function returns 1 on failure, the plugin will be unloaded again.
 */
int ts3plugin_init() {
#ifdef _WIN32
	/* Cleanup legacy DLL variants from prior installs (upstream binaries that
	 * predate the GameBaiters rename). Without this they coexist in plugins/
	 * and TS3 lists duplicate plugin entries.
	 */
	{
		char pluginsDir[PATH_BUFSIZE];
		if (ts3Functions.getConfigPath)
		{
			ts3Functions.getConfigPath(pluginsDir, PATH_BUFSIZE);
			std::string base(pluginsDir);
			if (!base.empty() && base.back() != '\\' && base.back() != '/')
				base.push_back('\\');
			base += "plugins\\";
			const char* legacy[] = {
				"rp_soundboard_win64.dll",
				"rp_soundboard_win32.dll",
				"rp_soundboard.dll",
				NULL
			};
			for (int i = 0; legacy[i]; i++)
			{
				std::string full = base + legacy[i];
				DeleteFileA(full.c_str());
			}
		}
	}
#endif
	sb_init();

    return 0;  /* 0 = success, 1 = failure, -2 = failure but client will not show a "failed to load" warning */
	/* -2 is a very special case and should only be used if a plugin displays a dialog (e.g. overlay) asking the user to disable
	 * the plugin again, avoiding the show another dialog by the client telling the user the plugin failed to load.
	 * For normal case, if a plugin really failed to load because of an error, the correct return value is 1. */
}

/* Custom code called right before the plugin is unloaded */
void ts3plugin_shutdown() {
	/*
	 * sb_kill() now drains the Qt event queue and stops the singleton
	 * SampleVisualizerThread before returning. See main.cpp.
	 */
	sb_kill();

	/* Free pluginID if we registered it */
	if(pluginID)
	{
		free(pluginID);
		pluginID = NULL;
	}
}

/****************************** Optional functions ********************************/
/*
 * Following functions are optional, if not needed you don't need to implement them.
 */

/*
 * If the plugin wants to use error return codes, plugin commands, hotkeys or menu items, it needs to register a command ID. This function will be
 * automatically called after the plugin was initialized. This function is optional. If you don't use these features, this function can be omitted.
 * Note the passed pluginID parameter is no longer valid after calling this function, so you must copy it and store it in the plugin.
 */
void ts3plugin_registerPluginID(const char* id) 
{
	const size_t sz = strlen(id) + 1;
	pluginID = (char*)malloc(sz * sizeof(char));
	_strcpy(pluginID, sz, id);
}

/* Plugin command keyword. Return NULL or "" if not used. */
const char* ts3plugin_commandKeyword() 
{
	return "rpsb";
}

/* Plugin processes console command. Return 0 if plugin handled the command, 1 if not handled. */
int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command) 
{
	char* args[3]; // <[conf] <snd>|<stop>> -> max 2, 3+ == error
	char* token; //last strtok result
	int argc = 0; //number of arguments
	
	char* tokanize = strdup(command); //create working copy of command for strtok that is not const
	if (tokanize != NULL)
	{
		token = strtok((char*)tokanize, " ");
		while (token != NULL && argc < 3) //read next token, but not more than 3
		{
			args[argc++] = token; //append token, increase arg counter
			token = strtok(NULL, " "); //try to read next token
		}
		free(tokanize);
	}

	//convert string to lower (if you know a lib you can use it)
	for (int a = 0; a<argc; a++)
	{
		char* b = args[argc];
		for (; *b != 0; b++)
			//assuming ascii encoding, turning on the 6th bit (value equals space) will convert it to lower case
			if (*b >= 'A' && *b <= 'Z')
				*b |= ' '; // tuning on 6th bit, converting upper case letters to lower case (see ascii table)
	}

	return sb_parseCommand(args, argc);
}

/* Client changed current server connection handler */
void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) 
{
	logDebug("CurrentServerConnectionChanged: %i", (int)serverConnectionHandlerID);
	//sb_onServerChange(serverConnectionHandlerID);
}


/* Required to release the memory for parameter "data" allocated in ts3plugin_infoData and ts3plugin_initMenus */
void ts3plugin_freeMemory(void* data) 
{
	free(data);
}

/*
 * Plugin requests to be always automatically loaded by the TeamSpeak 3 client unless
 * the user manually disabled it in the plugin dialog.
 * This function is optional. If missing, no autoload is assumed.
 */
int ts3plugin_requestAutoload()
{
	return 1;  /* 1 = request autoloaded, 0 = do not request autoload */
}

/*
 * If the plugin offers a configuration UI, it must return PLUGIN_OFFERS_CONFIGURE_QT_THREAD or
 * PLUGIN_OFFERS_CONFIGURE_NEW_THREAD. If not, return PLUGIN_OFFERS_NO_CONFIGURE.
 */
int ts3plugin_offersConfigure()
{
	return PLUGIN_OFFERS_CONFIGURE_QT_THREAD;
}

/*
 * Called when the user clicks "Settings" for this plugin in the TS3 plugin dialog.
 */
void ts3plugin_configure(void* handle, void* qParentWidget)
{
	sb_openDialog();
}

/*
 * Called when the user clicks "Uninstall" for this plugin in the TS3 plugin dialog.
 * Remove plugin assets and config files. TS3 handles the DLL itself.
 */
void ts3plugin_uninstall()
{
#ifdef _WIN32
	const char *cfgDir = getTs3ConfigPath();
	if (!cfgDir || !cfgDir[0])
		return;

	int wlen = MultiByteToWideChar(CP_UTF8, 0, cfgDir, -1, NULL, 0);
	std::wstring basePath(wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, cfgDir, -1, &basePath[0], wlen);
	basePath.resize(wlen - 1);

	std::wstring pluginDir = basePath + L"plugins\\rp_soundboard";
	std::wstring configFile = basePath + L"rpsb_debug.log";

	// Helper: delete a file, schedule for reboot if locked
	auto safeDelete = [](const std::wstring &path) {
		if (DeleteFileW(path.c_str()))
			return;
		DWORD err = GetLastError();
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
			return;
		// File is locked — schedule deletion on next reboot
		MoveFileExW(path.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
	};

	// Delete known asset files from the plugin subfolder
	const wchar_t *assetFiles[] = {
		L"\\rpmb_icon_16.png",
		L"\\Airhorn Sonata.mp3",
		L"\\Airhorn.mp3",
		L"\\Airporn.mp3",
		L"\\Peter Griffin Laugh.mp3",
		L"\\Spooky.mp3",
		L"\\avutil-60.dll",
		L"\\swresample-6.dll",
		L"\\avcodec-62.dll",
		L"\\avformat-62.dll",
		L"\\avfilter-11.dll",
		// Legacy names from prior FFmpeg 6.1 builds — kept in the
		// uninstall list so users upgrading from old soundboard
		// installs still get a clean plugin folder.
		L"\\avutil-58.dll",
		L"\\swresample-4.dll",
		L"\\avcodec-60.dll",
		L"\\avformat-60.dll",
		L"\\avfilter-9.dll",
	};
	for (const wchar_t *f : assetFiles)
		safeDelete(pluginDir + f);

	// Try to remove the directory (succeeds only if empty — safe)
	RemoveDirectoryW(pluginDir.c_str());
	if (GetLastError() == ERROR_DIR_NOT_EMPTY)
	{
		// Directory has user-added files; schedule for reboot removal
		MoveFileExW(pluginDir.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
	}

	// Remove debug log if present
	safeDelete(configFile);
#endif
}


/* Helper function to create a menu item */
static struct PluginMenuItem* createMenuItem(enum PluginMenuType type, int id, const char* text, const char* icon) 
{
	struct PluginMenuItem* menuItem = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
	menuItem->type = type;
	menuItem->id = id;
	_strcpy(menuItem->text, PLUGIN_MENU_BUFSZ, text);
	_strcpy(menuItem->icon, PLUGIN_MENU_BUFSZ, icon);
	return menuItem;
}

/* Some makros to make the code to create menu items a bit more readable */
#define BEGIN_CREATE_MENUS(x) const size_t sz = x + 1; size_t n = 0; *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * sz);
#define CREATE_MENU_ITEM(a, b, c, d) (*menuItems)[n++] = createMenuItem(a, b, c, d);
#define END_CREATE_MENUS (*menuItems)[n++] = NULL; assert(n == sz);

/*
 * Menu IDs for this plugin. Pass these IDs when creating a menuitem to the TS3 client. When the menu item is triggered,
 * ts3plugin_onMenuItemEvent will be called passing the menu ID of the triggered menu item.
 * These IDs are freely choosable by the plugin author. It's not really needed to use an enum, it just looks prettier.
 */
enum {
	MENU_ID_SHOW_CONFIG = 1,
	MENU_ID_SHOW_ABOUT,
	MENU_ID_CHECK_FOR_UPDATES,
};

/*
 * Initialize plugin menus.
 * This function is called after ts3plugin_init and ts3plugin_registerPluginID. A pluginID is required for plugin menus to work.
 * Both ts3plugin_registerPluginID and ts3plugin_freeMemory must be implemented to use menus.
 * If plugin menus are not used by a plugin, do not implement this function or return NULL.
 */
void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) 
{
	/*
	 * Create the menus
	 * There are three types of menu items:
	 * - PLUGIN_MENU_TYPE_CLIENT:  Client context menu
	 * - PLUGIN_MENU_TYPE_CHANNEL: Channel context menu
	 * - PLUGIN_MENU_TYPE_GLOBAL:  "Plugins" menu in menu bar of main window
	 *
	 * Menu IDs are used to identify the menu item when ts3plugin_onMenuItemEvent is called
	 *
	 * The menu text is required, max length is 128 characters
	 *
	 * The icon is optional, max length is 128 characters. When not using icons, just pass an empty string.
	 * Icons are loaded from a subdirectory in the TeamSpeak client plugins folder. The subdirectory must be named like the
	 * plugin filename, without dll/so/dylib suffix
	 * e.g. for "test_plugin.dll", icon "1.png" is loaded from <TeamSpeak 3 Client install dir>\plugins\test_plugin\1.png
	 */

	BEGIN_CREATE_MENUS(3);  /* IMPORTANT: Number of menu items must be correct! */
	CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_GLOBAL,  MENU_ID_SHOW_CONFIG,  "Open GameBaiters Soundboard",  "rpmb_icon_16.png");
	CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_GLOBAL,  MENU_ID_SHOW_ABOUT,  "About",  "rpmb_icon_16.png");
	CREATE_MENU_ITEM(PLUGIN_MENU_TYPE_GLOBAL, MENU_ID_CHECK_FOR_UPDATES, "Check for update", "rpmb_icon_16.png");
	END_CREATE_MENUS;  /* Includes an assert checking if the number of menu items matched */

	/*
	 * Specify an optional icon for the plugin. This icon is used for the plugins submenu within context and main menus
	 * If unused, set menuIcon to NULL
	 */
	*menuIcon = (char*)malloc(PLUGIN_MENU_BUFSZ * sizeof(char));
	_strcpy(*menuIcon, PLUGIN_MENU_BUFSZ, "rpmb_icon_16.png");

	/*
	 * Menus can be enabled or disabled with: ts3Functions.setPluginMenuEnabled(pluginID, menuID, 0|1);
	 * Test it with plugin command: /test enablemenu <menuID> <0|1>
	 * Menus are enabled by default. Please note that shown menus will not automatically enable or disable when calling this function to
	 * ensure Qt menus are not modified by any thread other the UI thread. The enabled or disable state will change the next time a
	 * menu is displayed.
	 */
	/* For example, this would disable MENU_ID_GLOBAL_2: */
	/* ts3Functions.setPluginMenuEnabled(pluginID, MENU_ID_GLOBAL_2, 0); */

	/* All memory allocated in this function will be automatically released by the TeamSpeak client later by calling ts3plugin_freeMemory */
}

/* Helper function to create a hotkey */
static struct PluginHotkey* createHotkey(const char* keyword, const char* description) 
{
	struct PluginHotkey* hotkey = (struct PluginHotkey*)malloc(sizeof(struct PluginHotkey));
	_strcpy(hotkey->keyword, PLUGIN_HOTKEY_BUFSZ, keyword);
	_strcpy(hotkey->description, PLUGIN_HOTKEY_BUFSZ, description);
	return hotkey;
}

/* Some makros to make the code to create hotkeys a bit more readable */
#define BEGIN_CREATE_HOTKEYS(x) const size_t sz = (x) + 1; size_t n = 0; *hotkeys = (struct PluginHotkey**)malloc(sizeof(struct PluginHotkey*) * sz);
#define CREATE_HOTKEY(a, b) (*hotkeys)[n++] = createHotkey(a, b);
#define END_CREATE_HOTKEYS (*hotkeys)[n++] = NULL; assert(n == sz);

/*
 * Initialize plugin hotkeys. If your plugin does not use this feature, this function can be omitted.
 * Hotkeys require ts3plugin_registerPluginID and ts3plugin_freeMemory to be implemented.
 * This function is automatically called by the client after ts3plugin_init.
 */
void ts3plugin_initHotkeys(struct PluginHotkey*** hotkeys) 
{
	/* Register hotkeys giving a keyword and a description.
	 * The keyword will be later passed to model. to identify which hotkey was triggered.
	 * The description is shown in the clients hotkey dialog. */
	int i;
	int numKeys = 200;
	int numExtra = NUM_CONFIGS + 6;
	char kw[PLUGIN_HOTKEY_BUFSZ];
	char desc[PLUGIN_HOTKEY_BUFSZ];

	BEGIN_CREATE_HOTKEYS(numKeys + numExtra);
	for(i = 0; i < numKeys; i++)
	{
		sb_getInternalHotkeyName(i, kw);
		sprintf(desc, "Press button %i", i+1);
		CREATE_HOTKEY(kw, desc);
	}

	for (i = 0; i < NUM_CONFIGS; i++)
	{
		sb_getInternalConfigHotkeyName(i, kw);
		sprintf(desc, "Activate sound config %i", i+1);
		CREATE_HOTKEY(kw, desc);
	}

    CREATE_HOTKEY(HOTKEY_STOP_ALL, "Stop all sounds");
	CREATE_HOTKEY(HOTKEY_PAUSE_ALL, "Pause/unpause sound");
	CREATE_HOTKEY(HOTKEY_MUTE_MYSELF, "Toggle 'Mute myself during playback'");
	CREATE_HOTKEY(HOTKEY_MUTE_ON_MY_CLIENT, "Toggle 'Mute on my client'");
	CREATE_HOTKEY(HOTKEY_VOLUME_INCREASE, "Increase volume by 20%");
	CREATE_HOTKEY(HOTKEY_VOLUME_DECREASE, "Decrease volume by 20%");
	END_CREATE_HOTKEYS;

	/* The client will call ts3plugin_freeMemory to release all allocated memory */
}

/************************** TeamSpeak callbacks ***************************/
/*
 * Following functions are optional, feel free to remove unused callbacks.
 * See the clientlib documentation for details on each function.
 */

/* Clientlib */
 
std::map<uint64, int> clientInputHardwareStateMap;
void ts3plugin_onUpdateClientEvent(uint64 serverConnectionHandlerID, anyID clientID, anyID invokerID, const char * invokerName, const char * invokerUniqueIdentifier)
{
	anyID myId = 0;
	if (checkError(ts3Functions.getClientID(serverConnectionHandlerID, &myId), "getClientID error"))
		return;
	if (myId != clientID)
		return;
	int inputState = 0;
	if (checkError(ts3Functions.getClientSelfVariableAsInt(serverConnectionHandlerID, (size_t)CLIENT_INPUT_HARDWARE, &inputState), "getClientSelfVariableAsInt error"))
		return;

	if (inputState)
	{
		int oldInputState = clientInputHardwareStateMap[serverConnectionHandlerID]; // Gets initialized to 0 when nonexistant
		if (!oldInputState)
		{
			logDebug("Input state of server %i enabled", (int)serverConnectionHandlerID);
			sb_onServerChange(serverConnectionHandlerID);
		}
	}
	clientInputHardwareStateMap[serverConnectionHandlerID] = inputState;
}


void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber)
{
	if (newStatus == STATUS_DISCONNECTED)
		clientInputHardwareStateMap.erase(serverConnectionHandlerID);

    sb_onConnectStatusChange(serverConnectionHandlerID, newStatus, errorNumber);
}

void ts3plugin_onEditMixedPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, short* samples, int sampleCount,
	int channels, const unsigned int* channelSpeakerArray, unsigned int* channelFillMask)
{
	sb_handlePlaybackData(serverConnectionHandlerID, samples, sampleCount, channels, channelSpeakerArray, channelFillMask);
}

void ts3plugin_onEditCapturedVoiceDataEvent(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, int* edited) 
{
	sb_handleCaptureData(serverConnectionHandlerID, samples, sampleCount, channels, edited);
}


/*
 * Called when a plugin menu item (see ts3plugin_initMenus) is triggered. Optional function, when not using plugin menus, do not implement this.
 * 
 * Parameters:
 * - serverConnectionHandlerID: ID of the current server tab
 * - type: Type of the menu (PLUGIN_MENU_TYPE_CHANNEL, PLUGIN_MENU_TYPE_CLIENT or PLUGIN_MENU_TYPE_GLOBAL)
 * - menuItemID: Id used when creating the menu item
 * - selectedItemID: Channel or Client ID in the case of PLUGIN_MENU_TYPE_CHANNEL and PLUGIN_MENU_TYPE_CLIENT. 0 for PLUGIN_MENU_TYPE_GLOBAL.
 */
void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID) 
{
	switch(type) {
		case PLUGIN_MENU_TYPE_GLOBAL:
			/* Global menu item was triggered. selectedItemID is unused and set to zero. */
			switch(menuItemID) 
			{
				case MENU_ID_SHOW_CONFIG:
					sb_openDialog();
					break;
				case MENU_ID_SHOW_ABOUT:
					sb_openAbout();
					break;
				case MENU_ID_CHECK_FOR_UPDATES:
					sb_checkForUpdates();
					break;
				default:
					break;
			}
			break;
		default:
			break;
	}
}

/* This function is called if a plugin hotkey was pressed. Omit if hotkeys are unused. */
void ts3plugin_onHotkeyEvent(const char* keyword) 
{
	sb_onHotkeyPressed(keyword);
}


void ts3plugin_onHotkeyRecordedEvent(const char* keyword, const char* key)
{
	sb_onHotkeyRecordedEvent(keyword, key);
}


void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID)
{
	//logDebug("onTalkStatusChangeEvent status=%i, isReceivedWhisper=%i, clientID=%i", status, isReceivedWhisper, (int)clientID);
	anyID myId = 0;
	if (checkError(ts3Functions.getClientID(serverConnectionHandlerID, &myId), "getClientID error"))
		return;
	if (clientID == myId && status == 0 && isReceivedWhisper == 0)
		sb_onStopTalking();
}


const char * getPluginID()
{
	return pluginID;
}


} // extern "C"
