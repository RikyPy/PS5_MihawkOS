/* SPDX-License-Identifier: GPL-3.0-or-later
 * Run unchanged as PE32/PE64 in a fresh prefix or an existing one. Check native
 * graphics dependencies, a real ICU converter, standard writes, GDI glyphs and
 * bitmap pixels, and DirectWrite discovery/glyph mapping/rasterization. A visible
 * GDI window carries the same text; captures qualify presentation separately.
 * No application data, network download, or per-program override is needed. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dwrite.h>

int _fltused;

static unsigned checks, failures;
static void check(int ok, const char *stage)
{
    char line[192];
    ++checks; if (!ok) ++failures;
    wsprintfA(line, "prefix-baseline: %s %s ms=%lu\n", stage, ok ? "ok" : "FAIL", GetTickCount());
    OutputDebugStringA(line);
}

static void text_check(const WCHAR *family, const char *name, IDWriteFactory *factory, IDWriteFontCollection *collection)
{
    static const WCHAR text[] = L"Settings Aa 0123";
    char line[240], resolved[80];
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO info = {0}; void *bits = NULL;
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 320; info.bmiHeader.biHeight = -64;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    HFONT font = CreateFontW(-24,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,family);
    WCHAR face[80] = {0}; WORD glyphs[16] = {0};
    HGDIOBJ old_bitmap = bitmap ? SelectObject(dc,bitmap) : NULL;
    HGDIOBJ old_font = font ? SelectObject(dc,font) : NULL;
    unsigned ink = 0;
    check(dc && bitmap && font && bits, "gdi resources");
    if (dc && bitmap && font && bits) {
        PatBlt(dc,0,0,320,64,WHITENESS);
        GetTextFaceW(dc,80,face);
        WideCharToMultiByte(CP_UTF8,0,face,-1,resolved,sizeof(resolved),NULL,NULL);
        DWORD n = GetGlyphIndicesW(dc,text,ARRAYSIZE(text)-1,glyphs,GGI_MARK_NONEXISTING_GLYPHS);
        unsigned missing = 0;
        for (unsigned i=0;i<ARRAYSIZE(text)-1;++i) if (glyphs[i]==0xffff) ++missing;
        check(n != GDI_ERROR && !missing, "gdi glyphs");
        check(TextOutW(dc,4,8,text,ARRAYSIZE(text)-1), "gdi draw");
        GdiFlush();
        for (unsigned i=0;i<320*64;++i) if ((((DWORD *)bits)[i]&0xffffff)!=0xffffff) ++ink;
        check(ink > 20, "gdi bitmap pixels");
        wsprintfA(line,"prefix-baseline: family=%s gdi_face=%s ink=%u\n",name,resolved,ink);
        OutputDebugStringA(line);
    }
    if (old_font) SelectObject(dc,old_font);
    if (old_bitmap) SelectObject(dc,old_bitmap);
    if (font) DeleteObject(font); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc);

    UINT32 index = 0; BOOL exists = FALSE;
    HRESULT hr = collection ? IDWriteFontCollection_FindFamilyName(collection,family,&index,&exists) : E_FAIL;
    check(SUCCEEDED(hr) && exists,"dwrite family");
    if (FAILED(hr) || !exists) return;
    IDWriteFontFamily *ff = NULL; IDWriteFont *f = NULL; IDWriteFontFace *font_face = NULL;
    hr = IDWriteFontCollection_GetFontFamily(collection,index,&ff);
    if (SUCCEEDED(hr)) hr = IDWriteFontFamily_GetFirstMatchingFont(ff,DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&f);
    if (SUCCEEDED(hr)) hr = IDWriteFont_CreateFontFace(f,&font_face);
    check(SUCCEEDED(hr) && font_face,"dwrite face");
    if (font_face) {
        UINT32 codepoints[] = {'A','a','0','1','2'};
        UINT16 ids[5] = {0}; FLOAT advances[] = {20,20,20,20,20};
        hr = IDWriteFontFace_GetGlyphIndices(font_face,codepoints,5,ids);
        check(SUCCEEDED(hr) && ids[0] && ids[1] && ids[2] && ids[3] && ids[4],"dwrite glyphs");
        DWRITE_GLYPH_RUN run = {font_face,24,5,ids,advances,NULL,FALSE,0};
        IDWriteGlyphRunAnalysis *analysis = NULL; RECT bounds = {0};
        hr = IDWriteFactory_CreateGlyphRunAnalysis(factory,&run,1,NULL,DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL,
            DWRITE_MEASURING_MODE_NATURAL,0,48,&analysis);
        if (SUCCEEDED(hr)) hr = IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis,DWRITE_TEXTURE_CLEARTYPE_3x1,&bounds);
        unsigned bytes = (bounds.right-bounds.left)*(bounds.bottom-bounds.top)*3, nonzero = 0;
        BYTE *alpha = bytes && bytes < 1024*1024 ? HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes) : NULL;
        if (alpha && SUCCEEDED(hr)) {
            hr = IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis,DWRITE_TEXTURE_CLEARTYPE_3x1,&bounds,alpha,bytes);
            for (unsigned i=0;i<bytes;++i) if (alpha[i]) ++nonzero;
        }
        check(SUCCEEDED(hr) && alpha && nonzero > 20,"dwrite raster pixels");
        wsprintfA(line,"prefix-baseline: family=%s dwrite_alpha=%u\n",name,nonzero);OutputDebugStringA(line);
        if (alpha) HeapFree(GetProcessHeap(),0,alpha);
        if (analysis) IDWriteGlyphRunAnalysis_Release(analysis);
        IDWriteFontFace_Release(font_face);
    }
    if (f) IDWriteFont_Release(f); if (ff) IDWriteFontFamily_Release(ff);
}

static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l)
{
    if (message==WM_PAINT) {
        PAINTSTRUCT paint; HDC dc=BeginPaint(window,&paint);
        FillRect(dc,&paint.rcPaint,(HBRUSH)GetStockObject(WHITE_BRUSH));
        const WCHAR *families[]={L"Arial",L"Tahoma",L"Times New Roman",L"Courier New",L"Segoe UI"};
        SetTextColor(dc,RGB(0,0,0));SetBkMode(dc,TRANSPARENT);
        for (unsigned i=0;i<5;++i) {
            HFONT font=CreateFontW(-28,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,0,0,families[i]);
            HGDIOBJ old=SelectObject(dc,font);
            TextOutW(dc,20,25+i*65,families[i],lstrlenW(families[i]));
            TextOutW(dc,310,25+i*65,L"Settings Aa 0123",16);
            SelectObject(dc,old);DeleteObject(font);
        }
        EndPaint(window,&paint);return 0;
    }
    return DefWindowProcW(window,message,w,l);
}

void mainCRTStartup(void)
{
    static const char output[]="prefix-baseline standard output\n";
    DWORD n=0;
    check(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),output,sizeof(output)-1,&n,NULL) && n==sizeof(output)-1,"stdout write");
    check(WriteFile(GetStdHandle(STD_ERROR_HANDLE),output,sizeof(output)-1,&n,NULL) && n==sizeof(output)-1,"stderr write");
    const WCHAR *libraries[]={L"d3d11.dll",L"dxgi.dll",L"d3d12.dll",L"icu.dll",L"opengl32.dll"};
    for (unsigned i=0;i<ARRAYSIZE(libraries);++i) {
        HMODULE m=LoadLibraryW(libraries[i]);check(m!=NULL,"runtime DLL import");if (m) FreeLibrary(m);
    }
    HMODULE icu=LoadLibraryW(L"icu.dll");
    void *(__cdecl *open_converter)(const char *,int *)=icu?(void *)GetProcAddress(icu,"ucnv_open"):NULL;
    void (__cdecl *close_converter)(void *)=icu?(void *)GetProcAddress(icu,"ucnv_close"):NULL;
    int status=0;void *converter=open_converter?open_converter("UTF-8",&status):NULL;
    check(converter && status<=0 && close_converter,"ICU UTF-8 converter");
    if (converter && close_converter) close_converter(converter);if (icu) FreeLibrary(icu);
    IDWriteFactory *factory=NULL;IDWriteFontCollection *collection=NULL;
    HRESULT hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,&IID_IDWriteFactory,(IUnknown **)&factory);
    if (SUCCEEDED(hr)) hr=IDWriteFactory_GetSystemFontCollection(factory,&collection,FALSE);
    check(SUCCEEDED(hr) && collection,"dwrite collection");
    const WCHAR *families[]={L"Arial",L"Tahoma",L"Times New Roman",L"Courier New"};
    const char *names[]={"Arial","Tahoma","Times New Roman","Courier New"};
    for (unsigned i=0;i<ARRAYSIZE(families);++i) text_check(families[i],names[i],factory,collection);
    if (collection) IDWriteFontCollection_Release(collection);if (factory) IDWriteFactory_Release(factory);
    WNDCLASSW type={0};type.lpfnWndProc=window_proc;type.hInstance=GetModuleHandleW(NULL);type.lpszClassName=L"pw-prefix-text";
    HWND window=RegisterClassW(&type)?CreateWindowW(type.lpszClassName,L"Prefix text probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        250,200,1000,440,NULL,NULL,type.hInstance,NULL):NULL;
    check(window!=NULL,"text window");
    if (window) {
        SetForegroundWindow(window);UpdateWindow(window);
        DWORD start=GetTickCount(), repaint=start;
        while (GetTickCount()-start<10000) {
            MSG msg;while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
            if (GetTickCount()-repaint>=500) {InvalidateRect(window,NULL,FALSE);repaint=GetTickCount();}
            Sleep(10);
        }
        DestroyWindow(window);
    }
    char line[192];wsprintfA(line,"PS5_PROTON_PREFIX_BASELINE %s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
    OutputDebugStringA(line);ExitProcess(failures?1:0);
}
