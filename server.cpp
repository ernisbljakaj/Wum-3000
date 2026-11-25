// server.cpp - Remote Desktop Controller (Windows + Linux)
//
// Build Linux:            g++ -O2 server.cpp -o server -lX11 -lpthread
// Build Windows (native):  g++ -O2 server.cpp -o server.exe -lws2_32 -lole32
// Build von Linux -> Win:  x86_64-w64-mingw32-g++ -O2 -static server.cpp -o server.exe \
//                          -lws2_32 -lwinpthread

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <windowsx.h>
  using SOCKET_ = SOCKET;
  #define SOCK_INVALID INVALID_SOCKET
  #define CLOSESOCK    closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
  #include <X11/keysym.h>
  #include <X11/Xatom.h>
  using SOCKET_ = int;
  #define SOCK_INVALID (-1)
  #define CLOSESOCK    close
  static Display* g_dpy = nullptr;
  static Window   g_win = 0;
  static GC       g_gc  = 0;
  static Atom     g_wmDelete = 0;
#endif

// ===================== Konfiguration =====================
static const int LISTEN_PORT = 4445;
static const int FRAME_SCALE = 2;   // muss zu client.cpp passen

// ===================== Protokoll =====================
#pragma pack(push, 1)
struct FrameMsg { uint32_t magic, type, width, height, dataSize; };
struct InputMsg { uint32_t magic, type; int32_t x, y; uint32_t flags, vk; };
#pragma pack(pop)
static const uint32_t MAGIC_FRAME = 0x52444652;
static const uint32_t MAGIC_INPUT = 0x5244494E;

// ===================== State =====================
static SOCKET_            g_listen = SOCK_INVALID;
static SOCKET_            g_client = SOCK_INVALID;
static std::atomic<bool>  g_running{true};
static std::atomic<bool>  g_connected{false};

static std::mutex               g_frameMutex;
static std::vector<uint8_t>     g_frame;
static int                      g_remoteW = 0, g_remoteH = 0;
static std::atomic<bool>        g_frameDirty{false};

