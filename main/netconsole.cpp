#include "netconsole.h"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sys/select.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "console.h"

static const char *TAG = "netconsole";

static const uint16_t PORT = 5323;
// esp_console_run copies into a buffer of exactly this size, so a longer line
// would be silently truncated into a different command.
static const size_t CMD_MAX = 256;
static const size_t TEE_BUF = 256;
static const uint8_t MAX_ATTEMPTS = 3;
static const uint32_t FAIL_DELAY_MS = 3000;
static const uint32_t AUTH_TIMEOUT_MS = 30000;
static const uint32_t IDLE_TIMEOUT_MS = 10UL * 60 * 1000;
// A client that cannot take 5760 bytes (one lwIP send buffer) in ten seconds is
// not reading, and this task is the only one that can accept the next one.
static const uint32_t SEND_TIMEOUT_MS = 10000;
static const uint32_t STOP_WAIT_MS = 15000;
// How long `passwd clear` takes to free the port, and how coarsely the idle
// timeout is measured. The ui task already wakes every 250 ms asleep, so a tick
// this slow adds nothing to the power budget.
static const uint32_t POLL_MS = 1000;
static const size_t SECRET_MIN = 8;
static const size_t SECRET_MAX = 63;
static const size_t DIGEST_HEX = 64;
static const size_t NONCE_HEX = 32;

static const char *NAMESPACE = "console";
static const char *SECRET_KEY = "secret";

static nvs_handle_t s_nvs = 0;
static TaskHandle_t volatile s_task = nullptr;
static volatile bool s_stopWanted = false;

static volatile int s_fd = -1;
static FILE *s_out = nullptr;
static FILE *s_ownStdout = nullptr;
static bool s_clientDead = false;
static char s_line[CMD_MAX];
static size_t s_used = 0;
static bool s_overlong = false;
static bool s_authed = false;
static uint8_t s_attempts = 0;
static char s_expected[DIGEST_HEX + 1];
static uint32_t s_deadline = 0;

// s_teeFd is the tee's only claim on the socket and is guarded by s_teeMux, so
// the session task can retire the fd knowing no hook is still holding it.
static SemaphoreHandle_t s_teeMux = nullptr;
static int s_teeFd = -1;
static std::atomic<uint32_t> s_dropped{0};

static uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// Length-checked on the way out as well as in: NVS is not a trust boundary this
// side of a flash tool, but a secret that fails the rule the setter enforces is
// corrupt, and a corrupt secret must not become a short one.
static bool loadSecret(char *out, size_t size) {
  size_t len = size;
  if (nvs_get_str(s_nvs, SECRET_KEY, out, &len) != ESP_OK) {
    return false;
  }
  len = strlen(out);
  return len >= SECRET_MIN && len <= SECRET_MAX;
}

static void hmacHex(const char *secret, const char *message, char *out) {
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)secret,
                  strlen(secret), (const uint8_t *)message, strlen(message), mac);
  for (size_t i = 0; i < sizeof(mac); i++) {
    snprintf(out + 2 * i, 3, "%02x", mac[i]);
  }
}

// Timing here is what stops an attacker walking the digest a byte at a time.
static bool sameDigest(const char *a, const char *b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < DIGEST_HEX; i++) {
    diff = (uint8_t)(diff | (a[i] ^ b[i]));
  }
  return diff == 0;
}

// esp_log_set_vprintf does hand back the hook it replaced, but capturing that
// return would leave a window where the tee is already live and the pointer it
// forwards through is still null. Nothing else in this firmware installs a
// hook, so the default is the exact restore target and is named directly.
static const vprintf_like_t BASE_VPRINTF = &vprintf;

