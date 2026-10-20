// SPDX-License-Identifier: GPL-3.0-or-later
#include "proton_loading.h"
#include "../ps5/src/platform.h"
#include "../src/pw_setup.h"
#include <ps5platform/kernel.h>
#include <ps5platform/videoout.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

extern "C" int scePthreadSetprio(pthread_t thread, int priority);
extern "C" int __real_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);

// A native presenter takes the display only when it opens VideoOut, not when
// the broker initializes its GPU. Wine's PRX presenters use their existing
// release_display callback; this covers the broker's statically linked presenter.
extern "C" int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter)
{
    proton_loading_stop("the game's presenter takes the display");
    return __real_sceVideoOutOpen(user, bus, index, parameter);
}

// ImGui's copy of stb_truetype (public domain / MIT), compiled privately here as imgui_draw.cpp does.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../external/imgui/imstb_truetype.h"
#pragma clang diagnostic pop

namespace {
constexpr unsigned kWidth = 1920, kHeight = 1080;
constexpr unsigned kBlocksX = (kWidth + 127) / 128, kBlocksY = (kHeight + 127) / 128;
constexpr size_t kFrameBytes = size_t(kBlocksX) * kBlocksY << 16;
constexpr size_t kAlign = size_t(2) << 20;
constexpr size_t kBufferBytes = (kFrameBytes + kAlign - 1) / kAlign * kAlign;
constexpr size_t kMemoryBytes = kBufferBytes * 2;
// Write-combined direct memory the display scans out and the CPU writes (PS5_Vulkan's demo renderer).
constexpr int kMemoryType = 3;
constexpr int kProtection = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ |
                            PS5_KERNEL_PROT_GPU_WRITE;
// The part of the screen that moves: the elapsed time and the spinner below it.
constexpr unsigned kDynX0 = 480, kDynX1 = 1440, kDynY0 = 515, kDynY1 = 712;

// Where pixel (x, y) lies in a buffer of VideoOut tiling 0, 64 KiB tiles of 128x128 four-byte
// pixels: the swizzle PS5_Vulkan measured, which its demo renderer writes from the CPU.
size_t tiled(unsigned x, unsigned y)
{
    const uint32_t offset = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u) ^
                            ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^
                            ((x << 9) & 0xa000u);
    return (size_t((y >> 7) * kBlocksX + (x >> 7)) << 16) + offset;
}

// Pixels are 0xAARRGGBB: bytes B, G, R, A, as the display reads PS5_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SDR.
uint32_t rgb(unsigned r, unsigned g, unsigned b) { return 0xff000000u | (r << 16) | (g << 8) | b; }
uint32_t blend(uint32_t under, uint32_t over, unsigned alpha)
{
    const unsigned keep = 255 - alpha;
    const unsigned r = (((under >> 16) & 0xff) * keep + ((over >> 16) & 0xff) * alpha) / 255;
    const unsigned g = (((under >> 8) & 0xff) * keep + ((over >> 8) & 0xff) * alpha) / 255;
    const unsigned b = ((under & 0xff) * keep + (over & 0xff) * alpha) / 255;
    return rgb(r, g, b);
}

uint64_t now_ns()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000u + uint64_t(now.tv_nsec);
}

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

bool load_font(Font &font, const std::string &path)
{
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size > 0) {
        font.data.resize(size_t(size));
        if (fread(font.data.data(), 1, font.data.size(), file) != font.data.size()) font.data.clear();
    }
    fclose(file);
    font.ok = !font.data.empty() &&
              stbtt_InitFont(&font.info, font.data.data(), stbtt_GetFontOffsetForIndex(font.data.data(), 0)) != 0;
    return font.ok;
}

std::vector<uint32_t> codepoints(const std::string &text)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const int length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t cp = length == 1 ? c : c & (0x3fu >> (length - 1));
        for (int k = 1; k < length && i + size_t(k) < text.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + size_t(k)]) & 0x3fu);
        out.push_back(cp);
        i += size_t(length);
    }
    return out;
}

float text_width(const Font &font, float pixels, const std::vector<uint32_t> &text)
{
    const float scale = stbtt_ScaleForPixelHeight(&font.info, pixels);
    float width = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&font.info, int(text[i]), &advance, &bearing);
        width += float(advance) * scale;
        if (i + 1 < text.size()) width += float(stbtt_GetCodepointKernAdvance(&font.info, int(text[i]), int(text[i + 1]))) * scale;
    }
    return width;
}

