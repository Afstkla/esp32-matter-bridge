#include "console.h"

#include <cstdio>

#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "nvs.h"

void consoleRegisterCmd(const char *name, const char *help, esp_console_cmd_func_t func) {
  const esp_console_cmd_t cmd = {
      .command = name,
      .help = help,
      .hint = nullptr,
      .func = func,
      .argtable = nullptr,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static int cmdPing(int argc, char **argv) {
  printf("PONG\n");
  return 0;
}

static int cmdHeap(int argc, char **argv) {
  printf("HEAP internal=%u psram=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return 0;
}

// Runs on the console task, so it works while another task is parked — which
// is the whole point: it names the line a wedged task is blocked on.
static int cmdBacktrace(int argc, char **argv) {
  esp_backtrace_print_all_tasks(16);
  return 0;
}

static int cmdNvs(int argc, char **argv) {
  nvs_stats_t stats;
  if (nvs_get_stats(nullptr, &stats) != ESP_OK) {
    printf("NVS stats unavailable\n");
    return 1;
  }
  printf("NVS used=%u free=%u total=%u namespaces=%u\n", (unsigned)stats.used_entries,
         (unsigned)stats.free_entries, (unsigned)stats.total_entries,
         (unsigned)stats.namespace_count);
  return 0;
}

void consoleStart() {
  esp_console_repl_t *repl = nullptr;
  esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  replConfig.prompt = "genie> ";
  // `add` builds a Matter endpoint on this task, which does not fit the 4 KB
  // the default config asks for.
  replConfig.task_stack_size = 8192;

  esp_console_dev_usb_serial_jtag_config_t jtagConfig = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&jtagConfig, &replConfig, &repl));

  esp_console_register_help_command();
  consoleRegisterCmd("ping", "Reply PONG", cmdPing);
  consoleRegisterCmd("heap", "Show free internal and PSRAM heap", cmdHeap);
  consoleRegisterCmd("nvs", "Show NVS entry usage", cmdNvs);
  consoleRegisterCmd("bt", "Backtrace every task", cmdBacktrace);

  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
