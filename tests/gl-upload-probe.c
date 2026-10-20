/* SPDX-License-Identifier: GPL-3.0-or-later
 * Same upload workload as PE32 and PE64 through production WGL/Zink. Six 4096x4096 RGBA8 textures stay alive
 * together (384 MiB), uploaded without an intervening GPU wait. Two create/upload/readback/delete rounds check
 * pixel correctness and reuse. Every boundary prints timing/progress before a possibly blocking driver call.
 * No game data. No budget override. The runner's watchdog reports a missing verdict as an unfinished run.
 * A PASS checks these pixels and calls, not smooth gameplay or full GL conformance. */
#include <windows.h>

void WINAPI glGenTextures(int, unsigned *);
void WINAPI glDeleteTextures(int, const unsigned *);
void WINAPI glBindTexture(unsigned, unsigned);
void WINAPI glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
void WINAPI glTexSubImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
void WINAPI glTexParameteri(unsigned, unsigned, int);
void WINAPI glReadPixels(int, int, int, int, unsigned, unsigned, void *);
void WINAPI glViewport(int, int, int, int);
void WINAPI glClearColor(float, float, float, float);
void WINAPI glClear(unsigned);
void WINAPI glFinish(void);
unsigned WINAPI glGetError(void);
const unsigned char *WINAPI glGetString(unsigned);

static void (WINAPI *bind_framebuffer)(unsigned, unsigned);
static void (WINAPI *gen_framebuffers)(int, unsigned *);
static void (WINAPI *delete_framebuffers)(int, const unsigned *);
static void (WINAPI *attach_texture)(unsigned, unsigned, unsigned, unsigned, int);
static unsigned (WINAPI *framebuffer_status)(unsigned);
static LARGE_INTEGER frequency;
int _fltused;

enum { SIDE = 4096, COUNT = 6, ROUNDS = 2, FRAMES = 60 };

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void fail(const char *where)
{
    char line[256];
    wsprintfA(line, "PS5_PROTON_GL_UPLOAD_PROBE FAIL: %s error=%x\n", where, glGetError());
    say(line);
    ExitProcess(1);
}

static LARGE_INTEGER now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}

static unsigned elapsed_ms(LARGE_INTEGER start)
{
    LARGE_INTEGER t = now();
    return (unsigned)((unsigned long long)(t.QuadPart - start.QuadPart) * 1000ull / (unsigned long long)frequency.QuadPart);
}

static unsigned pixel(int x, int y, int texture, int round)
{
    unsigned r = (x + texture * 17 + round * 61) & 255;
    unsigned g = (y + texture * 29 + round * 43) & 255;
    unsigned b = ((x >> 4) ^ (y >> 4) ^ (texture * 37 + round * 19)) & 255;
    return r | (g << 8) | (b << 16) | 0xff000000u;
}

static void verify_texture(unsigned texture, int index, int round, int patched)
{
    static const int xy[5][2] = {{0,0},{4095,0},{0,4095},{4095,4095},{2048,2048}};
    attach_texture(0x8D40, 0x8CE0, 0x0DE1, texture, 0);
    if (framebuffer_status(0x8D40) != 0x8CD5) fail("framebuffer incomplete");
    for (int i = 0; i < 5; i++) {
        unsigned actual = 0;
        int x = xy[i][0], y = xy[i][1];
        unsigned expected = patched && i == 4 ? pixel(x, y, index, round + 2) : pixel(x, y, index, round);
        glReadPixels(x, y, 1, 1, 0x1908, 0x1401, &actual);
        if (glGetError() || actual != expected) {
            char line[180];
            wsprintfA(line, "upload pixel texture=%d round=%d at=%d,%d want=%x actual=%x\n",
                      index, round, x, y, expected, actual);
            say(line); fail("pixel readback");
        }
    }
}