struct Screen {
    std::mutex lock;
    bool running = false;
    std::atomic<bool> stop{false};
    pthread_t thread{};
    int video = -1;
    int64_t physical = 0;
    void *mapped = nullptr;
    uint8_t *buffers[2] = {};
    std::vector<uint32_t> canvas, base;
    Font title_font, text_font;
    std::string name;
    uint64_t started = 0;
};
Screen screen;

// Draws text with its baseline at y, centred on x when centre is set, cut with an ellipsis to
// max_width; coverage from stb_truetype, blended over what is there.
void draw_text(std::vector<uint32_t> &canvas, const Font &font, float pixels, std::string text, float x, int y,
               uint32_t color, bool centre, float max_width = 1e9f)
{
    if (!font.ok) return;
    std::vector<uint32_t> cps = codepoints(text);
    if (text_width(font, pixels, cps) > max_width) {
        while (!cps.empty()) {
            cps.pop_back();
            std::vector<uint32_t> cut = cps;
            cut.push_back(0x2026);
            if (text_width(font, pixels, cut) <= max_width) { cps = cut; break; }
        }
    }
    const float scale = stbtt_ScaleForPixelHeight(&font.info, pixels);
    float pen = centre ? x - text_width(font, pixels, cps) / 2 : x;
    std::vector<unsigned char> glyph;
    for (size_t i = 0; i < cps.size(); ++i) {
        const int cp = int(cps[i]);
        const float shift = pen - std::floor(pen);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBoxSubpixel(&font.info, cp, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            glyph.assign(size_t(w) * size_t(h), 0);
            stbtt_MakeCodepointBitmapSubpixel(&font.info, glyph.data(), w, h, w, scale, scale, shift, 0, cp);
            for (int gy = 0; gy < h; ++gy) {
                const int py = y + y0 + gy;
                if (py < 0 || py >= int(kHeight)) continue;
                for (int gx = 0; gx < w; ++gx) {
                    const int px = int(std::floor(pen)) + x0 + gx;
                    const unsigned alpha = glyph[size_t(gy) * size_t(w) + size_t(gx)];
                    if (px < 0 || px >= int(kWidth) || !alpha) continue;
                    uint32_t &pixel = canvas[size_t(py) * kWidth + size_t(px)];
                    pixel = blend(pixel, color, alpha);
                }
            }
        }
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&font.info, cp, &advance, &bearing);
        pen += float(advance) * scale;
        if (i + 1 < cps.size()) pen += float(stbtt_GetCodepointKernAdvance(&font.info, cp, int(cps[i + 1]))) * scale;
    }
}

void draw_dot(std::vector<uint32_t> &canvas, float cx, float cy, float radius, uint32_t color, unsigned strength)
{
    for (int y = int(cy - radius - 1); y <= int(cy + radius + 1); ++y)
        for (int x = int(cx - radius - 1); x <= int(cx + radius + 1); ++x) {
            if (x < 0 || y < 0 || x >= int(kWidth) || y >= int(kHeight)) continue;
            const float d = std::sqrt((float(x) + 0.5f - cx) * (float(x) + 0.5f - cx) +
                                      (float(y) + 0.5f - cy) * (float(y) + 0.5f - cy)) - radius;
            const float coverage = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (coverage <= 0) continue;
            uint32_t &pixel = canvas[size_t(y) * kWidth + size_t(x)];
            pixel = blend(pixel, color, unsigned(coverage * float(strength)));
        }
}

void draw_base()
{
    for (unsigned y = 0; y < kHeight; ++y) {
        const unsigned t = y * 255 / (kHeight - 1);
        const uint32_t row = rgb(13 + (22 - 13) * t / 255, 17 + (27 - 17) * t / 255, 23 + (34 - 23) * t / 255);
        std::fill(screen.base.begin() + ptrdiff_t(y) * kWidth, screen.base.begin() + ptrdiff_t(y + 1) * kWidth, row);
    }
    draw_text(screen.base, screen.title_font, 64, screen.name, kWidth / 2.0f, 490, rgb(240, 243, 246), true, 1600);
    draw_text(screen.base, screen.text_font, 22, "MihawkOS", 64, int(kHeight) - 56, rgb(110, 118, 129), false);
    screen.canvas = screen.base;
}

