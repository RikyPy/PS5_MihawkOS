/* SPDX-License-Identifier: GPL-3.0-or-later
 * Media Foundation playback of an MP4 (H.264 + AAC), the way games open their videos: IMFSourceReader over the file
 * (winedmo's demuxer), H.264 decoded to NV12 (winedmo's decoder MFT), the same video converted to RGB32 by the
 * reader's video processing (winedmo's video processor), the AAC track decoded to float PCM, and the NV12 frames
 * again from a byte stream through a reader in asynchronous mode (a game's own stream and callback). Every result is
 * compared with what the host's FFmpeg decodes from the same generated clip (tools/make-mf-video-clip.py writes
 * C:\mf-video.mp4 and C:\mf-video.txt): the NV12 frames bit for bit, luma and chroma apart (H.264 decoding is
 * exact), the RGB32 frame's mean
 * colour and the audio's length and loudness within tolerances. No game data: the clip is a generated test pattern.
 *
 *   PS5_PROTON_MF_VIDEO PASS|FAIL checks=N failures=N frames=N decode_fps=N.N
 */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

int _fltused;   /* doubles without a C runtime: the probes link msvcrt's imports only */

static unsigned checks, failures;
static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    wvsprintfA(line, format, args);
    va_end(args);
    OutputDebugStringA(line);
}
static void check(int ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; say("mf-video: FAIL %s\n", what); }
}

static int within(unsigned value, unsigned expected, unsigned tolerance)
{
    return value + tolerance >= expected && value <= expected + tolerance;
}

static unsigned crc_table[256];
static unsigned crc32_update(unsigned crc, const BYTE *data, size_t size)
{
    crc = ~crc;
    while (size--) crc = crc_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* What the host decoded (tools/make-mf-video-clip.py). */
static struct
{
    unsigned width, height, frames, crc_luma, crc_chroma, sample_frame, mean_b, mean_g, mean_r, audio_frames, rms_milli;
} expect;

static IMFSourceReader *open_reader(const WCHAR *path, BOOL processing)
{
    IMFSourceReader *reader = NULL;
    IMFAttributes *attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1))) return NULL;
    IMFAttributes_SetUINT32(attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, processing);
    HRESULT hr = MFCreateSourceReaderFromURL(path, attributes, &reader);
    IMFAttributes_Release(attributes);
    if (FAILED(hr)) say("mf-video: MFCreateSourceReaderFromURL hr %#lx\n", hr);
    return SUCCEEDED(hr) ? reader : NULL;
}

static BOOL select_output(IMFSourceReader *reader, DWORD stream, const GUID *major, const GUID *subtype)
{
    IMFMediaType *type;
    IMFSourceReader_SetStreamSelection(reader, MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(IMFSourceReader_SetStreamSelection(reader, stream, TRUE)) || FAILED(MFCreateMediaType(&type)))
        return FALSE;
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, major);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, subtype);
    HRESULT hr = IMFSourceReader_SetCurrentMediaType(reader, stream, NULL, type);
    IMFMediaType_Release(type);
    if (FAILED(hr)) say("mf-video: SetCurrentMediaType hr %#lx\n", hr);
    return SUCCEEDED(hr);
}

/* The visible frame of a video sample: its first line and pitch, from the 2D buffer when there is one. */
struct frame { IMFMediaBuffer *buffer; IMF2DBuffer *buffer2d; BYTE *data; LONG pitch; };
static BOOL lock_frame(IMFSample *sample, LONG default_pitch, struct frame *frame)
{
    DWORD max, length;
    frame->buffer2d = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &frame->buffer))) return FALSE;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(frame->buffer, &IID_IMF2DBuffer, (void **)&frame->buffer2d)) &&
        SUCCEEDED(IMF2DBuffer_Lock2D(frame->buffer2d, &frame->data, &frame->pitch))) return TRUE;
    if (frame->buffer2d) IMF2DBuffer_Release(frame->buffer2d);
    frame->buffer2d = NULL;
    frame->pitch = default_pitch;
    if (SUCCEEDED(IMFMediaBuffer_Lock(frame->buffer, &frame->data, &max, &length))) return TRUE;
    IMFMediaBuffer_Release(frame->buffer);
    return FALSE;
}
static void unlock_frame(struct frame *frame)
{
    if (frame->buffer2d) { IMF2DBuffer_Unlock2D(frame->buffer2d); IMF2DBuffer_Release(frame->buffer2d); }
    else IMFMediaBuffer_Unlock(frame->buffer);
    IMFMediaBuffer_Release(frame->buffer);
}

