/* SPDX-License-Identifier: GPL-3.0-or-later
 * A game whose render thread is not the thread that exits. Java's launcher thread
 * calls ExitProcess while the game thread, which has the OpenGL context current, is
 * still alive; Windows ends the process, but on the console the presenter can only
 * be closed when no thread still has a context current (the PS5 OpenGL SDK refuses
 * eglDestroyContext with EGL_BAD_ACCESS), and a process that ends with its presenter
 * live leaves a frame open on the GPU.
 *
 * The main thread creates a window; a worker thread gives it an OpenGL context, makes
 * it current and presents GL_EXIT_FRAMES frames (default 30), then sits in Sleep with
 * the context still current, saying so. The main thread then calls ExitProcess. The
 * title's log must show the presenter closed completely ("surfaces 1/1, contexts
 * 1/1, terminate 1"). With GL_EXIT_BUSY=1 the worker keeps rendering instead of
 * sleeping, so it is inside GL calls with frames in flight when the process ends.
 * Ends by itself. */
#include <windows.h>

void WINAPI glClearColor(float red, float green, float blue, float alpha);
void WINAPI glClear(unsigned mask);

int _fltused; /* floating point without the C runtime */
static HWND window;
static HANDLE ready;
static volatile LONG frames, failed;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void number(char *out, unsigned value)
{
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
}

static DWORD WINAPI render(void *argument)
{
    PIXELFORMATDESCRIPTOR descriptor = {0};
    HDC dc = GetDC(window);
    HGLRC context;
    static WCHAR busy[8];
    int format, wanted = (int)(INT_PTR)argument, i;

    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    descriptor.cDepthBits = 24;
    format = ChoosePixelFormat(dc, &descriptor);
    if (!format || !SetPixelFormat(dc, format, &descriptor) || !(context = wglCreateContext(dc)) ||
        !wglMakeCurrent(dc, context)) {
        InterlockedExchange(&failed, 1);
        SetEvent(ready);
        return 1;
    }
    for (i = 0; i < wanted; i++) {
        glClearColor((float)(i % 8) / 8.0f, 0.25f, 0.5f, 1.0f);
        glClear(0x4000);
        SwapBuffers(dc);
        InterlockedIncrement(&frames);
    }
    SetEvent(ready);
    /* GL_EXIT_BUSY=1: keep rendering until the process is ended, so the thread is inside
     * GL calls with frames in flight when it is killed, as a game's render thread is */
    if (GetEnvironmentVariableW(L"GL_EXIT_BUSY", busy, 8))
        for (i = 0;; i++) {
            glClearColor((float)(i % 8) / 8.0f, 0.25f, 0.5f, 1.0f);
            glClear(0x4000);
            SwapBuffers(dc);
            InterlockedIncrement(&frames);
        }
    for (;;) Sleep(50); /* idle, in the process, with the context current */
}

void mainCRTStartup(void)
{
    static WCHAR setting[12];
    WNDCLASSA class = {0};
    char text[96], count[12];
    int wanted = 30;

    if (GetEnvironmentVariableW(L"GL_EXIT_FRAMES", setting, 12)) {
        unsigned i;
        wanted = 0;
        for (i = 0; setting[i] >= L'0' && setting[i] <= L'9'; i++) wanted = wanted * 10 + (setting[i] - L'0');
    }
    class.lpfnWndProc = DefWindowProcA;
    class.hInstance = GetModuleHandleA(NULL);
    class.lpszClassName = "ps5_gl_exit_probe";
    RegisterClassA(&class);
    window = CreateWindowA("ps5_gl_exit_probe", "gl exit probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           0, 0, 640, 360, NULL, NULL, class.hInstance, NULL);
    ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!window || !ready || !CreateThread(NULL, 0, render, (void *)(INT_PTR)wanted, 0, NULL)) {
        say("PS5_PROTON_GL_EXIT_PROBE FAIL: no window or thread\n");
        ExitProcess(1);
    }
    if (WaitForSingleObject(ready, 60000) != WAIT_OBJECT_0 || failed) {
        say("PS5_PROTON_GL_EXIT_PROBE FAIL: the render thread could not make a context current\n");
        ExitProcess(1);
    }
    number(count, (unsigned)frames);
    lstrcpyA(text, "PS5_PROTON_GL_EXIT_PROBE PASS: ");
    lstrcatA(text, count);
    lstrcatA(text, " frames presented; exiting from the main thread while the render thread holds its context\n");
    say(text);
    ExitProcess(0);
}
