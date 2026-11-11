// Offline tests for the idj plugin sandbox budget/interrupt logic (idj_sandbox.c).
// No network, no Swift — a mock host callback simulates slow and failing host calls.
//
// Build & run (from the repo root, any C11 compiler; shown for MinGW/gcc on Windows):
//   gcc -O1 -I Packages/QuickJS/Sources/CQuickJS -I Packages/QuickJS/Sources/CQuickJS/include \
//       Packages/QuickJS/Sources/CQuickJS/{quickjs,libregexp,libunicode,cutils,xsum,idj_sandbox}.c \
//       Packages/QuickJS/Tests/idj_sandbox_tests.c -lm -lpthread -o /tmp/idj_sandbox_tests
//   /tmp/idj_sandbox_tests
//
// Exit code 0 = all checks passed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "idj_sandbox.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("ok   - %s\n", msg); } \
    else { printf("FAIL - %s (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } \
} while (0)

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void busy_wait(double secs) {
    double end = now_s() + secs;
    while (now_s() < end) { /* spin: a stand-in for a slow host network call */ }
}

static char *dupstr(const char *s) {
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static int host_slow_calls = 0;

/// Mock host: "slow" burns 1.5s of wall clock (like a slow HTTP request), "fail" returns a
/// {"code","message"} error, anything else echoes the args JSON back.
static char *mock_host(void *opaque, const char *name, const char *args, int *is_error) {
    (void)opaque;
    if (strcmp(name, "slow") == 0) {
        host_slow_calls++;
        busy_wait(1.5);
        return dupstr("{\"ok\":true}");
    }
    if (strcmp(name, "fail") == 0) {
        *is_error = 1;
        return dupstr("{\"code\":\"E_HTTP\",\"message\":\"boom 500\"}");
    }
    return dupstr(args ? args : "null");
}

static int err_has(const char *err, const char *needle) {
    return err && strstr(err, needle) != NULL;
}

/// A JS busy loop of roughly `secs` seconds of *script* execution (calibrated coarsely;
/// only used where the exact duration does not matter, see the individual tests).
static const char *SPIN_FOREVER = "while (1) {}";
/// ~long but finite script task (a few hundred ms of JS on any machine built -O1).
/// Wrapped in a block: top-level `let` would leak into the global lexical environment and
/// collide with the next idj_eval_script on the same box.
static const char *LONG_TASK =
    "{ let s = 0; for (let i = 0; i < 30000000; i++) { s += i % 7; } s; }";

static void test_dead_loop_times_out(IDJBox *b) {
    char *err = NULL;
    double t0 = now_s();
    char *r = idj_eval_script(b, SPIN_FOREVER, 0.3, &err);
    double dt = now_s() - t0;
    CHECK(r == NULL, "dead loop: no result");
    CHECK(err_has(err, "E_TIMEOUT"), "dead loop: error code is E_TIMEOUT");
    CHECK(dt < 5.0, "dead loop: interrupted promptly (did not hang)");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);
}

static void test_zero_and_negative_timeout_disable_deadline(IDJBox *b) {
    char *err = NULL;
    char *r = idj_eval_script(b, LONG_TASK, 0, &err);
    CHECK(r != NULL, "timeout_sec=0: long task runs to completion");
    CHECK(err == NULL, "timeout_sec=0: no error");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    err = NULL;
    r = idj_eval_script(b, LONG_TASK, -2.5, &err);
    CHECK(r != NULL, "timeout_sec<0: long task runs to completion");
    CHECK(err == NULL, "timeout_sec<0: no error");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);
}

static void test_slow_host_call_not_billed_to_script(IDJBox *b) {
    // 1.5s of host time + a little JS, budget 1.0s: must succeed because only JS time counts.
    char *err = NULL;
    char *r = idj_eval_script(b,
        "{ const h = JSON.parse(__host('slow', '{}'));"
        "  let s = 0; for (let i = 0; i < 1000000; i++) { s += i % 5; }"
        "  ({ host: h.ok, sum: s }); }",
        1.0, &err);
    CHECK(host_slow_calls > 0, "slow host call happened");
    CHECK(r != NULL, "slow host call + short JS within budget: success");
    CHECK(r && strstr(r, "\"host\":true"), "slow host call: result value correct");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);
}

