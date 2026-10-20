/* SPDX-License-Identifier: GPL-3.0-or-later
 * How much host-visible GPU memory a Windows program can hold, and where it comes from: it makes N Direct3D 11 staging textures of
 * 64 MiB each (DXVK gives each a host-visible allocation shared with the title), writes every byte, keeps them for a few seconds so
 * the title's census and the helper's own records see them, reads them back and lets them go. Run it with PROTON_GPU_HOST_DMEM=0 (shared memory,
 * the title's 1 GiB flexible pool) and =1 (the application's direct memory) and compare the helper's `vm resident` and the title's
 * `system memory` lines (docs/PLAN.md, "Direct memory shared between processes").
 *
 * PS5_PROBE_TEXTURES (default 4, at most 12) sets N; PS5_PROBE_HOLD_SECONDS (default 12) how long they are kept. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
int _fltused;

static void say(const char *text)
{
    OutputDebugStringA(text);
}

static unsigned env_number(const char *name, unsigned fallback)
{
    char text[16];
    unsigned value = 0;
    DWORD length = GetEnvironmentVariableA(name, text, sizeof(text));
    if (!length || length >= sizeof(text)) return fallback;
    for (DWORD i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return fallback;
        value = value * 10 + (unsigned)(text[i] - '0');
    }
    return value ? value : fallback;
}

static unsigned char pattern(unsigned long long at, unsigned texture)
{
    return (unsigned char)((at * 131u + texture * 17u + (at >> 12)) & 0xff);
}

void mainCRTStartup(void)
{
    char line[200];
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *textures[12] = {0};
    D3D_FEATURE_LEVEL level;
    unsigned count = env_number("PS5_PROBE_TEXTURES", 4), hold = env_number("PS5_PROBE_HOLD_SECONDS", 12), made = 0, failures = 0;
    if (count > 12) count = 12;
    HRESULT result = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(result)) {
        wsprintfA(line, "PS5_PROTON_GPU_HOST_MEMORY FAIL: device %08lx\n", result);
        say(line);
        ExitProcess(1);
    }
    for (unsigned i = 0; i < count; i++) {
        D3D11_TEXTURE2D_DESC desc = {0};
        D3D11_MAPPED_SUBRESOURCE mapped;
        desc.Width = 4096;
        desc.Height = 4096;   /* 64 MiB of RGBA8 */
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
        result = ID3D11Device_CreateTexture2D(device, &desc, NULL, &textures[i]);
        if (FAILED(result)) {
            wsprintfA(line, "PS5_PROTON_GPU_HOST_MEMORY: texture %u refused %08lx\n", i, result);
            say(line);
            failures++;
            break;
        }
        result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)textures[i], 0, D3D11_MAP_READ_WRITE, 0, &mapped);
        if (FAILED(result)) {
            wsprintfA(line, "PS5_PROTON_GPU_HOST_MEMORY: map %u failed %08lx\n", i, result);
            say(line);
            failures++;
            break;
        }
        for (unsigned y = 0; y < desc.Height; y++) {
            unsigned char *row = (unsigned char *)mapped.pData + (unsigned long long)y * mapped.RowPitch;
            for (unsigned x = 0; x < desc.Width * 4; x++) row[x] = pattern((unsigned long long)y * desc.Width * 4 + x, i);
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)textures[i], 0);
        made++;
        wsprintfA(line, "PS5_PROTON_GPU_HOST_MEMORY: texture %u of %u written (%u MiB held)\n", i + 1, count, made * 64);
        say(line);
    }
    Sleep(hold * 1000);
    for (unsigned i = 0; i < made; i++) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        D3D11_TEXTURE2D_DESC desc;
        unsigned wrong = 0;
        ID3D11Texture2D_GetDesc(textures[i], &desc);
        result = ID3D11DeviceContext_Map(context, (ID3D11Resource *)textures[i], 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result)) {
            failures++;
            continue;
        }
        for (unsigned y = 0; y < desc.Height; y += 97) {
            unsigned char *row = (unsigned char *)mapped.pData + (unsigned long long)y * mapped.RowPitch;
            for (unsigned x = 0; x < desc.Width * 4; x += 61)
                if (row[x] != pattern((unsigned long long)y * desc.Width * 4 + x, i)) wrong++;
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)textures[i], 0);
        if (wrong) {
            wsprintfA(line, "PS5_PROTON_GPU_HOST_MEMORY: texture %u read back %u wrong samples\n", i, wrong);
            say(line);
            failures++;
        }
    }
    for (unsigned i = 0; i < made; i++) ID3D11Texture2D_Release(textures[i]);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    wsprintfA(line, "PS5_PROTON_GPU_HOST_MEMORY %s: textures=%u of %u failures=%u\n", failures || made != count ? "FAIL" : "PASS", made,
              count, failures);
    say(line);
    ExitProcess(failures || made != count);
}
