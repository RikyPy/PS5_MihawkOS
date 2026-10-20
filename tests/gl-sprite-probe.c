/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a sprite engine (MonoGame's SpriteBatch, as a .NET (MonoGame) game draws) asks of OpenGL, one feature at a time.
 *
 * Each step draws a quad into a 64x64 off-screen framebuffer, waits for the GPU (glFinish) and reads the centre
 * back: a colour attribute only; colour and position from a uniform vec4 array (MonoGame's vs_uniforms_vec4);
 * that times a texture; a uniform block; then 60 window frames of the textured quad with the vertex buffer
 * orphaned and refilled every frame, each presented. Every step is announced before it runs, so when the GPU
 * faults (a MonoGame game under Zink raised a protection fault in the owner, 2026-10-06; record removed from the tree) the last line
 * names the feature. The window's pixels are judged from the owner display's capture, not read back.
 *
 * Needs an OpenGL context with GLSL 1.40: Mesa's WGL over Zink in a helper, or the title's. Ends by itself. */
#include <windows.h>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
void WINAPI glClearColor(float, float, float, float);
void WINAPI glClear(unsigned);
void WINAPI glViewport(int, int, int, int);
void WINAPI glReadPixels(int, int, int, int, unsigned, unsigned, void *);
void WINAPI glFinish(void);
unsigned WINAPI glGetError(void);
const unsigned char *WINAPI glGetString(unsigned);
void WINAPI glGenTextures(int, unsigned *);
void WINAPI glBindTexture(unsigned, unsigned);
void WINAPI glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
void WINAPI glTexParameteri(unsigned, unsigned, int);
void WINAPI glDrawElements(unsigned, int, unsigned, const void *);
void WINAPI glEnable(unsigned);
void WINAPI glDisable(unsigned);
void WINAPI glBlendFunc(unsigned, unsigned);
int _fltused;

#define FUNCTIONS(F) \
    F(void, glGenBuffers, (int, unsigned *)) \
    F(void, glBindBuffer, (unsigned, unsigned)) \
    F(void, glBufferData, (unsigned, GLsizeiptr, const void *, unsigned)) \
    F(void, glBindBufferBase, (unsigned, unsigned, unsigned)) \
    F(unsigned, glCreateShader, (unsigned)) \
    F(void, glShaderSource, (unsigned, int, const GLchar *const *, const int *)) \
    F(void, glCompileShader, (unsigned)) \
    F(void, glGetShaderiv, (unsigned, unsigned, int *)) \
    F(void, glGetShaderInfoLog, (unsigned, int, int *, GLchar *)) \
    F(unsigned, glCreateProgram, (void)) \
    F(void, glAttachShader, (unsigned, unsigned)) \
    F(void, glBindAttribLocation, (unsigned, unsigned, const GLchar *)) \
    F(void, glLinkProgram, (unsigned)) \
    F(void, glGetProgramiv, (unsigned, unsigned, int *)) \
    F(void, glGetProgramInfoLog, (unsigned, int, int *, GLchar *)) \
    F(void, glUseProgram, (unsigned)) \
    F(int, glGetUniformLocation, (unsigned, const GLchar *)) \
    F(void, glUniform4fv, (int, int, const float *)) \
    F(void, glUniform1i, (int, int)) \
    F(unsigned, glGetUniformBlockIndex, (unsigned, const GLchar *)) \
    F(void, glUniformBlockBinding, (unsigned, unsigned, unsigned)) \
    F(void, glEnableVertexAttribArray, (unsigned)) \
    F(void, glVertexAttribPointer, (unsigned, int, unsigned, unsigned char, int, const void *)) \
    F(void, glActiveTexture, (unsigned)) \
    F(void, glGenFramebuffers, (int, unsigned *)) \
    F(void, glBindFramebuffer, (unsigned, unsigned)) \
    F(void, glFramebufferTexture2D, (unsigned, unsigned, unsigned, unsigned, int)) \
    F(unsigned, glCheckFramebufferStatus, (unsigned))
#define DECLARE(ret, name, args) static ret (WINAPI *name) args;
FUNCTIONS(DECLARE)

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void fail(const char *why)
{
    say("PS5_PROTON_GL_SPRITE_PROBE FAIL: "); say(why); say("\n");
    ExitProcess(1);
}

static void step(const char *what)
{
    say("PS5_PROTON_GL_SPRITE_PROBE step: "); say(what); say("\n");
}

static int load(void)
{
#define LOAD(ret, name, args) if (!(*(PROC *)&name = wglGetProcAddress(#name))) return 0;
    FUNCTIONS(LOAD)
    return 1;
}

static unsigned shader(unsigned kind, const char *source)
{
    unsigned object = glCreateShader(kind);
    int ok = 0;
    char log[512], line[640];
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    glGetShaderiv(object, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (!ok) {
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        wsprintfA(line, "shader compile: %s", log);
        fail(line);
    }
    return object;
}

static unsigned program(const char *vertex, const char *fragment)
{
    unsigned object = glCreateProgram();
    int ok = 0;
    char log[512], line[640];
    glAttachShader(object, shader(0x8B31 /* GL_VERTEX_SHADER */, vertex));
    glAttachShader(object, shader(0x8B30 /* GL_FRAGMENT_SHADER */, fragment));
    glBindAttribLocation(object, 0, "vs_v0");
    glBindAttribLocation(object, 1, "vs_v1");
    glBindAttribLocation(object, 2, "vs_v2");
    glLinkProgram(object);
    glGetProgramiv(object, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (!ok) {
        glGetProgramInfoLog(object, sizeof(log), NULL, log);
        wsprintfA(line, "program link: %s", log);
        fail(line);
    }
    return object;
}

/* A quad: position (x, y), colour (r, g, b, a), texture coordinates (u, v); eight floats a vertex. */
static float quad[4 * 8];
static const unsigned short indices[6] = {0, 1, 2, 2, 1, 3};

static void fill_quad(float r, float g, float b)
{
    static const float corners[4][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {-1, 1, 0, 1}, {1, 1, 1, 1}};
    for (int i = 0; i < 4; i++) {
        float *v = quad + 8 * i;
        v[0] = corners[i][0]; v[1] = corners[i][1];
        v[2] = r; v[3] = g; v[4] = b; v[5] = 1;
        v[6] = corners[i][2]; v[7] = corners[i][3];
    }
}

static void draw(unsigned vertices)
{
    glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, vertices);
    glBufferData(0x8892, sizeof(quad), NULL, 0x88E0 /* GL_STREAM_DRAW */);
    glBufferData(0x8892, sizeof(quad), quad, 0x88E0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, 0x1406 /* GL_FLOAT */, 0, 32, (const void *)0);
    glVertexAttribPointer(1, 4, 0x1406, 0, 32, (const void *)8);
    glVertexAttribPointer(2, 2, 0x1406, 0, 32, (const void *)24);
    glDrawElements(0x0004 /* GL_TRIANGLES */, 6, 0x1403 /* GL_UNSIGNED_SHORT */, (const void *)0);
}

static int close_to(int value, int want)
{
    return value >= want - 3 && value <= want + 3;
}

/* "evict" on the command line: after recording each draw and before the GPU runs it, write 64 MiB of unrelated
 * memory, which pushes the CPU caches' dirty lines (descriptors, uniforms the driver just wrote) out to memory. A
 * draw that is right only then reads memory the GPU does not see coherently with the CPU's caches. Only meaningful
 * with GALLIUM_THREAD=0: otherwise Mesa's driver thread records the draw later, at glFinish, after the eviction. */
static volatile unsigned char *evict_buffer;

static int has_word(const char *text, const char *word)
{
    for (const char *p = text; *p; p++) {
        int i = 0;
        if (p != text && p[-1] != ' ') continue;
        while (word[i] && p[i] == word[i]) i++;
        if (!word[i] && (!p[i] || p[i] == ' ')) return 1;
    }
    return 0;
}

static void evict(void)
{
    if (!evict_buffer) return;
    for (SIZE_T i = 0; i < ((SIZE_T)64 << 20); i += 64) evict_buffer[i] = (unsigned char)(evict_buffer[i] + 1);
}

static int failures;
static char first_failure[200];

/* Draw into the off-screen target, wait, read the centre, compare; a mismatch is counted and the next step runs. */
static void check(const char *what, unsigned vertices, int r, int g, int b)
{
    unsigned char pixel[4] = {0};
    char line[200];
    step(what);
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(0x4000);
    draw(vertices);
    evict();
    glFinish();
    glReadPixels(32, 32, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, pixel);
    wsprintfA(line, "%s: error 0x%x, pixel %d,%d,%d (want %d,%d,%d)", what, glGetError(), pixel[0], pixel[1],
              pixel[2], r, g, b);
    if (!close_to(pixel[0], r) || !close_to(pixel[1], g) || !close_to(pixel[2], b)) {
        if (!failures++) lstrcpynA(first_failure, line, sizeof(first_failure));
        step("MISMATCH");
    }
    step(line);
}

static const char attributes[] =
    "#version 140\n"
    "in vec4 vs_v0; in vec4 vs_v1; in vec2 vs_v2;\n"
    "out vec4 color; out vec2 uv;\n";

void mainCRTStartup(void)
{
    WNDCLASSA cls = {0};
    PIXELFORMATDESCRIPTOR pfd = {0};
    HWND window; HDC dc; HGLRC context; RECT rect;
    unsigned vertices, elements, texture, target, framebuffer, block;
    unsigned plain, sampled, uniforms, textured, blocked, stored, block_static, storage, storage_static;
    char line[512];
    int width, height, format;

    cls.lpfnWndProc = DefWindowProcA;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "ps5_gl_sprite_probe";
    if (!RegisterClassA(&cls)) fail("RegisterClass");
    window = CreateWindowA(cls.lpszClassName, "OpenGL sprite probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           0, 0, 640, 360, NULL, NULL, cls.hInstance, NULL);
    if (!window || !GetClientRect(window, &rect) || !(dc = GetDC(window))) fail("window");
    width = rect.right - rect.left; height = rect.bottom - rect.top;
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    format = ChoosePixelFormat(dc, &pfd);
    if (!format || !SetPixelFormat(dc, format, &pfd) || !(context = wglCreateContext(dc)) ||
        !wglMakeCurrent(dc, context)) fail("WGL context");
    wsprintfA(line, "renderer=%s version=%s", (const char *)glGetString(0x1F01), (const char *)glGetString(0x1F02));
    step(line);
    if (!load()) fail("GL 3.1 entry points");
    {
        /* What reached this process: a helper gets only part of the title's environment (inherited() in
         * wine/ps5/pw_wine_process.c), and a setting that never arrived must not be read as tested. */
        static const char *const names[] = {"GALLIUM_THREAD", "ZINK_DESCRIPTORS", "ZINK_DEBUG"};
        for (int i = 0; i < 3; i++) {
            char value[64];
            if (!GetEnvironmentVariableA(names[i], value, sizeof(value))) lstrcpyA(value, "(unset)");
            wsprintfA(line, "environment: %s=%s", names[i], value);
            step(line);
        }
        if (has_word(GetCommandLineA(), "evict")) {
            evict_buffer = VirtualAlloc(NULL, (SIZE_T)64 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            step(evict_buffer ? "cache eviction before each GPU wait" : "cache eviction: no memory");
        }
    }

    step("objects");
    glGenBuffers(1, &vertices);
    glGenBuffers(1, &elements);
    glBindBuffer(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, elements);
    glBufferData(0x8893, sizeof(indices), indices, 0x88E4 /* GL_STATIC_DRAW */);
    {
        static unsigned pixels[16 * 16];
        for (int i = 0; i < 16 * 16; i++) pixels[i] = 0xffcc9933u;    /* RGBA bytes 51,153,204,255 */
        glActiveTexture(0x84C0 /* GL_TEXTURE0 */);
        glGenTextures(1, &texture);
        glBindTexture(0x0DE1 /* GL_TEXTURE_2D */, texture);
        glTexImage2D(0x0DE1, 0, 0x8058 /* GL_RGBA8 */, 16, 16, 0, 0x1908, 0x1401, pixels);
        glTexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
        glTexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2600);
    }
    glGenTextures(1, &target);
    glBindTexture(0x0DE1, target);
    glTexImage2D(0x0DE1, 0, 0x8058, 64, 64, 0, 0x1908, 0x1401, NULL);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(0x8D40, framebuffer);
    glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, target, 0);
    if (glCheckFramebufferStatus(0x8D40) != 0x8CD5) fail("off-screen framebuffer incomplete");
    glBindTexture(0x0DE1, texture);

    step("programs");
    {
        static char vs[1024], fs[1024];
        wsprintfA(vs, "%svoid main() { gl_Position = vec4(vs_v0.xy, 0.0, 1.0); color = vs_v1; uv = vs_v2; }\n", attributes);
        plain = program(vs, "#version 140\nin vec4 color; in vec2 uv; out vec4 result;\n"
                            "void main() { result = color; }\n");
        /* A texture with no uniform: the colour attribute times the texture. */
        sampled = program(vs, "#version 140\nin vec4 color; in vec2 uv; out vec4 result; uniform sampler2D ps_s0;\n"
                              "void main() { result = texture(ps_s0, uv) * color; }\n");
        wsprintfA(vs, "%suniform vec4 vs_uniforms_vec4[2];\n"
                      "void main() { gl_Position = vec4(vs_v0.xy * vs_uniforms_vec4[0].xy, 0.0, 1.0);\n"
                      "  color = vs_uniforms_vec4[1]; uv = vs_v2; }\n", attributes);
        uniforms = program(vs, "#version 140\nin vec4 color; in vec2 uv; out vec4 result;\n"
                               "void main() { result = color; }\n");
        textured = program(vs, "#version 140\nin vec4 color; in vec2 uv; out vec4 result; uniform sampler2D ps_s0;\n"
                               "void main() { result = texture(ps_s0, uv) * color; }\n");
        wsprintfA(vs, "%slayout(std140) uniform Sprite { vec4 scale; vec4 tint; };\n"
                      "void main() { gl_Position = vec4(vs_v0.xy * scale.xy, 0.0, 1.0); color = tint; uv = vs_v2; }\n",
                  attributes);
        blocked = program(vs, "#version 140\nin vec4 color; in vec2 uv; out vec4 result;\n"
                              "void main() { result = color; }\n");
        /* The same values from a shader storage buffer: read through vector memory, where a uniform block's
         * reads are scalar. */
        stored = program("#version 430\nin vec4 vs_v0; in vec4 vs_v1; in vec2 vs_v2; out vec4 color; out vec2 uv;\n"
                         "layout(std430, binding = 1) readonly buffer Sprite { vec4 scale; vec4 tint; };\n"
                         "void main() { gl_Position = vec4(vs_v0.xy * scale.xy, 0.0, 1.0); color = tint; uv = vs_v2; }\n",
                         "#version 430\nin vec4 color; in vec2 uv; out vec4 result;\n"
                         "void main() { result = color; }\n");
    }

    fill_quad(0.8f, 0.4f, 0.2f);
    glUseProgram(plain);
    check("draw: colour attribute", vertices, 204, 102, 51);
    fill_quad(1, 1, 1);
    glUseProgram(sampled);
    check("draw: texture, no uniform", vertices, 51, 153, 204);
    fill_quad(0.8f, 0.4f, 0.2f);

    {
        const float values[8] = {1, 1, 0, 0, 0.2f, 0.8f, 0.4f, 1};
        glUseProgram(uniforms);
        glUniform4fv(glGetUniformLocation(uniforms, "vs_uniforms_vec4"), 2, values);
        check("draw: uniform vec4 array", vertices, 51, 204, 102);
    }
    {
        const float values[8] = {1, 1, 0, 0, 1, 1, 1, 1};
        glUseProgram(textured);
        glUniform4fv(glGetUniformLocation(textured, "vs_uniforms_vec4"), 2, values);
        glUniform1i(glGetUniformLocation(textured, "ps_s0"), 0);
        check("draw: uniforms and a texture", vertices, 51, 153, 204);
    }
    {
        const float values[8] = {1, 1, 0, 0, 0.4f, 0.2f, 0.8f, 1};
        glGenBuffers(1, &block);
        glBindBuffer(0x8A11 /* GL_UNIFORM_BUFFER */, block);
        glBufferData(0x8A11, sizeof(values), values, 0x88E8 /* GL_DYNAMIC_DRAW */);
        glUniformBlockBinding(blocked, glGetUniformBlockIndex(blocked, "Sprite"), 0);
        glBindBufferBase(0x8A11, 0, block);
        glUseProgram(blocked);
        check("draw: uniform block", vertices, 102, 51, 204);
    }
    {
        /* Static: the driver keeps it in GPU-only memory and fills it by a copy. */
        const float values[8] = {1, 1, 0, 0, 0.4f, 0.2f, 0.8f, 1};
        glGenBuffers(1, &block_static);
        glBindBuffer(0x8A11, block_static);
        glBufferData(0x8A11, sizeof(values), values, 0x88E4 /* GL_STATIC_DRAW */);
        glBindBufferBase(0x8A11, 0, block_static);
        glUseProgram(blocked);
        check("draw: uniform block, static", vertices, 102, 51, 204);
    }
    {
        const float values[8] = {1, 1, 0, 0, 0.8f, 0.8f, 0.2f, 1};
        glGenBuffers(1, &storage);
        glBindBuffer(0x90D2 /* GL_SHADER_STORAGE_BUFFER */, storage);
        glBufferData(0x90D2, sizeof(values), values, 0x88E8 /* GL_DYNAMIC_DRAW */);
        glBindBufferBase(0x90D2, 1, storage);
        glUseProgram(stored);
        check("draw: storage buffer", vertices, 204, 204, 51);
    }
    {
        const float values[8] = {1, 1, 0, 0, 0.2f, 0.4f, 0.8f, 1};
        glGenBuffers(1, &storage_static);
        glBindBuffer(0x90D2, storage_static);
        glBufferData(0x90D2, sizeof(values), values, 0x88E4);
        glBindBufferBase(0x90D2, 1, storage_static);
        glUseProgram(stored);
        check("draw: storage buffer, static", vertices, 51, 102, 204);
    }

    step("window: 60 frames of the textured quad, the vertex buffer refilled each frame");
    glBindFramebuffer(0x8D40, 0);
    glUseProgram(textured);
    glEnable(0x0BE2 /* GL_BLEND */);
    glBlendFunc(1 /* GL_ONE */, 0x0303 /* GL_ONE_MINUS_SRC_ALPHA */);
    for (int frame = 0; frame < 60; frame++) {
        MSG message;
        const float values[8] = {0.5f, 0.5f, 0, 0, 1, 1, 1, 1};
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageA(&message); }
        glViewport(0, 0, width, height);
        glClearColor(0.1f, 0.2f, 0.3f, 1);
        glClear(0x4000);
        glUniform4fv(glGetUniformLocation(textured, "vs_uniforms_vec4"), 2, values);
        draw(vertices);
        if (!SwapBuffers(dc)) fail("SwapBuffers");
        if (frame % 10 == 0) {
            glFinish();
            wsprintfA(line, "window frame %d presented, error 0x%x", frame, glGetError());
            step(line);
        }
    }
    glDisable(0x0BE2);
    if (!wglMakeCurrent(NULL, NULL) || !wglDeleteContext(context)) fail("context teardown");
    ReleaseDC(window, dc); DestroyWindow(window);
    if (failures) {
        wsprintfA(line, "%d of 8 off-screen draws wrong; first: %s", failures, first_failure);
        fail(line);
    }
    say("PS5_PROTON_GL_SPRITE_PROBE PASS: attribute, uniform array, texture, uniform block and storage buffer draws read back; "
        "60 textured window frames presented\n");
    ExitProcess(0);
}