/* The size of the frames a reader's stream delivers now: the frame and, when smaller, the visible aperture. */
static void frame_size(IMFSourceReader *reader, DWORD stream, UINT32 *width, UINT32 *height, UINT32 *coded_height,
                       LONG *pitch)
{
    IMFMediaType *type;
    MFVideoArea area;
    UINT64 size = 0;
    UINT32 stride = 0;
    *width = *height = *coded_height = 0;
    *pitch = 0;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(reader, stream, &type))) return;
    IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
    *width = (UINT32)(size >> 32);
    *height = *coded_height = (UINT32)size;
    if (SUCCEEDED(IMFMediaType_GetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), NULL)) &&
        area.Area.cx && area.Area.cy)
    {
        *width = area.Area.cx;
        *height = area.Area.cy;
    }
    if (SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride))) *pitch = (LONG)stride;
    IMFMediaType_Release(type);
}

/* Seconds spent inside ReadSample in the NV12 pass: the decoder's throughput as a game waits on it, without the
 * probe's own CRC over the frames. */
static double read_seconds;

static void nv12_pass(const WCHAR *path)
{
    UINT32 width, height, coded_height, frames = 0, crc_luma = 0, crc_chroma = 0, crc_buffer_chroma = 0;
    UINT32 buffer_rows = 0;   /* the luma rows the buffer holds, when its length says other than the type */
    LARGE_INTEGER frequency, start, end, before, after;
    IMFSourceReader *reader = open_reader(path, FALSE);
    LONG pitch;
    check(reader != NULL, "NV12 reader");
    if (!reader) return;
    check(select_output(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &MFMediaType_Video, &MFVideoFormat_NV12),
          "NV12 output type");
    frame_size(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &width, &height, &coded_height, &pitch);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (;;)
    {
        DWORD index, flags = 0;
        LONGLONG time;
        IMFSample *sample = NULL;
        QueryPerformanceCounter(&before);
        HRESULT hr = IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &index, &flags,
                                                &time, &sample);
        QueryPerformanceCounter(&after);
        read_seconds += (double)(after.QuadPart - before.QuadPart) / frequency.QuadPart;
        if (FAILED(hr)) { say("mf-video: NV12 ReadSample hr %#lx after %u frames\n", hr, frames); check(0, "NV12 read"); break; }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            frame_size(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &width, &height, &coded_height, &pitch);
        if (sample)
        {
            struct frame frame;
            if (lock_frame(sample, pitch ? pitch : (LONG)width, &frame))
            {
                DWORD length = 0;
                if (!frames && SUCCEEDED(IMFMediaBuffer_GetCurrentLength(frame.buffer, &length)))
                {
                    say("mf-video: first NV12 frame: %lu bytes, pitch %ld, 2D %u (a %ux%u frame is %u)\n", length,
                        frame.pitch, frame.buffer2d != NULL, width, coded_height, width * coded_height * 3 / 2);
                    if (frame.pitch > 0 && length > (DWORD)frame.pitch * coded_height * 3 / 2)
                        buffer_rows = length * 2 / 3 / frame.pitch;
                }
                if (buffer_rows)
                {
                    const BYTE *rows = frame.data + frame.pitch * buffer_rows;
                    for (UINT32 y = 0; y < height / 2; ++y)
                        crc_buffer_chroma = crc32_update(crc_buffer_chroma, rows + y * frame.pitch, width);
                }
                for (UINT32 y = 0; y < height; ++y)
                    crc_luma = crc32_update(crc_luma, frame.data + y * frame.pitch, width);
                const BYTE *chroma = frame.data + frame.pitch * coded_height;
                for (UINT32 y = 0; y < height / 2; ++y)
                    crc_chroma = crc32_update(crc_chroma, chroma + y * frame.pitch, width);
                unlock_frame(&frame);
                ++frames;
            }
            IMFSample_Release(sample);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    QueryPerformanceCounter(&end);
    double seconds = (double)(end.QuadPart - start.QuadPart) / frequency.QuadPart;
    unsigned fps10 = read_seconds > 0 ? (unsigned)(10 * frames / read_seconds) : 0;
    say("mf-video: nv12 %ux%u (coded height %u, pitch %d) frames=%u luma=%08x chroma=%08x (host %08x %08x) ms=%u "
        "read_ms=%u decode_fps=%u.%u\n", width, height, coded_height, (int)pitch, frames, crc_luma, crc_chroma,
        expect.crc_luma, expect.crc_chroma, (unsigned)(seconds * 1000), (unsigned)(read_seconds * 1000), fps10 / 10,
        fps10 % 10);
    check(width == expect.width && height == expect.height, "NV12 visible frame size");
    check(frames == expect.frames, "NV12 frame count");
    check(crc_luma == expect.crc_luma, "NV12 luma equals the host's decode");
    check(crc_chroma == expect.crc_chroma, "NV12 chroma equals the host's decode");
    if (buffer_rows)
        say("mf-video: the buffers hold %u luma rows, not the type's %u: chroma after them %s the host's (%08x)\n",
            buffer_rows, coded_height, crc_buffer_chroma == expect.crc_chroma ? "equals" : "differs from",
            crc_buffer_chroma);
    IMFSourceReader_Release(reader);
}

/* A reader in asynchronous mode over a byte stream, as a game that hands Media Foundation its own stream does: each
 * ReadSample returns at once and the sample arrives in OnReadSample on a work queue thread. A request whose callback
 * never comes is a timeout here, not a hang. */
struct read_callback
{
    IMFSourceReaderCallback IMFSourceReaderCallback_iface;
    HANDLE done;
    HRESULT status;
    DWORD flags;
    IMFSample *sample;
};
static struct read_callback *impl_from_callback(IMFSourceReaderCallback *iface)
{
    return CONTAINING_RECORD(iface, struct read_callback, IMFSourceReaderCallback_iface);
}
static HRESULT WINAPI callback_QueryInterface(IMFSourceReaderCallback *iface, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IMFSourceReaderCallback) || IsEqualIID(iid, &IID_IUnknown))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI callback_AddRef(IMFSourceReaderCallback *iface) { return 2; }
static ULONG WINAPI callback_Release(IMFSourceReaderCallback *iface) { return 1; }
static HRESULT WINAPI callback_OnReadSample(IMFSourceReaderCallback *iface, HRESULT status, DWORD index, DWORD flags,
                                            LONGLONG time, IMFSample *sample)
{
    struct read_callback *callback = impl_from_callback(iface);
    callback->status = status;
    callback->flags = flags;
    if ((callback->sample = sample)) IMFSample_AddRef(sample);
    SetEvent(callback->done);
    return S_OK;
}
static HRESULT WINAPI callback_OnFlush(IMFSourceReaderCallback *iface, DWORD index) { return S_OK; }
static HRESULT WINAPI callback_OnEvent(IMFSourceReaderCallback *iface, DWORD index, IMFMediaEvent *event) { return S_OK; }
static IMFSourceReaderCallbackVtbl callback_vtbl =
{
    callback_QueryInterface, callback_AddRef, callback_Release, callback_OnReadSample, callback_OnFlush,
    callback_OnEvent,
};

