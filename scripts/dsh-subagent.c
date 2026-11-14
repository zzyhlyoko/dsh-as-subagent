/* dsh-subagent: native launcher for `dsh --profile headless`.
 *
 * No .NET and no VC++ redistributable. Link dynamically only to libraries
 * that ship with Windows 10 or later (KERNEL32 and the Universal CRT):
 *
 *   scripts\build.cmd
 *
 * The build reads ../VERSION and writes version.h. -static-libgcc keeps
 * libgcc inside the exe. Do not pass -static (that pulls a static copy of
 * the CRT) and do not link vcruntime / msvcp.
 * stdout and stderr are UTF-8 when redirected. On a console they are
 * written with WriteConsoleW so Chinese still displays.
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "version.h"

static HANDLE g_out;
static HANDLE g_err;

static void die_oom(void) {
    ExitProcess(2);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die_oom();
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die_oom();
    return q;
}

typedef struct {
    char *p;
    int n;
    int cap;
} Buf;

static void buf_reserve(Buf *b, int extra) {
    if (b->n + extra <= b->cap) return;
    int cap = b->cap ? b->cap : 64;
    while (b->n + extra > cap) cap *= 2;
    b->p = (char *)xrealloc(b->p, (size_t)cap);
    b->cap = cap;
}

static void buf_add(Buf *b, const void *src, int n) {
    if (n <= 0) return;
    buf_reserve(b, n);
    memcpy(b->p + b->n, src, (size_t)n);
    b->n += n;
}

static void buf_str(Buf *b, const char *s) {
    buf_add(b, s, (int)strlen(s));
}

static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static wchar_t *utf8_to_wide(const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    int count = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    if (count < 0) count = 0;
    wchar_t *w = (wchar_t *)xmalloc(((size_t)count + 1) * sizeof(wchar_t));
    if (count) MultiByteToWideChar(CP_UTF8, 0, s, n, w, count);
    w[count] = 0;
    return w;
}

static char *wide_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        char *empty = (char *)xmalloc(1);
        empty[0] = 0;
        return empty;
    }
    char *s = (char *)xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static void write_bytes(HANDLE h, const char *s, int n) {
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        wchar_t *w = utf8_to_wide(s, n);
        DWORD written = 0;
        WriteConsoleW(h, w, (DWORD)wcslen(w), &written, NULL);
        free(w);
        return;
    }
    int off = 0;
    while (off < n) {
        DWORD written = 0;
        if (!WriteFile(h, s + off, (DWORD)(n - off), &written, NULL) || written == 0) break;
        off += (int)written;
    }
}

static void write_out(const char *s) { write_bytes(g_out, s, (int)strlen(s)); }
static void write_err(const char *s) { write_bytes(g_err, s, (int)strlen(s)); }

static int is_wspace(wchar_t c) {
    if (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20) return 1;
    if (c == 0x85 || c == 0xA0 || c == 0x1680) return 1;
    if (c >= 0x2000 && c <= 0x200A) return 1;
    if (c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000) return 1;
    return 0;
}

static wchar_t *wtrim_dup(const wchar_t *s) {
    if (!s) s = L"";
    while (*s && is_wspace(*s)) s++;
    size_t n = wcslen(s);
    while (n && is_wspace(s[n - 1])) n--;
    wchar_t *d = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
    memcpy(d, s, n * sizeof(wchar_t));
    d[n] = 0;
    return d;
}

static wchar_t *wcat_space(const wchar_t *a, const wchar_t *b) {
    size_t na = wcslen(a), nb = wcslen(b);
    wchar_t *d = (wchar_t *)xmalloc((na + nb + 2) * sizeof(wchar_t));
    memcpy(d, a, na * sizeof(wchar_t));
    d[na] = L' ';
    memcpy(d + na + 1, b, (nb + 1) * sizeof(wchar_t));
    return d;
}

static int streq(const wchar_t *a, const wchar_t *b) {
    return wcscmp(a, b) == 0;
}

static wchar_t *wstrdup(const wchar_t *s) {
    size_t n = wcslen(s);
    wchar_t *d = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
    memcpy(d, s, (n + 1) * sizeof(wchar_t));
    return d;
}

static int take_value(int argc, wchar_t **argv, int *i, wchar_t **out) {
    if (*i + 1 >= argc) return 0;
    *i += 1;
    *out = argv[*i];
    return 1;
}

static int parse_i32(const wchar_t *s, int *out) {
    if (!s || !*s) { *out = 0; return 0; }
    int sign = 1;
    const wchar_t *p = s;
    if (*p == L'-') { sign = -1; p++; }
    if (!*p) { *out = 0; return 0; }
    long v = 0;
    for (; *p; p++) {
        if (*p < L'0' || *p > L'9') { *out = 0; return 0; }
        v = v * 10 + (*p - L'0');
    }
    *out = (int)(sign < 0 ? -v : v);
    return 1;
}

static int parse_f64(const wchar_t *s, double *out) {
    if (!s || !*s) { *out = 0; return 0; }
    int sign = 1;
    const wchar_t *p = s;
    if (*p == L'-') { sign = -1; p++; }
    if (!*p) { *out = 0; return 0; }
    double v = 0;
    int any = 0;
    for (; *p && *p != L'.'; p++) {
        if (*p < L'0' || *p > L'9') { *out = 0; return 0; }
        v = v * 10 + (*p - L'0');
        any = 1;
    }
    if (*p == L'.') {
        p++;
        double place = 0.1;
        if (!*p && !any) { *out = 0; return 0; }
        for (; *p; p++) {
            if (*p < L'0' || *p > L'9') { *out = 0; return 0; }
            v += (*p - L'0') * place;
            place *= 0.1;
            any = 1;
        }
    }
    if (!any) { *out = 0; return 0; }
    *out = sign < 0 ? -v : v;
    return 1;
}

static int effort_ok(const wchar_t *s) {
    return streq(s, L"low") || streq(s, L"off") || streq(s, L"high") || streq(s, L"max");
}

/* Blank, whitespace, or the literal "default" means omit the field. dsh treats
 * a missing reasoningEffort as the adapter default (requested ?? defaultEffort).
 * The string "default" is not itself an effort id. */
