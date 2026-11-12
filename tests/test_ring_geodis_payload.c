/*
 * Host-side unit tests for ring_geodis's request payload builders.
 *
 * These three builders are where a wrong byte costs a whole turn: the GA
 * endpoint 400s on a top-level `modalities`, the gateway 500s when `arguments`
 * is an object rather than a string, and a silently truncated body is rejected
 * with an error that names nothing useful. The component cannot run on the
 * host, but these functions are pure — so they are compiled here and asserted
 * byte for byte.
 *
 * The assertions compare exact strings on purpose: field names and nesting are
 * the contract with the server, and "looks about right" is how the
 * top-level-modalities bug happened the first time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ring_geodis_payload.h"

/* Mirrors RING_GEODIS_PROMPT_MAX in ring_geodis_priv.h, which cannot be
 * included here (it pulls in ESP-IDF headers). The capacity test below keeps
 * the two in step. */
#define PROMPT_MAX_UNDER_TEST 512

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

#define CHECK_STR(got, want, msg)                                     \
    do {                                                              \
        checks++;                                                     \
        if (strcmp((got), (want)) != 0) {                             \
            failures++;                                               \
            printf("FAIL %s:%d: %s\n  got:  %s\n  want: %s\n",        \
                   __FILE__, __LINE__, (msg), (got), (want));         \
        }                                                             \
    } while (0)

/* ------------------------------------------------------- session.update */

/* The endpoint rejects the legacy top-level shape with `unknown_parameter`.
 * Catch that regression without needing a gateway. */
static void test_session_update_rejects_legacy_shape(void)
{
    char buf[RING_GEODIS_SESSION_UPDATE_CAP];
    CHECK(ring_geodis_payload_session_update(buf, sizeof(buf), "coral", true, true),
          "full session.update fits");

    /* Top-level keys are the bug the Phase-0 spike found. */
    CHECK(strstr(buf, "\"modalities\"") == NULL, "no top-level modalities");
    CHECK(strstr(buf, "\"turn_detection\"") == NULL ||
          strstr(buf, "\"audio\"") < strstr(buf, "\"turn_detection\""),
          "turn_detection is inside audio");

    CHECK(strncmp(buf, "{\"type\":\"session.update\",\"session\":{\"audio\":{",
                  strlen("{\"type\":\"session.update\",\"session\":{\"audio\":{")) == 0,
          "leading shape");
}

static void test_session_update_all_flags(void)
{
    char buf[RING_GEODIS_SESSION_UPDATE_CAP];

    CHECK(ring_geodis_payload_session_update(buf, sizeof(buf), "coral", true, true),
          "stt+ptt+voice");
    CHECK_STR(buf,
              "{\"type\":\"session.update\",\"session\":{\"audio\":{"
              "\"input\":{\"transcription\":{\"model\":\"whisper-1\"},"
              "\"turn_detection\":null},"
              "\"output\":{\"voice\":\"coral\"}}}}",
              "stt+ptt+voice exact");
}

static void test_session_update_flag_combinations(void)
{
    char buf[RING_GEODIS_SESSION_UPDATE_CAP];

    /* STT only: no turn_detection -> server VAD stays on. */
    CHECK(ring_geodis_payload_session_update(buf, sizeof(buf), "coral", true, false),
          "stt only");
    CHECK(strstr(buf, "\"transcription\"") != NULL, "stt only: transcription");
    CHECK(strstr(buf, "\"turn_detection\"") == NULL, "stt only: no turn_detection");
    CHECK(strstr(buf, "\"voice\":\"coral\"") != NULL, "stt only: voice");

    /* PTT only: turn_detection null, no transcription. */
    CHECK(ring_geodis_payload_session_update(buf, sizeof(buf), "coral", false, true),
          "ptt only");
    CHECK(strstr(buf, "\"transcription\"") == NULL, "ptt only: no transcription");
    CHECK(strstr(buf, "\"turn_detection\":null") != NULL, "ptt only: turn_detection");
    CHECK(strstr(buf, "\"voice\":\"coral\"") != NULL, "ptt only: voice");

    /* Neither: the input block is omitted entirely, not emitted empty. A
     * `"input":{}` is accepted but pointless, and an empty one is the kind of
     * thing that later grows a stray comma. */
    CHECK(ring_geodis_payload_session_update(buf, sizeof(buf), "coral", false, false),
          "no flags");
    CHECK(strstr(buf, "\"input\"") == NULL, "no flags: no input block");
    CHECK_STR(buf,
              "{\"type\":\"session.update\",\"session\":{\"audio\":{"
              "\"output\":{\"voice\":\"coral\"}}}}",
              "no flags exact");
}

