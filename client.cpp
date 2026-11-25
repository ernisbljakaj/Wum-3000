// client.cpp - Remote Desktop Agent (Windows + Linux)
//
// Build Linux:            g++ -O2 client.cpp -o client -lX11 -lXtst -lpthread
// Build Windows (native):  g++ -O2 client.cpp -o client.exe -lws2_32 -lole32
// Build von Linux -> Win:  x86_64-w64-mingw32-g++ -O2 -static client.cpp -o client.exe \
//                          -lws2_32 -lole32 -lwinpthread
//
// Nur mit ausdruecklicher Einwilligung der Person an diesem PC verwenden.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <vector>
#include <thread>
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
  #include <X11/extensions/XTest.h>
  using SOCKET_ = int;
  #define SOCK_INVALID (-1)
  #define CLOSESOCK    close
  static Display* g_dpy = nullptr;
#endif

// ===================== Konfiguration =====================
static const char* SERVER_IP     = "192.168.1.100"; // <-- IP des Controllers
static const int   SERVER_PORT   = 4445;
static const int   FRAME_SCALE   = 2;    // 1 = volle Aufloesung, 2 = halb
static const int   FRAME_SLEEP   = 80;   // ms -> ~12 FPS

// ===================== Protokoll =====================
#pragma pack(push, 1)
struct FrameMsg { uint32_t magic, type, width, height, dataSize; };
struct InputMsg { uint32_t magic, type; int32_t x, y; uint32_t flags, vk; };
#pragma pack(pop)
static const uint32_t MAGIC_FRAME = 0x52444652; // 'RDFR'
static const uint32_t MAGIC_INPUT = 0x5244494E; // 'RDIN'

static SOCKET_            g_sock = SOCK_INVALID;
static std::atomic<bool>  g_running{true};
static std::atomic<bool>  g_connected{false};

// ===================== Einwilligung =====================
static bool AskConsent() {
#ifdef _WIN32
    int r = MessageBoxA(NULL,
        "Ein Remote-Desktop-Controller moechte auf diesen PC zugreifen.\n\n"
        "Bildschirm wird uebertragen, Maus/Tastatur ferngesteuert.\n"
        "Es erscheint ein sichtbarer Hinweis waehrend der Sitzung.\n\nZustimmen?",
        "Remote Desktop - Einwilligung",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    return r == IDYES;
#else
    const char* msg =
        "Ein Remote-Desktop-Controller moechte auf diesen PC zugreifen.\n"
        "Bildschirm wird uebertragen, Maus/Tastatur ferngesteuert.\n"
        "Zustimmen? (y/n): ";
    if (isatty(0)) {
        fputs(msg, stderr); fflush(stderr);
        char c; if (scanf(" %c", &c) != 1) return false;
        return c == 'y' || c == 'Y';
    }
    int rc = system("xmessage -center -buttons 'Ja:0,Nein:1' "
                    "'Remote-Zugriff auf diesen PC erlauben?' >/dev/null 2>&1");
    return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
#endif
}

// ===================== Sichtbarer Hinweis =====================
#ifdef _WIN32
static HWND g_indicator = NULL;
static LRESULT CALLBACK IndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(180,0,0)); FillRect(dc, &rc, br);
        DeleteObject(br);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255));
        DrawTextA(dc, "REMOTE SESSION ACTIVE - Klick zum Beenden",
                  -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_LBUTTONDOWN || m == WM_CLOSE) {
        g_running = false;
        if (g_sock != SOCK_INVALID) shutdown(g_sock, SD_BOTH);
        DestroyWindow(h); return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}
static void CreateIndicator() {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = IndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = "RDIndicator";
    wc.hCursor       = LoadCursor(NULL, IDC_HAND);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    g_indicator = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "RDIndicator", "Remote Session", WS_POPUP,
        sw - 420, 20, 400, 40, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (g_indicator) ShowWindow(g_indicator, SW_SHOW);
}
#endif

