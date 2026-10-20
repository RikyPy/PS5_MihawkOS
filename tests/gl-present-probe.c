/* SPDX-License-Identifier: GPL-3.0-or-later
 * WGL presentation through the ordinary Windows OpenGL ABI. Back-buffer pixels
 * and SwapBuffers must pass; the owner display capture is checked separately. */
#include <windows.h>

void WINAPI glClearColor(float, float, float, float);
void WINAPI glClear(unsigned);
void WINAPI glEnable(unsigned);
void WINAPI glDisable(unsigned);
void WINAPI glScissor(int, int, int, int);
void WINAPI glReadPixels(int, int, int, int, unsigned, unsigned, void *);
unsigned WINAPI glGetError(void);
const unsigned char *WINAPI glGetString(unsigned);
int _fltused;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void fail(const char *why)
{
    say("PS5_PROTON_GL_PRESENT_PROBE FAIL: "); say(why); say("\n");
    ExitProcess(1);
}

/* An off-screen framebuffer, cleared and read back, before the window's: it tells a fault in image
 * readback from one in the window's swapchain. Reported, not judged. */
static void offscreen_check(void)
{
    void (WINAPI *gen_fb)(int, unsigned *) = (void *)wglGetProcAddress("glGenFramebuffers");
    void (WINAPI *bind_fb)(unsigned, unsigned) = (void *)wglGetProcAddress("glBindFramebuffer");
    void (WINAPI *gen_rb)(int, unsigned *) = (void *)wglGetProcAddress("glGenRenderbuffers");
    void (WINAPI *bind_rb)(unsigned, unsigned) = (void *)wglGetProcAddress("glBindRenderbuffer");
    void (WINAPI *storage)(unsigned, unsigned, int, int) = (void *)wglGetProcAddress("glRenderbufferStorage");
    void (WINAPI *attach)(unsigned, unsigned, unsigned, unsigned) = (void *)wglGetProcAddress("glFramebufferRenderbuffer");
    unsigned (WINAPI *status)(unsigned) = (void *)wglGetProcAddress("glCheckFramebufferStatus");
    unsigned fb = 0, rb = 0, complete;
    unsigned char pixel[4] = {0};
    char line[160];
    if (!gen_fb || !bind_fb || !gen_rb || !bind_rb || !storage || !attach || !status) {
        say("PS5_PROTON_GL_PRESENT_PROBE offscreen: no framebuffer objects\n");
        return;
    }
    gen_fb(1, &fb); bind_fb(0x8D40, fb);                 /* GL_FRAMEBUFFER */
    gen_rb(1, &rb); bind_rb(0x8D41, rb);                 /* GL_RENDERBUFFER */
    storage(0x8D41, 0x8058, 64, 64);                     /* GL_RGBA8 */
    attach(0x8D40, 0x8CE0, 0x8D41, rb);                  /* GL_COLOR_ATTACHMENT0 */
    complete = status(0x8D40);
    glClearColor(0.8f, 0.4f, 0.2f, 1.0f); glClear(0x4000);
    glReadPixels(32, 32, 1, 1, 0x1908, 0x1401, pixel);
    wsprintfA(line, "PS5_PROTON_GL_PRESENT_PROBE offscreen: status 0x%x, error 0x%x, pixel %d,%d,%d (want 204,102,51)\n",
              complete, glGetError(), pixel[0], pixel[1], pixel[2]);
    say(line);
    bind_fb(0x8D40, 0);
}

/* The number after name on the command line (name ends in '='), or fallback. */
static DWORD option(const char *name, DWORD fallback)
{
    const int length = lstrlenA(name);
    for (const char *at = GetCommandLineA(); *at; ++at) {
        int i = 0;
        DWORD value = 0;
        while (i < length && at[i] == name[i]) ++i;
        if (i < length || at[length] < '0' || at[length] > '9') continue;
        for (at += length; *at >= '0' && *at <= '9'; ++at) value = value * 10 + (DWORD)(*at - '0');
        return value;
    }
    return fallback;
}

/* --hold=N: after its verdict the window keeps presenting for N s without pumping messages, so the title's close
 * request never reaches it; --pause=S stops presenting S s in (a frame stall for the title's hang breaker). */