void draw_dynamic(unsigned frame)
{
    for (unsigned y = kDynY0; y < kDynY1; ++y)
        std::copy(screen.base.begin() + ptrdiff_t(y) * kWidth + kDynX0, screen.base.begin() + ptrdiff_t(y) * kWidth + kDynX1,
                  screen.canvas.begin() + ptrdiff_t(y) * kWidth + kDynX0);
    char line[64];
    snprintf(line, sizeof(line), "Starting\xc2\xa0\xc2\xb7\xc2\xa0%u s", unsigned((now_ns() - screen.started) / 1000000000u));
    draw_text(screen.canvas, screen.text_font, 30, line, kWidth / 2.0f, 556, rgb(139, 148, 158), true);
    const unsigned head = frame % 12;
    for (unsigned i = 0; i < 12; ++i) {
        const float angle = float(i) * 6.2831853f / 12 - 1.5707963f;
        const unsigned behind = (head + 12 - i) % 12;
        const unsigned strength = behind < 8 ? 255 - behind * 28 : 40;
        draw_dot(screen.canvas, 960 + 30 * std::cos(angle), 650 + 30 * std::sin(angle), 5, rgb(88, 166, 255), strength);
    }
}

void copy_region(uint8_t *buffer, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            *reinterpret_cast<uint32_t *>(buffer + tiled(x, y)) = screen.canvas[size_t(y) * kWidth + x];
}

// The display reads the buffer as the GPU does: CPU writes go out of the cache first.
void flush_blocks(uint8_t *buffer, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    for (unsigned by = y0 >> 7; by <= (y1 - 1) >> 7; ++by)
        for (unsigned bx = x0 >> 7; bx <= (x1 - 1) >> 7; ++bx) {
            uint8_t *block = buffer + ((size_t(by) * kBlocksX + bx) << 16);
            for (size_t at = 0; at < 65536; at += 64) __asm__ volatile("clflush (%0)" : : "r"(block + at) : "memory");
        }
    __asm__ volatile("mfence" ::: "memory");
}

void *run(void *)
{
    // Above the game's threads (695-700) like the title's own loop, and asleep nearly all the time.
    scePthreadSetprio(pthread_self(), 690);
    int back = 1;
    for (unsigned frame = 1; !screen.stop.load(); ++frame) {
        // The flip to the other buffer has shown, so this one is off the screen.
        for (int i = 0; i < 100 && sceVideoOutIsFlipPending(screen.video) > 0; ++i) usleep(2000);
        draw_dynamic(frame);
        copy_region(screen.buffers[back], kDynX0, kDynY0, kDynX1, kDynY1);
        flush_blocks(screen.buffers[back], kDynX0, kDynY0, kDynX1, kDynY1);
        sceVideoOutSubmitFlip(screen.video, back, PS5_VIDEO_OUT_FLIP_VSYNC, int64_t(frame));
        back ^= 1;
        for (int i = 0; i < 6 && !screen.stop.load(); ++i) usleep(10000);
    }
    return nullptr;
}

void release_memory()
{
    if (screen.mapped) sceKernelMunmap(screen.mapped, kMemoryBytes);
    if (screen.physical) sceKernelReleaseDirectMemory(screen.physical, kMemoryBytes);
    screen.mapped = nullptr;
    screen.physical = 0;
}
} // namespace