static int effort_blank(const wchar_t *s) {
    wchar_t *t = wtrim_dup(s);
    int blank = t[0] == 0 || streq(t, L"default");
    free(t);
    return blank;
}

/* provider|model or provider|model|effort. '|' separates the fields so a
 * model id may contain '/'. The effort field is optional and belongs only to
 * this entry. Blank or "default" omits reasoningEffort. */
static int parse_pool_entry(const wchar_t *s, wchar_t **prov, wchar_t **mod, wchar_t **effort) {
    wchar_t *t = wtrim_dup(s ? s : L"");
    wchar_t *bar1 = wcschr(t, L'|');
    if (!bar1) { free(t); return 0; }
    *bar1 = 0;
    wchar_t *bar2 = wcschr(bar1 + 1, L'|');
    if (bar2) {
        *bar2 = 0;
        if (wcschr(bar2 + 1, L'|')) { free(t); return 0; }
    }
    wchar_t *p = wtrim_dup(t);
    wchar_t *m = wtrim_dup(bar1 + 1);
    wchar_t *e = NULL;
    int ok = p[0] && m[0];
    if (ok && bar2) {
        wchar_t *et = wtrim_dup(bar2 + 1);
        if (!et[0] || effort_blank(et)) free(et);
        else if (!effort_ok(et)) { free(et); ok = 0; }
        else e = et;
    }
    free(t);
    if (!ok) { free(p); free(m); free(e); return 0; }
    *prov = p;
    *mod = m;
    *effort = e;
    return 1;
}