// ===================== Netzwerk =====================
static bool RecvAll(SOCKET_ s, void* b, int n) {
    char* p = (char*)b;
    while (n > 0 && g_running) {
        int r = (int)recv(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return n == 0;
}
static bool SendAll(SOCKET_ s, const void* b, int n) {
    const char* p = (const char*)b;
    while (n > 0) {
        int r = (int)send(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

static void SendInputMsg(int type, int x, int y, uint32_t flags, uint32_t vk) {
    if (!g_connected || g_client == SOCK_INVALID) return;
    InputMsg m;
    m.magic = MAGIC_INPUT; m.type = type;
    m.x = x; m.y = y; m.flags = flags; m.vk = vk;
    SendAll(g_client, &m, sizeof(m));
}

static void ReceiveFrames() {
    while (g_running && g_connected) {
        FrameMsg fm;
        if (!RecvAll(g_client, &fm, sizeof(fm))) break;
        if (fm.magic != MAGIC_FRAME) break;
        if (fm.dataSize > 100u * 1024u * 1024u) break;
        std::vector<uint8_t> buf(fm.dataSize);
        if (!RecvAll(g_client, buf.data(), (int)fm.dataSize)) break;
        {
            std::lock_guard<std::mutex> lk(g_frameMutex);
            g_frame     = std::move(buf);
            g_remoteW   = (int)fm.width;
            g_remoteH   = (int)fm.height;
        }
        g_frameDirty = true;
    }
    g_connected = false;
}

static void AcceptLoop() {
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == SOCK_INVALID) return;
    int opt = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(LISTEN_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0) {
        fprintf(stderr, "Bind auf %d fehlgeschlagen\n", LISTEN_PORT);
        g_running = false; return;
    }
    listen(g_listen, 1);
    fprintf(stderr, "[server] warte auf Verbindung auf Port %d...\n", LISTEN_PORT);

    while (g_running) {
        fd_set fd; FD_ZERO(&fd); FD_SET(g_listen, &fd);
        timeval tv = {0, 200000};
        int r = select((int)g_listen + 1, &fd, NULL, NULL, &tv);
        if (!g_running) break;
        if (r <= 0) continue;
        g_client = accept(g_listen, NULL, NULL);
        if (g_client == SOCK_INVALID) continue;
        g_connected = true;
        fprintf(stderr, "[server] Client verbunden\n");
        ReceiveFrames();
        g_connected = false;
        if (g_client != SOCK_INVALID) { CLOSESOCK(g_client); g_client = SOCK_INVALID; }
        fprintf(stderr, "[server] Client getrennt, warte auf neue Verbindung...\n");
    }
    if (g_listen != SOCK_INVALID) { CLOSESOCK(g_listen); g_listen = SOCK_INVALID; }
}

// ===================== Zeichnen =====================
#ifdef _WIN32
static HWND g_hwnd = NULL;
static void DrawFrame(HDC hdc, int cw, int ch) {
    std::lock_guard<std::mutex> lk(g_frameMutex);
    if (g_frame.empty() || g_remoteW == 0) {
        RECT rc = {0,0,cw,ch};
        HBRUSH bg = CreateSolidBrush(RGB(0,0,0));
        FillRect(hdc, &rc, bg); DeleteObject(bg);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(200,200,200));
        DrawTextA(hdc, "Warte auf Verbindung...", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_remoteW;
    bi.bmiHeader.biHeight      = -g_remoteH;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc, 0, 0, cw, ch, 0, 0, g_remoteW, g_remoteH,
                  g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}
static void SendMouseToClient(int mx, int my, uint32_t btns, HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int rx = (int)((double)mx * g_remoteW * FRAME_SCALE / (cw > 0 ? cw : 1));
    int ry = (int)((double)my * g_remoteH * FRAME_SCALE / (ch > 0 ? ch : 1));
    SendInputMsg(1, rx, ry, btns, 0);
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static uint32_t buttons = 0;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        DrawFrame(dc, rc.right, rc.bottom);
        EndPaint(h, &ps); return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        if (m == WM_LBUTTONDOWN) buttons |= 1;
        if (m == WM_LBUTTONUP)   buttons &= ~1u;
        if (m == WM_RBUTTONDOWN) buttons |= 2;
        if (m == WM_RBUTTONUP)   buttons &= ~2u;
        if (m == WM_MBUTTONDOWN) buttons |= 4;
        if (m == WM_MBUTTONUP)   buttons &= ~4u;
        SendMouseToClient(GET_X_LPARAM(l), GET_Y_LPARAM(l), buttons, h);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        SendInputMsg(3, 0, 0, (uint32_t)(int32_t)GET_WHEEL_DELTA_WPARAM(w), 0);
        return 0;
    }
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        uint32_t flags = (m == WM_KEYUP || m == WM_SYSKEYUP) ? 1 : 0;
        SendInputMsg(2, 0, 0, flags, (uint32_t)w);
        return 0;
    }
    case WM_CLOSE: g_running = false; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}
#else
static void DrawFrame(int cw, int ch) {
    std::lock_guard<std::mutex> lk(g_frameMutex);
    if (g_frame.empty() || g_remoteW == 0) return;
    XImage* img = XCreateImage(g_dpy, DefaultVisual(g_dpy, DefaultScreen(g_dpy)),
                               DefaultDepth(g_dpy, DefaultScreen(g_dpy)),
                               ZPixmap, 0, (char*)g_frame.data(),
                               g_remoteW, g_remoteH, 32, g_remoteW * 4);
    if (!img) return;
    XPutImage(g_dpy, g_win, g_gc, img, 0, 0, 0, 0, g_remoteW, g_remoteH);
    img->data = NULL;  // unser Puffer, XDestroyImage soll ihn nicht freigeben
    XDestroyImage(img);
}
static void SendMouseToClient(int mx, int my, uint32_t btns, int cw, int ch) {
    int rx = (int)((double)mx * g_remoteW * FRAME_SCALE / (cw > 0 ? cw : 1));
    int ry = (int)((double)my * g_remoteH * FRAME_SCALE / (ch > 0 ? ch : 1));
    SendInputMsg(1, rx, ry, btns, 0);
}
#endif

// ===================== main =====================
int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 1;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = "RDController";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA("RDController",
        "Remote Desktop - waiting for connection...",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
#else
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) { fprintf(stderr, "XOpenDisplay fehlgeschlagen\n"); return 1; }
    int scr = DefaultScreen(g_dpy);
    g_win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, scr),
                                0, 0, 1280, 720, 0,
                                BlackPixel(g_dpy, scr), BlackPixel(g_dpy, scr));
    XSelectInput(g_dpy, g_win, ExposureMask | KeyPressMask | KeyReleaseMask |
                               ButtonPressMask | ButtonReleaseMask |
                               PointerMotionMask | StructureNotifyMask);
    g_wmDelete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wmDelete, 1);
    XStoreName(g_dpy, g_win, "Remote Desktop");
    XMapWindow(g_dpy, g_win);
    g_gc = DefaultGC(g_dpy, scr);
    fprintf(stderr, "[server] Fenster erstellt, warte auf Verbindung...\n");
#endif

    std::thread acceptThread(AcceptLoop);

#ifdef _WIN32
    MSG msg;
    while (g_running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (g_frameDirty.exchange(false) && g_hwnd)
            InvalidateRect(g_hwnd, NULL, FALSE);
        Sleep(16);
    }