#ifndef _WIN32
static void SignalHandler(int) { g_running = false; }
static void IndicatorLoop() {
    int tick = 0;
    while (g_running && g_connected) {
        if (tick % 40 == 0)
            fprintf(stderr, "[REMOTE SESSION ACTIVE] Ctrl+C zum Beenden\n");
        tick++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
#endif

// ===================== Bildschirmaufnahme =====================
#ifdef _WIN32
static bool CaptureScreen(std::vector<uint8_t>& out, int& w, int& h) {
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) return false;
    HDC hScreen = GetDC(NULL);
    HDC hMem    = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hScreen, w, h);
    HGDIOBJ old  = SelectObject(hMem, hBmp);
    BitBlt(hMem, 0, 0, w, h, hScreen, 0, 0, SRCCOPY);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.resize((size_t)w * h * 4);
    GetDIBits(hMem, hBmp, 0, h, out.data(), &bi, DIB_RGB_COLORS);
    SelectObject(hMem, old); DeleteObject(hBmp);
    DeleteDC(hMem); ReleaseDC(NULL, hScreen);
    return true;
}
#else
static bool CaptureScreen(std::vector<uint8_t>& out, int& w, int& h) {
    if (!g_dpy) return false;
    Window root = DefaultRootWindow(g_dpy);
    XWindowAttributes attrs;
    XGetWindowAttributes(g_dpy, root, &attrs);
    w = attrs.width; h = attrs.height;
    if (w <= 0 || h <= 0) return false;
    XImage* img = XGetImage(g_dpy, root, 0, 0, w, h, AllPlanes, ZPixmap);
    if (!img) return false;
    out.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(&out[(size_t)y * w * 4], img->data + (size_t)y * img->bytes_per_line, (size_t)w * 4);
    XDestroyImage(img);
    return true;
}
#endif

// ===================== Herunterskalieren =====================
static void Downscale(const std::vector<uint8_t>& src, int sw, int sh,
                      std::vector<uint8_t>& dst, int& dw, int& dh) {
    dw = sw / FRAME_SCALE; dh = sh / FRAME_SCALE;
    if (dw <= 0 || dh <= 0) { dst.clear(); return; }
    dst.resize((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        const uint8_t* s = &src[(size_t)(y * FRAME_SCALE) * sw * 4];
        uint8_t* d = &dst[(size_t)y * dw * 4];
        for (int x = 0; x < dw; x++)
            memcpy(d + x * 4, s + (size_t)(x * FRAME_SCALE) * 4, 4);
    }
}

// ===================== Eingabe-Injektion =====================
#ifdef _WIN32
static uint32_t g_prevButtons = 0;
static void InjectMouse(int x, int y, uint32_t btns) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((double)x * 65535.0 / (sw > 1 ? sw - 1 : 1));
    in.mi.dy = (LONG)((double)y * 65535.0 / (sh > 1 ? sh - 1 : 1));
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    uint32_t c = btns & 7, p = g_prevButtons;
    if ((c&1) && !(p&1)) in.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    if (!(c&1) && (p&1)) in.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    if ((c&2) && !(p&2)) in.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
    if (!(c&2) && (p&2)) in.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
    if ((c&4) && !(p&4)) in.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
    if (!(c&4) && (p&4)) in.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
    g_prevButtons = c;
    SendInput(1, &in, sizeof(INPUT));
}
static void InjectKey(uint32_t vk, bool isUp) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    if (isUp) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}
static void InjectWheel(int delta) {
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}
#else
static KeySym VkToKeysym(uint32_t vk) {
    if (vk >= 0x41 && vk <= 0x5A) return XK_a + (vk - 0x41);
    if (vk >= 0x30 && vk <= 0x39) return XK_0 + (vk - 0x30);
    if (vk >= 0x70 && vk <= 0x7B) return XK_F1 + (vk - 0x70);
    switch (vk) {
    case 0x08: return XK_BackSpace; case 0x09: return XK_Tab;
    case 0x0D: return XK_Return;    case 0x1B: return XK_Escape;
    case 0x20: return XK_space;     case 0x10: case 0xA0: return XK_Shift_L;
    case 0xA1: return XK_Shift_R;   case 0x11: case 0xA2: return XK_Control_L;
    case 0xA3: return XK_Control_R; case 0x12: case 0xA4: return XK_Alt_L;
    case 0xA5: return XK_Alt_R;     case 0x5B: return XK_Super_L;
    case 0x5C: return XK_Super_R;   case 0x25: return XK_Left;
    case 0x26: return XK_Up;        case 0x27: return XK_Right;
    case 0x28: return XK_Down;      case 0x24: return XK_Home;
    case 0x23: return XK_End;       case 0x21: return XK_Page_Up;
    case 0x22: return XK_Page_Down; case 0x2D: return XK_Insert;
    case 0x2E: return XK_Delete;    case 0x14: return XK_Caps_Lock;
    }
    return NoSymbol;
}
static uint32_t g_prevButtons = 0;
static void InjectMouse(int x, int y, uint32_t btns) {
    if (!g_dpy) return;
    XTestFakeMotionEvent(g_dpy, -1, x, y, 0);
    uint32_t c = btns & 7, p = g_prevButtons;
    if ((c&1) != (p&1)) XTestFakeButtonEvent(g_dpy, 1, (c&1) ? True : False, 0);
    if ((c&4) != (p&4)) XTestFakeButtonEvent(g_dpy, 2, (c&4) ? True : False, 0);
    if ((c&2) != (p&2)) XTestFakeButtonEvent(g_dpy, 3, (c&2) ? True : False, 0);
    g_prevButtons = c;
    XFlush(g_dpy);
}
static void InjectKey(uint32_t vk, bool isUp) {
    if (!g_dpy) return;
    KeySym ks = VkToKeysym(vk);
    if (ks == NoSymbol) return;
    KeyCode kc = XKeysymToKeycode(g_dpy, ks);
    if (!kc) return;
    XTestFakeKeyEvent(g_dpy, kc, isUp ? False : True, 0);
    XFlush(g_dpy);
}
static void InjectWheel(int delta) {
    if (!g_dpy) return;
    int btn = (delta > 0) ? 4 : 5;
    XTestFakeButtonEvent(g_dpy, btn, True, 0);
    XTestFakeButtonEvent(g_dpy, btn, False, 0);
    XFlush(g_dpy);
}
#endif

