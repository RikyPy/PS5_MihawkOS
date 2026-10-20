/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a GL vertex attribute delivers to a shader, for every type, size and conversion
 * glVertexAttribPointer and glVertexAttribIPointer can ask for, and what feeding it costs.
 *
 * For each format the probe fills a vertex buffer with known bytes, draws four points whose
 * vertex shader passes the attribute through, captures the result with transform feedback
 * and compares it with the value computed here: converted to float without or with
 * normalization, kept as an integer, decoded from half float, or unpacked from 2_10_10_10.
 * Attributes missing from the pointer (size 1 to 3) must read as 0, 0, 0, 1. Every format
 * is tried tightly packed and again at an odd offset in a stride padded with junk, the way
 * a game packs a vertex.
 *
 * Then the footprint, as a regression guard: three frames of 32 glMultiDrawElementsBaseVertex
 * calls over a 2 MiB buffer in a format the hardware fetches natively must not allocate memory.
 * A driver that cannot fetch a format converts the buffer on the CPU into a new stream buffer;
 * GL_NVX_gpu_memory_info says how much is free at the lowest point of a frame, and a drop of
 * 64 MiB or more fails. (The driver before it fetched 16-bit attributes natively showed no
 * drop in this scenario, so this does not prove the conversion path costs memory; it only
 * catches a driver that starts holding memory per draw.)
 *
 * Needs an OpenGL 3.3 context from the title (WINE_PS5_OPENGL=1). Ends by itself. */
#include <windows.h>

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_INT_2_10_10_10_REV 0x8D9F
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define GL_BGRA 0x80E1
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_STREAM_DRAW 0x88E0
#define GL_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define GL_INTERLEAVED_ATTRIBS 0x8C8C
#define GL_RASTERIZER_DISCARD 0x8C89
#define GL_POINTS 0
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_EXTENSIONS 0x1F03
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#define GL_NO_ERROR 0

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

const unsigned char *WINAPI glGetString(unsigned name);
unsigned WINAPI glGetError(void);
void WINAPI glEnable(unsigned cap);
void WINAPI glDisable(unsigned cap);
void WINAPI glDrawArrays(unsigned mode, int first, int count);
void WINAPI glFinish(void);
void WINAPI glGetIntegerv(unsigned name, int *data);

#define FUNCTIONS \
    F(void, glGenBuffers, (int, unsigned *)) \
    F(void, glBindBuffer, (unsigned, unsigned)) \
    F(void, glBufferData, (unsigned, GLsizeiptr, const void *, unsigned)) \
    F(void, glGetBufferSubData, (unsigned, GLsizeiptr, GLsizeiptr, void *)) \
    F(void, glBindBufferBase, (unsigned, unsigned, unsigned)) \
    F(void, glGenVertexArrays, (int, unsigned *)) \
    F(void, glBindVertexArray, (unsigned)) \
    F(void, glEnableVertexAttribArray, (unsigned)) \
    F(void, glVertexAttribPointer, (unsigned, int, unsigned, unsigned char, int, const void *)) \
    F(void, glVertexAttribIPointer, (unsigned, int, unsigned, int, const void *)) \
    F(unsigned, glCreateShader, (unsigned)) \
    F(void, glShaderSource, (unsigned, int, const GLchar *const *, const int *)) \
    F(void, glCompileShader, (unsigned)) \
    F(void, glGetShaderiv, (unsigned, unsigned, int *)) \
    F(void, glGetShaderInfoLog, (unsigned, int, int *, char *)) \
    F(unsigned, glCreateProgram, (void)) \
    F(void, glAttachShader, (unsigned, unsigned)) \
    F(void, glBindAttribLocation, (unsigned, unsigned, const GLchar *)) \
    F(void, glTransformFeedbackVaryings, (unsigned, int, const GLchar *const *, unsigned)) \
    F(void, glLinkProgram, (unsigned)) \
    F(void, glGetProgramiv, (unsigned, unsigned, int *)) \
    F(void, glGetProgramInfoLog, (unsigned, int, int *, char *)) \
    F(void, glUseProgram, (unsigned)) \
    F(void, glBeginTransformFeedback, (unsigned)) \
    F(void, glEndTransformFeedback, (void)) \
    F(const unsigned char *, glGetStringi, (unsigned, unsigned))