void proton_loading_start(const char *name, const char *fonts)
{
    std::lock_guard<std::mutex> guard(screen.lock);
    if (screen.running) return;
    const char *setting = getenv("PROTON_LOADING_SCREEN");
    if (setting && !strcmp(setting, "0")) {
        say("loading screen: off (PROTON_LOADING_SCREEN=0)");
        return;
    }
    // The loading screen owns this opening; only subsequent presenters hand it off.
    screen.video = __real_sceVideoOutOpen(PS5_VIDEO_OUT_USER_SYSTEM, 0, 0, nullptr);
    if (screen.video < 0) {
        say("loading screen: none, sceVideoOutOpen failed %#x", unsigned(screen.video));
        return;
    }
    int status = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), kMemoryBytes, kAlign, kMemoryType,
                                               &screen.physical);
    if (status >= 0)
        status = sceKernelMapDirectMemory(&screen.mapped, kMemoryBytes, kProtection, 0, screen.physical, kAlign);
    if (status < 0) {
        say("loading screen: none, direct memory %#x", unsigned(status));
        release_memory();
        sceVideoOutClose(screen.video);
        return;
    }
    screen.buffers[0] = static_cast<uint8_t *>(screen.mapped);
    screen.buffers[1] = screen.buffers[0] + kBufferBytes;
    screen.name = name && *name ? name : "the game";
    const std::string folder = fonts ? fonts : "";
    const bool fonts_ok = load_font(screen.title_font, folder + "/Inter-SemiBold.ttf") &
                          load_font(screen.text_font, folder + "/Inter-Regular.ttf");
    screen.base.assign(size_t(kWidth) * kHeight, 0);
    screen.started = now_ns();
    draw_base();
    draw_dynamic(0);
    for (uint8_t *buffer : screen.buffers) {
        copy_region(buffer, 0, 0, kWidth, kHeight);
        flush_blocks(buffer, 0, 0, kWidth, kHeight);
    }
    uint8_t attribute[PS5_VIDEO_OUT_ATTRIBUTE_BYTES] = {};
    sceVideoOutSetFlipRate(screen.video, 0);
    sceVideoOutSetBufferAttribute2(attribute, PS5_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SDR, PS5_VIDEO_OUT_TILING_64KB_R_X,
                                   kWidth, kHeight, 0, 0, 0);
    ps5_video_out_buffer list[2] = {{screen.buffers[0], nullptr, {}}, {screen.buffers[1], nullptr, {}}};
    status = sceVideoOutRegisterBuffers2(screen.video, 0, 0, list, 2, attribute, 0, nullptr);
    if (status >= 0) status = sceVideoOutSubmitFlip(screen.video, 0, PS5_VIDEO_OUT_FLIP_VSYNC, 0);
    if (status < 0) {
        say("loading screen: none, VideoOut %#x", unsigned(status));
        sceVideoOutClose(screen.video);
        release_memory();
        return;
    }
    screen.stop = false;
    if (pthread_create(&screen.thread, nullptr, run, nullptr)) {
        say("loading screen: shown without its animation (no thread)");
        screen.thread = pthread_t{};
    }
    screen.running = true;
    say("loading screen: shown for \"%s\"%s", screen.name.c_str(), fonts_ok ? "" : " (fonts missing: no text)");
}

std::string proton_loading_stop(const char *why)
{
    std::lock_guard<std::mutex> guard(screen.lock);
    if (!screen.running) return std::string();
    screen.stop = true;
    if (screen.thread != pthread_t{}) pthread_join(screen.thread, nullptr);
    // A blank flip takes the screen's buffers off the display; VideoOut refuses to unregister a
    // set while one of its buffers is shown (PS5_Mesa's videoout WSI).
    const int blank = sceVideoOutSubmitFlip(screen.video, -1, PS5_VIDEO_OUT_FLIP_VSYNC, 0);
    for (int i = 0; i < 120 && sceVideoOutIsFlipPending(screen.video) > 0; ++i) sceVideoOutWaitVblank(screen.video);
    int unregistered = sceVideoOutUnregisterBuffers(screen.video, 0);
    for (int i = 0; i < 60 && unregistered == PS5_VIDEO_OUT_ERROR_BUSY; ++i) {
        sceVideoOutWaitVblank(screen.video);
        unregistered = sceVideoOutUnregisterBuffers(screen.video, 0);
    }
    const int closed = sceVideoOutClose(screen.video);
    // The memory goes only once the display no longer reads it.
    const bool free_memory = unregistered == 0 || closed == 0;
    if (free_memory) release_memory();
    screen.video = -1;
    std::vector<uint32_t>().swap(screen.canvas);
    std::vector<uint32_t>().swap(screen.base);
    screen.running = false;
    char line[256];
    snprintf(line, sizeof(line), "loading screen: closed after %.1f s (%s): blank flip %d, unregister %#x, close %#x, memory %s",
             double(now_ns() - screen.started) / 1e9, why ? why : "", blank, unsigned(unregistered), unsigned(closed),
             free_memory ? "released" : "kept (the display may still read it)");
    say("%s", line);
    return line;
}