// ===================== Netzwerk =====================
static bool SendAll(SOCKET_ s, const void* b, int n) {
    const char* p = (const char*)b;
    while (n > 0) {
        int r = (int)send(s, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

static void CaptureThread() {
    std::vector<uint8_t> full, scaled;
    int fw, fh, sw, sh;
    while (g_running && g_connected) {
        if (!CaptureScreen(full, fw, fh)) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        Downscale(full, fw, fh, scaled, sw, sh);
        if (scaled.empty()) continue;
        FrameMsg fm;
        fm.magic = MAGIC_FRAME; fm.type = 1;
        fm.width = (uint32_t)sw; fm.height = (uint32_t)sh;
        fm.dataSize = (uint32_t)scaled.size();
        if (!SendAll(g_sock, &fm, sizeof(fm))) break;
        if (!SendAll(g_sock, scaled.data(), (int)scaled.size())) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_SLEEP));
    }
    g_connected = false; g_running = false;
}

static void ReceiveThread() {
    InputMsg m;
    while (g_running && g_connected) {
        int n = (int)recv(g_sock, (char*)&m, sizeof(m), 0);
        if (n != sizeof(m)) break;
        if (m.magic != MAGIC_INPUT) continue;
        if      (m.type == 1) InjectMouse(m.x, m.y, m.flags);
        else if (m.type == 2) InjectKey(m.vk, (m.flags & 1) != 0);
        else if (m.type == 3) InjectWheel((int)m.flags);
    }
    g_connected = false; g_running = false;
}

// ===================== main =====================
int main() {
    if (!AskConsent()) return 0;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 1;
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  SignalHandler);
    signal(SIGTERM, SignalHandler);
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) { fprintf(stderr, "XOpenDisplay fehlgeschlagen\n"); return 1; }
#endif

    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == SOCK_INVALID) { fprintf(stderr, "socket fehlgeschlagen\n"); return 1; }

    sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &srv.sin_addr);
    if (connect(g_sock, (sockaddr*)&srv, sizeof(srv)) != 0) {
        fprintf(stderr, "Verbindung zu %s:%d fehlgeschlagen\n", SERVER_IP, SERVER_PORT);
        CLOSESOCK(g_sock); return 1;
    }
    g_connected = true;

#ifdef _WIN32
    CreateIndicator();
#endif

    std::thread cap(CaptureThread);
    std::thread rx(ReceiveThread);

#ifndef _WIN32
    IndicatorLoop();
#endif

#ifdef _WIN32
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
#endif

    g_running = false;
    if (g_sock != SOCK_INVALID) shutdown(g_sock, 2);
    if (cap.joinable()) cap.join();
    if (rx.joinable())  rx.join();
    if (g_sock != SOCK_INVALID) CLOSESOCK(g_sock);
#ifdef _WIN32
    WSACleanup();
#else
    if (g_dpy) XCloseDisplay(g_dpy);
#endif
    return 0;
}