static void test_budget_resumes_after_host_call(IDJBox *b) {
    // After the host call returns, the remaining budget still applies: dead loop → E_TIMEOUT.
    char *err = NULL;
    char *r = idj_eval_script(b,
        "JSON.parse(__host('slow', '{}')); while (1) {}",
        0.5, &err);
    CHECK(r == NULL, "dead loop after host call: no result");
    CHECK(err_has(err, "E_TIMEOUT"), "dead loop after host call: E_TIMEOUT (budget resumed)");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);
}

static void test_host_error_code_preserved(IDJBox *b) {
    char *err = NULL;
    char *r = idj_eval_script(b, "JSON.parse(__host('fail', '{}'))", 1.0, &err);
    CHECK(r == NULL, "host error: no result");
    CHECK(err_has(err, "E_HTTP"), "host error: code E_HTTP preserved");
    CHECK(err_has(err, "boom 500"), "host error: message preserved");
    CHECK(!err_has(err, "E_TIMEOUT"), "host error: not misreported as timeout");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);
}

static void test_plain_exception_not_timeout(IDJBox *b) {
    char *err = NULL;
    char *r = idj_eval_script(b, "throw new Error('kaput')", 1.0, &err);
    CHECK(r == NULL, "plain throw: no result");
    CHECK(err_has(err, "E_INTERNAL"), "plain throw: E_INTERNAL");
    CHECK(err_has(err, "kaput"), "plain throw: message preserved");
    CHECK(!err_has(err, "E_TIMEOUT"), "plain throw: not misreported as timeout");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);
}

static void test_never_settling_promise_is_E_INTERNAL(IDJBox *b) {
    char *err = NULL;
    double t0 = now_s();
    char *r = idj_eval_script(b, "new Promise(() => {})", 5.0, &err);
    double dt = now_s() - t0;
    CHECK(r == NULL, "never-settling promise: no result");
    CHECK(err_has(err, "E_INTERNAL"), "never-settling promise: E_INTERNAL");
    CHECK(dt < 5.0, "never-settling promise: returns instead of hanging");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);
}

static const char *PLUGIN_SRC =
    "export default {\n"
    "  async slowThenWork() {\n"
    "    const h = JSON.parse(__host('slow', '{}'));\n"
    "    let s = 0; for (let i = 0; i < 1000000; i++) { s += i % 5; }\n"
    "    return { host: h.ok, sum: s };\n"
    "  },\n"
    "  async failCall() { return JSON.parse(__host('fail', '{}')); },\n"
    "  spin() { while (1) {} },\n"
    "  add(a) { return (a.x || 0) + (a.y || 0); }\n"
    "};\n";

static IDJBox *load_plugin_box(void) {
    IDJBox *b = idj_new(64 * 1024 * 1024, 1024 * 1024, mock_host, NULL);
    if (!b) return NULL;
    char *err = NULL;
    if (idj_load(b, "", PLUGIN_SRC, "plugin.js", 5.0, &err) != 0) {
        printf("FAIL - plugin load: %s\n", err ? err : "(no error)");
        failures++;
        if (err) idj_str_free(err);
        idj_free(b);
        return NULL;
    }
    return b;
}

