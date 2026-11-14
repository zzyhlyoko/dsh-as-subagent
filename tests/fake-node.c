/* Test double for node.exe. Records the invocation and emits a scripted
 * NDJSON stream. Build: gcc -municode -O2 -s -static-libgcc -o node.exe fake-node.c
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *wide_to_utf8_n(const wchar_t *w, int chars) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, chars, NULL, 0, NULL, NULL);
    if (n < 0) n = 0;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) ExitProcess(2);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w, chars, s, n, NULL, NULL);
    s[n] = 0;
    return s;
}

static char *wide_to_utf8(const wchar_t *w) {
    return wide_to_utf8_n(w, -1);
}

static void write_handle(HANDLE h, const char *s) {
    DWORD wr = 0;
    DWORD n = (DWORD)strlen(s);
    if (n) WriteFile(h, s, n, &wr, NULL);
}

static char *read_all(HANDLE h) {
    size_t cap = 256, n = 0;
    char *p = (char *)malloc(cap);
    if (!p) ExitProcess(2);
    for (;;) {
        while (n + 2048 + 1 > cap) {
            cap *= 2;
            p = (char *)realloc(p, cap);
            if (!p) ExitProcess(2);
        }
        DWORD got = 0;
        if (!ReadFile(h, p + n, 2048, &got, NULL) || got == 0) break;
        n += got;
    }
    p[n] = 0;
    return p;
}

static void json_escape(HANDLE h, const char *s) {
    write_handle(h, "\"");
    if (!s) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char tmp[8];
        if (*p == '"' || *p == '\\') { tmp[0] = '\\'; tmp[1] = (char)*p; tmp[2] = 0; write_handle(h, tmp); }
        else if (*p == '\n') write_handle(h, "\\n");
        else if (*p == '\r') write_handle(h, "\\r");
        else if (*p == '\t') write_handle(h, "\\t");
        else if (*p < 0x20) { sprintf(tmp, "\\u%04x", *p); write_handle(h, tmp); }
        else { tmp[0] = (char)*p; tmp[1] = 0; write_handle(h, tmp); }
    }
    write_handle(h, "\"");
}

static char *read_file_utf8(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char *body = read_all(h);
    CloseHandle(h);
    return body;
}

static int env_int(const wchar_t *name) {
    wchar_t buf[32];
    DWORD n = GetEnvironmentVariableW(name, buf, 32);
    if (n == 0 || n >= 32) return 0;
    return _wtoi(buf);
}

static void emit_line(HANDLE out, const char *s) { write_handle(out, s); }

static int emit(HANDLE out, HANDLE err, const char *scenario, int attempt) {
    char line[128];
    if (strcmp(scenario, "ok") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-ok\"}\n");
        emit_line(out, "{\"type\":\"status\",\"phase\":\"turn_start\"}\n");
        emit_line(out, "{\"type\":\"text\",\"text\":\"partial\"}\n");
        emit_line(out, "{\"type\":\"thinking\",\"text\":\"hidden\"}\n");
        emit_line(out, "{\"type\":\"tool_call\",\"name\":\"echo\"}\n");
        emit_line(out, "{\"type\":\"tool_result\",\"ok\":true}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"ok-answer\"}\n");
        emit_line(out, "{\"type\":\"status\",\"phase\":\"turn_end\",\"reason\":{\"code\":\"END\"}}\n");
        emit_line(err, "dsh: reasoning: hi\n");
        return 0;
    }
    if (strcmp(scenario, "garbage") == 0) {
        emit_line(out, "\n   \nnot-json\n{\"nope\":1}\n");
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-garbage\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"kept\"}\n");
        return 0;
    }
    if (strcmp(scenario, "multi-final") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-multi\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"first\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"second\"}\n");
        return 0;
    }
    if (strcmp(scenario, "error-exit") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-err\"}\n");
        emit_line(out, "{\"type\":\"error\",\"message\":\"boom\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"partial-answer\"}\n");
        emit_line(out, "{\"type\":\"status\",\"phase\":\"turn_end\",\"reason\":{\"error\":{\"code\":\"MODEL_UNSUPPORTED\"}}}\n");
        emit_line(err, "dsh: MODEL_UNSUPPORTED: boom\n");
        return 1;
    }
    if (strcmp(scenario, "status-only") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-status\"}\n");
        emit_line(out, "{\"type\":\"status\",\"phase\":\"step_end\",\"reason\":{\"code\":\"IGNORE\"}}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"answer\"}\n");
        return 0;
    }
    if (strcmp(scenario, "turn-end-wins") == 0) {
        emit_line(out, "{\"type\":\"status\",\"phase\":\"step_end\",\"reason\":{\"code\":\"IGNORE\"}}\n");
        emit_line(out, "{\"type\":\"status\",\"phase\":\"turn_end\",\"reason\":{\"code\":\"KEEP\"}}\n");
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-reason\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"answer\"}\n");
        return 0;
    }
    if (strcmp(scenario, "empty-ok") == 0) return 0;
    if (strcmp(scenario, "quotes") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-quotes\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"say \\\"hi\\\" \\\\ end\"}\n");
        return 0;
    }
    if (strcmp(scenario, "long") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-long\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"");
        int i;
        for (i = 0; i < 10240; i++) emit_line(out, "A");
        emit_line(out, "\"}\n");
        return 0;
    }
    if (strcmp(scenario, "unicode") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-unicode\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"\xe4\xb8\xad\xe6\x96\x87\xe7\xad\x94\xe6\xa1\x88\"}\n");
        return 0;
    }
    if (strcmp(scenario, "only-final") == 0) {
        emit_line(out, "{\"type\":\"final\",\"text\":\"only\"}\n");
        return 0;
    }
    if (strcmp(scenario, "only-session") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-only\"}\n");
        return 0;
    }
    if (strcmp(scenario, "overwrite") == 0) {
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-old\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"old\"}\n");
        emit_line(out, "{\"type\":\"error\",\"message\":\"old-error\"}\n");
        emit_line(out, "{\"type\":\"status\",\"phase\":\"turn_end\",\"reason\":{\"code\":\"FIRST\"}}\n");
        emit_line(out, "{\"type\":\"session\",\"sessionId\":\"session-new\"}\n");
        emit_line(out, "{\"type\":\"final\",\"text\":\"new\"}\n");
        emit_line(out, "{\"type\":\"error\",\"message\":\"new-error\"}\n");
        emit_line(out, "{\"type\":\"status\",\"phase\":\"turn_end\",\"reason\":{\"code\":\"SECOND\"}}\n");
        return 0;
    }
    if (strcmp(scenario, "transient-fail") == 0) {
        sprintf(line, "{\"type\":\"session\",\"sessionId\":\"session-fail-%d\"}\n", attempt);
        emit_line(out, line);
        sprintf(line, "{\"type\":\"error\",\"message\":\"transient-%d\"}\n", attempt);
        emit_line(out, line);
        emit_line(out, "{\"type\":\"status\",\"phase\":\"turn_end\",\"reason\":{\"error\":{\"code\":\"TIMEOUT\"}}}\n");
        emit_line(err, "dsh: TIMEOUT: transient\n");
        return 1;
    }
    emit_line(err, "fake-node: unknown scenario\n");
    return 2;
}

int wmain(int argc, wchar_t **argv) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    char *stdin_bytes = read_all(GetStdHandle(STD_INPUT_HANDLE));

    wchar_t scenario_w[64];
    if (!GetEnvironmentVariableW(L"DSH_FAKE_SCENARIO", scenario_w, 64) || !scenario_w[0])
        wcscpy(scenario_w, L"ok");
    char *scenario = wide_to_utf8(scenario_w);
    int fails = env_int(L"DSH_FAKE_FAILS");

    int attempt = 1;
    wchar_t counter[MAX_PATH];
    if (GetEnvironmentVariableW(L"DSH_FAKE_COUNTER", counter, MAX_PATH) && counter[0]) {
        int n = 0;
        HANDLE h = CreateFileW(counter, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char num[32];
            DWORD got = 0;
            memset(num, 0, sizeof num);
            ReadFile(h, num, 30, &got, NULL);
            CloseHandle(h);
            n = atoi(num);
        }
        n++;
        attempt = n;
        h = CreateFileW(counter, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char num[32];
            sprintf(num, "%d", n);
            DWORD wr = 0;
            WriteFile(h, num, (DWORD)strlen(num), &wr, NULL);
            CloseHandle(h);
        }
    }

    wchar_t log_path[MAX_PATH];
    if (GetEnvironmentVariableW(L"DSH_FAKE_LOG", log_path, MAX_PATH) && log_path[0]) {
        HANDLE log = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (log != INVALID_HANDLE_VALUE) {
            write_handle(log, "{\"args\":[");
            int i;
            for (i = 1; i < argc; i++) {
                if (i > 1) write_handle(log, ",");
                char *a = wide_to_utf8(argv[i]);
                json_escape(log, a);
                free(a);
            }
            write_handle(log, "],\"stdin\":");
            json_escape(log, stdin_bytes);
            wchar_t cwd[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, cwd);
            char *cwd8 = wide_to_utf8(cwd);
            write_handle(log, ",\"cwd\":");
            json_escape(log, cwd8);
            free(cwd8);
            write_handle(log, ",\"patches\":[");
            int first = 1;
            for (i = 1; i < argc; i++) {
                if (wcscmp(argv[i], L"--patch") != 0 || i + 1 >= argc) continue;
                if (!first) write_handle(log, ",");
                first = 0;
                char *path = wide_to_utf8(argv[++i]);
                char *text = read_file_utf8(argv[i]);
                write_handle(log, "{\"path\":");
                json_escape(log, path);
                write_handle(log, ",\"text\":");
                if (text) json_escape(log, text);
                else write_handle(log, "null");
                write_handle(log, "}");
                free(path);
                free(text);
            }
            char *sc = wide_to_utf8(scenario_w);
            char tail[64];
            sprintf(tail, "],\"scenario\":");
            write_handle(log, tail);
            json_escape(log, sc);
            sprintf(tail, ",\"attempt\":%d}\n", attempt);
            write_handle(log, tail);
            free(sc);
            CloseHandle(log);
        }
    }

    if (strcmp(scenario, "fail-then-ok") == 0) {
        free(scenario);
        scenario = _strdup(attempt <= fails ? "transient-fail" : "ok");
    }
    int code = emit(out, err, scenario, attempt);
    free(scenario);
    free(stdin_bytes);
    return code;
}
