/* SPDX-License-Identifier: GPL-3.0-or-later
 * Game mode on the Mihawk desktop (docs/MIHAWK_OS.md), from inside the session: a borderless full-screen GPU window
 * is flipped directly (step 1, src/pw_window_direct.h) and the title unloads Explorer while it holds the screen
 * (step 3, src/pw_desktop_session.h); when it leaves, Explorer starts again and its taskbar shows the window that
 * was already open. Run under the session entry (mihawk.exe); OpenGL through Zink by default, --dxvk for Direct3D 11
 * through DXVK. The displayed frame is checked from a broker capture. Verdict: PS5_PROTON_GAME_MODE PASS/FAIL; the
 * unload and restore times go on its bench line. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <string.h>
void WINAPI glClearColor(float, float, float, float);
void WINAPI glClear(unsigned);
unsigned WINAPI glGetError(void);
int _fltused;
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static unsigned checks, failures, captures;
static BOOL use_dxvk, graphics_okay = TRUE;
static HWND gpu_window;
static HDC gpu_dc;
static HGLRC gpu_context;
static IDXGISwapChain *swap;
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11RenderTargetView *target;
static const COLORREF game = RGB(200, 60, 140);

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}
static void check(BOOL okay, const char *what)
{
    char text[256];
    ++checks;
    if (!okay) ++failures;
    wsprintfA(text, "game-mode %s: %s error=%lu\n", okay ? "PASS" : "FAIL", what, GetLastError());
    line(text);
}
static void render(void)
{
    if (!gpu_window || !IsWindowVisible(gpu_window)) return;
    if (use_dxvk) {
        const float colour[] = {GetRValue(game) / 255.0f, GetGValue(game) / 255.0f, GetBValue(game) / 255.0f, 1};
        if (!target) { graphics_okay = FALSE; return; }
        ID3D11DeviceContext_ClearRenderTargetView(context, target, colour);
        if (FAILED(IDXGISwapChain_Present(swap, 1, 0))) graphics_okay = FALSE;
        return;
    }
    if (!gpu_context) return;
    glClearColor(GetRValue(game) / 255.0f, GetGValue(game) / 255.0f, GetBValue(game) / 255.0f, 1.0f);
    glClear(0x4000);
    if (glGetError() || !SwapBuffers(gpu_dc)) graphics_okay = FALSE;
}
static void pump(DWORD ms)
{
    DWORD start = GetTickCount();
    do {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        render();
        Sleep(2);
    } while (GetTickCount() - start < ms);
}
static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(WHITE_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
static BOOL graphics(HWND hwnd)
{
    gpu_window = hwnd;
    if (use_dxvk) {
        DXGI_SWAP_CHAIN_DESC desc = {0};
        ID3D11Texture2D *back = NULL;
        desc.BufferDesc.Width = 1920; desc.BufferDesc.Height = 1080;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
        desc.OutputWindow = hwnd; desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                                 &desc, &swap, &device, NULL, &context)) ||
            FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&back)))
            return FALSE;
        HRESULT made = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &target);
        ID3D11Texture2D_Release(back);
        if (FAILED(made)) return FALSE;
        render();
        return graphics_okay;
    }
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    gpu_dc = GetDC(hwnd);
    int format = gpu_dc ? ChoosePixelFormat(gpu_dc, &pfd) : 0;
    if (!format || !SetPixelFormat(gpu_dc, format, &pfd) || !(gpu_context = wglCreateContext(gpu_dc)) ||
        !wglMakeCurrent(gpu_dc, gpu_context))
        return FALSE;
    render();
    return graphics_okay;
}
static void graphics_close(void)
{
    if (context) { ID3D11DeviceContext_ClearState(context); ID3D11DeviceContext_Flush(context); }
    if (target) { ID3D11RenderTargetView_Release(target); target = NULL; }
    if (swap) { IDXGISwapChain_Release(swap); swap = NULL; }
    if (context) { ID3D11DeviceContext_Release(context); context = NULL; }
    if (device) { ID3D11Device_Release(device); device = NULL; }
    if (gpu_context) { wglMakeCurrent(NULL, NULL); wglDeleteContext(gpu_context); gpu_context = NULL; }
    if (gpu_dc) { ReleaseDC(gpu_window, gpu_dc); gpu_dc = NULL; }
}
/* The taskbar, or NULL if it does not show within ms. */
static HWND taskbar(DWORD ms)
{
    DWORD start = GetTickCount();
    do {
        HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
        if (tray && IsWindowVisible(tray)) return tray;
        pump(20);
    } while (GetTickCount() - start < ms);
    return NULL;
}
struct find { const WCHAR *title; HWND found; };
static BOOL CALLBACK button(HWND hwnd, LPARAM data)
{
    struct find *find = (struct find *)data;
    WCHAR text[128];
    if (IsWindowVisible(hwnd) && GetWindowTextW(hwnd, text, ARRAY_SIZE(text)) && !lstrcmpW(text, find->title)) {
        find->found = hwnd;
        return FALSE;
    }
    return TRUE;
}
/* Whether the taskbar shows a button for the window titled title within ms. */
static BOOL button_for(const WCHAR *title, DWORD ms)
{
    DWORD start = GetTickCount();
    do {
        HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
        struct find find = {title, NULL};
        if (tray) EnumChildWindows(tray, button, (LPARAM)&find);
        if (find.found) return TRUE;
        pump(50);
    } while (GetTickCount() - start < ms);
    return FALSE;
}
struct sample { int x, y; COLORREF colour; };
/* The next presented frame, through the broker's numbered capture request, against samples and the pointer. */
static BOOL capture(const struct sample *samples, unsigned count)
{
    const char *base = "Z:\\data\\ps5proton\\logs\\game-frame";
    char request[MAX_PATH], path[MAX_PATH], header[32] = {0};
    DWORD start = GetTickCount(), read = 0;
    HANDLE f;
    POINT pointer = {0};
    wsprintfA(request, "%s.ppm.broker-request", base);
    wsprintfA(path, "%s-%02u.ppm", base, ++captures);
    check(SetCursorPos(1800, 100) && GetCursorPos(&pointer) && pointer.x == 1800 && pointer.y == 100,
          "pointer placed outside the samples");
    DeleteFileA(path);
    pump(100);
    f = CreateFileA(request, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(f);
    while (GetTickCount() - start < 10000) {
        f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            const char *expected = "P6\n1920 1080\n255\n";
            const DWORD bytes = lstrlenA(expected);
            if (ReadFile(f, header, 18, &read, NULL) && read >= 17 && !memcmp(header, expected, bytes) &&
                GetFileSize(f, NULL) == bytes + 1920 * 1080 * 3) {
                BOOL okay = TRUE;
                const struct sample pointer_sample = {1801, 102, RGB(255, 255, 255)};
                for (unsigned i = 0; i <= count; ++i) {
                    const struct sample *s = i < count ? &samples[i] : &pointer_sample;
                    unsigned char pixel[3];
                    char text[200];
                    SetFilePointer(f, bytes + (s->y * 1920 + s->x) * 3, NULL, FILE_BEGIN);
                    const BOOL match = ReadFile(f, pixel, 3, &read, NULL) && read == 3 && pixel[0] == GetRValue(s->colour) &&
                                       pixel[1] == GetGValue(s->colour) && pixel[2] == GetBValue(s->colour);
                    wsprintfA(text, "game-mode capture=%u sample=%d,%d expected=%u,%u,%u actual=%u,%u,%u result=%s\n",
                              captures, s->x, s->y, GetRValue(s->colour), GetGValue(s->colour), GetBValue(s->colour),
                              pixel[0], pixel[1], pixel[2], match ? "PASS" : "FAIL");
                    line(text);
                    okay = match && okay;
                }
                CloseHandle(f);
                return okay;
            }
            CloseHandle(f);
        }
        pump(20);
    }
    return FALSE;
}

