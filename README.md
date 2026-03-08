# Multithreaded HTTP Server in C (Winsock2)

A lightweight HTTP/1.1 server written in C for Windows using the Winsock2 API. Handles multiple client connections concurrently, serves static files, and exposes a simple JSON route.

---

## Features

- Multi-threaded client handling via `_beginthreadex` — socket passed directly per thread, no heap allocation per connection
- IPv4 & IPv6 dual-stack support (`IPV6_V6ONLY = 0` allows IPv4-mapped connections on an IPv6 socket)
- Static file serving with MIME type detection (`.html`, `.css`, `.js`, `.json`, `.png`, `.jpg`, `.gif`, `.svg`, `.txt`)
- Built-in `/hello` JSON route returning `{"message":"Hello from Windows HTTP Server!"}`
- Directory listing with `Content-Length` when `index.html` is absent
- Path traversal protection — requests escaping the docroot are rejected with 403
- Request logging to stdout: `[date] METHOD path -> status`
- Recv timeout (10 s) to prevent hung clients from holding threads indefinitely
- Query string stripping before filesystem resolution
- Fully offline, no external dependencies

---

## Project Structure

```
GIT_HTTP_SERVER_WINDOWS/
├── GitProject.c          # Server source code
├── GitProjectServer.exe  # Compiled binary (after build)
├── index.html            # Default page served at http://127.0.0.1:8080/
└── Makefile.win          # Dev-C++ project makefile
```

**Note:** The server looks for `index.html` in whichever directory you pass to `-r`. If it is missing, the server will return a directory listing instead. To get started quickly, drop any `index.html` into the same folder as the executable and run:

```bash
./GitProjectServer.exe -p 8080 -r .
```

---

## Build

**Command line (MinGW):**
```bash
gcc GitProject.c -o GitProjectServer.exe -lws2_32 -std=gnu11 -O2 -Wall -Wextra
```

> The `#pragma comment(lib, "Ws2_32.lib")` warning from MinGW is harmless — Winsock is linked correctly via `-lws2_32` on the command line.

**Dev-C++ 5.11:**  
Open the project file, press **F11** or go to **Execute → Compile & Run**.  
If you see Winsock linking errors, confirm your linker flags include `-lws2_32`.

---

## Usage

Start the server:
```bash
./GitProjectServer.exe -p 8080 -r .
```

Then open your browser:
- `http://127.0.0.1:8080/` — serves `index.html` from the specified directory
- `http://127.0.0.1:8080/hello` — returns `{"message":"Hello from Windows HTTP Server!"}`
- `http://127.0.0.1:8080/somedir/` — directory listing if no `index.html` present

---

## Multithreading Model

Each incoming connection is dispatched to a new OS thread:
```c
_beginthreadex(NULL, 0, client_thread, (void*)(uintptr_t)cs, 0, NULL);
```

The main thread stays free to call `accept` continuously. A 10-second `SO_RCVTIMEO` timeout is applied to each socket before dispatch so that slow or silent clients cannot hold a thread open indefinitely.

---

## Security

Requests are checked for path traversal before any file is opened. The raw URL target is joined with the docroot, resolved to an absolute path via `_fullpath`, and then verified to still sit inside the docroot prefix. Requests that escape it — e.g. `GET /../../sensitive.txt` — receive a `403 Forbidden` and are logged.

---

## Implementation Notes

- `WSAStartup` / `WSACleanup` manage the Winsock lifecycle
- `getaddrinfo` with `AF_UNSPEC` and `IPV6_V6ONLY = 0` enables dual-stack on a single socket
- `http_date()` produces a GMT string in HTTP date format using a `CRITICAL_SECTION` + `gmtime` wrapper for thread safety
- `log_request()` is also protected by a `CRITICAL_SECTION` so concurrent threads don't interleave log output
- `warnx()` for non-fatal messages, `die()` for fatal errors (calls `ExitProcess(1)`)
- Directory listing body is buffered in full before sending so `Content-Length` is always accurate

---

## License

MIT — free for personal and educational use.

---

## Author

**Dimitrios Dalaklidis** — CS student at the University of Western Macedonia, interested in backend development and systems programming.  
📧 dalaklidesdemetres@gmail.com