static int file_exists(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static wchar_t *join_path(const wchar_t *dir, const wchar_t *rel) {
    size_t n = wcslen(dir) + wcslen(rel) + 2;
    wchar_t *d = (wchar_t *)xmalloc(n * sizeof(wchar_t));
    wcscpy(d, dir);
    size_t len = wcslen(d);
    if (len && d[len - 1] != L'\\' && d[len - 1] != L'/') wcscat(d, L"\\");
    wcscat(d, rel);
    return d;
}

static int find_install(wchar_t **node_exe, wchar_t **bin_js) {
    DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
    if (need == 0) return 0;
    wchar_t *path = (wchar_t *)xmalloc(need * sizeof(wchar_t));
    GetEnvironmentVariableW(L"PATH", path, need);
    int pass;
    for (pass = 0; pass < 2; pass++) {
        wchar_t *scan = path;
        while (*scan) {
            wchar_t *semi = wcschr(scan, L';');
            size_t len = semi ? (size_t)(semi - scan) : wcslen(scan);
            wchar_t *dir = (wchar_t *)xmalloc((len + 1) * sizeof(wchar_t));
            memcpy(dir, scan, len * sizeof(wchar_t));
            dir[len] = 0;
            wchar_t *trim = dir;
            while (*trim == L' ' || *trim == L'\t') trim++;
            size_t tn = wcslen(trim);
            while (tn && (trim[tn - 1] == L' ' || trim[tn - 1] == L'\t')) trim[--tn] = 0;
            if (*trim) {
                wchar_t *cmd = join_path(trim, L"dsh.cmd");
                wchar_t *ps1 = join_path(trim, L"dsh.ps1");
                wchar_t *dexe = join_path(trim, L"dsh.exe");
                wchar_t *node = join_path(trim, L"node.exe");
                wchar_t *bin = join_path(trim, L"node_modules\\@deepseek-ai\\dsh\\lib\\bin.js");
                int has_marker = file_exists(cmd) || file_exists(ps1) || file_exists(dexe);
                int ok = file_exists(node) && file_exists(bin) && (pass == 1 || has_marker);
                free(cmd); free(ps1); free(dexe);
                if (ok) {
                    *node_exe = node;
                    *bin_js = bin;
                    free(dir);
                    free(path);
                    return 1;
                }
                free(node);
                free(bin);
            }
            free(dir);
            if (!semi) break;
            scan = semi + 1;
        }
    }
    free(path);
    return 0;
}

typedef struct {
    wchar_t **items;
    int n;
    int cap;
} WList;

static void wlist_add(WList *list, wchar_t *owned) {
    if (list->n + 1 > list->cap) {
        list->cap = list->cap ? list->cap * 2 : 8;
        list->items = (wchar_t **)xrealloc(list->items, (size_t)list->cap * sizeof(wchar_t *));
    }
    list->items[list->n++] = owned;
}

typedef struct {
    wchar_t *provider;
    wchar_t *model;
    wchar_t *effort;
} ModelRef;

typedef struct {
    ModelRef *items;
    int n;
    int cap;
} ModelList;

static void model_list_free(ModelList *list) {
    int i;
    for (i = 0; i < list->n; i++) {
        free(list->items[i].provider);
        free(list->items[i].model);
        free(list->items[i].effort);
    }
    free(list->items);
    list->items = NULL;
    list->n = list->cap = 0;
}

static void model_list_grow(ModelList *list) {
    if (list->n + 1 <= list->cap) return;
    list->cap = list->cap ? list->cap * 2 : 4;
    list->items = (ModelRef *)xrealloc(list->items, (size_t)list->cap * sizeof(ModelRef));
}

static void model_list_append(ModelList *list, wchar_t *provider, wchar_t *model, wchar_t *effort) {
    model_list_grow(list);
    list->items[list->n].provider = provider;
    list->items[list->n].model = model;
    list->items[list->n].effort = effort;
    list->n++;
}

static void model_list_insert_first(ModelList *list, wchar_t *provider, wchar_t *model, wchar_t *effort) {
    model_list_grow(list);
    if (list->n > 0)
        memmove(&list->items[1], &list->items[0], (size_t)list->n * sizeof(ModelRef));
    list->items[0].provider = provider;
    list->items[0].model = model;
    list->items[0].effort = effort;
    list->n++;
}

typedef struct {
    wchar_t *p;
    int n;
    int cap;
} WBuf;

static void wbuf_reserve(WBuf *b, int extra) {
    if (b->n + extra + 1 <= b->cap) return;
    int cap = b->cap ? b->cap : 64;
    while (b->n + extra + 1 > cap) cap *= 2;
    b->p = (wchar_t *)xrealloc(b->p, (size_t)cap * sizeof(wchar_t));
    b->cap = cap;
}

static void wbuf_ch(WBuf *b, wchar_t c) {
    wbuf_reserve(b, 1);
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}

static void wbuf_s(WBuf *b, const wchar_t *s) {
    size_t n = wcslen(s);
    wbuf_reserve(b, (int)n);
    memcpy(b->p + b->n, s, n * sizeof(wchar_t));
    b->n += (int)n;
    b->p[b->n] = 0;
}

static void wbuf_arg(WBuf *b, const wchar_t *arg) {
    if (b->n) wbuf_ch(b, L' ');
    int quote = (*arg == 0);
    const wchar_t *p;
    for (p = arg; *p; p++) {
        if (*p == L' ' || *p == L'\t' || *p == L'"') quote = 1;
    }
    if (!quote) { wbuf_s(b, arg); return; }
    wbuf_ch(b, L'"');
    for (;;) {
        unsigned bs = 0;
        while (*arg == L'\\') { arg++; bs++; }
        if (!*arg) {
            while (bs--) { wbuf_ch(b, L'\\'); wbuf_ch(b, L'\\'); }
            break;
        }
        if (*arg == L'"') {
            while (bs--) { wbuf_ch(b, L'\\'); wbuf_ch(b, L'\\'); }
            wbuf_ch(b, L'\\');
            wbuf_ch(b, L'"');
        } else {
            while (bs--) wbuf_ch(b, L'\\');
            wbuf_ch(b, *arg);
        }
        arg++;
    }
    wbuf_ch(b, L'"');
}

static char *read_stdin_utf8(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    Buf b;
    memset(&b, 0, sizeof b);
    if (!in || in == INVALID_HANDLE_VALUE) {
        buf_reserve(&b, 1);
        b.p[0] = 0;
        return b.p;
    }
    for (;;) {
        char tmp[4096];
        DWORD n = 0;
        if (!ReadFile(in, tmp, sizeof tmp, &n, NULL) || n == 0) break;
        buf_add(&b, tmp, (int)n);
    }
    buf_add(&b, "", 1);
    b.n -= 1;
    return b.p;
}

static void json_escape(Buf *b, const char *s) {
    buf_str(b, "\"");
    if (!s) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            char e[3] = {'\\', (char)c, 0};
            buf_str(b, e);
        } else if (c == '\n') buf_str(b, "\\n");
        else if (c == '\r') buf_str(b, "\\r");
        else if (c == '\t') buf_str(b, "\\t");
        else if (c < 0x20) {
            char hex[8];
            sprintf(hex, "\\u%04x", c);
            buf_str(b, hex);
        } else {
            buf_add(b, p, 1);
        }
    }
    buf_str(b, "\"");
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_put(Buf *b, unsigned code) {
    if (code < 0x80) {
        char c = (char)code;
        buf_add(b, &c, 1);
    } else if (code < 0x800) {
        char s[2] = {(char)(0xC0 | (code >> 6)), (char)(0x80 | (code & 0x3F))};
        buf_add(b, s, 2);
    } else if (code < 0x10000) {
        char s[3] = {
            (char)(0xE0 | (code >> 12)),
            (char)(0x80 | ((code >> 6) & 0x3F)),
            (char)(0x80 | (code & 0x3F))
        };
        buf_add(b, s, 3);
    } else {
        char s[4] = {
            (char)(0xF0 | (code >> 18)),
            (char)(0x80 | ((code >> 12) & 0x3F)),
            (char)(0x80 | ((code >> 6) & 0x3F)),
            (char)(0x80 | (code & 0x3F))
        };
        buf_add(b, s, 4);
    }
}

