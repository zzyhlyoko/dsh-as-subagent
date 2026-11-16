/* Boundary smoke tests for scripts/dsh-subagent.exe.
 *
 * gcc -municode -O2 -s -static-libgcc -o smoke.exe smoke.c
 * smoke.exe <repo> <fake-node.exe>
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
static const char *g_why;
static wchar_t g_install[MAX_PATH];
static wchar_t g_exe[MAX_PATH];
static wchar_t g_missing_dir[MAX_PATH];
static wchar_t g_not_dir[MAX_PATH];
static wchar_t g_child_cwd[MAX_PATH];
static wchar_t g_user_patch[MAX_PATH];
static wchar_t g_missing_patch[MAX_PATH];
static wchar_t g_saved_path[32768];
static char g_user_patch_utf8[MAX_PATH * 3];
static char g_missing_patch_utf8[MAX_PATH * 3];
static char g_child_cwd_utf8[MAX_PATH * 3];
static char g_version_out[160];
static wchar_t g_empty_dsh[MAX_PATH];
static wchar_t g_web_dsh[MAX_PATH];
static wchar_t g_portable_shim[MAX_PATH];
static wchar_t g_portable_node[MAX_PATH];
static wchar_t g_portable_bin[MAX_PATH];
static wchar_t g_portable_home[MAX_PATH];
static wchar_t g_broken_shim[MAX_PATH];
static char g_portable_node_utf8[MAX_PATH * 3];
static char g_portable_bin_utf8[MAX_PATH * 3];
static char g_portable_home_utf8[MAX_PATH * 3];

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) ExitProcess(2);
    return p;
}

static char *wide_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static wchar_t *utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

typedef struct { wchar_t *p; int n, cap; } WBuf;
static void wbuf_ch(WBuf *b, wchar_t c) {
    if (b->n + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 128;
        b->p = (wchar_t *)realloc(b->p, (size_t)b->cap * sizeof(wchar_t));
    }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}
static void wbuf_s(WBuf *b, const wchar_t *s) {
    while (*s) wbuf_ch(b, *s++);
}
static void wbuf_arg(WBuf *b, const wchar_t *arg) {
    if (b->n) wbuf_ch(b, L' ');
    int quote = (*arg == 0);
    const wchar_t *p;
    for (p = arg; *p; p++) if (*p == L' ' || *p == L'\t' || *p == L'"') quote = 1;
    if (!quote) { wbuf_s(b, arg); return; }
    wbuf_ch(b, L'"');
    for (;;) {
        unsigned bs = 0;
        while (*arg == L'\\') { arg++; bs++; }
        if (!*arg) { while (bs--) { wbuf_ch(b, L'\\'); wbuf_ch(b, L'\\'); } break; }
        if (*arg == L'"') {
            while (bs--) { wbuf_ch(b, L'\\'); wbuf_ch(b, L'\\'); }
            wbuf_ch(b, L'\\'); wbuf_ch(b, L'"');
        } else {
            while (bs--) wbuf_ch(b, L'\\');
            wbuf_ch(b, *arg);
        }
        arg++;
    }
    wbuf_ch(b, L'"');
}

typedef struct { HANDLE h; char *p; int n; } ReadJob;

static DWORD WINAPI read_thread(LPVOID param) {
    ReadJob *b = (ReadJob *)param;
    size_t cap = 256, n = 0;
    char *p = (char *)xmalloc(cap);
    for (;;) {
        while (n + 4096 + 1 > cap) { cap *= 2; p = (char *)realloc(p, cap); }
        DWORD got = 0;
        if (!ReadFile(b->h, p + n, 4096, &got, NULL) || got == 0) break;
        n += got;
    }
    p[n] = 0;
    b->p = p;
    b->n = (int)n;
    return 0;
}

typedef struct {
    int code;
    char *out;
    char *err;
    char *log;
    DWORD ms;
} Result;

static Result run_exe(const wchar_t *arg_cmd, const char *stdin_utf8, const char *scenario, int fails, int layout, int web_home, int clear_home) {
    Result r;
    memset(&r, 0, sizeof r);
    wchar_t case_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, case_dir);
    wchar_t unique[64];
    swprintf(unique, 64, L"dsh-case-%lu-%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    wchar_t dir[MAX_PATH];
    wcscpy(dir, case_dir);
    if (dir[wcslen(dir) - 1] != L'\\') wcscat(dir, L"\\");
    wcscat(dir, unique);
    CreateDirectoryW(dir, NULL);
    wchar_t work[MAX_PATH], log[MAX_PATH], counter[MAX_PATH];
    wcscpy(work, dir); wcscat(work, L"\\work");
    wcscpy(log, dir); wcscat(log, L"\\log.ndjson");
    wcscpy(counter, dir); wcscat(counter, L"\\counter.txt");
    CreateDirectoryW(work, NULL);

    wchar_t path_buf[32768];
    if (layout == 1) wcscpy(path_buf, L"C:\\Windows\\System32");
    else if (layout == 2) {
        wcscpy(path_buf, g_portable_shim);
        wcscat(path_buf, L";C:\\Windows\\System32");
    } else if (layout == 3) {
        wcscpy(path_buf, g_broken_shim);
        wcscat(path_buf, L";C:\\Windows\\System32");
    } else { wcscpy(path_buf, g_install); wcscat(path_buf, L";"); wcscat(path_buf, g_saved_path); }
    SetEnvironmentVariableW(L"PATH", path_buf);
    SetEnvironmentVariableW(L"DSH_FAKE_LOG", log);
    SetEnvironmentVariableW(L"DSH_FAKE_COUNTER", counter);
    SetEnvironmentVariableW(L"DSH_FAKE_SCENARIO", utf8_to_wide(scenario ? scenario : "ok"));
    wchar_t fails_w[16];
    swprintf(fails_w, 16, L"%d", fails);
    SetEnvironmentVariableW(L"DSH_FAKE_FAILS", fails_w);
    if (clear_home) SetEnvironmentVariableW(L"DSH_HOME", NULL);
    else SetEnvironmentVariableW(L"DSH_HOME", web_home ? g_web_dsh : g_empty_dsh);

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r, in_w, out_r, out_w, err_r, err_w;
    CreatePipe(&out_r, &out_w, &sa, 0);
    CreatePipe(&err_r, &err_w, &sa, 0);
    CreatePipe(&in_r, &in_w, &sa, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    WBuf cmd;
    memset(&cmd, 0, sizeof cmd);
    wbuf_arg(&cmd, g_exe);
    if (arg_cmd && arg_cmd[0]) { wbuf_ch(&cmd, L' '); wbuf_s(&cmd, arg_cmd); }

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    SIZE_T attr = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)xmalloc(attr);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr);
    HANDLE inherited[3] = {in_r, out_w, err_w};
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, NULL, NULL);

    wchar_t *cmdline = (wchar_t *)xmalloc((wcslen(cmd.p) + 1) * sizeof(wchar_t));
    wcscpy(cmdline, cmd.p);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    DWORD t0 = GetTickCount();
    BOOL ok = CreateProcessW(g_exe, cmdline, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                             NULL, work, &si.StartupInfo, &pi);
    free(cmdline);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    CloseHandle(in_r); CloseHandle(out_w); CloseHandle(err_w);
    if (!ok) {
        r.code = -1;
        r.out = _strdup("");
        r.err = _strdup("CreateProcess failed");
        r.log = _strdup("");
        CloseHandle(in_w); CloseHandle(out_r); CloseHandle(err_r);
        return r;
    }
    ReadJob ob, eb;
    memset(&ob, 0, sizeof ob);
    memset(&eb, 0, sizeof eb);
    ob.h = out_r;
    eb.h = err_r;
    HANDLE th1 = CreateThread(NULL, 0, read_thread, &ob, 0, NULL);
    HANDLE th2 = CreateThread(NULL, 0, read_thread, &eb, 0, NULL);
    if (stdin_utf8 && stdin_utf8[0]) {
        DWORD wr = 0;
        WriteFile(in_w, stdin_utf8, (DWORD)strlen(stdin_utf8), &wr, NULL);
    }
    CloseHandle(in_w);
    WaitForSingleObject(th1, INFINITE);
    WaitForSingleObject(th2, INFINITE);
    CloseHandle(th1); CloseHandle(th2);
    CloseHandle(out_r); CloseHandle(err_r);
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code == STILL_ACTIVE) { TerminateProcess(pi.hProcess, 1); code = 1; }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    r.ms = GetTickCount() - t0;
    r.code = (int)code;
    r.out = ob.p;
    r.err = eb.p;

    HANDLE lh = CreateFileW(log, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lh == INVALID_HANDLE_VALUE) r.log = _strdup("");
    else {
        DWORD sz = GetFileSize(lh, NULL);
        char *buf = (char *)xmalloc(sz + 1);
        DWORD got = 0;
        ReadFile(lh, buf, sz, &got, NULL);
        buf[got] = 0;
        r.log = buf;
        CloseHandle(lh);
    }
    free(cmd.p);
    return r;
}

static void free_result(Result *r) {
    free(r->out); free(r->err); free(r->log);
}

static int hex_n(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *decode_string(const char **pp) {
    const char *p = *pp;
    if (*p == 'n' && strncmp(p, "null", 4) == 0) { *pp = p + 4; return NULL; }
    if (*p != '"') return NULL;
    p++;
    size_t cap = 64, n = 0;
    char *o = (char *)xmalloc(cap);
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == 'u') {
                int h0 = hex_n(p[0]), h1 = hex_n(p[1]), h2 = hex_n(p[2]), h3 = hex_n(p[3]);
                unsigned code = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                p += 4;
                char tmp[4];
                wchar_t w = (wchar_t)code;
                int kn = WideCharToMultiByte(CP_UTF8, 0, &w, 1, tmp, 4, NULL, NULL);
                if (n + (size_t)kn + 1 > cap) { cap *= 2; o = (char *)realloc(o, cap); }
                memcpy(o + n, tmp, (size_t)kn);
                n += (size_t)kn;
                continue;
            } else c = e;
        }
        if (n + 2 > cap) { cap *= 2; o = (char *)realloc(o, cap); }
        o[n++] = c;
    }
    if (*p == '"') p++;
    o[n] = 0;
    *pp = p;
    return o;
}

static const char *after_key(const char *json, const char *key) {
    if (!json) return NULL;
    char pat[80];
    sprintf(pat, "\"%s\":", key);
    const char *last = NULL;
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        last = p;
        p += strlen(pat);
    }
    return last ? last + strlen(pat) : NULL;
}

static char *field_string(const char *json, const char *key) {
    const char *p = after_key(json, key);
    if (!p) return NULL;
    return decode_string(&p);
}

static int field_is_null(const char *json, const char *key) {
    const char *p = after_key(json, key);
    return p && strncmp(p, "null", 4) == 0;
}

static int line_count(const char *log) {
    int n = 0;
    const char *p = log;
    if (!p || !*p) return 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (!nl) { if (*p) n++; break; }
        if (nl > p) n++;
        p = nl + 1;
    }
    return n;
}

static const char *nth_line(const char *log, int index) {
    int n = 0;
    const char *p = log;
    while (*p) {
        if (n == index) return p;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
        n++;
    }
    return NULL;
}

static int contains(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

static char g_why_buf[192];
static void require(int cond, const char *msg) {
    if (!cond && !g_why) {
        strncpy(g_why_buf, msg ? msg : "failed", sizeof g_why_buf - 1);
        g_why_buf[sizeof g_why_buf - 1] = 0;
        g_why = g_why_buf;
    }
}

static char *stdin_of(const Result *r, int index) {
    const char *line = nth_line(r->log, index);
    if (!line) return NULL;
    return field_string(line, "stdin");
}

static int calls(const Result *r) { return line_count(r->log); }

static int line_has(const char *log, int index, const char *frag) {
    const char *line = nth_line(log, index);
    if (!line || !frag) return 0;
    const char *nl = strchr(line, '\n');
    size_t n = nl ? (size_t)(nl - line) : strlen(line);
    size_t m = strlen(frag);
    size_t i;
    if (m == 0 || m > n) return 0;
    for (i = 0; i + m <= n; i++) if (memcmp(line + i, frag, m) == 0) return 1;
    return 0;
}

typedef struct {
    const char *name;
    wchar_t *cmd;
    const char *stdin_utf8;
    const char *scenario;
    int fails;
    int layout;
    int web_home;
    int clear_home;
    void (*check)(const Result *);
} Case;

static Case *g_cases;
static int g_case_n, g_case_cap;

static void add(const char *name, const char *cmd_utf8, const char *in, const char *scenario, int fails, int no_dsh, void (*check)(const Result *)) {
    if (g_case_n + 1 > g_case_cap) {
        g_case_cap = g_case_cap ? g_case_cap * 2 : 32;
        g_cases = (Case *)realloc(g_cases, (size_t)g_case_cap * sizeof(Case));
    }
    Case *c = &g_cases[g_case_n++];
    c->name = name;
    c->cmd = cmd_utf8 ? utf8_to_wide(cmd_utf8) : utf8_to_wide("");
    c->stdin_utf8 = in ? in : "";
    c->scenario = scenario ? scenario : "ok";
    c->fails = fails;
    c->layout = no_dsh ? 1 : 0;
    c->web_home = 0;
    c->clear_home = 0;
    c->check = check;
}

static void check_empty(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "empty task"), "stderr");
}
static void check_unknown(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "unknown option"), "stderr");
}
static void check_effort_bad(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "low/off/high/max"), "stderr");
}
static void check_version(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 0, "launched");
    require(strcmp(r->out, g_version_out) == 0, "version");
}
static void check_help(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->out, "Usage:"), "usage");
    require(contains(r->out, "--session-id"), "session flag");
    require(contains(r->out, "--permission"), "permission flag");
    require(contains(r->out, "--node"), "node flag");
    require(contains(r->out, "--dsh-bin"), "dsh-bin flag");
    require(contains(r->out, "--dsh-home"), "dsh-home flag");
}
static void check_portable(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    require(contains(r->log, "pkg dir"), "bin path keeps its space");
    require(contains(r->log, "portable-route"), "profile from the forwarder home");
    require(contains(r->err, "DSH_HOME"), "home notice");
    require(contains(r->err, "dsh-portable"), "home path");
    require(contains(r->err, "using web profile"), "web profile");
    require(!contains(r->log, "super-secret-token"), "profile secret stays out of the log");
    require(!contains(r->err, "super-secret-token"), "script secret stays off stderr");
}
static void check_explicit_install(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    require(contains(r->log, "pkg dir"), "explicit bin");
    require(contains(r->log, "portable-route"), "explicit home profile");
    require(contains(r->err, "DSH_HOME"), "home notice");
    require(!contains(r->err, "super-secret-token"), "no secret");
}
static void check_bad_node(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "--node is not a file"), "stderr");
    require(contains(r->err, "no-such-node.exe"), "path");
    require(!contains(r->err, "super-secret-token"), "no secret");
}
static void check_bad_bin(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "--dsh-bin is not a file"), "stderr");
    require(contains(r->err, "no-such-bin.js"), "path");
}
static void check_bad_home(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "--dsh-home is not a directory"), "stderr");
}
static void check_broken_forwarder(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "cannot locate dsh install"), "stderr");
    require(contains(r->err, "dsh-broken-shim"), "launcher path");
    require(contains(r->err, "--node"), "node hint");
    require(contains(r->err, "--dsh-bin"), "bin hint");
    require(!contains(r->err, "super-secret-token"), "secret not echoed");
}
static void check_node_value(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "--node requires"), "stderr");
}
static void check_no_install(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "cannot locate dsh install"), "stderr");
    require(contains(r->err, "--node"), "node hint");
    require(contains(r->err, "--dsh-bin"), "bin hint");
    require(contains(r->err, "--dsh-home"), "home hint");
}
static void check_cwd_bad(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "not a directory"), "stderr");
}
static void check_once_ok(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
}
static void check_fast_retry(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 2, "calls");
    require(r->ms < 3000, "slept");
}
static void expect_stdin(const Result *r, const char *want) {
    char *got = stdin_of(r, 0);
    require(got && strcmp(got, want) == 0, "stdin");
    free(got);
}
static void check_hello(const Result *r) { expect_stdin(r, "hello"); }
static void check_alpha(const Result *r) { expect_stdin(r, "alpha beta"); }
static void check_short(const Result *r) { expect_stdin(r, "via-short"); }
static void check_from_arg(const Result *r) { expect_stdin(r, "from-arg"); }
static void check_from_stdin(const Result *r) { expect_stdin(r, "from-stdin"); }
static void check_dash_task(const Result *r) { expect_stdin(r, "-"); }
static void check_hello_world(const Result *r) { expect_stdin(r, "hello world"); }
static void check_multi(const Result *r) { expect_stdin(r, "line1\nline2"); }
static void check_cn_task(const Result *r) { expect_stdin(r, "\xe4\xb8\xad\xe6\x96\x87\xe4\xbb\xbb\xe5\x8a\xa1"); }
static void check_long_task(const Result *r) {
    char *got = stdin_of(r, 0);
    require(got && strlen(got) == 400 && got[0] == 'L' && got[399] == 'L', "long stdin");
    free(got);
}
static void check_model_chat(const Result *r) {
    require(calls(r) == 1, "calls");
    require(contains(r->log, "provider: deepseek-official"), "provider");
    require(contains(r->log, "model: deepseek-chat"), "model");
    require(!contains(r->log, "reasoningEffort"), "no effort");
}
static void check_provider_only(const Result *r) {
    require(contains(r->log, "provider: other-route"), "provider");
    require(contains(r->log, "model: deepseek-flash"), "model");
    require(!contains(r->log, "reasoningEffort"), "no effort");
}
static void check_effort(const Result *r, const char *effort) {
    require(contains(r->log, "provider: deepseek-official"), "provider");
    require(contains(r->log, "model: deepseek-flash"), "model");
    char frag[64];
    sprintf(frag, "reasoningEffort: %s", effort);
    require(contains(r->log, frag), frag);
}
static void check_effort_high(const Result *r) { check_effort(r, "high"); }
static void check_effort_low(const Result *r) { check_effort(r, "low"); }
static void check_effort_off(const Result *r) { check_effort(r, "off"); }
static void check_effort_max(const Result *r) { check_effort(r, "max"); }
static void check_all_three(const Result *r) {
    require(contains(r->log, "provider: other-route"), "provider");
    require(contains(r->log, "model: deepseek-chat"), "model");
    require(contains(r->log, "reasoningEffort: max"), "effort");
}
static void expect_permission(const Result *r, const char *preset, const char *policy) {
    char frag[96];
    sprintf(frag, "defaultPreset: %s", preset);
    require(contains(r->log, frag), frag);
    sprintf(frag, "mode: %s", preset);
    require(contains(r->log, frag), frag);
    sprintf(frag, "policy: %s", policy);
    require(contains(r->log, frag), frag);
    require(contains(r->log, "id: sandbox-policy"), "sandbox id");
    require(contains(r->log, "id: approval"), "approval id");
    require(contains(r->log, "id: permission"), "permission id");
    require(contains(r->log, "read-only:"), "read-only preset");
    require(contains(r->log, "workspace-write:"), "workspace-write preset");
    require(contains(r->log, "danger-full-access:"), "full preset");
    sprintf(frag, "permission %s", preset);
    require(contains(r->err, frag), "notice");
}
static void check_no_patch(const Result *r) {
    require(calls(r) == 1, "calls");
    require(!contains(r->log, "agent-default-model"), "unexpected model patch");
    require(!contains(r->log, "\"patches\":[]"), "permission patch missing");
    require(!contains(r->err, "using web profile"), "no web profile");
    expect_permission(r, "read-only", "ask");
}
static void check_permission_bad(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "read-only, workspace-write, or danger-full-access"), "stderr");
}
static void check_workspace(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    expect_permission(r, "workspace-write", "ask");
}
static void check_full(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    expect_permission(r, "danger-full-access", "never");
}
static void check_permission_last(const Result *r) {
    const char *hello = strstr(r->log, "hello: true");
    const char *model = strstr(r->log, "agent-default-model");
    const char *perm = strstr(r->log, "defaultPreset: workspace-write");
    require(hello && model && perm, "pieces");
    require(hello < model && model < perm, "permission last");
    expect_permission(r, "workspace-write", "ask");
}
static void check_web_profile(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    const char *providers = strstr(r->log, "id: llm-pi-ai");
    const char *creds = strstr(r->log, "disabled: false");
    const char *hello = strstr(r->log, "hello: true");
    const char *web_default = strstr(r->log, "provider: blue-vocalno");
    const char *selected = strstr(r->log, "provider: ark");
    require(providers && creds && hello && web_default && selected, "pieces");
    require(providers < creds && creds < hello && hello < selected, "order");
    require(web_default < selected, "selection overrides web default");
    require(contains(r->log, "id: volcengine/deepseek-v4.1-flash"), "slash model");
    require(!contains(r->log, "ui-settings-general"), "ui dropped");
    require(contains(r->err, "using web profile"), "notice");
}
static void check_web_default(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    require(contains(r->log, "provider: blue-vocalno"), "web default provider");
    require(contains(r->log, "model: volcengine/deepseek-v4.1-flash"), "web default model");
    require(!contains(r->log, "deepseek-flash"), "no headless default");
    require(contains(r->log, "disabled: false"), "credentials");
    require(!contains(r->log, "ui-settings-general"), "ui dropped");
    require(contains(r->err, "using web profile"), "notice");
}
static void check_pool_bad(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "provider|model"), "stderr");
}
static void check_pool_effort_flag(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "reasoning-effort applies to --provider/--model"), "stderr");
}
static void check_pool_one(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    require(!contains(r->err, "model pool switch"), "no switch");
    require(line_has(r->log, 0, "provider: providerA"), "provider");
    require(line_has(r->log, 0, "model: model1"), "model");
    require(line_has(r->log, 0, "reasoningEffort: high"), "effort");
    require(!line_has(r->log, 0, "deepseek-flash"), "no implicit model");
}
static void check_pool_omit_effort(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    require(line_has(r->log, 0, "provider: providerA"), "provider");
    require(line_has(r->log, 0, "model: model2"), "model");
    require(!line_has(r->log, 0, "reasoningEffort"), "omitted");
}
static void check_pool_slash_model(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    require(line_has(r->log, 0, "provider: ark"), "provider");
    require(!line_has(r->log, 0, "provider: volcengine"), "slash is not the provider");
    require(line_has(r->log, 0, "model: volcengine/deepseek-v4.1-flash"), "model");
    require(line_has(r->log, 0, "reasoningEffort: high"), "effort");
}
static void check_model_slash(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 1, "calls");
    require(contains(r->log, "provider: ark"), "provider");
    require(!contains(r->log, "provider: volcengine"), "slash is not the provider");
    require(contains(r->log, "model: volcengine/deepseek-v4.1-flash"), "model");
    require(!contains(r->log, "reasoningEffort"), "omitted");
}
static void summary_is(const Result *r, int code, const char *session, const char *answer);

static void check_pool_switch(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 2, "calls");
    require(line_has(r->log, 0, "model: model1"), "first model");
    require(line_has(r->log, 0, "reasoningEffort: high"), "first effort");
    require(line_has(r->log, 0, "\"stdin\":\"t\""), "first stdin");
    require(line_has(r->log, 1, "provider: providerA"), "second provider");
    require(line_has(r->log, 1, "model: model2"), "second model");
    require(!line_has(r->log, 1, "reasoningEffort"), "second omits effort");
    require(line_has(r->log, 1, "\"stdin\":\"t\""), "second stdin");
    require(contains(r->err, "model pool switch 2/2 from providerA|model1|high to providerA|model2 after 1 failed attempts."), "switch");
    require(!contains(r->err, "fallback"), "not a fallback");
    summary_is(r, 0, "session-ok", "ok-answer");
}
static void check_pool_retry_then_switch(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 3, "calls");
    require(contains(r->err, "attempt 1/2 failed"), "retry");
    require(!contains(r->err, "attempt 2/2"), "no retry line for the switch");
    require(line_has(r->log, 0, "model: model1"), "first");
    require(line_has(r->log, 1, "model: model1"), "retry stays");
    require(!line_has(r->log, 1, "reasoningEffort"), "no effort");
    require(line_has(r->log, 2, "model: model3"), "next");
    require(line_has(r->log, 2, "reasoningEffort: max"), "next effort");
    require(contains(r->err, "model pool switch 2/2 from providerA|model1 to providerB|model3|max after 2 failed attempts."), "switch");
}
static void check_pool_exhausted(const Result *r) {
    require(r->code == 1, "exit");
    require(calls(r) == 4, "calls");
    require(line_has(r->log, 0, "model: model1"), "m1");
    require(line_has(r->log, 0, "reasoningEffort: high"), "e1");
    require(line_has(r->log, 1, "model: model2"), "m2");
    require(!line_has(r->log, 1, "reasoningEffort"), "e2");
    require(line_has(r->log, 2, "provider: providerB"), "m3 provider");
    require(line_has(r->log, 2, "model: model3"), "m3");
    require(line_has(r->log, 3, "model: model666"), "m4");
    require(line_has(r->log, 3, "reasoningEffort: low"), "e4");
    require(!contains(r->log, "deepseek-official"), "no official fallback");
    require(contains(r->err, "model pool switch 2/4 from providerA|model1|high to providerA|model2 after 1 failed attempts."), "switch2");
    require(contains(r->err, "model pool switch 3/4 from providerA|model2 to providerB|model3 after 1 failed attempts."), "switch3");
    require(contains(r->err, "model pool switch 4/4 from providerB|model3 to route|model666|low after 1 failed attempts."), "switch4");
    require(!contains(r->err, "fallback"), "not a fallback");
    summary_is(r, 1, "session-fail-4", NULL);
}
static void check_pool_primary_first(const Result *r) {
    require(r->code == 0, "exit");
    require(calls(r) == 2, "calls");
    require(line_has(r->log, 0, "provider: providerA"), "first provider");
    require(line_has(r->log, 0, "model: model1"), "first model");
    require(line_has(r->log, 0, "reasoningEffort: low"), "first effort");
    require(line_has(r->log, 1, "provider: providerB"), "second provider");
    require(line_has(r->log, 1, "model: model3"), "second model");
    require(!line_has(r->log, 1, "reasoningEffort"), "second omits effort");
    require(contains(r->err, "model pool switch 2/2 from providerA|model1|low to providerB|model3 after 1 failed attempts."), "switch");
}
static char *args_joined(const Result *r) {
    const char *line = nth_line(r->log, 0);
    const char *p = after_key(line, "args");
    if (!p || *p != '[') return _strdup("");
    p++;
    size_t cap = 128, n = 0;
    char *o = (char *)xmalloc(cap);
    o[0] = 0;
    while (*p && *p != ']') {
        if (*p == ',') { p++; continue; }
        if (*p != '"') { p++; continue; }
        char *s = decode_string(&p);
        size_t add = strlen(s) + 2;
        if (n + add + 1 > cap) { cap *= 2; o = (char *)realloc(o, cap); }
        if (n) o[n++] = '\n';
        memcpy(o + n, s, strlen(s));
        n += strlen(s);
        o[n] = 0;
        free(s);
    }
    return o;
}
static void check_shape_base(const Result *r, const char *session) {
    char *args = args_joined(r);
    require(contains(args, "--profile\nheadless\n"), "prefix");
    const char *last = strrchr(args, '\n');
    require(last && strcmp(last + 1, "-") == 0, "dash");
    if (session && session[0]) {
        require(contains(args, "\n--json\n--session-id\n"), "json before session");
        require(contains(args, session), "session");
    } else {
        require(contains(args, "\n--json\n-"), "json");
        require(!contains(args, "--session-id"), "no session");
    }
    free(args);
}
static void check_shape(const Result *r) { check_shape_base(r, ""); }
static void check_session(const Result *r) { check_shape_base(r, "session-abc"); }
static void check_session_space(const Result *r) { check_shape_base(r, "sess ion"); }
static void check_cwd(const Result *r) {
    char *cwd = field_string(nth_line(r->log, 0), "cwd");
    require(cwd && _stricmp(cwd, g_child_cwd_utf8) == 0, "cwd");
    require(contains(r->err, "dsh-subagent: cwd "), "notice");
    free(cwd);
}
static void check_cwd_mismatch(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "was recorded in"), "stderr");
}
static void check_cwd_gone(const Result *r) {
    require(r->code == 2, "exit");
    require(calls(r) == 0, "launched");
    require(contains(r->err, "which is not a directory"), "stderr");
}
static void check_caller_cwd(const Result *r) {
    char *cwd = field_string(nth_line(r->log, 0), "cwd");
    require(calls(r) == 1, "calls");
    require(cwd && strstr(cwd, "\\work") != NULL, "case work dir");
    require(_stricmp(cwd, g_child_cwd_utf8) != 0, "not the recorded dir");
    require(!contains(r->err, "dsh-subagent: cwd "), "no recorded cwd");
    free(cwd);
}
static void check_user_then_model(const Result *r) {
    check_model_chat(r);
    char *args = args_joined(r);
    char *user_at = strstr(args, g_user_patch_utf8);
    require(user_at != NULL, "user patch");
    require(strstr(user_at + 1, "--patch") != NULL, "model patch follows");
    free(args);
    require(contains(r->log, "hello: true"), "user text");
}
static void check_short_patch(const Result *r) {
    require(!contains(r->log, "agent-default-model"), "no model");
    char *args = args_joined(r);
    require(contains(args, g_user_patch_utf8), "forwarded");
    free(args);
}
static void check_missing_patch(const Result *r) {
    char *args = args_joined(r);
    require(contains(args, g_missing_patch_utf8), "path");
    free(args);
    require(contains(r->log, "\"text\":null"), "null text");
}
static void summary_is(const Result *r, int code, const char *session, const char *answer) {
    require(r->code == code, "exit");
    char num[16];
    sprintf(num, "\"exitCode\":%d", code);
    require(contains(r->out, num), "json exit");
    if (!session) require(field_is_null(r->out, "sessionId"), "session null");
    else {
        char *got = field_string(r->out, "sessionId");
        require(got && strcmp(got, session) == 0, "session");
        free(got);
    }
    if (!answer) require(field_is_null(r->out, "answer"), "answer null");
    else {
        char *got = field_string(r->out, "answer");
        require(got && strcmp(got, answer) == 0, "answer");
        free(got);
    }
}
static void check_ok_json(const Result *r) {
    summary_is(r, 0, "session-ok", "ok-answer");
    require(contains(r->out, "\"code\":\"END\""), "reason");
    require(field_is_null(r->out, "error"), "error null");
    require(contains(r->err, "dsh: reasoning: hi"), "stderr");
}
static void check_raw(const Result *r) {
    require(r->code == 0, "exit");
    require(strcmp(r->out, "ok-answer\n") == 0, "raw");
    require(!contains(r->out, "exitCode"), "not json");
}
static void check_error_exit(const Result *r) {
    summary_is(r, 1, "session-err", "partial-answer");
    char *err = field_string(r->out, "error");
    require(err && strcmp(err, "boom") == 0, "error");
    free(err);
    require(contains(r->out, "MODEL_UNSUPPORTED"), "reason");
    require(contains(r->err, "dsh: MODEL_UNSUPPORTED: boom"), "stderr");
}
static void check_raw_fail(const Result *r) {
    require(r->code == 1, "exit");
    require(strcmp(r->out, "partial-answer\n") == 0, "raw");
}
static void check_empty_ok(const Result *r) {
    summary_is(r, 0, NULL, NULL);
    require(field_is_null(r->out, "reason"), "reason");
    require(field_is_null(r->out, "error"), "error");
}
static void check_only_final(const Result *r) { summary_is(r, 0, NULL, "only"); }
static void check_only_session(const Result *r) { summary_is(r, 0, "session-only", NULL); }
static void check_multi_final(const Result *r) { summary_is(r, 0, "session-multi", "second"); }
static void check_garbage(const Result *r) {
    summary_is(r, 0, "session-garbage", "kept");
    require(field_is_null(r->out, "reason"), "reason");
    require(field_is_null(r->out, "error"), "error");
}
static void check_status_only(const Result *r) {
    summary_is(r, 0, "session-status", "answer");
    require(field_is_null(r->out, "reason"), "reason");
}
static void check_turn_end(const Result *r) {
    summary_is(r, 0, "session-reason", "answer");
    require(contains(r->out, "\"code\":\"KEEP\""), "reason");
    require(!contains(r->out, "IGNORE"), "old reason");
}
static void check_overwrite(const Result *r) {
    summary_is(r, 0, "session-new", "new");
    char *err = field_string(r->out, "error");
    require(err && strcmp(err, "new-error") == 0, "error");
    free(err);
    require(contains(r->out, "SECOND"), "reason");
    require(!contains(r->out, "FIRST"), "old reason");
}
static void check_quotes(const Result *r) { summary_is(r, 0, "session-quotes", "say \"hi\" \\ end"); }
static void check_long_answer(const Result *r) {
    char *ans = field_string(r->out, "answer");
    require(ans && strlen(ans) == 10240, "len");
    require(ans && ans[0] == 'A' && ans[10239] == 'A', "content");
    free(ans);
}
static void check_cn_answer(const Result *r) {
    summary_is(r, 0, "session-unicode", "\xe4\xb8\xad\xe6\x96\x87\xe7\xad\x94\xe6\xa1\x88");
}
static void check_fail_once(const Result *r) {
    require(calls(r) == 1, "calls");
    summary_is(r, 1, "session-fail-1", NULL);
    char *err = field_string(r->out, "error");
    require(err && strcmp(err, "transient-1") == 0, "error");
    free(err);
    require(contains(r->out, "TIMEOUT"), "reason");
}
static void check_retry_ok(const Result *r) {
    require(calls(r) == 3, "calls");
    summary_is(r, 0, "session-ok", "ok-answer");
    char *a = stdin_of(r, 0);
    char *b = stdin_of(r, 1);
    char *c = stdin_of(r, 2);
    require(a && b && c && strcmp(a, "again") == 0 && strcmp(a, b) == 0 && strcmp(a, c) == 0, "same task");
    free(a); free(b); free(c);
    require(contains(r->err, "attempt 1/3"), "retry 1");
    require(contains(r->err, "attempt 2/3"), "retry 2");
    require(!contains(r->err, "attempt 3/3"), "no success retry line");
}
static void check_retry_exhausted(const Result *r) {
    require(calls(r) == 2, "calls");
    summary_is(r, 1, "session-fail-2", NULL);
    char *err = field_string(r->out, "error");
    require(err && strcmp(err, "transient-2") == 0, "last error");
    free(err);
    require(contains(r->err, "attempt 1/2"), "retry");
}

static int rva_to_off(IMAGE_NT_HEADERS64 *nt, DWORD rva) {
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int i;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD va = sec[i].VirtualAddress;
        DWORD size = sec[i].Misc.VirtualSize;
        if (rva >= va && rva < va + size) return (int)(sec[i].PointerToRawData + (rva - va));
    }
    return -1;
}

static int dll_allowed(const char *name) {
    if (_stricmp(name, "KERNEL32.dll") == 0) return 1;
    if (_strnicmp(name, "api-ms-win-crt-", 15) == 0) return 1;
    if (_strnicmp(name, "api-ms-win-core-", 16) == 0) return 1;
    return 0;
}

static void check_imports(void) {
    HANDLE h = CreateFileW(g_exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("FAIL import table\ncannot open exe\n"); g_failed++; return; }
    DWORD sz = GetFileSize(h, NULL);
    HANDLE map = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    BYTE *b = (BYTE *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
    int bad = 0;
    char bad_name[256] = "";
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        bad = 1;
        strcpy(bad_name, "not PE32+");
    } else {
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        int off = rva_to_off(nt, dir.VirtualAddress);
        IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(b + off);
        for (; imp->Name; imp++) {
            int noff = rva_to_off(nt, imp->Name);
            const char *name = (const char *)(b + noff);
            if (!dll_allowed(name)) {
                bad = 1;
                strncpy(bad_name, name, sizeof bad_name - 1);
                break;
            }
        }
    }
    UnmapViewOfFile(b);
    CloseHandle(map);
    CloseHandle(h);
    if (bad) {
        printf("FAIL exe only imports Windows system DLLs\nunexpected import: %s\n", bad_name);
        g_failed++;
    } else {
        printf("PASS exe only imports Windows system DLLs\n");
    }
    (void)sz;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: smoke.exe <repo> <fake-node.exe>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    wcscpy(g_exe, argv[1]);
    wcscat(g_exe, L"\\scripts\\dsh-subagent.exe");
    {
        wchar_t ver_path[MAX_PATH];
        wcscpy(ver_path, argv[1]);
        wcscat(ver_path, L"\\VERSION");
        HANDLE vh = CreateFileW(ver_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        char raw[64];
        DWORD got = 0;
        memset(raw, 0, sizeof raw);
        if (vh != INVALID_HANDLE_VALUE) {
            ReadFile(vh, raw, (DWORD)sizeof raw - 1, &got, NULL);
            CloseHandle(vh);
        }
        raw[got] = 0;
        int n = (int)strlen(raw);
        while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r' || raw[n - 1] == ' ')) raw[--n] = 0;
        sprintf(g_version_out, "dsh-subagent %s\n", raw);
    }
    GetEnvironmentVariableW(L"PATH", g_saved_path, 32768);

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    wcscpy(g_empty_dsh, temp);
    if (g_empty_dsh[wcslen(g_empty_dsh) - 1] != L'\\') wcscat(g_empty_dsh, L"\\");
    wcscat(g_empty_dsh, L"dsh-subagent-empty-home");
    CreateDirectoryW(g_empty_dsh, NULL);
    wcscpy(g_web_dsh, temp);
    if (g_web_dsh[wcslen(g_web_dsh) - 1] != L'\\') wcscat(g_web_dsh, L"\\");
    wcscat(g_web_dsh, L"dsh-subagent-web-home");
    CreateDirectoryW(g_web_dsh, NULL);
    {
        wchar_t profiles[MAX_PATH], web[MAX_PATH], patch[MAX_PATH];
        wcscpy(profiles, g_web_dsh); wcscat(profiles, L"\\profiles");
        wcscpy(web, profiles); wcscat(web, L"\\web");
        wcscpy(patch, web); wcscat(patch, L"\\cordis.patch.yml");
        CreateDirectoryW(profiles, NULL);
        CreateDirectoryW(web, NULL);
        const char *fixture =
            "- id: ui-settings-general\n"
            "  config:\n"
            "    welcomeNoticeVersion: 1\n"
            "- id: llm-pi-ai\n"
            "  config:\n"
            "    providers:\n"
            "      blue-vocalno:\n"
            "        models:\n"
            "          - id: volcengine/deepseek-v4.1-flash\n"
            "- id: agent-default-model\n"
            "  config:\n"
            "    provider: blue-vocalno\n"
            "    model: volcengine/deepseek-v4.1-flash\n";
        HANDLE ph = CreateFileW(patch, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ph != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(ph, fixture, (DWORD)strlen(fixture), &wr, NULL);
            CloseHandle(ph);
        }
    }
    wcscpy(g_install, temp);
    if (g_install[wcslen(g_install) - 1] != L'\\') wcscat(g_install, L"\\");
    wcscat(g_install, L"dsh-as-subagent-install");
    CreateDirectoryW(g_install, NULL);
    wchar_t node[MAX_PATH], bin_dir[MAX_PATH], bin[MAX_PATH], dshcmd[MAX_PATH];
    wcscpy(node, g_install); wcscat(node, L"\\node.exe");
    wcscpy(bin_dir, g_install); wcscat(bin_dir, L"\\node_modules\\@deepseek-ai\\dsh\\lib");
    wchar_t part[MAX_PATH];
    wcscpy(part, g_install); wcscat(part, L"\\node_modules");
    CreateDirectoryW(part, NULL);
    wcscat(part, L"\\@deepseek-ai");
    CreateDirectoryW(part, NULL);
    wcscat(part, L"\\dsh");
    CreateDirectoryW(part, NULL);
    wcscat(part, L"\\lib");
    CreateDirectoryW(part, NULL);
    wcscpy(bin, bin_dir); wcscat(bin, L"\\bin.js");
    wcscpy(dshcmd, g_install); wcscat(dshcmd, L"\\dsh.cmd");
    CopyFileW(argv[2], node, FALSE);
    {
        wchar_t base[MAX_PATH], part[MAX_PATH], node_dir[MAX_PATH];
        HANDLE ph;
        char *root8, *node8, body[MAX_PATH * 8];
        const char *inner =
            "@ECHO off\r\n"
            "SET dp0=%~dp0\r\n"
            "IF EXIST \"%dp0%\\node.exe\" (\r\n"
            "  SET \"_prog=%dp0%\\node.exe\"\r\n"
            ") ELSE (\r\n"
            "  SET \"_prog=node\"\r\n"
            ")\r\n"
            "\"%_prog%\" \"%dp0%\\..\\@deepseek-ai\\dsh\\lib\\bin.js\" %*\r\n";
        const char *profile =
            "- id: ui-settings-general\n"
            "  config:\n"
            "    secretNote: super-secret-token\n"
            "- id: llm-portable\n"
            "  config:\n"
            "    providers:\n"
            "      portable-route:\n"
            "        models:\n"
            "          - id: portable-flash\n"
            "- id: agent-default-model\n"
            "  config:\n"
            "    provider: portable-route\n"
            "    model: portable-flash\n";
        const char *broken =
            "@echo off\r\n"
            "set \"API_KEY=super-secret-token\"\r\n"
            "rem super-secret-token\r\n"
            "echo hello\r\n";
        wcscpy(base, temp);
        if (base[wcslen(base) - 1] != L'\\') wcscat(base, L"\\");
        wcscat(base, L"dsh-portable");
        wcscpy(g_portable_shim, base); wcscat(g_portable_shim, L"\\shim");
        wcscpy(node_dir, base); wcscat(node_dir, L"\\node\\My Node");
        wcscpy(g_portable_node, node_dir); wcscat(g_portable_node, L"\\node.exe");
        wcscpy(g_portable_home, base); wcscat(g_portable_home, L"\\data");
        wcscpy(part, base); wcscat(part, L"\\pkg dir\\app\\node_modules\\.bin");
        wcscpy(g_portable_bin, base);
        wcscat(g_portable_bin, L"\\pkg dir\\app\\node_modules\\@deepseek-ai\\dsh\\lib\\bin.js");
        {
            wchar_t dirs[8][MAX_PATH];
            int di;
            wcscpy(dirs[0], base);
            wcscpy(dirs[1], g_portable_shim);
            wcscpy(dirs[2], base); wcscat(dirs[2], L"\\node");
            wcscpy(dirs[3], node_dir);
            wcscpy(dirs[4], base); wcscat(dirs[4], L"\\pkg dir");
            wcscpy(dirs[5], base); wcscat(dirs[5], L"\\pkg dir\\app");
            wcscpy(dirs[6], base); wcscat(dirs[6], L"\\pkg dir\\app\\node_modules");
            wcscpy(dirs[7], part);
            for (di = 0; di < 8; di++) CreateDirectoryW(dirs[di], NULL);
            wcscpy(part, base); wcscat(part, L"\\pkg dir\\app\\node_modules\\@deepseek-ai");
            CreateDirectoryW(part, NULL);
            wcscat(part, L"\\dsh");
            CreateDirectoryW(part, NULL);
            wcscat(part, L"\\lib");
            CreateDirectoryW(part, NULL);
            wcscpy(part, g_portable_home);
            CreateDirectoryW(part, NULL);
            wcscat(part, L"\\profiles");
            CreateDirectoryW(part, NULL);
            wcscat(part, L"\\web");
            CreateDirectoryW(part, NULL);
        }
        CopyFileW(argv[2], g_portable_node, FALSE);
        ph = CreateFileW(g_portable_bin, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        CloseHandle(ph);
        wcscpy(part, g_portable_shim); wcscat(part, L"\\dsh.cmd");
        root8 = wide_to_utf8(base);
        node8 = wide_to_utf8(node_dir);
        sprintf(body,
                "@echo off\r\n"
                "setlocal\r\n"
                "set \"DSH_ROOT=%s\"\r\n"
                "set \"NODE_BIN=%s\"\r\n"
                "set \"DSH_HOME=%%DSH_ROOT%%\\data\"\r\n"
                "set \"PATH=%%NODE_BIN%%;%%PATH%%\"\r\n"
                "call \"%%DSH_ROOT%%\\pkg dir\\app\\node_modules\\.bin\\dsh.cmd\" %%*\r\n",
                root8, node8);
        ph = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ph != INVALID_HANDLE_VALUE) {
            DWORD pwr = 0;
            WriteFile(ph, body, (DWORD)strlen(body), &pwr, NULL);
            CloseHandle(ph);
        }
        wcscpy(part, base); wcscat(part, L"\\pkg dir\\app\\node_modules\\.bin\\dsh.cmd");
        ph = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ph != INVALID_HANDLE_VALUE) {
            DWORD pwr = 0;
            WriteFile(ph, inner, (DWORD)strlen(inner), &pwr, NULL);
            CloseHandle(ph);
        }
        wcscpy(part, g_portable_home); wcscat(part, L"\\profiles\\web\\cordis.patch.yml");
        ph = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ph != INVALID_HANDLE_VALUE) {
            DWORD pwr = 0;
            WriteFile(ph, profile, (DWORD)strlen(profile), &pwr, NULL);
            CloseHandle(ph);
        }
        free(root8);
        free(node8);
        {
            char *nu = wide_to_utf8(g_portable_node);
            char *bu = wide_to_utf8(g_portable_bin);
            char *hu = wide_to_utf8(g_portable_home);
            strcpy(g_portable_node_utf8, nu);
            strcpy(g_portable_bin_utf8, bu);
            strcpy(g_portable_home_utf8, hu);
            free(nu); free(bu); free(hu);
        }
        wcscpy(g_broken_shim, temp);
        if (g_broken_shim[wcslen(g_broken_shim) - 1] != L'\\') wcscat(g_broken_shim, L"\\");
        wcscat(g_broken_shim, L"dsh-broken-shim");
        CreateDirectoryW(g_broken_shim, NULL);
        wcscpy(part, g_broken_shim); wcscat(part, L"\\dsh.cmd");
        ph = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ph != INVALID_HANDLE_VALUE) {
            DWORD pwr = 0;
            WriteFile(ph, broken, (DWORD)strlen(broken), &pwr, NULL);
            CloseHandle(ph);
        }
    }
    HANDLE hf = CreateFileW(bin, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CloseHandle(hf);
    hf = CreateFileW(dshcmd, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CloseHandle(hf);

    GetTempPathW(MAX_PATH, g_missing_dir);
    wcscat(g_missing_dir, L"dsh-missing-dir");
    wcscpy(g_not_dir, temp); wcscat(g_not_dir, L"dsh-not-a-dir.txt");
    hf = CreateFileW(g_not_dir, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0; WriteFile(hf, "x", 1, &wr, NULL); CloseHandle(hf);
    wcscpy(g_child_cwd, temp); wcscat(g_child_cwd, L"dsh-child-cwd");
    CreateDirectoryW(g_child_cwd, NULL);
    wcscpy(g_user_patch, temp); wcscat(g_user_patch, L"dsh-user.yml");
    hf = CreateFileW(g_user_patch, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    WriteFile(hf, "hello: true\n", 12, &wr, NULL); CloseHandle(hf);
    wcscpy(g_missing_patch, temp); wcscat(g_missing_patch, L"dsh-no-such.yml");
    DeleteFileW(g_missing_patch);

    char *up = wide_to_utf8(g_user_patch);
    char *mp = wide_to_utf8(g_missing_patch);
    char *cd = wide_to_utf8(g_child_cwd);
    strcpy(g_user_patch_utf8, up);
    strcpy(g_missing_patch_utf8, mp);
    strcpy(g_child_cwd_utf8, cd);
    free(up); free(mp); free(cd);

    char long_task[512];
    memset(long_task, 'L', 400);
    long_task[400] = 0;
    char cmd_long[640];
    sprintf(cmd_long, "--task %s", long_task);

    char cmd_missing[MAX_PATH * 3], cmd_file[MAX_PATH * 3], cmd_cwd[MAX_PATH * 3], cmd_patch[MAX_PATH * 4], cmd_shortp[MAX_PATH * 3], cmd_badpatch[MAX_PATH * 3];
    char *miss = wide_to_utf8(g_missing_dir);
    char *file = wide_to_utf8(g_not_dir);
    char *cwd = wide_to_utf8(g_child_cwd);
    char *user = wide_to_utf8(g_user_patch);
    char *nop = wide_to_utf8(g_missing_patch);
    sprintf(cmd_missing, "--task t --cwd \"%s\"", miss);
    sprintf(cmd_file, "--task t --cwd \"%s\"", file);
    sprintf(cmd_cwd, "--task t --cwd \"%s\"", cwd);
    {
        wchar_t sessions[MAX_PATH];
        wcscpy(sessions, g_empty_dsh);
        wcscat(sessions, L"\\storages");
        CreateDirectoryW(sessions, NULL);
        wcscat(sessions, L"\\session_projcache");
        CreateDirectoryW(sessions, NULL);
        wcscat(sessions, L"\\sessions");
        CreateDirectoryW(sessions, NULL);
        char esc[MAX_PATH * 2];
        char gone[MAX_PATH * 2];
        int i, j = 0, k = 0;
        for (i = 0; cwd[i] && j + 2 < (int)sizeof esc; i++) {
            if (cwd[i] == '\\' || cwd[i] == '"') esc[j++] = '\\';
            esc[j++] = cwd[i];
        }
        esc[j] = 0;
        for (i = 0; miss[i] && k + 2 < (int)sizeof gone; i++) {
            if (miss[i] == '\\' || miss[i] == '"') gone[k++] = '\\';
            gone[k++] = miss[i];
        }
        gone[k] = 0;
        char body[MAX_PATH * 4];
        wchar_t rec_file[MAX_PATH], gone_file[MAX_PATH];
        wcscpy(rec_file, sessions); wcscat(rec_file, L"\\session-recorded.json");
        wcscpy(gone_file, sessions); wcscat(gone_file, L"\\session-gone.json");
        sprintf(body, "{\"version\":7,\"record\":{\"identity\":{\"cwd\":\"%s\"},\"rows\":{}}}\n", esc);
        HANDLE cf = CreateFileW(rec_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD cwr = 0;
        if (cf != INVALID_HANDLE_VALUE) { WriteFile(cf, body, (DWORD)strlen(body), &cwr, NULL); CloseHandle(cf); }
        sprintf(body, "{\"version\":7,\"record\":{\"identity\":{\"cwd\":\"%s\"},\"rows\":{\"cwd\":\"later\"}}}}\n", gone);
        cf = CreateFileW(gone_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (cf != INVALID_HANDLE_VALUE) { WriteFile(cf, body, (DWORD)strlen(body), &cwr, NULL); CloseHandle(cf); }
    }
    sprintf(cmd_patch, "--task t --patch \"%s\" --model deepseek-chat", user);
    sprintf(cmd_shortp, "-t t -p \"%s\"", user);
    sprintf(cmd_badpatch, "--task t --patch \"%s\"", nop);

    add("exe no args and empty stdin", "", "", "ok", 0, 1, check_empty);
    add("exe --task whitespace", "--task \"   \"", "", "ok", 0, 1, check_empty);
    add("exe stdin whitespace", "", " \t\r\n", "ok", 0, 1, check_empty);
    add("exe unknown long option", "--nope", "", "ok", 0, 1, check_unknown);
    add("exe unknown short option", "-x", "", "ok", 0, 1, check_unknown);
    add("exe reasoning-effort case", "--task t --reasoning-effort HIGH", "", "ok", 0, 1, check_effort_bad);
    add("exe reasoning-effort medium", "--task t --reasoning-effort medium", "", "ok", 0, 1, check_effort_bad);
    add("exe reasoning-effort Default is rejected", "--task t --reasoning-effort Default", "", "ok", 0, 1, check_effort_bad);
    add("exe empty reasoning-effort omits the field", "--task t --model deepseek-chat --reasoning-effort \"\"", "", "ok", 0, 0, check_model_chat);
    add("exe blank reasoning-effort omits the field", "--task t --model deepseek-chat --reasoning-effort \"   \"", "", "ok", 0, 0, check_model_chat);
    add("exe reasoning-effort default omits the field", "--task t --model deepseek-chat --reasoning-effort default", "", "ok", 0, 0, check_model_chat);
    add("exe reasoning-effort default with spaces omits the field", "--task t --model deepseek-chat --reasoning-effort \"  default  \"", "", "ok", 0, 0, check_model_chat);
    add("exe empty reasoning-effort alone writes no patch", "--task t --reasoning-effort \"\"", "", "ok", 0, 0, check_no_patch);
    add("exe reasoning-effort default alone writes no patch", "--task t --reasoning-effort default", "", "ok", 0, 0, check_no_patch);
    add("exe missing reasoning-effort value omits the field", "--task t --model deepseek-chat --reasoning-effort", "", "ok", 0, 0, check_model_chat);
    add("exe --version prints VERSION", "--version", "", "ok", 0, 1, check_version);
    add("exe --version does not run a task", "--task t --version", "", "ok", 0, 1, check_version);
    add("exe --help", "--help", "", "ok", 0, 1, check_help);
    add("exe -h", "-h", "", "ok", 0, 1, check_help);
    add("exe PATH has no dsh install", "--task t", "", "ok", 0, 1, check_no_install);
    add("exe --cwd missing", cmd_missing, "", "ok", 0, 0, check_cwd_bad);
    add("exe --cwd is a file", cmd_file, "", "ok", 0, 0, check_cwd_bad);
    add("exe --retries 0 clamps to 1", "--task t --retries 0", "", "ok", 0, 0, check_once_ok);
    add("exe --retries -4 clamps to 1", "--task t --retries -4", "", "ok", 0, 0, check_once_ok);
    add("exe --retries abc clamps to 1", "--task t --retries abc", "", "ok", 0, 0, check_once_ok);
    add("exe --retries missing value stays 1", "--task t --retries", "", "ok", 0, 0, check_once_ok);
    add("exe --retry-delay -5 clamps to 0", "--task t --retries 2 --retry-delay -5", "", "fail-then-ok", 1, 0, check_fast_retry);
    add("exe --retry-delay foo is 0", "--task t --retry-delay foo", "", "ok", 0, 0, check_once_ok);
    add("exe --task trims and does not add a newline", "--task \"  hello  \"", "", "ok", 0, 0, check_hello);
    add("exe two positionals join with one space", "alpha beta", "", "ok", 0, 0, check_alpha);
    add("exe -t is --task", "-t via-short", "", "ok", 0, 0, check_short);
    add("exe --task wins over stdin", "--task from-arg", "from-stdin", "ok", 0, 0, check_from_arg);
    add("exe reads stdin when there is no task", "", "from-stdin", "ok", 0, 0, check_from_stdin);
    add("exe a lone - reads stdin", "-", "from-stdin", "ok", 0, 0, check_from_stdin);
    add("exe --task with no value falls back to stdin", "--task", "from-stdin", "ok", 0, 0, check_from_stdin);
    add("exe --task - is the task text", "--task -", "ignored", "ok", 0, 0, check_dash_task);
    add("exe positional text is appended", "--task hello world", "", "ok", 0, 0, check_hello_world);
    add("exe multiline task keeps the inner newline", "--task \"line1\nline2\"", "", "ok", 0, 0, check_multi);
    add("exe UTF-8 chinese task", "--task \xe4\xb8\xad\xe6\x96\x87\xe4\xbb\xbb\xe5\x8a\xa1", "", "ok", 0, 0, check_cn_task);
    add("exe long task is not truncated", cmd_long, "", "ok", 0, 0, check_long_task);
    add("exe --model defaults provider", "--task t --model deepseek-chat", "", "ok", 0, 0, check_model_chat);
    add("exe --provider defaults model", "--task t --provider other-route", "", "ok", 0, 0, check_provider_only);
    add("exe reasoning-effort alone writes both defaults", "--task t --reasoning-effort high", "", "ok", 0, 0, check_effort_high);
    add("exe reasoning-effort low", "--task t --reasoning-effort low", "", "ok", 0, 0, check_effort_low);
    add("exe reasoning-effort off", "--task t --reasoning-effort off", "", "ok", 0, 0, check_effort_off);
    add("exe reasoning-effort max", "--task t --reasoning-effort max", "", "ok", 0, 0, check_effort_max);
    add("exe provider model and effort together", "--task t --provider other-route --model deepseek-chat --reasoning-effort max", "", "ok", 0, 0, check_all_three);
    add("exe no model args means no patch", "--task t", "", "ok", 0, 0, check_no_patch);
    add("exe --model with no value means no patch", "--task t --model", "", "ok", 0, 0, check_no_patch);
    add("exe extra --patch comes before the model patch", cmd_patch, "", "ok", 0, 0, check_user_then_model);
    add("exe -p is --patch", cmd_shortp, "", "ok", 0, 0, check_short_patch);
    add("exe --patch with no value is ignored", "--task t --patch", "", "ok", 0, 0, check_no_patch);
    add("exe a missing patch file is still forwarded", cmd_badpatch, "", "ok", 0, 0, check_missing_patch);
    add("exe default argv shape has no session id", "--task t", "", "ok", 0, 0, check_shape);
    add("exe --session-id follows --json", "--task t --session-id session-abc", "", "ok", 0, 0, check_session);
    add("exe -s accepts a session id with a space", "-t t -s \"sess ion\"", "", "ok", 0, 0, check_session_space);
    add("exe --cwd becomes the child cwd", cmd_cwd, "", "ok", 0, 0, check_cwd);
    {
        char cmd_slash[MAX_PATH * 4];
        char slash[MAX_PATH * 3];
        int i;
        for (i = 0; cwd[i] && i + 1 < (int)sizeof slash; i++) slash[i] = cwd[i] == '\\' ? '/' : cwd[i];
        slash[i] = 0;
        sprintf(cmd_slash, "--task t --cwd \"%s/\"", slash);
        add("exe --cwd accepts a trailing slash and forward slashes", cmd_slash, "", "ok", 0, 0, check_cwd);
        add("exe resume uses the recorded cwd", "--task t --session-id session-recorded", "", "ok", 0, 0, check_cwd);
        char cmd_match[MAX_PATH * 3];
        sprintf(cmd_match, "--task t --session-id session-recorded --cwd \"%s\"", cwd);
        add("exe resume accepts the recorded cwd", cmd_match, "", "ok", 0, 0, check_cwd);
        char *install = wide_to_utf8(g_install);
        char cmd_bad[MAX_PATH * 4];
        sprintf(cmd_bad, "--task t --session-id session-recorded --cwd \"%s\"", install);
        add("exe resume rejects a different cwd", cmd_bad, "", "ok", 0, 0, check_cwd_mismatch);
        free(install);
        add("exe resume refuses a missing recorded cwd", "--task t --session-id session-gone", "", "ok", 0, 0, check_cwd_gone);
        add("exe resume without a cache keeps the caller cwd", "--task t --session-id session-unknown", "", "ok", 0, 0, check_caller_cwd);
    }
    add("exe -c is --cwd", cmd_cwd, "", "ok", 0, 0, NULL); /* replaced below */
    g_cases[g_case_n - 1].cmd = utf8_to_wide(""); /* placeholder overwritten next */
    /* rebuild -c command properly by replacing the last case */
    {
        char cmdc[MAX_PATH * 3];
        sprintf(cmdc, "-t t -c \"%s\"", cwd);
        free(g_cases[g_case_n - 1].cmd);
        g_cases[g_case_n - 1].cmd = utf8_to_wide(cmdc);
        g_cases[g_case_n - 1].check = check_cwd;
        g_cases[g_case_n - 1].name = "exe -c is --cwd";
    }
    add("exe success JSON and stderr passthrough", "--task t", "", "ok", 0, 0, check_ok_json);
    add("exe --raw prints only the answer", "--task t --raw", "", "ok", 0, 0, check_raw);
    add("exe child exit 1 is kept in JSON", "--task t", "", "error-exit", 0, 0, check_error_exit);
    add("exe --raw on failure still prints only the answer", "--task t --raw", "", "error-exit", 0, 0, check_raw_fail);
    add("exe no events and exit 0 yields null fields", "--task t", "", "empty-ok", 0, 0, check_empty_ok);
    add("exe a single final event", "--task t", "", "only-final", 0, 0, check_only_final);
    add("exe a single session event", "--task t", "", "only-session", 0, 0, check_only_session);
    add("exe multiple finals keep the last", "--task t", "", "multi-final", 0, 0, check_multi_final);
    add("exe blank lines and bad JSON are skipped", "--task t", "", "garbage", 0, 0, check_garbage);
    add("exe non-turn_end reason is ignored", "--task t", "", "status-only", 0, 0, check_status_only);
    add("exe turn_end reason replaces an earlier status", "--task t", "", "turn-end-wins", 0, 0, check_turn_end);
    add("exe repeated fields keep the last value", "--task t", "", "overwrite", 0, 0, check_overwrite);
    add("exe quotes and backslashes round-trip", "--task t", "", "quotes", 0, 0, check_quotes);
    add("exe 10240-char answer is not truncated", "--task t", "", "long", 0, 0, check_long_answer);
    add("exe chinese answer round-trips as UTF-8", "--task t", "", "unicode", 0, 0, check_cn_answer);
    add("exe default retries stops after one failure", "--task t", "", "fail-then-ok", 5, 0, check_fail_once);
    add("exe third attempt is the one that is kept", "--task again --retries 3 --retry-delay 0", "", "fail-then-ok", 2, 0, check_retry_ok);
    add("exe exhausted retries keep the last attempt", "--task t --retries 2 --retry-delay 0", "", "fail-then-ok", 5, 0, check_retry_exhausted);
    add("exe model-pool without a pipe", "--task t --model-pool noslash", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool old slash form is rejected", "--task t --model-pool providerA/volcengine/deepseek-v4.1-flash", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool missing provider", "--task t --model-pool |model1", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool missing model", "--task t --model-pool providerA|", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool empty entry", "--task t --model-pool \"\"", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool missing value", "--task t --model-pool", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool four segments", "--task t --model-pool providerA|model1|high|extra", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool bad effort", "--task t --model-pool providerA|model1|HIGH", "", "ok", 0, 1, check_pool_bad);
    add("exe model-pool one entry keeps its own effort", "--task t --model-pool providerA|model1|high", "", "ok", 0, 0, check_pool_one);
    add("exe model-pool default effort is omitted", "--task t --model-pool providerA|model2|default", "", "ok", 0, 0, check_pool_omit_effort);
    add("exe model-pool trailing pipe omits effort", "--task t --model-pool providerA|model2|", "", "ok", 0, 0, check_pool_omit_effort);
    add("exe model-pool trims each segment", "--task t --model-pool \"  providerA  |  model2  |  default  \"", "", "ok", 0, 0, check_pool_omit_effort);
    add("exe model-pool keeps a slash inside the model id", "--task t --model-pool ark|volcengine/deepseek-v4.1-flash|high", "", "ok", 0, 0, check_pool_slash_model);
    add("exe --model keeps a slash inside the model id", "--task t --provider ark --model volcengine/deepseek-v4.1-flash", "", "ok", 0, 0, check_model_slash);
    add("exe model-pool stops after the first success", "--task t --model-pool providerA|model1|high --model-pool providerA|model2", "", "ok", 0, 0, check_pool_one);
    add("exe model-pool switches after retries are exhausted", "--task t --retries 1 --retry-delay 0 --model-pool providerA|model1|high --model-pool providerA|model2", "", "fail-then-ok", 1, 0, check_pool_switch);
    add("exe model-pool retries the same entry before switching", "--task t --retries 2 --retry-delay 0 --model-pool providerA|model1 --model-pool providerB|model3|max", "", "fail-then-ok", 2, 0, check_pool_retry_then_switch);
    add("exe model-pool exhaustion does not invent deepseek-official", "--task t --retries 1 --retry-delay 0 --model-pool providerA|model1|high --model-pool providerA|model2 --model-pool providerB|model3 --model-pool route|model666|low", "", "fail-then-ok", 9, 0, check_pool_exhausted);
    add("exe --model is the first pool entry", "--task t --retries 1 --retry-delay 0 --provider providerA --model model1 --reasoning-effort low --model-pool providerB|model3", "", "fail-then-ok", 1, 0, check_pool_primary_first);
    add("exe global reasoning-effort does not cover a pool entry", "--task t --reasoning-effort high --model-pool providerA|model2", "", "ok", 0, 1, check_pool_effort_flag);
    {
        char cmd_web[MAX_PATH * 4];
        sprintf(cmd_web, "--task t --patch \"%s\" --provider ark --model volcengine/deepseek-v4.1-flash", g_user_patch_utf8);
        add("exe web profile supplies providers and saved credentials", cmd_web, "", "ok", 0, 0, check_web_profile);
        g_cases[g_case_n - 1].web_home = 1;
    }
    add("exe web profile default model is kept when none is requested", "--task t", "", "ok", 0, 0, check_web_default);
    g_cases[g_case_n - 1].web_home = 1;
    add("exe --no-web-profile ignores a present web profile", "--task t --no-web-profile", "", "ok", 0, 0, check_no_patch);
    g_cases[g_case_n - 1].web_home = 1;
    add("exe default permission is read-only", "--task t", "", "ok", 0, 0, check_no_patch);
    add("exe --permission missing value stays read-only", "--task t --permission", "", "ok", 0, 0, check_no_patch);
    add("exe --permission readonly is read-only", "--task t --permission readonly", "", "ok", 0, 0, check_no_patch);
    add("exe --permission workspace is workspace-write", "--task t --permission workspace", "", "ok", 0, 0, check_workspace);
    add("exe --permission workspace-write", "--task t --permission workspace-write", "", "ok", 0, 0, check_workspace);
    add("exe --permission danger-full-access skips approval", "--task t --permission danger-full-access", "", "ok", 0, 0, check_full);
    add("exe --permission full is danger-full-access", "--task t --permission full", "", "ok", 0, 0, check_full);
    add("exe --permission danger full access", "--task t --permission \"danger full access\"", "", "ok", 0, 0, check_full);
    add("exe --permission full-access", "--task t --permission full-access", "", "ok", 0, 0, check_full);
    add("exe --permission unknown value", "--task t --permission yolo", "", "ok", 0, 1, check_permission_bad);
    add("exe --permission empty value", "--task t --permission \"\"", "", "ok", 0, 1, check_permission_bad);
    add("exe --permission Read-only is rejected", "--task t --permission Read-only", "", "ok", 0, 1, check_permission_bad);
    {
        char cmd_perm[MAX_PATH * 4];
        sprintf(cmd_perm, "--task t --patch \"%s\" --model deepseek-chat --permission workspace-write", g_user_patch_utf8);
        add("exe permission patch follows the model patch", cmd_perm, "", "ok", 0, 0, check_permission_last);
    }
    add("exe portable dsh.cmd resolves split node, bin, and DSH_HOME", "--task t", "", "ok", 0, 0, check_portable);
    g_cases[g_case_n - 1].layout = 2;
    g_cases[g_case_n - 1].clear_home = 1;
    {
        char cmd_explicit[MAX_PATH * 12];
        char cmd_home[MAX_PATH * 4];
        sprintf(cmd_explicit, "--task t --node \"%s\" --dsh-bin \"%s\" --dsh-home \"%s\"",
                g_portable_node_utf8, g_portable_bin_utf8, g_portable_home_utf8);
        add("exe --node and --dsh-bin accept separate directories", cmd_explicit, "", "ok", 0, 1, check_explicit_install);
        {
            char *file = wide_to_utf8(g_not_dir);
            sprintf(cmd_home, "--task t --dsh-home \"%s\"", file);
            free(file);
        }
        add("exe --dsh-home rejects a file", cmd_home, "", "ok", 0, 0, check_bad_home);
    }
    add("exe --node rejects a missing file", "--task t --node C:\\no-such-node.exe", "", "ok", 0, 1, check_bad_node);
    add("exe --dsh-bin rejects a missing file", "--task t --dsh-bin C:\\no-such-bin.js", "", "ok", 0, 1, check_bad_bin);
    add("exe --node without a value is an error", "--task t --node", "", "ok", 0, 1, check_node_value);
    add("exe a forwarding dsh.cmd that names nothing lists the script", "--task t", "", "ok", 0, 0, check_broken_forwarder);
    g_cases[g_case_n - 1].layout = 3;
    g_cases[g_case_n - 1].clear_home = 1;

    check_imports();
    int i;
    for (i = 0; i < g_case_n; i++) {
        Case *c = &g_cases[i];
        g_why = NULL;
        Result r = run_exe(c->cmd, c->stdin_utf8, c->scenario, c->fails, c->layout, c->web_home, c->clear_home);
        c->check(&r);
        if (g_why) {
            g_failed++;
            printf("FAIL %s\n%s\nexit=%d\nSTDOUT=%s\nSTDERR=%s\nLOG=%s\n", c->name, g_why, r.code, r.out, r.err, r.log);
        } else {
            printf("PASS %s\n", c->name);
        }
        free_result(&r);
    }
    printf("\n%d cases, %d failed\n", g_case_n + 1, g_failed);
    free(miss); free(file); free(cwd); free(user); free(nop);
    return g_failed ? 1 : 0;
}