#else
    static uint32_t buttons = 0;
    while (g_running) {
        while (XPending(g_dpy)) {
            XEvent ev; XNextEvent(g_dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) DrawFrame(ev.xexpose.width, ev.xexpose.height);
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == g_wmDelete) g_running = false;
                break;
            case MotionNotify: {
                int cw = 1280, ch = 720;
                SendMouseToClient(ev.xmotion.x, ev.xmotion.y, buttons, cw, ch);
                break;
            }
            case ButtonPress:
                if (ev.xbutton.button == Button1) buttons |= 1;
                if (ev.xbutton.button == Button3) buttons |= 2;
                if (ev.xbutton.button == Button2) buttons |= 4;
                if (ev.xbutton.button == Button4) SendInputMsg(3, 0, 0, 120, 0);
                if (ev.xbutton.button == Button5) SendInputMsg(3, 0, 0, (uint32_t)-120, 0);
                SendMouseToClient(ev.xbutton.x, ev.xbutton.y, buttons, 1280, 720);
                break;
            case ButtonRelease:
                if (ev.xbutton.button == Button1) buttons &= ~1u;
                if (ev.xbutton.button == Button3) buttons &= ~2u;
                if (ev.xbutton.button == Button2) buttons &= ~4u;
                SendMouseToClient(ev.xbutton.x, ev.xbutton.y, buttons, 1280, 720);
                break;
            case KeyPress: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                uint32_t vk = 0;
                if (ks >= XK_a && ks <= XK_z) vk = 0x41 + (ks - XK_a);
                else if (ks >= XK_A && ks <= XK_Z) vk = 0x41 + (ks - XK_A);
                else if (ks >= XK_0 && ks <= XK_9) vk = 0x30 + (ks - XK_0);
                else if (ks >= XK_F1 && ks <= XK_F12) vk = 0x70 + (ks - XK_F1);
                else if (ks == XK_Shift_L) vk = 0xA0;
                else if (ks == XK_Shift_R) vk = 0xA1;
                else if (ks == XK_Control_L) vk = 0xA2;
                else if (ks == XK_Control_R) vk = 0xA3;
                else if (ks == XK_Alt_L) vk = 0xA4;
                else if (ks == XK_Alt_R) vk = 0xA5;
                else if (ks == XK_Return) vk = 0x0D;
                else if (ks == XK_space) vk = 0x20;
                else if (ks == XK_BackSpace) vk = 0x08;
                else if (ks == XK_Tab) vk = 0x09;
                else if (ks == XK_Escape) vk = 0x1B;
                else if (ks == XK_Left) vk = 0x25;
                else if (ks == XK_Up) vk = 0x26;
                else if (ks == XK_Right) vk = 0x27;
                else if (ks == XK_Down) vk = 0x28;
                if (vk) SendInputMsg(2, 0, 0, 0, vk);
                break;
            }
            case KeyRelease: {
                // Auto-Repeat ignorieren
                if (XEventsQueued(g_dpy, QueuedAfterReading)) {
                    XEvent next; XPeekEvent(g_dpy, &next);
                    if (next.type == KeyPress && next.xkey.time == ev.xkey.time &&
                        next.xkey.keycode == ev.xkey.keycode) {
                        XNextEvent(g_dpy, &next); break;
                    }
                }
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                uint32_t vk = 0;
                if (ks >= XK_a && ks <= XK_z) vk = 0x41 + (ks - XK_a);
                else if (ks >= XK_A && ks <= XK_Z) vk = 0x41 + (ks - XK_A);
                else if (ks >= XK_0 && ks <= XK_9) vk = 0x30 + (ks - XK_0);
                else if (ks >= XK_F1 && ks <= XK_F12) vk = 0x70 + (ks - XK_F1);
                else if (ks == XK_Shift_L) vk = 0xA0;
                else if (ks == XK_Shift_R) vk = 0xA1;
                else if (ks == XK_Control_L) vk = 0xA2;
                else if (ks == XK_Control_R) vk = 0xA3;
                else if (ks == XK_Alt_L) vk = 0xA4;
                else if (ks == XK_Alt_R) vk = 0xA5;
                else if (ks == XK_Return) vk = 0x0D;
                else if (ks == XK_space) vk = 0x20;
                else if (ks == XK_BackSpace) vk = 0x08;
                else if (ks == XK_Tab) vk = 0x09;
                else if (ks == XK_Escape) vk = 0x1B;
                if (vk) SendInputMsg(2, 0, 0, 1, vk);
                break;
            }
            }
        }
        if (g_frameDirty.exchange(false)) {
            Window r; int x, y; unsigned int w, h, b, d;
            XGetGeometry(g_dpy, g_win, &r, &x, &y, &w, &h, &b, &d);
            DrawFrame((int)w, (int)h);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
#endif

    g_running = false;
    if (g_client != SOCK_INVALID) { shutdown(g_client, 2); CLOSESOCK(g_client); }
    if (g_listen != SOCK_INVALID) { CLOSESOCK(g_listen); }
    if (acceptThread.joinable()) acceptThread.join();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}