static char *json_decode_string(const char **pp, const char *end) {
    const char *p = *pp;
    if (p >= end || *p != '"') return NULL;
    p++;
    Buf b;
    memset(&b, 0, sizeof b);
    while (p < end) {
        char c = *p++;
        if (c == '"') {
            buf_add(&b, "", 1);
            b.n -= 1;
            *pp = p;
            return b.p;
        }
        if (c != '\\') {
            buf_add(&b, &c, 1);
            continue;
        }
        if (p >= end) break;
        char e = *p++;
        if (e == '"' || e == '\\' || e == '/') buf_add(&b, &e, 1);
        else if (e == 'b') buf_add(&b, "\b", 1);
        else if (e == 'f') buf_add(&b, "\f", 1);
        else if (e == 'n') buf_add(&b, "\n", 1);
        else if (e == 'r') buf_add(&b, "\r", 1);
        else if (e == 't') buf_add(&b, "\t", 1);
        else if (e == 'u' && p + 4 <= end) {
            int h0 = hex_nibble(p[0]), h1 = hex_nibble(p[1]), h2 = hex_nibble(p[2]), h3 = hex_nibble(p[3]);
            if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) break;
            unsigned code = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
            p += 4;
            if (code >= 0xD800 && code <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                int g0 = hex_nibble(p[2]), g1 = hex_nibble(p[3]), g2 = hex_nibble(p[4]), g3 = hex_nibble(p[5]);
                if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0) {
                    unsigned low = (unsigned)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + (((code - 0xD800) << 10) | (low - 0xDC00));
                        p += 6;
                    }
                }
            }
            utf8_put(&b, code);
        } else break;
    }
    buf_free(&b);
    return NULL;
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static int value_bounds(const char **pp, const char *end) {
    const char *p = skip_ws(*pp, end);
    if (p >= end) return 0;
    if (*p == '"') {
        p++;
        int esc = 0;
        for (; p < end; p++) {
            if (esc) { esc = 0; continue; }
            if (*p == '\\') { esc = 1; continue; }
            if (*p == '"') { p++; *pp = p; return 1; }
        }
        return 0;
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        int in_str = 0, esc = 0;
        for (; p < end; p++) {
            char c = *p;
            if (in_str) {
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') in_str = 0;
                continue;
            }
            if (c == '"') { in_str = 1; continue; }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0 && c == close) { p++; *pp = p; return 1; }
            }
        }
        return 0;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
    *pp = p;
    return 1;
}