#define F(ret, name, args) static ret (WINAPI *name) args;
FUNCTIONS
#undef F

int _fltused; /* floating point without the C runtime */

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void decimal(char *out, unsigned value)
{
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
}

static void hex(char *out, unsigned value)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) out[i] = digits[(value >> (28 - 4 * i)) & 15];
    out[8] = 0;
}

static unsigned float_bits(float value)
{
    union { float f; unsigned u; } both;
    both.f = value;
    return both.u;
}

static float half_to_float(unsigned half)
{
    unsigned sign = (half >> 15) & 1, exponent = (half >> 10) & 31, mantissa = half & 0x3ff;
    union { unsigned u; float f; } out;

    if (exponent == 0) {
        out.f = (float)mantissa * (1.0f / 16777216.0f); /* subnormal: mantissa * 2^-24 */
        if (sign) out.f = -out.f;
        return out.f;
    }
    out.u = (sign << 31) | ((exponent + 112) << 23) | (mantissa << 13);
    return out.f;
}

enum mode { SCALED, NORMALIZED, INTEGER, HALF, FLOATING, PACKED_SCALED, PACKED_NORMALIZED, BGRA8 };

struct format {
    const char *name;
    unsigned type;
    int size;      /* components, or GL_BGRA */
    enum mode mode;
    int bytes;     /* channel bytes (packed formats: the whole element) */
    int is_signed;
};

static int packed(const struct format *f)
{
    return f->mode == PACKED_SCALED || f->mode == PACKED_NORMALIZED;
}

static int channels(const struct format *f)
{
    return f->size == GL_BGRA ? 4 : f->size;
}

static int element_bytes(const struct format *f)
{
    return packed(f) ? 4 : channels(f) * f->bytes;
}

static unsigned read_unsigned(const unsigned char *p, int bytes)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < bytes; i++) v |= (unsigned)p[i] << (8 * i);
    return v;
}

/* The values the shader must see for one vertex: floats in want_f, integers in want_i. */
static void expected(const struct format *f, const unsigned char *p, float *want_f, int *want_i, int *integer)
{
    int i, count = channels(f);

    *integer = f->mode == INTEGER;
    for (i = 0; i < 4; i++) { want_f[i] = i == 3 ? 1.0f : 0.0f; want_i[i] = i == 3; }
    if (packed(f)) {
        static const int field[4] = { 10, 10, 10, 2 };
        unsigned bits = read_unsigned(p, 4);
        int shift = 0;
        for (i = 0; i < 4; i++) {
            int width = field[i];
            unsigned raw = (bits >> shift) & ((1u << width) - 1);
            int value = f->is_signed ? ((int)(raw << (32 - width)) >> (32 - width)) : (int)raw;
            float x = (float)value;
            if (f->mode == PACKED_NORMALIZED) {
                x /= (float)(f->is_signed ? (1 << (width - 1)) - 1 : (1 << width) - 1);
                if (x < -1.0f) x = -1.0f;
            }
            want_f[i] = x;
            shift += width;
        }
        return;
    }
    for (i = 0; i < count; i++) {
        unsigned raw = read_unsigned(p + i * f->bytes, f->bytes);
        int slot = i;
        int value = f->bytes == 4 ? (int)raw : f->is_signed ? ((int)(raw << (32 - 8 * f->bytes)) >> (32 - 8 * f->bytes)) : (int)raw;
        if (f->size == GL_BGRA) slot = i == 0 ? 2 : i == 2 ? 0 : i;
        switch (f->mode) {
        case INTEGER:
            want_i[slot] = value;
            break;
        case SCALED:
            want_f[slot] = (float)value;
            break;
        case NORMALIZED: case BGRA8: {
            float x = (float)value / (float)(f->is_signed ? (1 << (8 * f->bytes - 1)) - 1 : (1 << (8 * f->bytes)) - 1);
            if (x < -1.0f) x = -1.0f;
            want_f[slot] = x;
            break;
        }
        case HALF:
            want_f[slot] = half_to_float(raw);
            break;
        case FLOATING: {
            union { unsigned u; float f; } both;
            both.u = raw;
            want_f[slot] = both.f;
            break;
        }
        default: break;
        }
    }
}