// Runs on whichever task logged — the CHIP task holding the stack lock, a WiFi
// callback, anything — so it must never wait on anything unbounded. A mutex it
// does not get and a socket that will not take the bytes both end as a dropped
// line. Forwarding to the default hook comes first, so a task keeps whatever
// stdout it already had.
//
// Not lock-free: MSG_DONTWAIT still takes lwIP's core lock (this build has
// CONFIG_LWIP_TCPIP_CORE_LOCKING). That wait is bounded — the core lock is a
// priority-inheriting mutex, held only for the length of one lwIP operation,
// and released while a blocking write parks — and it cannot cycle, because
// nothing holding the core lock ever takes the CHIP stack lock.
//
// The session task is skipped because its own stdout is already the socket:
// teeing there would print every line twice.
static int teeVprintf(const char *fmt, va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  int written = BASE_VPRINTF(fmt, ap);
  if (s_fd >= 0 && xTaskGetCurrentTaskHandle() != s_task) {
    if (xSemaphoreTake(s_teeMux, 0) != pdTRUE) {
      s_dropped++;
    } else {
      if (s_teeFd < 0) {
        s_dropped++;
      } else {
        char line[TEE_BUF];
        int len = vsnprintf(line, sizeof(line), fmt, copy);
        if (len > (int)sizeof(line) - 1) {
          len = (int)sizeof(line) - 1;
        }
        if (len > 0 && send(s_teeFd, line, len, MSG_DONTWAIT) != len) {
          s_dropped++;
        }
      }
      xSemaphoreGive(s_teeMux);
    }
  }
  va_end(copy);
  return written;
}

static void teeTake(int fd) {
  xSemaphoreTake(s_teeMux, portMAX_DELAY);
  s_teeFd = fd;
  xSemaphoreGive(s_teeMux);
}

static void say(const char *text) {
  send(s_fd, text, strlen(text), 0);
}

// The session task's stdout, so every printf a command makes goes here. A
// client that stops reading fills the send buffer and would otherwise park this
// task inside a command for as long as it liked — and this is the task that
// accepts the next client, runs the idle timer and closes the socket, so that
// stall is the whole feature hanging.
//
// SO_SNDTIMEO bounds one send. This bounds the session: after the first send
// that does not complete, the client is dead, the rest of the command's output
// is swallowed at memory speed, and dispatch() closes the session as soon as
// esp_console_run returns. So a wedged client costs one timeout, once, ever.
static int sessionWrite(void *, const char *data, int len) {
  if (!s_clientDead && send(s_fd, data, len, 0) != len) {
    s_clientDead = true;
  }
  return len;
}

static void closeSession(const char *why) {
  if (s_fd < 0) {
    return;
  }
  if (s_authed) {
    printf("BYE %s, %u log lines dropped\n", why, (unsigned)s_dropped.load());
    fflush(stdout);
  }
  teeTake(-1);
  esp_log_set_vprintf(BASE_VPRINTF);
  if (s_out != nullptr) {
    stdout = s_ownStdout;
    fclose(s_out);  // holds no fd of its own; sessionWrite owns the socket
    s_out = nullptr;
  }
  close(s_fd);
  s_fd = -1;
  s_authed = false;
  s_clientDead = false;
  ESP_LOGI(TAG, "session closed: %s", why);
}