static int object_find(const char *obj, int len, const char *key, const char **val, int *val_len) {
    const char *p = obj;
    const char *end = obj + len;
    p = skip_ws(p, end);
    if (p >= end || *p != '{') return 0;
    p++;
    size_t key_len = strlen(key);
    while (p < end) {
        p = skip_ws(p, end);
        if (p < end && *p == '}') return 0;
        if (p >= end || *p != '"') return 0;
        const char *key_start = p;
        char *got = json_decode_string(&p, end);
        if (!got) return 0;
        int match = ((size_t)strlen(got) == key_len && memcmp(got, key, key_len) == 0);
        free(got);
        (void)key_start;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return 0;
        p++;
        const char *vs = skip_ws(p, end);
        const char *ve = vs;
        if (!value_bounds(&ve, end)) return 0;
        if (match) {
            *val = vs;
            *val_len = (int)(ve - vs);
            return 1;
        }
        p = skip_ws(ve, end);
        if (p < end && *p == ',') p++;
    }
    return 0;
}

static int object_string(const char *obj, int len, const char *key, char **out) {
    const char *val;
    int val_len;
    if (!object_find(obj, len, key, &val, &val_len)) return 0;
    if (val_len >= 4 && memcmp(val, "null", 4) == 0) { *out = NULL; return 1; }
    const char *p = val;
    char *decoded = json_decode_string(&p, val + val_len);
    if (!decoded) return 0;
    *out = decoded;
    return 1;
}

static int object_raw(const char *obj, int len, const char *key, char **out) {
    const char *val;
    int val_len;
    if (!object_find(obj, len, key, &val, &val_len)) return 0;
    char *copy = (char *)xmalloc((size_t)val_len + 1);
    memcpy(copy, val, (size_t)val_len);
    copy[val_len] = 0;
    *out = copy;
    return 1;
}

typedef struct {
    char *session;
    char *answer;
    char *error;
    char *reason;
    int exit_code;
} RunOut;

static void run_out_clear(RunOut *r) {
    free(r->session); free(r->answer); free(r->error); free(r->reason);
    memset(r, 0, sizeof *r);
    r->exit_code = 1;
}

static void consume_events(RunOut *r, const char *stdout_bytes, int n) {
    int i = 0;
    while (i < n) {
        int start = i;
        while (i < n && stdout_bytes[i] != '\n') i++;
        int len = i - start;
        if (i < n && stdout_bytes[i] == '\n') i++;
        while (len && (stdout_bytes[start + len - 1] == '\r' || stdout_bytes[start + len - 1] == ' ' ||
                       stdout_bytes[start + len - 1] == '\t')) len--;
        int sp = start;
        while (len && (stdout_bytes[sp] == ' ' || stdout_bytes[sp] == '\t')) { sp++; len--; }
        if (len <= 0) continue;
        const char *line = stdout_bytes + sp;
        char *type = NULL;
        if (!object_string(line, len, "type", &type) || !type) continue;
        if (strcmp(type, "session") == 0) {
            char *id = NULL;
            if (object_string(line, len, "sessionId", &id)) {
                free(r->session);
                r->session = id;
            }
        } else if (strcmp(type, "final") == 0) {
            char *text = NULL;
            if (object_string(line, len, "text", &text)) {
                free(r->answer);
                r->answer = text;
            }
        } else if (strcmp(type, "error") == 0) {
            char *msg = NULL;
            if (object_string(line, len, "message", &msg)) {
                free(r->error);
                r->error = msg;
            }
        } else if (strcmp(type, "status") == 0) {
            char *phase = NULL;
            if (object_string(line, len, "phase", &phase) && phase && strcmp(phase, "turn_end") == 0) {
                char *reason = NULL;
                if (object_raw(line, len, "reason", &reason)) {
                    free(r->reason);
                    r->reason = reason;
                }
            }
            free(phase);
        }
        free(type);
    }
}

typedef struct {
    HANDLE h;
    Buf buf;
} ReadJob;

static DWORD WINAPI read_stdout_thread(LPVOID param) {
    ReadJob *job = (ReadJob *)param;
    for (;;) {
        char tmp[4096];
        DWORD n = 0;
        if (!ReadFile(job->h, tmp, sizeof tmp, &n, NULL) || n == 0) break;
        buf_add(&job->buf, tmp, (int)n);
    }
    return 0;
}

static int run_once(const wchar_t *node, const wchar_t *cmd, const char *task_utf8, RunOut *out) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) return 1;
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) return 1;
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);

    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE err_inherit = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &err_inherit,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) {
        err_inherit = err;
    }

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = err_inherit;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)xmalloc(attr_size);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size);
    HANDLE inherited[3];
    inherited[0] = in_read;
    inherited[1] = out_write;
    inherited[2] = err_inherit;
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                              inherited, sizeof inherited, NULL, NULL);

    wchar_t *cmdline = (wchar_t *)xmalloc((wcslen(cmd) + 1) * sizeof(wchar_t));
    wcscpy(cmdline, cmd);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessW(node, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             NULL, NULL, &si.StartupInfo, &pi);
    free(cmdline);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (err_inherit && err_inherit != err) CloseHandle(err_inherit);
    if (!ok) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        write_err("dsh-subagent: failed to start node.exe\n");
        return 1;
    }

    ReadJob job;
    memset(&job, 0, sizeof job);
    job.h = out_read;
    HANDLE thread = CreateThread(NULL, 0, read_stdout_thread, &job, 0, NULL);
    DWORD task_len = (DWORD)strlen(task_utf8);
    DWORD written = 0;
    if (task_len) WriteFile(in_write, task_utf8, task_len, &written, NULL);
    CloseHandle(in_write);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    CloseHandle(out_read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    consume_events(out, job.buf.p ? job.buf.p : "", job.buf.n);
    buf_free(&job.buf);
    out->exit_code = (int)code;
    return (int)code;
}