void mainCRTStartup(void)
{
    WNDCLASSW type = {0};
    char text[256];
    use_dxvk = wcsstr(GetCommandLineW(), L"--dxvk") != NULL;
    type.lpfnWndProc = proc; type.hInstance = GetModuleHandleW(NULL);
    type.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); type.lpszClassName = L"MihawkGameModeProbe";
    RegisterClassW(&type);
    check(taskbar(20000) != NULL, "the session's taskbar is up");
    /* A window the user already had open: its taskbar button must come back with the desktop. */
    HWND companion = CreateWindowExW(0, type.lpszClassName, L"Game mode companion", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                     100, 100, 480, 320, NULL, NULL, type.hInstance, NULL);
    check(companion && button_for(L"Game mode companion", 5000), "the taskbar shows the open window");
    /* The game: borderless, the whole screen. */
    HWND full = CreateWindowExW(0, type.lpszClassName, L"Game mode probe", WS_POPUP | WS_VISIBLE, 0, 0, 1920, 1080,
                                NULL, NULL, type.hInstance, NULL);
    SetForegroundWindow(full);
    check(full && graphics(full), use_dxvk ? "Direct3D 11 renders the full-screen window" :
                                             "OpenGL renders the full-screen window");
    const DWORD shown = GetTickCount();
    while (FindWindowW(L"Shell_TrayWnd", NULL) && GetTickCount() - shown < 15000) pump(16);
    const DWORD unload_ms = GetTickCount() - shown;
    check(!FindWindowW(L"Shell_TrayWnd", NULL), "Explorer is unloaded while the full-screen game holds the screen");
    const struct sample samples[] = {{0, 0, game}, {1919, 1079, game}, {960, 540, game}, {960, 1060, game},
                                     {10, 1070, game}, {1910, 1050, game}};
    check(capture(samples, ARRAY_SIZE(samples)), "the screen is exactly the game's frame, where the taskbar was too");
    pump(2000);
    check(graphics_okay, "every frame was presented");
    /* The game leaves: Explorer starts again, and its taskbar shows the window that was open. */
    graphics_close();
    DestroyWindow(full);
    gpu_window = NULL;
    const DWORD left = GetTickCount();
    const HWND tray = taskbar(30000);
    const DWORD restore_ms = GetTickCount() - left;
    check(tray != NULL, "Explorer is back after the game left");
    const DWORD adopted = GetTickCount();
    check(button_for(L"Game mode companion", 10000), "the restarted taskbar shows the window already open");
    const DWORD adopt_ms = GetTickCount() - adopted;
    wsprintfA(text, "PS5_PROTON_GAME_MODE bench unload_ms=%lu restore_ms=%lu adopt_ms=%lu\n", unload_ms, restore_ms, adopt_ms);
    line(text);
    DestroyWindow(companion);
    wsprintfA(text, "PS5_PROTON_GAME_MODE %s api=%s checks=%u failures=%u\n", failures ? "FAIL" : "PASS",
              use_dxvk ? "d3d11" : "opengl", checks, failures);
    line(text);
    PostMessageW(GetShellWindow() ? GetShellWindow() : GetDesktopWindow(), WM_CLOSE, 0, 0);
    ExitProcess(failures ? 1 : 0);
}