/* Bytes that cannot be mistaken for each other; half floats stay finite, floats stay normal. */
static unsigned char sample_byte(const struct format *f, int vertex, int index)
{
    unsigned v = (unsigned)(vertex * 53 + index * 29 + 7 + vertex * index * 3);

    if (f->mode == HALF) {
        int lane = index / 2;
        unsigned exponent = 12u + (unsigned)((vertex + lane) % 8), mantissa = (unsigned)(vertex * 331 + lane * 97) & 0x3ff;
        unsigned half = ((unsigned)((vertex + lane) & 1) << 15) | (exponent << 10) | mantissa;
        return (unsigned char)((index & 1) ? half >> 8 : half & 0xff);
    }
    if (f->mode == FLOATING) {
        int lane = index / 4;
        unsigned word = 0x3f800000u | ((unsigned)((vertex + lane) & 1) << 31) | ((unsigned)(vertex * 4099 + lane * 331) & 0x7fffff);
        return (unsigned char)(word >> (8 * (index % 4)));
    }
    return (unsigned char)(v ^ (v >> 3));
}

static unsigned compile(unsigned type, const char *source)
{
    int status = 0;
    unsigned shader = glCreateShader(type);

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        int length = 0;
        glGetShaderInfoLog(shader, sizeof(log) - 1, &length, log);
        log[length < 0 ? 0 : length] = 0;
        say("PS5_PROTON_GL_VERTEX_PROBE shader log: ");
        say(log);
        return 0;
    }
    return shader;
}

static unsigned link_program(const char *source)
{
    static const char *const varyings[] = { "r" };
    int status = 0;
    unsigned vertex = compile(GL_VERTEX_SHADER, source), program;

    if (!vertex) return 0;
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glBindAttribLocation(program, 0, "a");
    glTransformFeedbackVaryings(program, 1, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        int length = 0;
        glGetProgramInfoLog(program, sizeof(log) - 1, &length, log);
        log[length < 0 ? 0 : length] = 0;
        say("PS5_PROTON_GL_VERTEX_PROBE link log: ");
        say(log);
        return 0;
    }
    return program;
}

static unsigned vao, tf_buffer, data_buffer, programs[3];
static int failures, checked;

static int close_enough(float got, float want)
{
    float diff = got - want, scale = want < 0 ? -want : want;
    if (diff < 0) diff = -diff;
    if (got != got || want != want) return float_bits(got) == float_bits(want);
    return diff <= 1e-5f + 2e-6f * scale;
}