/* Static: a reader may still call its callback on a work queue thread after its last release. */
static struct read_callback callback = {{&callback_vtbl}};

static void async_pass(const WCHAR *path)
{
    UINT32 width, height, coded_height, frames = 0, crc_luma = 0, crc_chroma = 0;
    IMFSourceReader *reader = NULL;
    IMFAttributes *attributes;
    IMFByteStream *stream;
    LONG pitch;
    HRESULT hr;
    if (FAILED(hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path, &stream)))
    {
        say("mf-video: MFCreateFile hr %#lx\n", hr);
        check(0, "async byte stream");
        return;
    }
    if (!callback.done) callback.done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (SUCCEEDED(hr = MFCreateAttributes(&attributes, 1)))
    {
        IMFAttributes_SetUnknown(attributes, &MF_SOURCE_READER_ASYNC_CALLBACK,
                                 (IUnknown *)&callback.IMFSourceReaderCallback_iface);
        hr = MFCreateSourceReaderFromByteStream(stream, attributes, &reader);
        IMFAttributes_Release(attributes);
    }
    IMFByteStream_Release(stream);
    if (FAILED(hr)) say("mf-video: MFCreateSourceReaderFromByteStream hr %#lx\n", hr);
    check(reader != NULL, "async reader over a byte stream");
    if (!reader) return;
    check(select_output(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &MFMediaType_Video, &MFVideoFormat_NV12),
          "async NV12 output type");
    frame_size(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &width, &height, &coded_height, &pitch);
    for (;;)
    {
        if (FAILED(hr = IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, NULL, NULL,
                                                   NULL)))
        {
            say("mf-video: async ReadSample hr %#lx after %u frames\n", hr, frames);
            check(0, "async read request");
            break;
        }
        if (WaitForSingleObject(callback.done, 10000))
        {
            say("mf-video: async OnReadSample never came after %u frames\n", frames);
            check(0, "async sample within 10 s");
            break;
        }
        if (FAILED(callback.status))
        {
            say("mf-video: async OnReadSample hr %#lx after %u frames\n", callback.status, frames);
            check(0, "async read");
            break;
        }
        if (callback.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            frame_size(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &width, &height, &coded_height, &pitch);
        if (callback.sample)
        {
            struct frame frame;
            if (lock_frame(callback.sample, pitch ? pitch : (LONG)width, &frame))
            {
                for (UINT32 y = 0; y < height; ++y)
                    crc_luma = crc32_update(crc_luma, frame.data + y * frame.pitch, width);
                const BYTE *chroma = frame.data + frame.pitch * coded_height;
                for (UINT32 y = 0; y < height / 2; ++y)
                    crc_chroma = crc32_update(crc_chroma, chroma + y * frame.pitch, width);
                unlock_frame(&frame);
                ++frames;
            }
            IMFSample_Release(callback.sample);
            callback.sample = NULL;
        }
        if (callback.flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    say("mf-video: async nv12 frames=%u luma=%08x chroma=%08x\n", frames, crc_luma, crc_chroma);
    check(frames == expect.frames, "async NV12 frame count");
    check(crc_luma == expect.crc_luma && crc_chroma == expect.crc_chroma, "async NV12 frames equal the host's decode");
    IMFSourceReader_Release(reader);
}

static void rgb32_pass(const WCHAR *path)
{
    UINT32 width, height, coded_height, frames = 0;
    double sum[3] = {0};
    IMFSourceReader *reader = open_reader(path, TRUE);
    LONG pitch;
    check(reader != NULL, "RGB32 reader");
    if (!reader) return;
    check(select_output(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &MFMediaType_Video, &MFVideoFormat_RGB32),
          "RGB32 output type");
    frame_size(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &width, &height, &coded_height, &pitch);
    for (;;)
    {
        DWORD index, flags = 0;
        LONGLONG time;
        IMFSample *sample = NULL;
        if (FAILED(IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &index, &flags, &time,
                                              &sample))) { check(0, "RGB32 read"); break; }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            frame_size(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &width, &height, &coded_height, &pitch);
        if (sample)
        {
            struct frame frame;
            if (frames == expect.sample_frame && lock_frame(sample, pitch ? pitch : (LONG)width * 4, &frame))
            {
                /* The mean colour does not depend on the rows' order, so a bottom-up frame reads the same. */
                for (UINT32 y = 0; y < height; ++y)
                    for (UINT32 x = 0; x < width; ++x)
                        for (int c = 0; c < 3; ++c) sum[c] += frame.data[y * frame.pitch + x * 4 + c];
                unlock_frame(&frame);
            }
            ++frames;
            IMFSample_Release(sample);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    double pixels = (double)width * height;
    unsigned b = pixels ? (unsigned)(sum[0] / pixels + 0.5) : 0, g = pixels ? (unsigned)(sum[1] / pixels + 0.5) : 0,
             r = pixels ? (unsigned)(sum[2] / pixels + 0.5) : 0;
    say("mf-video: rgb32 %ux%u frames=%u frame%u mean bgr=%u,%u,%u (host %u,%u,%u)\n", width, height, frames,
        expect.sample_frame, b, g, r, expect.mean_b, expect.mean_g, expect.mean_r);
    check(frames == expect.frames, "RGB32 frame count");
    check(within(b, expect.mean_b, 12) && within(g, expect.mean_g, 12) && within(r, expect.mean_r, 12),
          "RGB32 frame colour within 12 of the host's");
    IMFSourceReader_Release(reader);
}

static void audio_pass(const WCHAR *path)
{
    UINT32 channels = 0, rate = 0;
    unsigned long long frames = 0;
    double energy = 0, count = 0;
    IMFSourceReader *reader = open_reader(path, FALSE);
    IMFMediaType *type;
    check(reader != NULL, "audio reader");
    if (!reader) return;
    check(select_output(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, &MFMediaType_Audio, &MFAudioFormat_Float),
          "float audio output type");
    if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type)))
    {
        IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
        IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        IMFMediaType_Release(type);
    }
    for (;;)
    {
        DWORD index, flags = 0, max, length;
        LONGLONG time;
        IMFSample *sample = NULL;
        IMFMediaBuffer *buffer;
        BYTE *data;
        if (FAILED(IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &index, &flags, &time,
                                              &sample))) { check(0, "audio read"); break; }
        if (sample && SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer)))
        {
            if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, &max, &length)))
            {
                const float *values = (const float *)data;
                for (DWORD i = 0; i < length / sizeof(float); ++i) energy += (double)values[i] * values[i];
                count += length / sizeof(float);
                if (channels) frames += length / sizeof(float) / channels;
                IMFMediaBuffer_Unlock(buffer);
            }
            IMFMediaBuffer_Release(buffer);
        }
        if (sample) IMFSample_Release(sample);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    unsigned rms_milli = count ? (unsigned)(1000.0 * __builtin_sqrt(energy / count) + 0.5) : 0;
    say("mf-video: audio %u Hz x%u frames=%u rms=%u/1000 (host %u frames, %u/1000)\n", rate, channels, (unsigned)frames,
        rms_milli, expect.audio_frames, expect.rms_milli);
    check(rate == 48000 && channels == 2, "48 kHz stereo audio");
    check(frames + 4096 >= expect.audio_frames && frames <= expect.audio_frames + 4096,
          "audio length within 4096 frames of the host's");
    check(within(rms_milli, expect.rms_milli, 30), "audio loudness within 0.03 of the host's");
    IMFSourceReader_Release(reader);
}