static void print_help(void) {
    write_out(
        "Usage: dsh-subagent [options] [task...]\n"
        "Drive `dsh --profile headless` as an out-of-process subagent.\n"
        "Options:\n"
        "  --task <text>          task text (default: positional args, else stdin)\n"
        "  --session-id <id>      resume a persisted session\n"
        "  --cwd <dir>            working directory for the child session\n"
        "  --provider <name>      provider route (default deepseek-official)\n"
        "  --model <id>           model id (default deepseek-flash when overriding)\n"
        "  --model-pool <p>|<m>[|<e>]  pool entry, repeatable; '|' separates fields so model may contain '/'\n"
        "  --reasoning-effort <e> low / off / high / max; empty or default omits it\n"
        "  --patch <file>         extra --patch overlay (repeatable)\n"
        "  --retries <n>          total attempts on failure (default 1)\n"
        "  --retry-delay <sec>    delay between retries (default 2)\n"
        "  --raw                  print only the final answer\n"
        "  --version              print the version and exit\n"
        "  --help                 show this help\n");
}

static const char POOL_ENTRY_ERR[] =
    "dsh-subagent: model-pool entry must be provider|model or provider|model|effort "
    "(effort is low/off/high/max; omit it for the model default). "
    "Use '|' between fields so a model id may contain '/'.\n";

static int write_model_patch(const wchar_t *path, const wchar_t *provider, const wchar_t *model, const wchar_t *effort) {
    char *prov = wide_to_utf8(provider);
    char *mod = wide_to_utf8(model);
    char *eff = effort ? wide_to_utf8(effort) : NULL;
    Buf b;
    memset(&b, 0, sizeof b);
    buf_str(&b, "- id: agent-default-model\n  config:\n    provider: ");
    buf_str(&b, prov);
    buf_str(&b, "\n    model: ");
    buf_str(&b, mod);
    if (eff && eff[0]) {
        buf_str(&b, "\n    reasoningEffort: ");
        buf_str(&b, eff);
    }
    buf_str(&b, "\n");
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = 0;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(h, b.p, (DWORD)b.n, &wr, NULL) && wr == (DWORD)b.n;
        CloseHandle(h);
    }
    buf_free(&b);
    free(prov); free(mod); free(eff);
    return ok;
}

static wchar_t *make_patch(const wchar_t *provider, const wchar_t *model, const wchar_t *effort) {
    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) return NULL;
    wchar_t name[128];
    swprintf(name, 128, L"dsh-subagent-model-%08lx%08lx.yml",
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    wchar_t *path = join_path(temp, name);
    if (!write_model_patch(path, provider, model, effort)) {
        free(path);
        return NULL;
    }
    return path;
}

static void buf_model_ref(Buf *b, const ModelRef *model) {
    char *p = wide_to_utf8(model->provider);
    char *m = wide_to_utf8(model->model);
    buf_str(b, p);
    buf_str(b, "|");
    buf_str(b, m);
    if (model->effort && model->effort[0]) {
        char *e = wide_to_utf8(model->effort);
        buf_str(b, "|");
        buf_str(b, e);
        free(e);
    }
    free(p);
    free(m);
}

static void write_pool_switch(int index, int total, const ModelRef *from, const ModelRef *to, int attempts) {
    Buf b;
    memset(&b, 0, sizeof b);
    char num[64];
    sprintf(num, "dsh-subagent: model pool switch %d/%d from ", index, total);
    buf_str(&b, num);
    buf_model_ref(&b, from);
    buf_str(&b, " to ");
    buf_model_ref(&b, to);
    sprintf(num, " after %d failed attempts.\n", attempts);
    buf_str(&b, num);
    buf_reserve(&b, 1);
    b.p[b.n] = 0;
    write_err(b.p);
    buf_free(&b);
}