static void test_idj_load_excludes_host_time(void) {
    // Module top level makes a 1.5s host call while the load budget is 1.0s: must still load.
    IDJBox *b = idj_new(64 * 1024 * 1024, 1024 * 1024, mock_host, NULL);
    CHECK(b != NULL, "idj_load test: box created");
    if (!b) return;
    char *err = NULL;
    int rc = idj_load(b, "",
        "const h = JSON.parse(__host('slow', '{}'));\n"
        "export default { ok() { return h.ok; } };\n",
        "plugin.js", 1.0, &err);
    CHECK(rc == 0, "idj_load: host time excluded from load budget");
    CHECK(err == NULL, "idj_load: no error");
    if (err) idj_str_free(err);
    if (rc == 0) {
        err = NULL;
        char *r = idj_call(b, "ok", "null", 1.0, &err);
        CHECK(r && strcmp(r, "true") == 0, "idj_load: plugin usable after slow-host load");
        if (r) idj_str_free(r);
        if (err) idj_str_free(err);
    }
    idj_free(b);
}

static void test_idj_call_paths(void) {
    IDJBox *b = load_plugin_box();
    CHECK(b != NULL, "plugin loads (idj_load within budget)");
    if (!b) return;

    CHECK(idj_has(b, "add") == 1, "idj_has finds exported function");
    CHECK(idj_has(b, "nope") == 0, "idj_has rejects missing function");

    // Normal call.
    char *err = NULL;
    char *r = idj_call(b, "add", "{\"x\":2,\"y\":3}", 1.0, &err);
    CHECK(r && strcmp(r, "5") == 0, "idj_call add: result is 5");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    // Unimplemented function → E_UNSUPPORTED.
    err = NULL;
    r = idj_call(b, "nope", "null", 1.0, &err);
    CHECK(r == NULL, "missing function: no result");
    CHECK(err_has(err, "E_UNSUPPORTED"), "missing function: E_UNSUPPORTED");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    // Slow host call inside idj_call: 1.5s host time, budget 1.0s → must still succeed.
    err = NULL;
    r = idj_call(b, "slowThenWork", "null", 1.0, &err);
    CHECK(r != NULL, "idj_call slowThenWork: host time excluded from budget");
    CHECK(r && strstr(r, "\"host\":true"), "idj_call slowThenWork: result value correct");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    // Host error inside idj_call keeps its code.
    err = NULL;
    r = idj_call(b, "failCall", "null", 1.0, &err);
    CHECK(r == NULL, "idj_call failCall: no result");
    CHECK(err_has(err, "E_HTTP"), "idj_call failCall: E_HTTP preserved");
    CHECK(!err_has(err, "E_TIMEOUT"), "idj_call failCall: not misreported as timeout");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    // Dead loop inside idj_call → E_TIMEOUT.
    err = NULL;
    r = idj_call(b, "spin", "null", 0.3, &err);
    CHECK(r == NULL, "idj_call spin: no result");
    CHECK(err_has(err, "E_TIMEOUT"), "idj_call spin: E_TIMEOUT");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    // The box is still usable after a timeout (deadline was disarmed).
    err = NULL;
    r = idj_call(b, "add", "{\"x\":1,\"y\":1}", 1.0, &err);
    CHECK(r && strcmp(r, "2") == 0, "idj_call works again after a timeout");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    // idj_call with timeout_sec=0: long task not interrupted.
    err = NULL;
    r = idj_call(b, "add", "{\"x\":4,\"y\":4}", 0, &err);
    CHECK(r && strcmp(r, "8") == 0, "idj_call timeout_sec=0: no deadline");
    if (r) idj_str_free(r);
    if (err) idj_str_free(err);

    idj_free(b);
}

int main(void) {
    IDJBox *b = idj_new(64 * 1024 * 1024, 1024 * 1024, mock_host, NULL);
    if (!b) { printf("FAIL - idj_new\n"); return 1; }

    test_dead_loop_times_out(b);
    test_zero_and_negative_timeout_disable_deadline(b);
    test_slow_host_call_not_billed_to_script(b);
    test_budget_resumes_after_host_call(b);
    test_host_error_code_preserved(b);
    test_plain_exception_not_timeout(b);
    test_never_settling_promise_is_E_INTERNAL(b);
    idj_free(b);

    test_idj_load_excludes_host_time();
    test_idj_call_paths();

    if (failures) { printf("\n%d check(s) FAILED\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