static BOOL read_expectations(const WCHAR *name)
{
    char text[512] = {0};
    DWORD read = 0;
    HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(file, text, sizeof(text) - 1, &read, NULL);
    CloseHandle(file);
    /* Eleven numbers, the fourth and fifth (the CRCs) in hexadecimal. */
    unsigned *fields[] = {&expect.width, &expect.height, &expect.frames, &expect.crc_luma, &expect.crc_chroma,
                          &expect.sample_frame,
                          &expect.mean_b, &expect.mean_g, &expect.mean_r, &expect.audio_frames, &expect.rms_milli};
    const char *at = text;
    for (unsigned i = 0; i < ARRAYSIZE(fields); ++i)
    {
        unsigned base = i == 3 || i == 4 ? 16 : 10, value = 0, digits = 0;
        while (*at == ' ' || *at == '\n' || *at == '\r') ++at;
        for (;; ++at, ++digits)
        {
            unsigned digit = *at >= '0' && *at <= '9' ? *at - '0' : base == 16 && *at >= 'a' && *at <= 'f' ? *at - 'a' + 10 : 99;
            if (digit >= base) break;
            value = value * base + digit;
        }
        if (!digits) return FALSE;
        *fields[i] = value;
    }
    return TRUE;
}

/* The clip: the command line's last word (a path without spaces), C:\mf-video.mp4 when there is none; its
 * expectations are beside it with .txt for its extension. */
