/* settings_ini.h - <package>\settings.ini, the settings file the launcher and the game share (Windows).
 *
 * Format: one  NAME=VALUE  per line (NAME = the TOTK_* environment variable it stands for), ';' or '#' starts a comment
 * line, "[section]" lines are ignored. "%~dp0" in a value is the package folder (as in settings.bat).
 * Precedence: a variable already set in the environment wins over the file (so TOTK_FOO=1 on the command line still
 * overrides); settings_ini_load() sets the rest. settings_ini_set() rewrites one line in place (in-game menu). */
#pragma once

/* The file: TOTK_SETTINGS_INI if set, else <folder of the exe>\..\settings.ini (bin\totk-win.exe -> the package). */
const char *settings_ini_path(void);
/* Reads the file and sets every NAME the environment does not have yet. Returns the number of variables set, -1 if
   there is no file. */
int settings_ini_load(void);
/* Writes NAME=VALUE (replacing NAME's line, or appending one) and sets it in the environment; value NULL or "" removes
   the line and unsets it. The file is replaced atomically. Returns 0 on success. */
int settings_ini_set(const char *name, const char *value);

/* The same parser for tests: calls fn(name, value, user) for each setting line of text. */
void settings_ini_parse(const char *text, void (*fn)(const char *name, const char *value, void *user), void *user);