static void report(const struct format *f, const char *layout, int vertex, const char *what, unsigned got, unsigned want)
{
    char text[240], number[12];

    lstrcpyA(text, "PS5_PROTON_GL_VERTEX_PROBE FAIL ");
    lstrcatA(text, f->name);
    lstrcatA(text, ", ");
    lstrcatA(text, layout);
    lstrcatA(text, ", vertex ");
    decimal(number, (unsigned)vertex);
    lstrcatA(text, number);
    lstrcatA(text, ": ");
    lstrcatA(text, what);
    lstrcatA(text, " got ");
    hex(number, got);
    lstrcatA(text, number);
    lstrcatA(text, " want ");
    hex(number, want);
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

static void run_one(const struct format *f, int odd)
{
    unsigned char data[256], bytes[64];
    const char *layout = odd ? "odd offset" : "tight";
    int element = element_bytes(f), chan = packed(f) ? 4 : f->bytes;
    int offset = odd ? chan : 0, stride = odd ? element + 3 * chan : element;
    int size = offset + 3 * stride + element, v, i;
    unsigned error, program = programs[f->mode == INTEGER ? (f->is_signed ? 1 : 2) : 0];

    for (i = 0; i < size; i++) ((volatile unsigned char *)data)[i] = 0xcc; /* volatile: no memset without the C runtime */
    for (v = 0; v < 4; v++)
        for (i = 0; i < element; i++) data[offset + v * stride + i] = sample_byte(f, v, i);

    while (glGetError() != GL_NO_ERROR) { } /* an earlier call's error is not this format's */
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, data_buffer);
    glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    if (f->mode == INTEGER)
        glVertexAttribIPointer(0, f->size, f->type, stride, (const void *)(INT_PTR)offset);
    else
        glVertexAttribPointer(0, f->size, f->type, f->mode == NORMALIZED || f->mode == PACKED_NORMALIZED || f->mode == BGRA8,
                              stride, (const void *)(INT_PTR)offset);
    glBindBuffer(GL_ARRAY_BUFFER, tf_buffer);
    glBufferData(GL_ARRAY_BUFFER, 64, NULL, GL_STREAM_DRAW);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, tf_buffer);
    glUseProgram(program);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, 4);
    glEndTransformFeedback();
    glGetBufferSubData(GL_ARRAY_BUFFER, 0, 64, bytes);
    error = glGetError();
    checked++;
    if (error != GL_NO_ERROR) {
        report(f, layout, 0, "GL error", error, 0);
        failures++;
        return;
    }
    for (v = 0; v < 4; v++) {
        float want_f[4];
        int want_i[4], integer, k;
        expected(f, data + offset + v * stride, want_f, want_i, &integer);
        for (k = 0; k < 4; k++) {
            unsigned got = ((unsigned *)bytes)[v * 4 + k];
            if (integer ? (int)got != want_i[k] : !close_enough(((float *)bytes)[v * 4 + k], want_f[k])) {
                report(f, layout, v, integer ? "component (int)" : "component (float bits)", got,
                       integer ? (unsigned)want_i[k] : float_bits(want_f[k]));
                failures++;
                return;
            }
        }
    }
}

static int has_extension(const char *name)
{
    int count = 0, i;

    glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &count);
    for (i = 0; i < count; i++) {
        const char *at = (const char *)glGetStringi(GL_EXTENSIONS, (unsigned)i);
        if (at && !lstrcmpA(at, name)) return 1;
    }
    return 0;
}

static void (WINAPI *glMultiDrawElementsBaseVertex)(unsigned, const int *, unsigned, const void *const *, int, const int *);

static HDC probe_dc;