static void test_session_update_voice_optional(void)
{
    char buf[RING_GEODIS_SESSION_UPDATE_CAP];

    /* NULL and "" both mean "leave the server's voice alone". */
    CHECK(ring_geodis_payload_session_update(buf, sizeof(buf), NULL, true, true),
          "null voice");
    CHECK(strstr(buf, "\"output\"") == NULL, "null voice: no output block");
    CHECK(strstr(buf, "\"transcription\"") != NULL, "null voice: keeps input");

    CHECK(ring_geodis_payload_session_update(buf, sizeof(buf), "", true, true),
          "empty voice");
    CHECK(strstr(buf, "\"output\"") == NULL, "empty voice: no output block");

    /* Exact shape when there is no trailing comma to worry about. */
    CHECK_STR(buf,
              "{\"type\":\"session.update\",\"session\":{\"audio\":{"
              "\"input\":{\"transcription\":{\"model\":\"whisper-1\"},"
              "\"turn_detection\":null}}}}",
              "no voice exact");
}

static void test_session_update_overflow(void)
{
    char small[64];

    /* Too small: must report false, never produce a truncated-but-plausible
     * event (the callers turn this into ERROR rather than sending it). */
    CHECK(!ring_geodis_payload_session_update(small, sizeof(small), "coral",
                                              true, true),
          "small buffer rejected");

    /* Even a buffer that only fits the opening must fail, not write past it. */
    char tiny[8];
    CHECK(!ring_geodis_payload_session_update(tiny, sizeof(tiny), "coral",
                                              true, true),
          "tiny buffer rejected");
    CHECK(tiny[sizeof(tiny) - 1] == '\0', "tiny buffer NUL-terminated");
}

/* --------------------------------------------------------- token body */

static void test_token_body_exact(void)
{
    char buf[RING_GEODIS_TOKEN_BODY_CAP];

    CHECK(ring_geodis_payload_token_body(buf, sizeof(buf), "chat-abc",
                                         "Tu es un agent GEODIS."),
          "token body with prompt");
    CHECK_STR(buf,
              "{\"chat_session_id\":\"chat-abc\","
              "\"system_prompt\":\"Tu es un agent GEODIS.\"}",
              "token body exact");
}

static void test_token_body_prompt_optional(void)
{
    char buf[RING_GEODIS_TOKEN_BODY_CAP];

    /* An empty system_prompt must be omitted, not sent as "": the gateway
     * treats an explicit empty prompt as "no instructions at all" and the
     * assistant loses its persona silently. */
    CHECK(ring_geodis_payload_token_body(buf, sizeof(buf), "chat-abc", ""),
          "empty prompt");
    CHECK_STR(buf, "{\"chat_session_id\":\"chat-abc\"}", "empty prompt omitted");

    CHECK(ring_geodis_payload_token_body(buf, sizeof(buf), "chat-abc", NULL),
          "null prompt");
    CHECK_STR(buf, "{\"chat_session_id\":\"chat-abc\"}", "null prompt omitted");
}

/* The reported bug: a French prompt full of quotes and newlines escapes to
 * nearly twice its length. The old fixed 768-byte buffer truncated the body,
 * the POST went out malformed, and the only log was "token: HTTP 400". */
static void test_token_body_worst_case_escapes(void)
{
    char *prompt = malloc(PROMPT_MAX_UNDER_TEST + 1);
    CHECK(prompt != NULL, "alloc prompt");
    if (prompt == NULL) {
        return;
    }
    /* Every byte is a quote: escaped to `\"`, exactly 2x. */
    memset(prompt, '"', PROMPT_MAX_UNDER_TEST);
    prompt[PROMPT_MAX_UNDER_TEST] = '\0';

    char *buf = malloc(RING_GEODIS_TOKEN_BODY_CAP);
    CHECK(buf != NULL, "alloc token buf");
    if (buf != NULL) {
        CHECK(ring_geodis_payload_token_body(buf, RING_GEODIS_TOKEN_BODY_CAP,
                                             "chat-abc", prompt),
              "worst-case escaped prompt fits");
        CHECK(strlen(buf) < RING_GEODIS_TOKEN_BODY_CAP, "worst case in bounds");
        /* The body must still end with the closing brace (not truncated
         * mid-string). */
        size_t n = strlen(buf);
        CHECK(n >= 2 && buf[n - 1] == '}' && buf[n - 2] == '"', "worst case closed");
        free(buf);
    }
    free(prompt);
}

static void test_token_body_overflow(void)
{
    char small[40];
    CHECK(!ring_geodis_payload_token_body(small, sizeof(small), "chat-abc",
                                          "un prompt beaucoup trop long pour ce buffer"),
          "small token buffer rejected");
}

/* ---------------------------------------------------------- tool body */

static void test_tool_body_arguments_is_string(void)
{
    char buf[512];

    /* `arguments` arrives as JSON *text*. It must be embedded as a string
     * (quotes doubled by escaping), never spliced in raw — the gateway treats
     * a raw object as the wrong type and returns 500. */
    CHECK(ring_geodis_payload_tool_body(buf, sizeof(buf), "rechercher_base_geodis",
                                        "{\"numero\":\"12345\"}", "sess-1"),
          "tool body");
    CHECK_STR(buf,
              "{\"name\":\"rechercher_base_geodis\","
              "\"arguments\":\"{\\\"numero\\\":\\\"12345\\\"}\","
              "\"session_id\":\"sess-1\"}",
              "tool body exact");
}

