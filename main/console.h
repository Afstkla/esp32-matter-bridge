#pragma once

#include "esp_console.h"

// Starts the esp_console REPL over USB-Serial-JTAG and registers the
// built-in commands (help, ping, heap). Call once from app_main after NVS
// init; later tasks call consoleRegisterCmd to add their own commands
// before or after this.
void consoleStart();

// Thin wrapper so command registration reads as one line per command.
void consoleRegisterCmd(const char *name, const char *help, esp_console_cmd_func_t func);