/* A game draws a region of a big vertex buffer with glMultiDrawElementsBaseVertex, many times a frame. */
static int footprint(void)
{
    enum { VERTICES = 256 * 1024, DRAWS = 32, INDICES = 4096 };
    static unsigned indices[INDICES];
    static unsigned short vertices[VERTICES * 4];
    const void *offsets[4];
    int counts[4], bases[4] = { 0, 0, 0, 0 }, before = 0, after = 0, i, frame;
    unsigned buffer, index_buffer;
    char text[160], number[12];
    int measure;

    glMultiDrawElementsBaseVertex = (void (WINAPI *)(unsigned, const int *, unsigned, const void *const *, int, const int *))
        wglGetProcAddress("glMultiDrawElementsBaseVertex");
    measure = has_extension("GL_NVX_gpu_memory_info");
    if (!glMultiDrawElementsBaseVertex) {
        say("PS5_PROTON_GL_VERTEX_PROBE SKIP footprint: glMultiDrawElementsBaseVertex is not offered\n");
        return 0;
    }
    for (i = 0; i < VERTICES * 4; i++) vertices[i] = (unsigned short)(i * 7);
    /* indices that reach the whole buffer, so the range a draw reads is the whole buffer */
    for (i = 0; i < INDICES; i++) indices[i] = (unsigned)((unsigned long long)i * (VERTICES - 1) / (INDICES - 1));
    for (i = 0; i < 4; i++) { counts[i] = INDICES / 4; offsets[i] = (const void *)(INT_PTR)(i * (INDICES / 4) * 4); }
    glGenBuffers(1, &buffer);
    glGenBuffers(1, &index_buffer);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, index_buffer);
    glBufferData(0x8893, sizeof(indices), indices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    /* unsigned shorts, four per vertex, not normalized: how Sodium's chunk vertices are laid out */
    glVertexAttribPointer(0, 4, GL_UNSIGNED_SHORT, 0, 8, (const void *)0);
    glUseProgram(programs[0]);
    glDisable(GL_RASTERIZER_DISCARD);
    while (glGetError() != GL_NO_ERROR) { }
    glFinish();
    if (measure) glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &before);
    after = before;
    /* three frames the way a game draws them: many multi-draws, then the swap; the lowest free memory a frame reaches counts */
    for (frame = 0; frame < 3; frame++) {
        int now = 0;
        for (i = 0; i < DRAWS; i++)
            glMultiDrawElementsBaseVertex(GL_POINTS, counts, GL_UNSIGNED_INT, offsets, 4, bases);
        if (measure) glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &now);
        if (measure && now < after) after = now;
        SwapBuffers(probe_dc);
    }
    if (measure) {
        lstrcpyA(text, "PS5_PROTON_GL_VERTEX_PROBE footprint: free ");
        decimal(number, (unsigned)before >> 10);
        lstrcatA(text, number);
        lstrcatA(text, " MiB before, ");
        decimal(number, (unsigned)after >> 10);
        lstrcatA(text, number);
        lstrcatA(text, " MiB at the lowest, after 32 multi-draws a frame over a 2 MiB buffer\n");
        say(text);
    } else {
        say("PS5_PROTON_GL_VERTEX_PROBE SKIP footprint measurement: GL_NVX_gpu_memory_info is not offered (the draws still run)\n");
    }
    glFinish();
    if (glGetError() != GL_NO_ERROR) {
        say("PS5_PROTON_GL_VERTEX_PROBE FAIL footprint: the draws raised a GL error\n");
        return 1;
    }
    if (measure && (!before || before - after >= (64 << 10))) {
        say("PS5_PROTON_GL_VERTEX_PROBE FAIL footprint: the draws allocated memory (the format is converted on the CPU)\n");
        return 1;
    }
    return 0;
}

static const struct format formats[] = {
#define S(name, type, size, mode, bytes, sign) { name, type, size, mode, bytes, sign },
#define FOUR(base, type, mode, bytes, sign) \
    S(base "1", type, 1, mode, bytes, sign) S(base "2", type, 2, mode, bytes, sign) \
    S(base "3", type, 3, mode, bytes, sign) S(base "4", type, 4, mode, bytes, sign)
    FOUR("BYTE scaled x", GL_BYTE, SCALED, 1, 1)
    FOUR("BYTE normalized x", GL_BYTE, NORMALIZED, 1, 1)
    FOUR("BYTE integer x", GL_BYTE, INTEGER, 1, 1)
    FOUR("UNSIGNED_BYTE scaled x", GL_UNSIGNED_BYTE, SCALED, 1, 0)
    FOUR("UNSIGNED_BYTE normalized x", GL_UNSIGNED_BYTE, NORMALIZED, 1, 0)
    FOUR("UNSIGNED_BYTE integer x", GL_UNSIGNED_BYTE, INTEGER, 1, 0)
    FOUR("SHORT scaled x", GL_SHORT, SCALED, 2, 1)
    FOUR("SHORT normalized x", GL_SHORT, NORMALIZED, 2, 1)
    FOUR("SHORT integer x", GL_SHORT, INTEGER, 2, 1)
    FOUR("UNSIGNED_SHORT scaled x", GL_UNSIGNED_SHORT, SCALED, 2, 0)
    FOUR("UNSIGNED_SHORT normalized x", GL_UNSIGNED_SHORT, NORMALIZED, 2, 0)
    FOUR("UNSIGNED_SHORT integer x", GL_UNSIGNED_SHORT, INTEGER, 2, 0)
    FOUR("HALF_FLOAT x", GL_HALF_FLOAT, HALF, 2, 1)
    FOUR("FLOAT x", GL_FLOAT, FLOATING, 4, 1)
    FOUR("INT integer x", GL_INT, INTEGER, 4, 1)
    FOUR("UNSIGNED_INT integer x", GL_UNSIGNED_INT, INTEGER, 4, 0)
    S("UNSIGNED_INT_2_10_10_10_REV scaled", GL_UNSIGNED_INT_2_10_10_10_REV, 4, PACKED_SCALED, 4, 0)
    S("UNSIGNED_INT_2_10_10_10_REV normalized", GL_UNSIGNED_INT_2_10_10_10_REV, 4, PACKED_NORMALIZED, 4, 0)
    S("INT_2_10_10_10_REV scaled", GL_INT_2_10_10_10_REV, 4, PACKED_SCALED, 4, 1)
    S("INT_2_10_10_10_REV normalized", GL_INT_2_10_10_10_REV, 4, PACKED_NORMALIZED, 4, 1)
    S("BGRA UNSIGNED_BYTE normalized", GL_UNSIGNED_BYTE, GL_BGRA, BGRA8, 1, 0)
};

