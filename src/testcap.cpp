// testcap.exe - opens the "scrcpy-vcam" camera like a normal app would (Media Foundation
// source reader), measures the frame rate and saves the last frame as testcap.bmp.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <vector>
#include "common.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static void SaveBmp(const char* path, const BYTE* nv12, int w, int h)
{
    std::vector<BYTE> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int Y = nv12[y * w + x] - 16;
            const BYTE* uv = nv12 + w * h + (y / 2) * w + (x & ~1);
            int U = uv[0] - 128, V = uv[1] - 128;
            auto clamp = [](int v) { return (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v); };
            BYTE* p = &rgb[((size_t)(h - 1 - y) * w + x) * 3];
            p[2] = clamp((298 * Y + 409 * V + 128) >> 8);
            p[1] = clamp((298 * Y - 100 * U - 208 * V + 128) >> 8);
            p[0] = clamp((298 * Y + 516 * U + 128) >> 8);
        }
    BITMAPFILEHEADER fh{ 0x4d42 };
    BITMAPINFOHEADER ih{ sizeof(ih), w, h, 1, 24 };
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)rgb.size();
    FILE* f;
    if (fopen_s(&f, path, "wb") == 0)
    {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(rgb.data(), rgb.size(), 1, f);
        fclose(f);
    }
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    IMFAttributes* attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attrs, &devices, &count);
    IMFActivate* chosen = nullptr;
    for (UINT32 i = 0; i < count; i++)
    {
        WCHAR* name = nullptr;
        UINT32 len;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        wprintf(L"camera: %s\n", name);
        if (wcsncmp(name, VCAM_NAME, wcslen(VCAM_NAME)) == 0) chosen = devices[i];
        CoTaskMemFree(name);
    }
    if (!chosen)
    {
        printf("scrcpy-vcam camera not found (is scrcpy-vcam.exe running?)\n");
        return 1;
    }

    IMFMediaSource* source = nullptr;
    hr = chosen->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) { printf("ActivateObject 0x%08X\n", hr); return 1; }
    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(source, nullptr, &reader);
    if (FAILED(hr)) { printf("CreateSourceReader 0x%08X\n", hr); return 1; }

    IMFMediaType* type = nullptr;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    printf("format %ux%u\n", w, h);

    std::vector<BYTE> last;
    ULONGLONG start = GetTickCount64();
    int frames = 0;
    while (GetTickCount64() - start < 5000)
    {
        DWORD flags = 0;
        LONGLONG ts;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr)) { printf("ReadSample 0x%08X\n", hr); break; }
        if (!sample) continue;
        frames++;
        IMFMediaBuffer* buf;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)))
        {
            BYTE* d; DWORD len;
            buf->Lock(&d, nullptr, &len);
            last.assign(d, d + len);
            buf->Unlock();
            buf->Release();
        }
        sample->Release();
    }
    printf("%d frames in 5 s (%.1f fps)\n", frames, frames / 5.0);
    if (last.size() >= (size_t)w * h * 3 / 2)
    {
        SaveBmp("testcap.bmp", last.data(), w, h);
        printf("saved testcap.bmp\n");
    }
    source->Shutdown();
    return 0;
}