static void test_tool_body_escaping(void)
{
    char buf[512];

    /* Quotes, backslashes and UTF-8 in the args must survive: the args are
     * often a French shipment reference or a name with an accent. */
    CHECK(ring_geodis_payload_tool_body(buf, sizeof(buf), "rechercher_base_geodis",
                                        "{\"q\":\"L\\\"Hermitage\"}", "s1"),
          "quotes+backslash");
    CHECK(strstr(buf, "\\\"q\\\"") != NULL, "inner quotes escaped");
    CHECK(strstr(buf, "L\\\\\\\"Hermitage") != NULL, "backslash-quote escaped");

    CHECK(ring_geodis_payload_tool_body(buf, sizeof(buf), "n",
                                        "{\"ville\":\"Besançon\"}", "s"),
          "utf-8 args");
    CHECK(strstr(buf, "Besan") != NULL, "utf-8 prefix present");
}

static void test_tool_body_defaults(void)
{
    char buf[256];

    /* Absent args/name must not produce `null` in the JSON. */
    CHECK(ring_geodis_payload_tool_body(buf, sizeof(buf), NULL, NULL, NULL),
          "nulls accepted");
    CHECK(strstr(buf, "\"name\":\"\"") != NULL, "null name -> empty string");
    CHECK(strstr(buf, "\"arguments\":\"{}\"") != NULL, "null args -> {}");
    CHECK(strstr(buf, "null") == NULL, "no literal null in body");
}

static void test_tool_body_overflow(void)
{
    char small[32];
    CHECK(!ring_geodis_payload_tool_body(small, sizeof(small), "n", "{}", "s"),
          "small tool buffer rejected");
}

/* ---------------------------------------------------- tool output item */

static void test_tool_output_exact(void)
{
    char buf[512];

    CHECK(ring_geodis_payload_tool_output(buf, sizeof(buf), "call-7",
                                          "{\"statut\":\"en transit\"}"),
          "tool output fits");
    CHECK_STR(buf,
              "{\"type\":\"conversation.item.create\",\"item\":"
              "{\"type\":\"function_call_output\",\"call_id\":\"call-7\","
              "\"output\":\"{\\\"statut\\\":\\\"en transit\\\"}\"}}",
              "tool output exact");
}

/* An output that does not fit must degrade to a short, *valid* error item.
 * The old code sent a truncated JSON event, which the model cannot parse, and
 * the turn simply hung. */
static void test_tool_output_falls_back(void)
{
    char buf[200];
    char big[400];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    CHECK(ring_geodis_payload_tool_output(buf, sizeof(buf), "call-7", big),
          "fallback produced");
    CHECK(strstr(buf, "lookup_unavailable") != NULL, "fallback names the error");
    CHECK(strstr(buf, "response_too_large") != NULL, "fallback has detail");
    CHECK(strstr(buf, "call-7") != NULL, "fallback keeps call_id");
    CHECK(strstr(buf, "xxxx") == NULL, "fallback drops the oversized payload");
    CHECK(buf[strlen(buf) - 1] == '}', "fallback is closed JSON");
    CHECK(strstr(buf, "\"type\":\"function_call_output\"") != NULL,
          "fallback is still a function_call_output");
}

static void test_tool_output_hopeless_buffer(void)
{
    char tiny[16];
    /* Smaller than even the fallback: report false so the caller logs and
     * drops, rather than sending half an item. */
    CHECK(!ring_geodis_payload_tool_output(tiny, sizeof(tiny), "call-7",
                                           "{}"),
          "hopeless buffer rejected");
}

/* ------------------------------------------------------------ constants */

static void test_capacity_constants(void)
{
    /* tests/Makefile cannot see ring_geodis_priv.h (it pulls in ESP-IDF
     * headers), so the prompt limit is duplicated. Guard the copy. */
    CHECK(RING_GEODIS_TOKEN_BODY_CAP == PROMPT_MAX_UNDER_TEST * 2 + 320,
          "token cap formula");
    CHECK(RING_GEODIS_SESSION_UPDATE_CAP >= 256, "session.update cap sane");
}

int main(void)
{
    test_session_update_rejects_legacy_shape();
    test_session_update_all_flags();
    test_session_update_flag_combinations();
    test_session_update_voice_optional();
    test_session_update_overflow();
    test_token_body_exact();
    test_token_body_prompt_optional();
    test_token_body_worst_case_escapes();
    test_token_body_overflow();
    test_tool_body_arguments_is_string();
    test_tool_body_escaping();
    test_tool_body_defaults();
    test_tool_body_overflow();
    test_tool_output_exact();
    test_tool_output_falls_back();
    test_tool_output_hopeless_buffer();
    test_capacity_constants();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