// The guided setup's first session: its cover (src/pw_setup.h, "The first session's cover"). Two frames, so the one
// drawn is never the one the broker may be copying: once show returns, the broker reads only the frame it was given.
namespace {
struct Cover {
    std::mutex lock;
    std::atomic<bool> running{false}, stop{false};
    std::atomic<int> stage{PW_SETUP_BUILD_SETTINGS};
    pthread_t thread{};
    bool (*show)(const void *) = nullptr;
    std::vector<uint32_t> picture, frames[2];
    std::string path;
    uint64_t started = 0;
    unsigned shown = 0, refused = 0;
};
Cover cover;

// The launcher's capture (ps5/src/example_ps5.cpp): P6, 1920x1080, RGB.
bool load_picture(const char *path, std::vector<uint32_t> &out)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned width = 0, height = 0, depth = 0;
    bool ok = fscanf(file, "P6 %u %u %u", &width, &height, &depth) == 3 && fgetc(file) != EOF &&
              width == PW_SETUP_COVER_WIDTH && height == PW_SETUP_COVER_HEIGHT && depth == 255;
    std::vector<unsigned char> bytes(ok ? size_t(width) * height * 3 : 0);
    ok = ok && fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    fclose(file);
    if (!ok) return false;
    out.resize(bytes.size() / 3);
    for (size_t i = 0; i < out.size(); ++i) out[i] = rgb(bytes[i * 3], bytes[i * 3 + 1], bytes[i * 3 + 2]);
    return true;
}

void *cover_run(void *)
{
    // As the loading screen: above the game's threads, and asleep nearly all the time.
    scePthreadSetprio(pthread_self(), 690);
    int back = 0;
    for (unsigned tick = 0; !cover.stop.load(); ++tick) {
        pw_setup_cover_draw(cover.frames[back].data(), cover.picture.data(), PwSetupBuild(cover.stage.load()), tick);
        if (cover.show(cover.frames[back].data())) back ^= 1, ++cover.shown;
        else ++cover.refused;
        for (int i = 0; i < 10 && !cover.stop.load(); ++i) usleep(10000);
    }
    return nullptr;
}
} // namespace

bool proton_cover_start(const char *picture, bool (*show)(const void *))
{
    std::lock_guard<std::mutex> guard(cover.lock);
    if (cover.running) return true;
    if (!show || !load_picture(picture, cover.picture)) {
        say("setup: no cover (%s is missing or not a 1920x1080 picture); the desktop shows", picture);
        std::vector<uint32_t>().swap(cover.picture);
        return false;
    }
    cover.frames[0] = cover.frames[1] = cover.picture;
    cover.show = show;
    cover.path = picture;
    cover.started = now_ns();
    cover.stop = false;
    if (pthread_create(&cover.thread, nullptr, cover_run, nullptr)) {
        say("setup: no cover (no thread); the desktop shows");
        for (auto *frame : {&cover.picture, &cover.frames[0], &cover.frames[1]}) std::vector<uint32_t>().swap(*frame);
        return false;
    }
    cover.running = true;
    say("setup: the cover is shown instead of the desktop until the session ends");
    return true;
}

void proton_cover_process(const char *command_line)
{
    if (!cover.running.load()) return;
    int was = cover.stage.load();
    for (;;) {
        const int next = pw_setup_build_next(PwSetupBuild(was), command_line);
        if (next == was) return;
        if (cover.stage.compare_exchange_weak(was, next)) {
            say("setup: the cover's stage %d, %.1f s in", next, double(now_ns() - cover.started) / 1e9);
            return;
        }
    }
}

void proton_cover_ready()
{
    if (cover.running.load()) cover.stage = PW_SETUP_BUILD_READY;
}

std::string proton_cover_stop(const char *why)
{
    std::lock_guard<std::mutex> guard(cover.lock);
    if (!cover.running) return std::string();
    cover.stop = true;
    pthread_join(cover.thread, nullptr);
    bool cleared = false;
    for (int i = 0; i < 10 && !cleared; ++i) cleared = cover.show(nullptr);
    // A broker that never let the cover go may still read the frames: they are kept.
    if (cleared)
        for (auto *frame : {&cover.picture, &cover.frames[0], &cover.frames[1]}) std::vector<uint32_t>().swap(*frame);
    unlink(cover.path.c_str());
    cover.running = false;
    char line[256];
    snprintf(line, sizeof(line), "setup: the cover closed after %.1f s (%s): %u frames shown, %u refused, stage %d, %s",
             double(now_ns() - cover.started) / 1e9, why ? why : "", cover.shown, cover.refused, cover.stage.load(),
             cleared ? "taken off" : "still held by the broker");
    say("%s", line);
    return line;
}
