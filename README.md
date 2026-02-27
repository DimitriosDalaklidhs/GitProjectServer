# Multithreaded HTTP Server in C (Winsock2)

A lightweight HTTP server written in C for Windows using the Winsock2 API. Handles multiple client connections concurrently, serves static files, and exposes a simple JSON route.

---

## Features

- Multi-threaded client handling via `_beginthreadex`
- IPv4 & IPv6 dual-stack support (binds on both `127.0.0.1` and `::1`)
- Static file serving (`.html`, `.css`, `.js`, `.png`, etc.)
- Built-in `/hello` JSON route for testing
- Proper HTTP response headers (date, content-type, connection)
- Directory listing fallback when `index.html` is missing
- Fully offline, no external dependencies

---

## Build

**Command line (MinGW):**
```bash
gcc GitProject.c -o GitProjectServer.exe -lws2_32 -std=c99
```

**Dev-C++ 5.11:**  
Open `GitProject.dev`, press **F11** or go to **Execute → Compile & Run**.  
If you see Winsock linking errors, ensure your linker includes `-lws2_32`.

---

## Usage

Start the server:
```bash
GitProjectServer.exe -p 8080 -r www
```

Then open your browser:

- `http://127.0.0.1:8080/` — serves `www/index.html`
- `http://127.0.0.1:8080/hello` — returns `{ "message": "Hello World" }`

---

## Multithreading Model

Each incoming connection is dispatched to a new thread:

```c
_beginthreadex(NULL, 0, client_thread, a, 0, NULL);
```

This keeps the main thread free so it accept new connections while existing ones are being handled.

---

## Implementation Notes

- Uses `WSAStartup` / `WSACleanup` for Winsock lifecycle management
- `getaddrinfo` with `AF_UNSPEC` and `IPV6_V6ONLY = 0` allows IPv4-mapped connections on an IPv6 socket
- `warnx()` for non-fatal logging, `die()` for fatal errors (calls `ExitProcess(1)`)
- `http_date()` returns a GMT date string in HTTP format using a thread-safe wrapper via `CRITICAL_SECTION` and `gmtime`

---

## License

MIT — free for personal and educational use.

---

## Author

**Dimitrios Dalaklidis** — CS student at the University of Western Macedonia, interested in backend development and systems programming.  
📧 dalaklidesdemetres@gmail.com