static void openSession(int fd) {
  char secret[SECRET_MAX + 2] = {0};
  if (!loadSecret(secret, sizeof(secret))) {
    close(fd);
    return;
  }

  char nonce[NONCE_HEX + 1];
  for (size_t i = 0; i < NONCE_HEX / 8; i++) {
    sprintf(nonce + i * 8, "%08" PRIx32, esp_random());
  }
  hmacHex(secret, nonce, s_expected);
  memset(secret, 0, sizeof(secret));

  s_fd = fd;
  s_used = 0;
  s_overlong = false;
  s_attempts = 0;
  s_authed = false;
  s_clientDead = false;
  s_deadline = nowMs() + AUTH_TIMEOUT_MS;
  s_dropped = 0;
  timeval sendTimeout = {.tv_sec = (time_t)(SEND_TIMEOUT_MS / 1000), .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

  char greeting[16 + NONCE_HEX];
  snprintf(greeting, sizeof(greeting), "CHALLENGE %s\n", nonce);
  say(greeting);
  ESP_LOGI(TAG, "session open, awaiting the answer");
}

static void authenticate(const char *reply) {
  if (strlen(reply) == DIGEST_HEX && sameDigest(reply, s_expected)) {
    s_out = fwopen(nullptr, sessionWrite);
    if (s_out == nullptr) {
      closeSession("no stdout for the socket");
      return;
    }
    setvbuf(s_out, nullptr, _IOLBF, TEE_BUF);
    // The client keys its handshake on this line, so it has to be the last
    // thing sent before the log tee is free to interleave.
    say("OK\n");
    s_ownStdout = stdout;
    stdout = s_out;
    s_authed = true;
    s_deadline = nowMs() + IDLE_TIMEOUT_MS;
    teeTake(s_fd);
    esp_log_set_vprintf(teeVprintf);
    ESP_LOGI(TAG, "session authenticated");
    return;
  }

  s_attempts++;
  // Three seconds the listener spends deaf, which is the price of holding the
  // penalty here rather than in a second task.
  vTaskDelay(pdMS_TO_TICKS(FAIL_DELAY_MS));
  if (s_attempts >= MAX_ATTEMPTS) {
    say("DENIED\n");
    closeSession("bad secret");
    return;
  }
  say("RETRY\n");
}

// ponytail: esp_console_run parses into one static buffer shared with the USB
// REPL, so typing on both consoles at the same instant can garble a command.
// Fixing it means owning the REPL loop instead of reusing it; not worth it for
// two consoles one person drives.
static void runCommand(const char *line) {
  int ret = 0;
  esp_err_t err = esp_console_run(line, &ret);
  if (err == ESP_ERR_NOT_FOUND) {
    printf("ERR unknown command\n");
  } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
    printf("ERR %s\n", esp_err_to_name(err));
  } else if (err == ESP_OK && ret != 0) {
    printf("ERR command returned %d\n", ret);
  }
  fflush(stdout);
}

static void dispatch() {
  s_line[s_used] = '\0';
  size_t len = s_used;
  bool overlong = s_overlong;
  s_used = 0;
  s_overlong = false;

  if (overlong) {
    if (s_authed) {
      printf("ERR line longer than %u bytes\n", (unsigned)CMD_MAX);
      fflush(stdout);
      return;
    }
    say("DENIED\n");
    closeSession("oversized answer");
    return;
  }
  if (!s_authed) {
    authenticate(s_line);
    return;
  }
  s_deadline = nowMs() + IDLE_TIMEOUT_MS;
  if (len > 0) {
    runCommand(s_line);
  }
  if (s_clientDead) {
    closeSession("client stopped reading");
  }
}

static void pump() {
  char chunk[128];
  int n = recv(s_fd, chunk, sizeof(chunk), 0);
  if (n <= 0) {
    closeSession(n == 0 ? "client hung up" : "socket error");
    return;
  }
  for (int i = 0; i < n && s_fd >= 0; i++) {
    char c = chunk[i];
    if (c == '\r') {
      continue;
    }
    if (c != '\n') {
      if (s_used + 1 < sizeof(s_line)) {
        s_line[s_used++] = c;
      } else {
        s_overlong = true;
      }
      continue;
    }
    dispatch();
  }
}