int wmain(int argc, wchar_t **argv) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_err = GetStdHandle(STD_ERROR_HANDLE);

    wchar_t *task = NULL;
    int task_set = 0;
    wchar_t *session = NULL;
    wchar_t *cwd = NULL;
    wchar_t *provider = NULL;
    wchar_t *model = NULL;
    wchar_t *effort = NULL;
    ModelList models;
    memset(&models, 0, sizeof models);
    WList patches;
    memset(&patches, 0, sizeof patches);
    int retries = 1;
    double retry_delay = 2;
    int raw = 0;

    int i;
    for (i = 1; i < argc; i++) {
        wchar_t *a = argv[i];
        if (streq(a, L"--task") || streq(a, L"-t")) {
            wchar_t *v = NULL;
            free(task);
            task = NULL;
            task_set = 0;
            if (take_value(argc, argv, &i, &v)) {
                task = (wchar_t *)xmalloc((wcslen(v) + 1) * sizeof(wchar_t));
                wcscpy(task, v);
                task_set = 1;
            }
        } else if (streq(a, L"--session-id") || streq(a, L"-s")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) session = v;
        } else if (streq(a, L"--cwd") || streq(a, L"-c")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) cwd = v;
        } else if (streq(a, L"--provider")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) provider = v;
        } else if (streq(a, L"--model")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) model = v;
        } else if (streq(a, L"--model-pool")) {
            wchar_t *v = NULL;
            wchar_t *p = NULL, *m = NULL, *e = NULL;
            if (!take_value(argc, argv, &i, &v) || !parse_pool_entry(v, &p, &m, &e)) {
                write_err(POOL_ENTRY_ERR);
                free(task);
                model_list_free(&models);
                return 2;
            }
            model_list_append(&models, p, m, e);
        } else if (streq(a, L"--reasoning-effort")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) effort = v;
        } else if (streq(a, L"--patch") || streq(a, L"-p")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) wlist_add(&patches, v);
        } else if (streq(a, L"--retries")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) parse_i32(v, &retries);
        } else if (streq(a, L"--retry-delay")) {
            wchar_t *v = NULL;
            if (take_value(argc, argv, &i, &v)) parse_f64(v, &retry_delay);
        } else if (streq(a, L"--raw")) {
            raw = 1;
        } else if (streq(a, L"--version")) {
            write_out("dsh-subagent " DSH_SUBAGENT_VERSION "\n");
            free(task);
            model_list_free(&models);
            return 0;
        } else if (streq(a, L"--help") || streq(a, L"-h")) {
            print_help();
            free(task);
            model_list_free(&models);
            return 0;
        } else if (a[0] == L'-' && !streq(a, L"-")) {
            char *opt = wide_to_utf8(a);
            write_err("dsh-subagent: unknown option ");
            write_err(opt);
            write_err("\n");
            free(opt);
            free(task);
            model_list_free(&models);
            return 2;
        } else if (!streq(a, L"-")) {
            if (!task_set) {
                task = (wchar_t *)xmalloc((wcslen(a) + 1) * sizeof(wchar_t));
                wcscpy(task, a);
                task_set = 1;
            } else {
                wchar_t *joined = wcat_space(task ? task : L"", a);
                free(task);
                task = joined;
            }
        }
    }

    if (!task_set) {
        char *bytes = read_stdin_utf8();
        free(task);
        task = utf8_to_wide(bytes, (int)strlen(bytes));
        free(bytes);
        task_set = 1;
    }
    {
        wchar_t *trimmed = wtrim_dup(task ? task : L"");
        free(task);
        task = trimmed;
    }
    if (!task[0]) {
        write_err("dsh-subagent: empty task (use --task or pipe stdin).\n");
        free(task);
        model_list_free(&models);
        return 2;
    }
    if (retries < 1) retries = 1;
    if (retry_delay < 0) retry_delay = 0;
    if (effort && effort_blank(effort)) effort = NULL;
    if (effort && !effort_ok(effort)) {
        write_err("dsh-subagent: reasoning-effort must be low/off/high/max, or empty for the model default.\n");
        free(task);
        model_list_free(&models);
        return 2;
    }
    if (provider || model) {
        model_list_insert_first(&models,
            wstrdup(provider ? provider : L"deepseek-official"),
            wstrdup(model ? model : L"deepseek-flash"),
            effort ? wstrdup(effort) : NULL);
    } else if (models.n == 0 && effort) {
        model_list_insert_first(&models,
            wstrdup(L"deepseek-official"),
            wstrdup(L"deepseek-flash"),
            wstrdup(effort));
    } else if (models.n > 0 && effort) {
        write_err("dsh-subagent: reasoning-effort applies to --provider/--model; put per-model effort in --model-pool as provider|model|effort.\n");
        free(task);
        model_list_free(&models);
        return 2;
    }

    wchar_t *node = NULL;
    wchar_t *bin = NULL;
    if (!find_install(&node, &bin)) {
        write_err("dsh-subagent: cannot locate dsh install (node.exe + @deepseek-ai/dsh/lib/bin.js).\n");
        free(task);
        model_list_free(&models);
        return 2;
    }
    wchar_t *old_cwd = NULL;
    if (cwd) {
        if (!dir_exists(cwd)) {
            write_err("dsh-subagent: cwd is not a directory: ");
            char *c = wide_to_utf8(cwd);
            write_err(c);
            write_err("\n");
            free(c);
            free(task); free(node); free(bin);
            model_list_free(&models);
            return 2;
        }
        DWORD n = GetCurrentDirectoryW(0, NULL);
        old_cwd = (wchar_t *)xmalloc(n * sizeof(wchar_t));
        GetCurrentDirectoryW(n, old_cwd);
        SetCurrentDirectoryW(cwd);
    }

    wchar_t *tmp_patch = NULL;
    if (models.n > 0) {
        tmp_patch = make_patch(models.items[0].provider, models.items[0].model, models.items[0].effort);
        if (!tmp_patch) {
            write_err("dsh-subagent: cannot write model patch.\n");
            free(task); free(node); free(bin);
            if (old_cwd) { SetCurrentDirectoryW(old_cwd); free(old_cwd); }
            model_list_free(&models);
            return 2;
        }
    }

    WBuf cmd;
    memset(&cmd, 0, sizeof cmd);
    wbuf_arg(&cmd, node);
    wbuf_arg(&cmd, bin);
    wbuf_s(&cmd, L" --profile headless");
    for (i = 0; i < patches.n; i++) {
        wbuf_s(&cmd, L" --patch");
        wbuf_arg(&cmd, patches.items[i]);
    }
    if (tmp_patch) {
        wbuf_s(&cmd, L" --patch");
        wbuf_arg(&cmd, tmp_patch);
    }
    wbuf_s(&cmd, L" --json");
    if (session) {
        wbuf_s(&cmd, L" --session-id");
        wbuf_arg(&cmd, session);
    }
    wbuf_s(&cmd, L" -");

    char *task_utf8 = wide_to_utf8(task);
    RunOut last;
    memset(&last, 0, sizeof last);
    last.exit_code = 1;
    int model_count = models.n > 0 ? models.n : 1;
    int mi;
    int fail_code = 0;
    for (mi = 0; mi < model_count && !fail_code; mi++) {
        if (models.n > 0 && mi > 0) {
            write_pool_switch(mi + 1, models.n, &models.items[mi - 1], &models.items[mi], retries);
            if (!write_model_patch(tmp_patch, models.items[mi].provider, models.items[mi].model, models.items[mi].effort)) {
                write_err("dsh-subagent: cannot write model patch.\n");
                fail_code = 2;
                break;
            }
        }
        for (i = 1; i <= retries; i++) {
            run_out_clear(&last);
            run_once(node, cmd.p, task_utf8, &last);
            if (last.exit_code == 0) break;
            if (i < retries) {
                char msg[128];
                sprintf(msg, "dsh-subagent: attempt %d/%d failed (exit=%d), retry in %gs.\n",
                        i, retries, last.exit_code, retry_delay);
                write_err(msg);
                if (retry_delay > 0) Sleep((DWORD)(retry_delay * 1000.0 + 0.5));
            }
        }
        if (last.exit_code == 0) break;
    }

    if (fail_code) {
        /* A later pool entry could not be written. Keep the stderr error and skip the summary. */
    } else if (raw) {
        if (last.answer) {
            write_out(last.answer);
            write_out("\n");
        }
    } else {
        Buf json;
        memset(&json, 0, sizeof json);
        char num[32];
        sprintf(num, "{\"exitCode\":%d,", last.exit_code);
        buf_str(&json, num);
        buf_str(&json, "\"sessionId\":");
        if (last.session) json_escape(&json, last.session);
        else buf_str(&json, "null");
        buf_str(&json, ",\"answer\":");
        if (last.answer) json_escape(&json, last.answer);
        else buf_str(&json, "null");
        buf_str(&json, ",\"reason\":");
        if (last.reason) buf_str(&json, last.reason);
        else buf_str(&json, "null");
        buf_str(&json, ",\"error\":");
        if (last.error) json_escape(&json, last.error);
        else buf_str(&json, "null");
        buf_str(&json, "}\n");
        write_bytes(g_out, json.p, json.n);
        buf_free(&json);
    }

    int code = fail_code ? fail_code : last.exit_code;
    run_out_clear(&last);
    if (tmp_patch) {
        DeleteFileW(tmp_patch);
        free(tmp_patch);
    }
    if (old_cwd) {
        SetCurrentDirectoryW(old_cwd);
        free(old_cwd);
    }
    free(task_utf8);
    free(cmd.p);
    free(task);
    free(node);
    free(bin);
    free(patches.items);
    model_list_free(&models);
    return code;
}
