// Simple Multithreaded HTTP/1.1 Server for Windows (Dev-C++)
// - Dual-stack listener (IPv6 + IPv4-mapped -> 127.0.0.1 works)
// - GET/HEAD, /hello route, static files, directory listing
// Build: gcc -std=c11 -O2 -Wall -Wextra -o websrv websrv.c -lws2_32
// websrv.c — Simple Multithreaded HTTP/1.1 Server for Windows (Dev-C++)
// - Dual-stack listener (IPv6 with IPv4-mapped so 127.0.0.1 works)
// - GET/HEAD, /hello route, static files, directory listing
// Build: gcc -std=gnu11 -O2 -Wall -Wextra -o ProjectGitLab.exe websrv.c -lws2_32

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

#define MAX_REQ_LINE    4096
#define SEND_BUF        8192
#define DEFAULT_PORT    "8080"
#define DEFAULT_DOCROOT "."

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

/* Portable GMT date formatter (MinGW-safe) */
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
    if ((size_t)n > sizeof buf) n = (int)sizeof buf;
    return send_all(s, buf, (size_t)n);
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
// file + directory serving
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

static int send_dir_listing(SOCKET s, const char* fs_path, const char* url_path) {
    char head[1024];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><head><title>Index of %s</title></head>"
        "<body><h1>Index of %s</h1><ul>",
        url_path, url_path);
    if (n < 0) return -1;
    if (send_all(s, head, (size_t)n) != 0) return -1;

    char pattern[MAX_PATH];
    WIN32_FIND_DATAA f; HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", fs_path);
    h = FindFirstFileA(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return -1;

    do {
        const char* name = f.cFileName;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        int isdir = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        char line[1024];
        n = snprintf(line, sizeof line, "<li><a href=\"%s%s%s\">%s%s</a></li>",
            url_path, (url_path[strlen(url_path)-1]=='/')? "" : "/", name, name, isdir? "/" : "");
        if (n < 0 || send_all(s, line, (size_t)n) != 0) { FindClose(h); return -1; }
    } while (FindNextFileA(h, &f));
    FindClose(h);

    return send_all(s, "</ul></body></html>", 20);
}

// ------------------------------------------------------------
// request handling
typedef struct {
    SOCKET s;
    char   docroot[1024];
} ConnArgs;

static unsigned __stdcall client_thread(void* arg_) {
    ConnArgs* a = (ConnArgs*)arg_;
    SOCKET s = a->s;
    char docroot[1024];
    char buf[MAX_REQ_LINE];
    int n;
    char method[8], target[1024];

    strcpy(docroot, a->docroot);
    free(a);

    n = recv(s, buf, sizeof buf - 1, 0);
    if (n <= 0) { closesocket(s); return 0; }
    buf[n] = 0;

    // Parse very basic: METHOD SP PATH
    method[0] = target[0] = 0;
    sscanf(buf, "%7s %1023s", method, target);
    if (method[0] == 0 || target[0] == 0) {
        send_simple(s, 400, "Bad Request", "text/plain", "Bad Request\n");
        closesocket(s);
        return 0;
    }

    // ? Serve index.html when path is "/"
    if (strcmp(target, "/") == 0) {
        strcpy(target, "/index.html");
    }

    if (strncmp(target, "/hello", 6) == 0) {
        handle_hello(s, method);
    } else {
        char path[2048];
        struct _stat64 st;

        // join docroot + target (skip leading '/')
        snprintf(path, sizeof path, "%s\\%s", docroot, target[0]=='/' ? target+1 : target);

        if (_stat64(path, &st) == 0) {
            if (st.st_mode & _S_IFDIR) {
                // try index.html inside the directory
                char idx[2048];
                if (path[strlen(path)-1] == '\\' || path[strlen(path)-1] == '/')
                    snprintf(idx, sizeof idx, "%sindex.html", path);
                else
                    snprintf(idx, sizeof idx, "%s\\index.html", path);

                if (_stat64(idx, &st) == 0 && (st.st_mode & _S_IFREG))
                    (void)send_file(s, idx, method);
                else
                    (void)send_dir_listing(s, path, target);
            } else if (st.st_mode & _S_IFREG) {
                (void)send_file(s, path, method);
            } else {
                send_404(s);
            }
        } else {
            send_404(s);
        }
    }

    shutdown(s, SD_BOTH);
    closesocket(s);
    return 0;
}

// ------------------------------------------------------------
// dual-stack listener (IPv6 w/ IPv4-mapped addresses)
static SOCKET open_listen_dual(const char* port) {
    struct addrinfo hints, *res=NULL, *rp;
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
            if (listen(sfd, 128) == 0) break;  // success
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
    char docroot[1024];
    SOCKET server;
    WSADATA wsa;

    // defaults
    strncpy(docroot, DEFAULT_DOCROOT, sizeof docroot - 1);
    docroot[sizeof docroot - 1] = 0;

    // args: -p PORT  -r DOCROOT
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            strncpy(docroot, argv[++i], sizeof docroot - 1);
            docroot[sizeof docroot - 1] = 0;
        }
    }

    // validate docroot
    {
        char absdir[4096];
        struct _stat64 st;
        if (!_fullpath(absdir, docroot, sizeof absdir)) die("docroot invalid: %s", docroot);
        if (_stat64(absdir, &st) != 0 || !(st.st_mode & _S_IFDIR)) die("docroot invalid: %s", docroot);
        strncpy(docroot, absdir, sizeof docroot - 1);
        docroot[sizeof docroot - 1] = 0;
    }

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) die("WSAStartup failed");

    server = open_listen_dual(port);

    warnx("Server running at http://127.0.0.1:%s/ serving %s", port, docroot);

    for (;;) {
        struct sockaddr_storage ss; int slen = sizeof ss;
        SOCKET cs = accept(server, (struct sockaddr*)&ss, &slen);
        if (cs == INVALID_SOCKET) continue;

        ConnArgs* a = (ConnArgs*)calloc(1, sizeof *a);
        a->s = cs;
        strncpy(a->docroot, docroot, sizeof a->docroot - 1);
        a->docroot[sizeof a->docroot - 1] = 0;

        uintptr_t th = _beginthreadex(NULL, 0, client_thread, a, 0, NULL);
        if (th) CloseHandle((HANDLE)th);
        else { closesocket(cs); free(a); }
    }

    closesocket(server);
    WSACleanup();
    return 0;
}

