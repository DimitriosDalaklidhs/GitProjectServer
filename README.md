# GitProjectServer

A **multithreaded HTTP server written in C** using **Winsock2** for Windows.  
This lightweight project demonstrates how to handle multiple client connections concurrently using `_beginthreadex` and serve static files or dynamic JSON routes.



##  Features
Multi-threaded client handling using `_beginthreadex`
 IPv4 & IPv6 dual-stack support (serves both `127.0.0.1` and `::1`)
  Serves static files (e.g. `.html`, `.css`, `.js`, `.png`, etc.)
  Built-in `/hello` JSON route for testing
  Proper HTTP headers (date, content-type, connection)
  Simple directory listings when `index.html` is missing
  Works fully offline on Windows





##  Build Instructions

###  Option 1 — Using Dev-C++ 5.11
1. Open `GitProject.dev` in Dev-C++  
2. Press **F11** or click **Execute → Compile & Run**
3. The output executable (e.g. `ProjectGitLab.exe`) will be generated in the project directory.

>  If you see Winsock linking errors, make sure your linker includes:
> ```
> -lws2_32
> ```

---

###  Option 2 — Using Command Line (MinGW)
If you have MinGW installed, run:

```bash
gcc GitProject.c -o GitProjectServer.exe -lws2_32 -std=c99
```
---
Then launch the server with:
```bash
GitProjectServer.exe -p 8080 -r www
```
---
 USAGE:

Once the server is running, open your browser and go to:
http://127.0.0.1:8080/
 → serves www/index.html


http://127.0.0.1:8080/hello
 → returns JSON: Hello World
 Multithreading Model

Each incoming connection is handled in a separate thread using:
```C
_beginthreadex(NULL, 0, client_thread, a, 0, NULL);
```

This ensures responsive performance even with multiple clients connected simultaneously.


### Implementation Notes 

- Uses WSAStartup / WSACleanup and the WinSock2 API.

- Uses getaddrinfo with AF_UNSPEC and binds on IPv6 with IPV6_V6ONLY = 0 to allow IPv4-mapped connections.

- Logging and fatal errors go through small helpers:

       1) warnx() for non-fatal logs

       2) die() for fatal errors (prints a message and calls ExitProcess(1)).

- http_date() returns a proper GMT date string in HTTP format, using a thread-safe wrapper on MinGW via CRITICAL_SECTION plus gmtime.
---
# License

This project is licensed under the MIT License — free for personal and educational use.


---
### Author

Dimitrios Dalaklidis is an aspiring backend developer with a strong academic foundation in Informatics and hands-on experience in systems programming, data structures, and software architecture. His work reflects a methodical approach to problem solving, supported by practical exposure to multi-language development environments and structured programming disciplines. He has completed a range of projects involving low-level system operations in C, object-oriented application design in Java, browser-based scripting, and networked communication models.

His technical interests center on backend system design, algorithmic efficiency, and the construction of reliable, maintainable software. He actively pursues opportunities to expand his expertise through academically driven projects and independent research, with an emphasis on building robust systems that adhere to professional development practices and modern software engineering principles.

For professional communication, he can be reached at: dalaklidesdemetres@gmail.com

Dimitrios Dalaklidhs
Undergraduate Informatics student at the University of Western Macedonia
Focused on low-level systems, algorithms, and C programming.

GitHub: DimitriosDalaklidhs