void mainCRTStartup(void)
{
    WNDCLASSA cls = {0}; PIXELFORMATDESCRIPTOR pfd = {0}; RECT rect;
    cls.lpfnWndProc = DefWindowProcA; cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "ps5_gl_upload_probe";
    if (!RegisterClassA(&cls)) fail("RegisterClass");
    HWND window = CreateWindowA(cls.lpszClassName, "Texture upload probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 0, 0, 640, 360, NULL, NULL, cls.hInstance, NULL);
    HDC dc = window ? GetDC(window) : NULL;
    if (!dc || !GetClientRect(window, &rect)) fail("window");
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    int format = ChoosePixelFormat(dc, &pfd);
    HGLRC context;
    if (!format || !SetPixelFormat(dc, format, &pfd) || !(context = wglCreateContext(dc)) ||
        !wglMakeCurrent(dc, context)) fail("WGL context");
    bind_framebuffer = (void *)wglGetProcAddress("glBindFramebuffer");
    gen_framebuffers = (void *)wglGetProcAddress("glGenFramebuffers");
    delete_framebuffers = (void *)wglGetProcAddress("glDeleteFramebuffers");
    attach_texture = (void *)wglGetProcAddress("glFramebufferTexture2D");
    framebuffer_status = (void *)wglGetProcAddress("glCheckFramebufferStatus");
    if (!bind_framebuffer || !gen_framebuffers || !delete_framebuffers || !attach_texture || !framebuffer_status)
        fail("framebuffer entry points");
    QueryPerformanceFrequency(&frequency);
    char line[256];
    wsprintfA(line, "upload begin bits=%d textures=%d side=%d resident_mib=384 renderer=%s\n",
              (int)(sizeof(void *) * 8), COUNT, SIDE, (const char *)glGetString(0x1F01));
    say(line);
    SIZE_T bytes = (SIZE_T)SIDE * SIDE * 4;
    unsigned *data = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!data) fail("CPU upload allocation");
    unsigned framebuffer; gen_framebuffers(1, &framebuffer);
    unsigned max_upload_ms = 0, max_present_ms = 0, max_frame_ms = 0, reads = 0;
    for (int round = 0; round < ROUNDS; round++) {
        unsigned textures[COUNT] = {0};
        glGenTextures(COUNT, textures);
        for (int index = 0; index < COUNT; index++) {
            for (int y = 0; y < SIDE; y++) for (int x = 0; x < SIDE; x++) data[y * SIDE + x] = pixel(x,y,index,round);
            wsprintfA(line, "upload boundary round=%d texture=%d\n", round, index); say(line);
            glBindTexture(0x0DE1, textures[index]);
            glTexParameteri(0x0DE1, 0x2801, 0x2600); glTexParameteri(0x0DE1, 0x2800, 0x2600);
            LARGE_INTEGER start = now();
            glTexImage2D(0x0DE1, 0, 0x8058, SIDE, SIDE, 0, 0x1908, 0x1401, data);
            unsigned ms = elapsed_ms(start); if (ms > max_upload_ms) max_upload_ms = ms;
            unsigned error = glGetError();
            wsprintfA(line, "upload complete round=%d texture=%d ms=%u error=%x\n", round,index,ms,error); say(line);
            if (error) fail("texture upload");
        }
        say("upload boundary wait for burst\n"); glFinish();
        if (glGetError()) fail("upload burst");
        bind_framebuffer(0x8D40, framebuffer);
        for (int index = 0; index < COUNT; index++) {
            verify_texture(textures[index], index, round, 0); reads += 5;
            unsigned update = pixel(2048,2048,index,round + 2);
            glBindTexture(0x0DE1, textures[index]);
            glTexSubImage2D(0x0DE1, 0, 2048, 2048, 1, 1, 0x1908, 0x1401, &update);
            verify_texture(textures[index], index, round, 1); reads += 5;
        }
        bind_framebuffer(0x8D40, 0);
        for (int frame = 0; frame < FRAMES; frame++) {
            LARGE_INTEGER frame_start = now();
            MSG message;
            if (frame % 10 == 0) { wsprintfA(line,"upload frame boundary round=%d frame=%d\n",round,frame); say(line); }
            while (PeekMessageA(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageA(&message); }
            glViewport(0,0,rect.right-rect.left,rect.bottom-rect.top);
            glClearColor((float)(frame % 3 == 0), (float)(frame % 3 == 1), (float)(frame % 3 == 2), 1);
            glClear(0x4000);
            LARGE_INTEGER start = now();
            if (!SwapBuffers(dc)) fail("SwapBuffers");
            glFinish();
            unsigned ms = elapsed_ms(start); if (ms > max_present_ms) max_present_ms = ms;
            unsigned frame_ms = elapsed_ms(frame_start); if (frame_ms > max_frame_ms) max_frame_ms = frame_ms;
            if (glGetError()) fail("window frame");
            if (frame % 10 == 0) { wsprintfA(line,"upload presented round=%d frame=%d swap_ms=%u frame_ms=%u\n",round,frame,ms,frame_ms); say(line); }
        }
        /* Drop the FBO attachment too, otherwise the last texture is retained after deletion. */
        bind_framebuffer(0x8D40, framebuffer); attach_texture(0x8D40,0x8CE0,0x0DE1,0,0);
        bind_framebuffer(0x8D40,0); glBindTexture(0x0DE1,0);
        glDeleteTextures(COUNT,textures); glFinish();
        if (glGetError()) fail("texture release");
        wsprintfA(line,"upload released round=%d\n",round); say(line);
    }
    delete_framebuffers(1,&framebuffer); VirtualFree(data,0,MEM_RELEASE);
    if (!wglMakeCurrent(NULL,NULL) || !wglDeleteContext(context)) fail("context teardown");
    ReleaseDC(window,dc); DestroyWindow(window);
    wsprintfA(line,"PS5_PROTON_GL_UPLOAD_PROBE PASS: bits=%d reads=%u frames=%d rounds=%d upload_max_ms=%u present_max_ms=%u frame_max_ms=%u\n",
              (int)(sizeof(void *) * 8),reads,FRAMES * ROUNDS,ROUNDS,max_upload_ms,max_present_ms,max_frame_ms); say(line);
    ExitProcess(0);
}
