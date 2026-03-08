// websrv.c — Simple Multithreaded HTTP/1.1 Server for Windows (Dev-C++)
// - Dual-stack listener (IPv6 with IPv4-mapped so 127.0.0.1 works)
// - GET/HEAD, /hello route, static files, directory listing
// Build: gcc -std=gnu11 -O2 -Wall -Wextra -o websrv.exe websrv.c -lws2_32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#pragma comment(lib, "Ws2_32.lib")

#define MAX_REQ_LINE    8192    // enough for headers too
#define SEND_BUF        8192
#define DEFAULT_PORT    "8080"
#define DEFAULT_DOCROOT "."
#define RECV_TIMEOUT_MS 10000  // 10 s recv timeout

// ------------------------------------------------------------
// Global docroot (set once in main, read-only after that)
static char g_docroot[4096];

// ------------------------------------------------------------
// utils
static void die(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); ExitProcess(1);
}

static void warnx(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

/* Thread-safe GMT date formatter */
static const char* http_date(time_t t, char* buf, size_t n) {
    struct tm g;
#if defined(_MSC_VER)
    gmtime_s(&g, &t);
#else
    static CRITICAL_SECTION cs;
    static int inited = 0;
    if (!inited) { InitializeCriticalSection(&cs); inited = 1; }
    EnterCriticalSection(&cs);
    {
        struct tm* pg = gmtime(&t);
        if (pg) g = *pg; else memset(&g, 0, sizeof g);
    }
    LeaveCriticalSection(&cs);
#endif
    strftime(buf, n, "%a, %d %b %Y %H:%M:%S GMT", &g);
    return buf;
}

/* Thread-safe request logger */
static CRITICAL_SECTION g_log_cs;
static int              g_log_cs_inited = 0;

static void log_request(const char* method, const char* target, int code) {
    if (!g_log_cs_inited) return;
    char d[64];
    EnterCriticalSection(&g_log_cs);
    fprintf(stdout, "[%s] %s %s -> %d\n",
            http_date(time(NULL), d, sizeof d), method, target, code);
    fflush(stdout);
    LeaveCriticalSection(&g_log_cs);
}

static const char* mime_type(const char* path) {
    const char* dot = strrchr(path, '.');
    char ext[16];
    char* p;

    if (!dot) return "application/octet-stream";
    strncpy(ext, dot, sizeof ext - 1);
    ext[sizeof ext - 1] = 0;

    for (p = ext; *p; ++p) *p = (char)tolower((unsigned char)*p);

    if (!strcmp(ext, ".html") || !strcmp(ext, ".htm")) return "text/html; charset=utf-8";
    if (!strcmp(ext, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(ext, ".js"))   return "application/javascript";
    if (!strcmp(ext, ".json")) return "application/json";
    if (!strcmp(ext, ".png"))  return "image/png";
    if (!strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcmp(ext, ".gif"))  return "image/gif";
    if (!strcmp(ext, ".svg"))  return "image/svg+xml";
    if (!strcmp(ext, ".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static int send_all(SOCKET s, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t n = len;
    while (n) {
        int w = send(s, p, (int)n, 0);
        if (w == SOCKET_ERROR) return -1;
        p += w; n -= w;
    }
    return 0;
}

static int sendf(SOCKET s, const char* fmt, ...) {
    char buf[SEND_BUF];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof buf) n = (int)(sizeof buf) - 1;
    return send_all(s, buf, (size_t)n);
}

// ------------------------------------------------------------
// Read entire HTTP request head (up to \r\n\r\n) with timeout.
// Returns number of bytes in buf, or -1 on error/timeout.
static int recv_request(SOCKET s, char* buf, int bufsz) {
    int total = 0;
    while (total < bufsz - 1) {
        int n = recv(s, buf + total, bufsz - 1 - total, 0);
        if (n <= 0) return -1;   // error or connection closed
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;   // full head received
    }
    return total;
}

// ------------------------------------------------------------
// Path traversal guard.
// Resolves joined path and verifies it is inside g_docroot.
// Returns 0 if safe, -1 if path escapes docroot.
static int safe_path(const char* joined, char* resolved, size_t resolvsz) {
    if (!_fullpath(resolved, joined, (int)resolvsz)) return -1;

    // Both paths are now absolute.
    size_t rootlen = strlen(g_docroot);
    if (strncmp(resolved, g_docroot, rootlen) != 0) return -1;

    // The next char after the prefix must be '\0', '\\', or '/' —
    // guards against g_docroot="C:\foo" matching "C:\foobar\..."
    char next = resolved[rootlen];
    if (next != '\0' && next != '\\' && next != '/') return -1;

    return 0;
}
// ------------------------------------------------------------
// HTTP helpers
static void send_simple(SOCKET s, int code, const char* reason,
                        const char* type, const char* body)
{
    size_t blen = body ? strlen(body) : 0;
    char d[64];
    sendf(s, "HTTP/1.1 %d %s\r\n", code, reason);
    sendf(s, "Date: %s\r\n", http_date(time(NULL), d, sizeof d));
    sendf(s, "Server: win-http/1.0\r\n");
    sendf(s, "Content-Type: %s\r\n", type ? type : "text/plain");
    sendf(s, "Content-Length: %zu\r\n", blen);
    sendf(s, "Connection: close\r\n\r\n");
    if (blen) send_all(s, body, blen);
}

static void send_404(SOCKET s) {
    send_simple(s, 404, "Not Found", "text/html",
      "<html><body><h1>404 Not Found</h1></body></html>");
}

static void send_403(SOCKET s) {
    send_simple(s, 403, "Forbidden", "text/html",
      "<html><body><h1>403 Forbidden</h1></body></html>");
}

// ------------------------------------------------------------
// route: /hello
static void handle_hello(SOCKET s, const char* method) {
    const char* msg = "{\"message\":\"Hello from Windows HTTP Server!\"}\n";
    char d[64];
    sendf(s, "HTTP/1.1 200 OK\r\n");
    sendf(s, "Date: %s\r\n", http_date(time(NULL), d, sizeof d));
    sendf(s, "Server: win-http/1.0\r\n");
    sendf(s, "Content-Type: application/json\r\n");
    sendf(s, "Content-Length: %zu\r\n", strlen(msg));
    sendf(s, "Connection: close\r\n\r\n");
    if (_stricmp(method, "HEAD") != 0) send_all(s, msg, strlen(msg));
}

// ------------------------------------------------------------
// file serving
static int send_file(SOCKET s, const char* path, const char* method) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    struct _stat64 st;
    if (_stat64(path, &st) != 0) { fclose(f); return -1; }

    size_t size = (size_t)st.st_size;
    const char* type = mime_type(path);
    char d[64];

    sendf(s, "HTTP/1.1 200 OK\r\n");
    sendf(s, "Date: %s\r\n", http_date(time(NULL), d, sizeof d));
    sendf(s, "Server: win-http/1.0\r\n");
    sendf(s, "Content-Type: %s\r\n", type);
    sendf(s, "Content-Length: %zu\r\n", size);
    sendf(s, "Connection: close\r\n\r\n");

    if (_stricmp(method, "HEAD") == 0) { fclose(f); return 0; }

    char buf[SEND_BUF]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        if (send_all(s, buf, r) != 0) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

// ------------------------------------------------------------
// directory listing — builds full body first so Content-Length is known
static int send_dir_listing(SOCKET s, const char* fs_path, const char* url_path) {
    // Build the listing body into a dynamic buffer
    size_t body_cap = 4096, body_len = 0;
    char* body = (char*)malloc(body_cap);
    if (!body) return -1;

#define BODY_APPENDF(fmt, ...) \
    do { \
        int _n; \
        while (1) { \
            _n = snprintf(body + body_len, body_cap - body_len, fmt, ##__VA_ARGS__); \
            if (_n < 0) { free(body); return -1; } \
            if ((size_t)_n < body_cap - body_len) { body_len += (size_t)_n; break; } \
            body_cap *= 2; \
            char* _tmp = (char*)realloc(body, body_cap); \
            if (!_tmp) { free(body); return -1; } \
            body = _tmp; \
        } \
    } while (0)

    BODY_APPENDF(
        "<html><head><title>Index of %s</title></head>"
        "<body><h1>Index of %s</h1><ul>",
        url_path, url_path);

    char pattern[MAX_PATH];
    WIN32_FIND_DATAA f; HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", fs_path);
    h = FindFirstFileA(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) { free(body); return -1; }

    do {
        const char* name = f.cFileName;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        int isdir = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const char* trail_url = (url_path[strlen(url_path)-1] == '/') ? "" : "/";

        BODY_APPENDF("<li><a href=\"%s%s%s\">%s%s</a></li>",
            url_path, trail_url, name, name, isdir ? "/" : "");
    } while (FindNextFileA(h, &f));
    FindClose(h);

    BODY_APPENDF("</ul></body></html>");

#undef BODY_APPENDF

    // Now we know the exact length — send proper headers
    char d[64];
    sendf(s, "HTTP/1.1 200 OK\r\n");
    sendf(s, "Date: %s\r\n", http_date(time(NULL), d, sizeof d));
    sendf(s, "Server: win-http/1.0\r\n");
    sendf(s, "Content-Type: text/html; charset=utf-8\r\n");
    sendf(s, "Content-Length: %zu\r\n", body_len);
    sendf(s, "Connection: close\r\n\r\n");

    int rc = send_all(s, body, body_len);
    free(body);
    return rc;
}

// ------------------------------------------------------------
// request handling
static unsigned __stdcall client_thread(void* arg_) {
    SOCKET s = (SOCKET)(uintptr_t)arg_;
    char buf[MAX_REQ_LINE];
    char method[8], target[1024];

    // --- recv with timeout already set on the socket (see main) ---
    int n = recv_request(s, buf, sizeof buf);
    if (n <= 0) { closesocket(s); return 0; }

    method[0] = target[0] = 0;
    sscanf(buf, "%7s %1023s", method, target);
    if (method[0] == 0 || target[0] == 0) {
        send_simple(s, 400, "Bad Request", "text/plain", "Bad Request\n");
        log_request("?", "?", 400);
        closesocket(s);
        return 0;
    }

    // Strip query string so file paths work cleanly
    char* qs = strchr(target, '?');
    if (qs) *qs = '\0';

    // Serve index.html when path is "/"
    if (strcmp(target, "/") == 0)
        strcpy(target, "/index.html");

    if (strncmp(target, "/hello", 6) == 0) {
        handle_hello(s, method);
        log_request(method, target, 200);
    } else {
        char joined[4096], resolved[4096];
        struct _stat64 st;

        // Join docroot + target (skip leading '/')
        snprintf(joined, sizeof joined, "%s\\%s",
                 g_docroot, target[0] == '/' ? target + 1 : target);

        // --- PATH TRAVERSAL GUARD ---
        if (safe_path(joined, resolved, sizeof resolved) != 0) {
            send_403(s);
            log_request(method, target, 403);
            goto done;
        }

        if (_stat64(resolved, &st) == 0) {
            if (st.st_mode & _S_IFDIR) {
                char idx[4096];
                char last = resolved[strlen(resolved) - 1];
                if (last == '\\' || last == '/')
                    snprintf(idx, sizeof idx, "%sindex.html", resolved);
                else
                    snprintf(idx, sizeof idx, "%s\\index.html", resolved);

                // idx also needs traversal check (it's derived, so it's safe,
                // but run it anyway for defence-in-depth)
                char idx_res[4096];
                if (safe_path(idx, idx_res, sizeof idx_res) == 0 &&
                    _stat64(idx_res, &st) == 0 && (st.st_mode & _S_IFREG)) {
                    (void)send_file(s, idx_res, method);
                    log_request(method, target, 200);
                } else {
                    (void)send_dir_listing(s, resolved, target);
                    log_request(method, target, 200);
                }
            } else if (st.st_mode & _S_IFREG) {
                (void)send_file(s, resolved, method);
                log_request(method, target, 200);
            } else {
                send_404(s);
                log_request(method, target, 404);
            }
        } else {
            send_404(s);
            log_request(method, target, 404);
        }
    }

done:
    shutdown(s, SD_BOTH);
    closesocket(s);
    return 0;
}

// ------------------------------------------------------------
// dual-stack listener (IPv6 w/ IPv4-mapped addresses)
static SOCKET open_listen_dual(const char* port) {
    struct addrinfo hints, *res = NULL, *rp;
    SOCKET sfd = INVALID_SOCKET;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &res) != 0) die("getaddrinfo failed");

    for (rp = res; rp; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == INVALID_SOCKET) continue;

        BOOL yes = TRUE;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

        if (rp->ai_family == AF_INET6) {
            DWORD off = 0;
            setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off);
        }

        if (bind(sfd, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
            if (listen(sfd, 128) == 0) break;   // success
        }
        closesocket(sfd); sfd = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (sfd == INVALID_SOCKET) die("could not bind to port %s", port);
    return sfd;
}
// ------------------------------------------------------------
int main(int argc, char** argv) {
    const char* port = DEFAULT_PORT;
    const char* docroot_arg = DEFAULT_DOCROOT;
    SOCKET server;
    WSADATA wsa;

    // Parse args: -p PORT  -r DOCROOT
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) docroot_arg = argv[++i];
    }

    // Validate and resolve docroot into global
    {
        struct _stat64 st;
        if (!_fullpath(g_docroot, docroot_arg, sizeof g_docroot))
            die("docroot invalid: %s", docroot_arg);
        if (_stat64(g_docroot, &st) != 0 || !(st.st_mode & _S_IFDIR))
            die("docroot not a directory: %s", g_docroot);
    }

    // Initialise logging critical section
    InitializeCriticalSection(&g_log_cs);
    g_log_cs_inited = 1;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup failed");

    server = open_listen_dual(port);

    warnx("Serving %s on http://127.0.0.1:%s/", g_docroot, port);

    for (;;) {
        struct sockaddr_storage ss; int slen = sizeof ss;
        SOCKET cs = accept(server, (struct sockaddr*)&ss, &slen);
        if (cs == INVALID_SOCKET) continue;

        // Apply recv timeout so hung clients don't hold a thread forever
        DWORD tmo = RECV_TIMEOUT_MS;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);

        uintptr_t th = _beginthreadex(NULL, 0, client_thread,
                                      (void*)(uintptr_t)cs, 0, NULL);
        if (th) CloseHandle((HANDLE)th);
        else    { closesocket(cs); }
    }

    closesocket(server);
    WSACleanup();
    DeleteCriticalSection(&g_log_cs);
    return 0;
}