static int openListener() {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  // Binding the wildcard address does not need an interface, so this succeeds
  // long before WiFi has an IP; the retry is for a port still in TIME_WAIT.
  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// One task does both the accepting and the session, which is what makes a
// second caller a polite "BUSY" rather than a connection queued behind a
// debugging session nobody is watching.
static void listenTask(void *) {
  int listenFd = -1;
  bool complained = false;
  while (!s_stopWanted) {
    if (listenFd < 0) {
      listenFd = openListener();
      if (listenFd < 0) {
        // Once, not once a second: a port that stays unavailable would otherwise
        // bury every other line in the log.
        if (!complained) {
          ESP_LOGW(TAG, "port %u not available (errno %d), retrying quietly", PORT, errno);
          complained = true;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        continue;
      }
      complained = false;
      ESP_LOGI(TAG, "listening on port %u", PORT);
    }

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listenFd, &readable);
    int maxFd = listenFd;
    if (s_fd >= 0) {
      FD_SET(s_fd, &readable);
      maxFd = s_fd > maxFd ? s_fd : maxFd;
    }
    timeval tick = {.tv_sec = (time_t)(POLL_MS / 1000), .tv_usec = 0};
    int ready = select(maxFd + 1, &readable, nullptr, nullptr, &tick);

    if (s_fd >= 0 && (int32_t)(nowMs() - s_deadline) >= 0) {
      closeSession(s_authed ? "idle" : "no answer");
    }
    if (ready <= 0) {
      continue;
    }
    if (s_fd >= 0 && FD_ISSET(s_fd, &readable)) {
      pump();
    }
    if (FD_ISSET(listenFd, &readable)) {
      int fd = accept(listenFd, nullptr, nullptr);
      if (fd < 0) {
        // Ten sockets between here and everything Matter wants, so running out
        // is the plausible failure. The pending connection stays queued and
        // keeps select() firing, hence the pause as well as the complaint.
        ESP_LOGW(TAG, "accept failed (errno %d)", errno);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
      } else if (s_fd >= 0) {
        send(fd, "BUSY\n", 5, 0);
        close(fd);
      } else {
        openSession(fd);
      }
    }
  }

  closeSession("listener stopped");
  if (listenFd >= 0) {
    close(listenFd);
  }
  ESP_LOGI(TAG, "listener stopped");
  s_task = nullptr;
  vTaskDelete(nullptr);
}

// False means the caller must not claim the console is up: a task still on its
// way out still owns the port, and silently reusing it would report a listener
// that is about to delete itself.
static bool startListener() {
  if (s_task != nullptr) {
    return !s_stopWanted;
  }
  s_stopWanted = false;
  // `add` builds a Matter endpoint on whichever task runs it, which is why the
  // USB REPL asks for 8 KB too.
  TaskHandle_t created = nullptr;
  xTaskCreate(listenTask, "netconsole", 8192, nullptr, 2, &created);
  s_task = created;
  return s_task != nullptr;
}

// Waits it out so the command does not return before the port is free, which is
// the only way "the listener is gone" can be checked from the next line. The
// wait comfortably outlasts the one send timeout a wedged client can cost, so a
// false here means something is genuinely stuck and the caller has to say so.
static bool stopListener() {
  if (s_task == nullptr) {
    return true;
  }
  s_stopWanted = true;
  for (uint32_t waited = 0; waited < STOP_WAIT_MS && s_task != nullptr; waited += 100) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return s_task == nullptr;
}

// The session task runs commands too, so this is the one command that has to
// know which console asked. A secret typed into a session that only exists
// because of the old secret is not a secret anybody has proven they know.
static int cmdPasswd(int argc, char **argv) {
  if (xTaskGetCurrentTaskHandle() == s_task) {
    printf("ERR passwd is USB-only\n");
    return 1;
  }
  if (argc != 2) {
    printf("ERR usage: passwd <secret|clear>\n");
    return 1;
  }
  if (strcmp(argv[1], "clear") == 0) {
    nvs_erase_key(s_nvs, SECRET_KEY);
    nvs_commit(s_nvs);
    if (!stopListener()) {
      printf("PASSWD cleared, but the listener has not let go of port %u\n", PORT);
      return 1;
    }
    printf("PASSWD cleared\n");
    return 0;
  }
  size_t len = strlen(argv[1]);
  if (len < SECRET_MIN || len > SECRET_MAX) {
    printf("ERR secret must be %u..%u characters\n", (unsigned)SECRET_MIN, (unsigned)SECRET_MAX);
    return 1;
  }
  if (nvs_set_str(s_nvs, SECRET_KEY, argv[1]) != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
    printf("ERR secret not stored\n");
    return 1;
  }
  if (!startListener()) {
    printf("PASSWD set, but the previous listener is still shutting down\n");
    return 1;
  }
  printf("PASSWD set, console on port %u\n", PORT);
  return 0;
}

void netconsoleBegin() {
  ESP_ERROR_CHECK(nvs_open(NAMESPACE, NVS_READWRITE, &s_nvs));
  s_teeMux = xSemaphoreCreateMutex();
  consoleRegisterCmd("passwd", "Set the network console secret, or 'clear' to remove it", cmdPasswd);

  char secret[SECRET_MAX + 2] = {0};
  if (loadSecret(secret, sizeof(secret))) {
    memset(secret, 0, sizeof(secret));
    startListener();
  }
}