static void hold_window(HDC dc, DWORD hold, DWORD pause)
{
    const DWORD start = GetTickCount();
    DWORD now;
    while ((now = GetTickCount() - start) < hold * 1000) {
        if (pause && now >= pause * 1000) { Sleep(hold * 1000 - now); break; }
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f); glClear(0x4000);
        SwapBuffers(dc);
        Sleep(50);
    }
}

static int color(const unsigned char *pixel, int r, int g, int b)
{
    return pixel[0] >= r-3 && pixel[0] <= r+3 &&
           pixel[1] >= g-3 && pixel[1] <= g+3 &&
           pixel[2] >= b-3 && pixel[2] <= b+3;
}

void mainCRTStartup(void)
{
    WNDCLASSA cls = {0};
    PIXELFORMATDESCRIPTOR pfd = {0};
    HWND window; HDC dc; HGLRC context; RECT rect;
    unsigned char center[4], background[4];
    int format, frame, width, height, mismatches = 0;
    char first[200] = "";
    cls.lpfnWndProc = DefWindowProcA;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "ps5_gl_present_probe";
    if (!RegisterClassA(&cls)) fail("RegisterClass");
    window = CreateWindowA(cls.lpszClassName, "OpenGL presentation probe",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360, NULL, NULL, cls.hInstance, NULL);
    if (!window || !GetClientRect(window, &rect) || !(dc = GetDC(window))) fail("window");
    width = rect.right-rect.left; height = rect.bottom-rect.top;
    if (width < 8 || height < 8) fail("client extent");
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd) || !(context = wglCreateContext(dc)) ||
        !wglMakeCurrent(dc, context)) fail("WGL context");
    {
        char line[512];
        const unsigned char *renderer = glGetString(0x1F01), *version = glGetString(0x1F02);
        wsprintfA(line, "PS5_PROTON_GL_PRESENT_PROBE renderer=%s version=%s client=%dx%d\n",
                  renderer ? (const char *)renderer : "(none)", version ? (const char *)version : "(none)", width, height);
        say(line);
    }
    offscreen_check();
    for (frame = 0; frame < 60; ++frame) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageA(&message);
        }
        glDisable(0x0c11); /* GL_SCISSOR_TEST */
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f); glClear(0x4000);
        glEnable(0x0c11); glScissor(width/4, height/4, width/2, height/2);
        glClearColor(0.8f, 0.4f, 0.2f, 1.0f); glClear(0x4000);
        glDisable(0x0c11);
        glReadPixels(width/2, height/2, 1, 1, 0x1908, 0x1401, center);
        glReadPixels(1, 1, 1, 1, 0x1908, 0x1401, background);
        {
            const unsigned error = glGetError();
            /* Counted, not fatal: the frames are still presented, so the owner display's capture shows
             * what reached the screen even when reading the back buffer does not work. */
            if (error || !color(center, 204, 102, 51) || !color(background, 26, 51, 77)) {
                if (!mismatches++)
                    wsprintfA(first, "frame %d, error 0x%x, center %d,%d,%d (want 204,102,51), "
                              "background %d,%d,%d (want 26,51,77)", frame, error, center[0], center[1], center[2],
                              background[0], background[1], background[2]);
            }
        }
        if (!SwapBuffers(dc)) fail("SwapBuffers");
        Sleep(50);
    }
    if (!mismatches && option("--hold=", 0)) {
        say("PS5_PROTON_GL_PRESENT_PROBE PASS: 60 swaps, 120 pixel checks; holding the window without pumping messages\n");
        hold_window(dc, option("--hold=", 0), option("--pause=", 0));
        ExitProcess(0);
    }
    if (!wglMakeCurrent(NULL, NULL) || !wglDeleteContext(context)) fail("context teardown");
    ReleaseDC(window, dc); DestroyWindow(window);
    if (mismatches) {
        char why[300];
        wsprintfA(why, "rendered pixels: %d of 60 frames read back wrong after 60 swaps; first: %s", mismatches, first);
        fail(why);
    }
    say("PS5_PROTON_GL_PRESENT_PROBE PASS: 60 swaps, 120 pixel checks, context released\n");
    ExitProcess(0);
}