static const char *const sources[3] = {
    "#version 140\nin vec4 a; out vec4 r; void main() { r = a; gl_Position = vec4(0.0); gl_PointSize = 1.0; }\n",
    "#version 140\nin ivec4 a; flat out ivec4 r; void main() { r = a; gl_Position = vec4(0.0); gl_PointSize = 1.0; }\n",
    "#version 140\nin uvec4 a; flat out uvec4 r; void main() { r = a; gl_Position = vec4(0.0); gl_PointSize = 1.0; }\n",
};

static HWND window;

static void fail_step(const char *step)
{
    char text[160], number[12];

    lstrcpyA(text, "PS5_PROTON_GL_VERTEX_PROBE FAIL: no OpenGL context: ");
    lstrcatA(text, step);
    lstrcatA(text, " failed, error ");
    decimal(number, GetLastError());
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
    ExitProcess(1);
}

/* The context belongs to a thread of its own, as a game's render thread does. */
static DWORD WINAPI render(void *argument)
{
    PIXELFORMATDESCRIPTOR descriptor = {0};
    HDC dc;
    HGLRC context = NULL;
    char text[256], number[12];
    unsigned i;
    int format;

    (void)argument;
    dc = GetDC(window);
    probe_dc = dc;
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    descriptor.cDepthBits = 24;
    format = dc ? ChoosePixelFormat(dc, &descriptor) : 0;
    if (!dc) fail_step("GetDC");
    if (!format) fail_step("ChoosePixelFormat");
    if (!SetPixelFormat(dc, format, &descriptor)) fail_step("SetPixelFormat");
    if (!(context = wglCreateContext(dc))) fail_step("wglCreateContext");
    if (!wglMakeCurrent(dc, context)) fail_step("wglMakeCurrent");
    {
        /* What a game does: a legacy context only to find wglCreateContextAttribsARB, then core contexts. WGL_ARB_create_context
         * lets an implementation return a higher version than asked for, so 3.2 and 3.3 core requests must succeed on a 4.6
         * driver. Each is created, made current, read back and destroyed. The tests run on a core context when one can be
         * made, and on the legacy one otherwise (the shaders are GLSL 1.40, which both accept). */
        static const int versions[][2] = { {3, 2}, {3, 3}, {4, 6} };
        HGLRC (WINAPI *create)(HDC, HGLRC, const int *) = (HGLRC (WINAPI *)(HDC, HGLRC, const int *))wglGetProcAddress("wglCreateContextAttribsARB");
        HGLRC best = NULL;
        unsigned v;

        if (!create) fail_step("wglGetProcAddress(wglCreateContextAttribsARB)");
        for (v = 0; v < sizeof(versions) / sizeof(versions[0]); v++) {
            int attributes[7] = { 0x2091 /* MAJOR */, versions[v][0], 0x2092 /* MINOR */, versions[v][1], 0x9126 /* PROFILE_MASK */, 1 /* CORE */, 0 };
            HGLRC core;
            const char *version = "-";
            DWORD error;
            SetLastError(0);
            core = create(dc, NULL, attributes);
            error = GetLastError();
            if (core && wglMakeCurrent(dc, core)) version = (const char *)glGetString(GL_VERSION);
            lstrcpyA(text, "PS5_PROTON_GL_VERTEX_PROBE core context ");
            decimal(number, (unsigned)versions[v][0]);
            lstrcatA(text, number);
            lstrcatA(text, ".");
            decimal(number, (unsigned)versions[v][1]);
            lstrcatA(text, number);
            if (core) {
                lstrcatA(text, ": created, GL_VERSION ");
                lstrcatA(text, version ? version : "?");
                lstrcatA(text, "\n");
            } else {
                lstrcatA(text, ": FAILED to create, error ");
                decimal(number, error);
                lstrcatA(text, number);
                lstrcatA(text, "\n");
                failures++;
            }
            say(text);
            wglMakeCurrent(NULL, NULL);
            if (core) {
                if (best) wglDeleteContext(best);
                best = core;
            }
        }
        if (best) {
            wglDeleteContext(context);
            context = best;
        } else {
            say("PS5_PROTON_GL_VERTEX_PROBE no core context: the tests run on the legacy context\n");
        }
        if (!wglMakeCurrent(dc, context)) fail_step("wglMakeCurrent (tests)");
    }
#define F(ret, name, args) name = (ret (WINAPI *) args)wglGetProcAddress(#name); \
    if (!name) { say("PS5_PROTON_GL_VERTEX_PROBE FAIL: missing " #name "\n"); ExitProcess(1); }
    FUNCTIONS
#undef F
    lstrcpyA(text, "PS5_PROTON_GL_VERTEX_PROBE GL_RENDERER ");
    lstrcatA(text, (const char *)glGetString(GL_RENDERER));
    lstrcatA(text, ", GL_VERSION ");
    lstrcatA(text, (const char *)glGetString(GL_VERSION));
    lstrcatA(text, "\n");
    say(text);
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &tf_buffer);
    glGenBuffers(1, &data_buffer);
    for (i = 0; i < 3; i++) {
        programs[i] = link_program(sources[i]);
        if (!programs[i]) {
            say("PS5_PROTON_GL_VERTEX_PROBE FAIL: the pass-through shaders do not build\n");
            ExitProcess(1);
        }
    }
    glEnable(GL_RASTERIZER_DISCARD);
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        run_one(&formats[i], 0);
        run_one(&formats[i], 1);
    }
    lstrcpyA(text, "PS5_PROTON_GL_VERTEX_PROBE ");
    decimal(number, (unsigned)checked);
    lstrcatA(text, number);
    lstrcatA(text, " draws checked, ");
    decimal(number, (unsigned)failures);
    lstrcatA(text, number);
    lstrcatA(text, " failed\n");
    say(text);
    failures += footprint();
    say(failures ? "PS5_PROTON_GL_VERTEX_PROBE FAIL\n" : "PS5_PROTON_GL_VERTEX_PROBE PASS\n");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context);
    return (DWORD)(failures != 0);
}

void mainCRTStartup(void)
{
    WNDCLASSA class = {0};
    HANDLE thread;
    DWORD result = 1;

    class.lpfnWndProc = DefWindowProcA;
    class.hInstance = GetModuleHandleA(NULL);
    class.lpszClassName = "ps5_gl_vertex_probe";
    RegisterClassA(&class);
    window = CreateWindowA("ps5_gl_vertex_probe", "gl vertex probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360,
                           NULL, NULL, class.hInstance, NULL);
    thread = window ? CreateThread(NULL, 0, render, NULL, 0, NULL) : NULL;
    if (!thread) fail_step("no window or render thread");
    if (WaitForSingleObject(thread, 300000) != WAIT_OBJECT_0) {
        say("PS5_PROTON_GL_VERTEX_PROBE FAIL: the render thread did not finish\n");
        ExitProcess(1);
    }
    GetExitCodeThread(thread, &result);
    ExitProcess(result);
}
