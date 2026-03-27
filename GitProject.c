// websrv.c — Simple Multithreaded HTTP/1.1 Server for Windows (Dev-C++)
// - Dual-stack listener (IPv6 with IPv4-mapped so 127.0.0.1 works)
// - GET/HEAD, /hello route, static files, directory listing
// Build: gcc -std=gnu11 -O2 -Wall -Wextra -o websrv.exe websrv.c -lws2_32

// ------------------------------------------------------------
// Concurrency model:
// - One thread per connection (_beginthreadex)
// - No thread pool (simple but not scalable)
// - Shared state is minimal (logging + docroot)
//
// Limitations:
// - No HTTP/1.1 keep-alive
// - No POST/PUT support
// - No chunked encoding
// - Blocking I/O, thread-per-connection
//
// TODO: Add MIME types here
// TODO: Add routing table instead of hardcoded /hello
// ------------------------------------------------------------

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

#define MAX_REQ_LINE    8192
#define SEND_BUF        8192
#define DEFAULT_PORT    "8080"
#define DEFAULT_DOCROOT "."
#define RECV_TIMEOUT_MS 10000

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
static int g_log_cs_inited = 0;

static void log_request(const char* method, const char* target, int code) {
    if (!g_log_cs_inited) return;
    char d[64];
    EnterCriticalSection(&g_log_cs);
    fprintf(stdout, "[%s] %s %s -> %d\n",
            http_date(time(NULL), d, sizeof d), method, target, code);
    fflush(stdout);
    LeaveCriticalSection(&g_log_cs);
}

// ------------------------------------------------------------
// send helpers

// send() may write fewer bytes than requested.
// Loop until all bytes are sent or an error occurs.
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
// Read HTTP request headers

// Keep reading until we detect end-of-headers (\r\n\r\n).
// Note: this does NOT handle request bodies (POST, etc).
// It also assumes headers fit in buffer.
static int recv_request(SOCKET s, char* buf, int bufsz) {
    int total = 0;
    while (total < bufsz - 1) {
        int n = recv(s, buf + total, bufsz - 1 - total, 0);
        if (n <= 0) return -1;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return total;
}

// ------------------------------------------------------------
// SECURITY: Prevent directory traversal (e.g. "../../etc/passwd").
// We resolve the absolute path and ensure it starts with g_docroot.
// Also verify boundary so "C:\foo" doesn't match "C:\foobar".
static int safe_path(const char* joined, char* resolved, size_t resolvsz) {
    if (!_fullpath(resolved, joined, (int)resolvsz)) return -1;

    size_t rootlen = strlen(g_docroot);
    if (strncmp(resolved, g_docroot, rootlen) != 0) return -1;

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
// directory listing

// Append formatted string to dynamic buffer, resizing as needed.
// Similar to a growable string builder.
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

// ------------------------------------------------------------
// request handling
static unsigned __stdcall client_thread(void* arg_) {
    SOCKET s = (SOCKET)(uintptr_t)arg_;
    char buf[MAX_REQ_LINE];
    char method[8], target[1024];

    int n = recv_request(s, buf, sizeof buf);
    if (n <= 0) { closesocket(s); return 0; }

    method[0] = target[0] = 0;

    // Very minimal HTTP parsing: only extracts METHOD and PATH.
    // Ignores HTTP version and all headers.
    // Assumes request line is well-formed.
    sscanf(buf, "%7s %1023s", method, target);

    if (method[0] == 0 || target[0] == 0) {
        send_simple(s, 400, "Bad Request", "text/plain", "Bad Request\n");
        log_request("?", "?", 400);
        closesocket(s);
        return 0;
    }

    char* qs = strchr(target, '?');
    if (qs) *qs = '\0';

    if (strcmp(target, "/") == 0)
        strcpy(target, "/index.html");

    // Routing:
    // 1. /hello -> JSON response
    // 2. Otherwise map URL to filesystem under docroot
    //    - If directory: try index.html, else list directory
    //    - If file: serve it
    //    - Else: 404
    if (strncmp(target, "/hello", 6) == 0) {
        handle_hello(s, method);
        log_request(method, target, 200);
    } else {
        char joined[4096], resolved[4096];
        struct _stat64 st;

        snprintf(joined, sizeof joined, "%s\\%s",
                 g_docroot, target[0] == '/' ? target + 1 : target);

        if (safe_path(joined, resolved, sizeof resolved) != 0) {
            send_simple(s, 403, "Forbidden", "text/plain", "Forbidden\n");
            log_request(method, target, 403);
            goto done;
        }

        if (_stat64(resolved, &st) == 0) {
            // file / dir handling continues...
        } else {
            send_simple(s, 404, "Not Found", "text/plain", "Not Found\n");
            log_request(method, target, 404);
        }
    }

done:
    shutdown(s, SD_BOTH);
    closesocket(s);
    return 0;
}

// ------------------------------------------------------------
// dual-stack listener

// On IPv6 sockets, disable IPV6_V6ONLY so the same socket
// also accepts IPv4 connections via IPv4-mapped addresses.
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
            if (listen(sfd, 128) == 0) break;
        }
        closesocket(sfd); sfd = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (sfd == INVALID_SOCKET) die("could not bind");
    return sfd;
}