static void clip_paths(WCHAR *clip, WCHAR *expectations, unsigned size)
{
    const WCHAR *line = GetCommandLineW(), *last = NULL, *at;
    lstrcpynW(clip, L"C:\\mf-video.mp4", size);
    if (*line == '"')
    {
        for (++line; *line && *line != '"'; ++line) continue;
        if (*line) ++line;
    }
    else while (*line && *line != ' ') ++line;
    for (at = line; *at; ++at)
        if (*at != ' ' && (at == line || at[-1] == ' ')) last = at;
    if (last) lstrcpynW(clip, last, size);
    lstrcpynW(expectations, clip, size);
    WCHAR *dot = NULL;
    for (WCHAR *c = expectations; *c; ++c) if (*c == '.') dot = c;
    if (dot && dot - expectations + 5 < (int)size) lstrcpyW(dot, L".txt");
}

void mainCRTStartup(void)
{
    WCHAR path[MAX_PATH], expectations[MAX_PATH];
    clip_paths(path, expectations, MAX_PATH);

    for (unsigned i = 0; i < 256; ++i)
    {
        unsigned c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    check(read_expectations(expectations), "the clip's expectations (tools/make-mf-video-clip.py)");
    check(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)), "CoInitializeEx");
    check(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)), "MFStartup");
    nv12_pass(path);
    double fps = read_seconds > 0 ? expect.frames / read_seconds : 0;
    async_pass(path);
    rgb32_pass(path);
    if (expect.audio_frames) audio_pass(path);   /* a clip without audio says 0 frames */
    MFShutdown();
    CoUninitialize();
    unsigned fps10 = (unsigned)(10 * fps);
    say("PS5_PROTON_MF_VIDEO %s checks=%u failures=%u frames=%u decode_fps=%u.%u\n", failures ? "FAIL" : "PASS",
        checks, failures, expect.frames, fps10 / 10, fps10 % 10);
    ExitProcess(failures != 0);
}
