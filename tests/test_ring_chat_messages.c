/*
 * Host-side unit tests for the ring_chat message builder.
 *
 * ring_llm.c's build_messages() is static, so this test includes the
 * translation unit and renames the symbol to expose it (white-box). The real
 * ring_chat.c is linked in for ring_chat_s and the history API, so the tests
 * exercise the same init/add path the apps use.
 *
 * Covers: empty/system-only output, message order, escaping, the JSON buffer
 * cap, the history storage cap, and the max_history_turns window.
 */
#include <stdio.h>
#include <string.h>

#include "ring_chat_priv.h"

/* Expose the static builder for white-box testing. */
#define build_messages test_build_messages
#include "ring_llm.c"
#undef build_messages

static int checks;
static int failures;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);      \
        }                                                             \
    } while (0)

static const ring_chat_cfg_t base_cfg = {
    .stt_model = "asr",
    .llm_model = "llm",
    .tts_model = "tts",
    .voice = "voice",
    .system_prompt = NULL,
    .stt_language = NULL,
    .max_history_turns = 20,
};

static void test_empty_and_system(void)
{
    char buf[512];
    int n;

    ring_chat_cfg_t cfg = base_cfg;
    CHECK(ring_chat_init(&cfg, "sk-test") == ESP_OK, "init empty");

    n = test_build_messages(buf, sizeof(buf));
    CHECK(n == 2, "empty length");
    CHECK(strcmp(buf, "[]") == 0, "empty value");

    cfg.system_prompt = "You are a test.";
    CHECK(ring_chat_init(&cfg, "sk-test") == ESP_OK, "init system");
    n = test_build_messages(buf, sizeof(buf));
    CHECK(n > 2, "system length");
    CHECK(strcmp(buf, "[{\"role\":\"system\",\"content\":\"You are a test.\"}]") == 0,
          "system value");
}

static void test_message_order(void)
{
    char buf[512];
    int n;

    ring_chat_cfg_t cfg = base_cfg;
    CHECK(ring_chat_init(&cfg, "sk-test") == ESP_OK, "init order");

    CHECK(ring_chat_add_user("hi") == ESP_OK, "add user");
    CHECK(ring_chat_add_assistant("yo") == ESP_OK, "add assistant");

    n = test_build_messages(buf, sizeof(buf));
    CHECK(n > 0, "order length");

    char *user = strstr(buf, "{\"role\":\"user\",\"content\":\"hi\"}");
    char *asst = strstr(buf, "{\"role\":\"assistant\",\"content\":\"yo\"}");
    CHECK(user != NULL, "user message present");
    CHECK(asst != NULL, "assistant message present");
    CHECK(user < asst, "user before assistant");

    /* No trailing comma before ]. */
    CHECK(strstr(buf, ",]") == NULL, "no trailing comma");
    CHECK(buf[n - 1] == ']', "ends with bracket");
}

static void test_escaping(void)
{
    char buf[1024];
    int n;

    ring_chat_cfg_t cfg = base_cfg;
    CHECK(ring_chat_init(&cfg, "sk-test") == ESP_OK, "init escaping");

    ring_chat_add_user("a\"b\\c\nd\x01");
    ring_chat_add_assistant("plain");

    n = test_build_messages(buf, sizeof(buf));
    CHECK(n > 0, "escaping length");

    CHECK(strstr(buf, "\\\"") != NULL, "quote escaped");
    CHECK(strstr(buf, "\\\\") != NULL, "backslash escaped");
    CHECK(strstr(buf, "\\n") != NULL, "newline escaped");
    CHECK(strstr(buf, "\\u0001") != NULL, "control char escaped");
    CHECK(strchr(buf, '\n') == NULL, "no raw newline in output");
    CHECK(strchr(buf, '\x01') == NULL, "no raw control char in output");
}

static void test_buffer_cap(void)
{
    ring_chat_cfg_t cfg = base_cfg;
    cfg.system_prompt = "You are a test.";
    CHECK(ring_chat_init(&cfg, "sk-test") == ESP_OK, "init buffer cap");

    char small[16];
    int r = test_build_messages(small, sizeof(small));
    CHECK(r == -1, "small buffer returns -1");

    /* A generous buffer succeeds. */
    char big[1024];
    r = test_build_messages(big, sizeof(big));
    CHECK(r > 0, "big buffer succeeds");
}

static void test_history_storage_cap(void)
{
    /* The storage must hold MAX_HISTORY_TURNS pairs regardless of the
     * window setting, otherwise a small max_history_turns would truncate the
     * history at half the documented number of turns. */
    ring_chat_cfg_t cfg = base_cfg;
    cfg.max_history_turns = 1;
    CHECK(ring_chat_init(&cfg, "sk-test") == ESP_OK, "init small window");

    CHECK(ring_chat_s.hist.cap >= RING_CHAT_MAX_HISTORY_TURNS * 2,
          "storage cap is the compile-time worst case");
    CHECK(ring_chat_add_user("u") == ESP_OK, "small window add user");
    CHECK(ring_chat_add_assistant("a") == ESP_OK, "small window add assistant");
}

static void test_history_window(void)
{
    char buf[2048];
    int n;

    ring_chat_cfg_t cfg = base_cfg;
    cfg.max_history_turns = 2;
    CHECK(ring_chat_init(&cfg, "sk-test") == ESP_OK, "init window");

    for (int i = 0; i < 5; i++) {
        char u[16], a[16];
        snprintf(u, sizeof(u), "u%d", i);
        snprintf(a, sizeof(a), "a%d", i);
        CHECK(ring_chat_add_user(u) == ESP_OK, "window add user");
        CHECK(ring_chat_add_assistant(a) == ESP_OK, "window add assistant");
    }

    n = test_build_messages(buf, sizeof(buf));
    CHECK(n > 0, "window length");

    /* Only the last 2 turns (u3/a3, u4/a4) are kept. */
    CHECK(strstr(buf, "\"u0\"") == NULL, "window drops u0");
    CHECK(strstr(buf, "\"u1\"") == NULL, "window drops u1");
    CHECK(strstr(buf, "\"u3\"") != NULL, "window keeps u3");
    CHECK(strstr(buf, "\"u4\"") != NULL, "window keeps u4");
}

int main(void)
{
    test_empty_and_system();
    test_message_order();
    test_escaping();
    test_buffer_cap();
    test_history_storage_cap();
    test_history_window();

    printf("ring_chat build_messages: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
