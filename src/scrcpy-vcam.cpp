// scrcpy-vcam.exe - streams an Android phone's screen and camera (via adb + scrcpy-server) into
// the "scrcpy-vcam" Media Foundation virtual camera, switching between them per PowerPoint slide.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfvirtualcamera.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <strmif.h>
#include <timeapi.h>
#include <cguid.h>
#include <oleauto.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <stdio.h>
#include <stdint.h>
#include "common.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")

static std::atomic<bool> g_quit{false};
static HANDLE g_doneEvent;

struct Config
{
    std::wstring adb;
    std::wstring server;
    std::string version;
    int maxFps = 60;
    int bitRate = 16000000;
    std::string cameraFacing = "back";
    bool camera = true;
};
static Config g_cfg;

// Both phone streams run all the time so that switching between them is instant.
enum { SRC_SCREEN = 0, SRC_CAMERA = 1 };

struct StreamSource
{
    const char* label;
    int port;
    const char* jar;        // own jar path per stream, so pkill only hits our own leftovers
    int maxSize;
    bool autoSize = true;   // adapt maxSize so the phone delivers exactly the camera frame size
    std::atomic<SOCKET> sock{INVALID_SOCKET};
    std::atomic<int> fps{0};
    std::atomic<int> decodeUs{0};
    std::atomic<int> jitterUs{0};
    std::atomic<ULONGLONG> lastFrame{0};
    std::atomic<bool> live{false};
};
static StreamSource g_sources[2] = {
    { "Screen", 27199, "/data/local/tmp/scrcpy-vcam-screen.jar", 1080 },
    { "Camera", 27200, "/data/local/tmp/scrcpy-vcam-camera.jar", 1920 },
};
static std::atomic<int> g_active{SRC_SCREEN};
static std::atomic<int> g_slide{0}; // current PowerPoint slide, 0 = unknown

template <class T> static void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

static void Status(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("\r%02d:%02d:%02d  %-70s\n", t.wHour, t.wMinute, t.wSecond, buf);
}

static std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// ---------------------------------------------------------------------------
// Process helpers

static bool Launch(const std::wstring& cmdline, HANDLE* process, HANDLE* stdoutRead)
{
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (stdoutRead && !CreatePipe(&rd, &wr, &sa, 0)) return false;
    if (rd) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr ? wr : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = wr ? wr : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (wr) CloseHandle(wr);
    if (!ok)
    {
        if (rd) CloseHandle(rd);
        return false;
    }
    CloseHandle(pi.hThread);
    *process = pi.hProcess;
    if (stdoutRead) *stdoutRead = rd;
    return true;
}

// Runs a command to completion and returns its exit code (-1 if it could not start).
static int Run(const std::wstring& cmdline, std::string* output = nullptr)
{
    HANDLE proc, rd;
    if (!Launch(cmdline, &proc, &rd)) return -1;
    std::string out;
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (output) *output = out;
    return (int)code;
}

static std::wstring Adb(const std::string& serial, const std::string& args)
{
    std::wstring cmd = L"\"" + g_cfg.adb + L"\"";
    if (!serial.empty()) cmd += L" -s " + Widen(serial);
    return cmd + L" " + Widen(args);
}

static std::wstring FindOnPath(const wchar_t* exe)
{
    wchar_t path[MAX_PATH];
    if (!SearchPathW(nullptr, exe, nullptr, MAX_PATH, path, nullptr)) return L"";
    // winget installs symlinks, resolve to the real install directory
    HANDLE f = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (f == INVALID_HANDLE_VALUE) return path;
    wchar_t real[MAX_PATH];
    DWORD n = GetFinalPathNameByHandleW(f, real, MAX_PATH, FILE_NAME_NORMALIZED);
    CloseHandle(f);
    if (n == 0 || n >= MAX_PATH) return path;
    std::wstring r = real;
    if (r.rfind(L"\\\\?\\", 0) == 0) r = r.substr(4);
    return r;
}

static std::wstring DirOf(const std::wstring& p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}

static bool Exists(const std::wstring& p)
{
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Uses the scrcpy installation for scrcpy-server (the server version must match exactly).
static bool LocateTools()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring selfDir = DirOf(self);

    std::wstring scrcpy = FindOnPath(L"scrcpy.exe");
    std::wstring scrcpyDir = scrcpy.empty() ? L"" : DirOf(scrcpy);

    if (Exists(selfDir + L"\\scrcpy-server")) g_cfg.server = selfDir + L"\\scrcpy-server";
    else if (!scrcpyDir.empty() && Exists(scrcpyDir + L"\\scrcpy-server")) g_cfg.server = scrcpyDir + L"\\scrcpy-server";
    if (g_cfg.server.empty())
    {
        Status("scrcpy not found on PATH. Install it, e.g. winget install Genymobile.scrcpy");
        return false;
    }

    g_cfg.adb = FindOnPath(L"adb.exe");
    if (g_cfg.adb.empty() && Exists(scrcpyDir + L"\\adb.exe")) g_cfg.adb = scrcpyDir + L"\\adb.exe";
    if (g_cfg.adb.empty())
    {
        Status("adb not found");
        return false;
    }

    // "scrcpy 4.1 <https://...>"
    std::string out;
    if (!scrcpy.empty() && Run(L"\"" + scrcpy + L"\" --version", &out) == 0 && out.rfind("scrcpy ", 0) == 0)
    {
        size_t end = out.find_first_of(" \r\n", 7);
        g_cfg.version = out.substr(7, end - 7);
    }
    if (g_cfg.version.empty())
    {
        Status("could not determine the scrcpy version");
        return false;
    }
    // the stream format (session packets) is the one of scrcpy 4.x
    if (atoi(g_cfg.version.c_str()) != 4)
    {
        Status("scrcpy %s is not supported, scrcpy-vcam needs scrcpy 4.x", g_cfg.version.c_str());
        return false;
    }
    Status("scrcpy %s, adb: %s", g_cfg.version.c_str(), Narrow(g_cfg.adb).c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Shared memory producer + letterboxing scaler

struct Publisher
{
    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    VcamShm* shm = nullptr;
    ULONGLONG lastAttempt = 0;
    int geometry = 0;       // bumps whenever the source size changes
    int slotGeometry[2] = { -1, -1 };

    // scaler state
    int srcW = 0, srcH = 0, dstX = 0, dstY = 0, dstW = 0, dstH = 0, subs = 1;
    struct Tap { int i; int f; };
    std::vector<Tap> xTapsY, yTapsY, xTapsC, yTapsC;

    bool Ensure()
    {
        if (shm) return true;
        ULONGLONG now = GetTickCount64();
        if (now - lastAttempt < 500) return false;
        lastAttempt = now;
        // created by the camera DLL inside the frame server once an app opens the camera
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, VCAM_SHM_NAME);
        if (!mapping) return false;
        event = OpenEventW(EVENT_MODIFY_STATE, FALSE, VCAM_EVENT_NAME);
        shm = (VcamShm*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VcamShm));
        if (!shm || !event)
        {
            if (shm) UnmapViewOfFile(shm);
            if (event) CloseHandle(event);
            CloseHandle(mapping);
            shm = nullptr;
            mapping = event = nullptr;
            return false;
        }
        shm->width = VCAM_WIDTH;
        shm->height = VCAM_HEIGHT;
        shm->magic = VCAM_MAGIC;
        slotGeometry[0] = slotGeometry[1] = -1;
        Status("camera in use by an app");
        return true;
    }

    bool CameraWatched()
    {
        return shm && GetTickCount64() - (ULONGLONG)shm->readerTick < 1000;
    }

    static void BuildTaps(std::vector<Tap>& taps, int src, int dst, int subs)
    {
        taps.resize((size_t)dst * subs);
        for (int d = 0; d < dst; d++)
            for (int s = 0; s < subs; s++)
            {
                double pos = (d + (s + 0.5) / subs) * src / dst - 0.5;
                if (pos < 0) pos = 0;
                int i = (int)pos;
                int f = (int)((pos - i) * 256);
                if (i >= src - 1) { i = src > 1 ? src - 2 : 0; f = src > 1 ? 256 : 0; }
                taps[(size_t)d * subs + s] = { i, f };
            }
    }

    void Configure(int w, int h)
    {
        if (w == srcW && h == srcH) return;
        srcW = w;
        srcH = h;
        double scale = min((double)VCAM_WIDTH / w, (double)VCAM_HEIGHT / h);
        dstW = min(VCAM_WIDTH, (int)(w * scale + 0.5) & ~1);
        dstH = min(VCAM_HEIGHT, (int)(h * scale + 0.5) & ~1);
        dstX = ((VCAM_WIDTH - dstW) / 2) & ~1;
        dstY = ((VCAM_HEIGHT - dstH) / 2) & ~1;
        subs = scale < 0.75 ? 2 : 1; // supersample when shrinking to keep text readable
        BuildTaps(xTapsY, w, dstW, subs);
        BuildTaps(yTapsY, h, dstH, subs);
        BuildTaps(xTapsC, w / 2, dstW / 2, subs);
        BuildTaps(yTapsC, h / 2, dstH / 2, subs);
        geometry++;
        Status("phone %dx%d -> %dx%d in %dx%d frame", w, h, dstW, dstH, VCAM_WIDTH, VCAM_HEIGHT);
    }

    // Scales one plane. channels = 1 for Y, 2 for interleaved UV.
    static void ScalePlane(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride,
        const std::vector<Tap>& xt, const std::vector<Tap>& yt, int dw, int dh, int subs, int channels)
    {
        const int shift = 16 + (subs == 2 ? 2 : 0);
        for (int y = 0; y < dh; y++)
        {
            uint8_t* out = dst + (size_t)y * dstStride;
            for (int x = 0; x < dw; x++)
            {
                for (int c = 0; c < channels; c++)
                {
                    uint32_t acc = 0;
                    for (int sy = 0; sy < subs; sy++)
                    {
                        const Tap& ty = yt[(size_t)y * subs + sy];
                        const uint8_t* r0 = src + (size_t)ty.i * srcStride;
                        const uint8_t* r1 = r0 + srcStride;
                        for (int sx = 0; sx < subs; sx++)
                        {
                            const Tap& tx = xt[(size_t)x * subs + sx];
                            int o = tx.i * channels + c;
                            uint32_t top = r0[o] * (256 - tx.f) + r0[o + channels] * tx.f;
                            uint32_t bot = r1[o] * (256 - tx.f) + r1[o + channels] * tx.f;
                            acc += top * (256 - ty.f) + bot * ty.f;
                        }
                    }
                    out[x * channels + c] = (uint8_t)((acc + (1u << (shift - 1))) >> shift);
                }
            }
        }
    }

    std::mutex lock;

    // Only frames of the active source reach the camera.
    void Publish(int source, const uint8_t* y, const uint8_t* uv, int stride, int w, int h)
    {
        if (source != g_active) return;
        std::lock_guard<std::mutex> g(lock);
        if (!Ensure()) return;
        Configure(w, h);
        const int W = VCAM_WIDTH, H = VCAM_HEIGHT;
        int slot = (int)((shm->seq + 1) & 1);
        uint8_t* frame = shm->frames[slot];
        uint8_t* dstUV = frame + W * H;
        if (slotGeometry[slot] != geometry)
        {
            memset(frame, 16, (size_t)W * H);
            memset(dstUV, 128, (size_t)W * H / 2);
            slotGeometry[slot] = geometry;
        }
        uint8_t* dy = frame + (size_t)dstY * W + dstX;
        uint8_t* duv = dstUV + (size_t)(dstY / 2) * W + dstX;
        if (dstW == w && dstH == h)
        {
            for (int r = 0; r < h; r++) memcpy(dy + (size_t)r * W, y + (size_t)r * stride, w);
            for (int r = 0; r < h / 2; r++) memcpy(duv + (size_t)r * W, uv + (size_t)r * stride, w);
        }
        else
        {
            ScalePlane(y, stride, dy, W, xTapsY, yTapsY, dstW, dstH, subs, 1);
            ScalePlane(uv, stride, duv, W, xTapsC, yTapsC, dstW / 2, dstH / 2, subs, 2);
        }
        shm->publishTime[slot] = MFGetSystemTime();
        MemoryBarrier();
        InterlockedIncrement64(&shm->seq);
        SetEvent(event);
    }
};

// ---------------------------------------------------------------------------
// H.264 decoding with the Windows built-in decoder MFT (low latency mode)

struct Decoder
{
    IMFTransform* mft = nullptr;
    IMFSample* outSample = nullptr;
    UINT32 frameW = 0, frameH = 0; // decoder buffer size (may be aligned, e.g. 1088)
    LONG stride = 0;
    int visW = 0, visH = 0;

    ~Decoder()
    {
        SafeRelease(outSample);
        SafeRelease(mft);
    }

    HRESULT Init(int w, int h)
    {
        visW = w;
        visH = h;
        HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft));
        if (FAILED(hr)) return hr;

        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(mft->GetAttributes(&attrs)))
        {
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            attrs->Release();
        }
        ICodecAPI* codec = nullptr;
        if (SUCCEEDED(mft->QueryInterface(IID_PPV_ARGS(&codec))))
        {
            VARIANT v;
            v.vt = VT_UI4;
            v.ulVal = TRUE;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            codec->Release();
        }

        IMFMediaType* in = nullptr;
        hr = MFCreateMediaType(&in);
        if (FAILED(hr)) return hr;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, w, h);
        hr = mft->SetInputType(0, in, 0);
        in->Release();
        if (FAILED(hr)) return hr;

        hr = SetOutputType();
        if (FAILED(hr)) return hr;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return S_OK;
    }

    HRESULT SetOutputType()
    {
        for (DWORD i = 0;; i++)
        {
            IMFMediaType* t = nullptr;
            HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
            if (FAILED(hr)) return hr;
            GUID sub;
            if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12)
            {
                hr = mft->SetOutputType(0, t, 0);
                if (SUCCEEDED(hr))
                {
                    MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &frameW, &frameH);
                    UINT32 s = 0;
                    stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? (LONG)s : (LONG)frameW;
                    MFVideoArea area;
                    if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr)))
                    {
                        visW = area.Area.cx;
                        visH = area.Area.cy;
                    }
                }
                t->Release();
                if (FAILED(hr)) return hr;
                return AllocOutput();
            }
            t->Release();
        }
    }

    HRESULT AllocOutput()
    {
        SafeRelease(outSample);
        MFT_OUTPUT_STREAM_INFO info{};
        HRESULT hr = mft->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) return hr;
        DWORD size = max(info.cbSize, (DWORD)(stride * frameH * 3 / 2));
        IMFMediaBuffer* buf = nullptr;
        hr = MFCreateMemoryBuffer(size, &buf);
        if (FAILED(hr)) return hr;
        hr = MFCreateSample(&outSample);
        if (SUCCEEDED(hr)) hr = outSample->AddBuffer(buf);
        buf->Release();
        return hr;
    }

    template <class F> HRESULT Decode(const uint8_t* data, size_t len, int64_t ptsUs, F&& onFrame)
    {
        IMFMediaBuffer* buf = nullptr;
        HRESULT hr = MFCreateMemoryBuffer((DWORD)len, &buf);
        if (FAILED(hr)) return hr;
        BYTE* p;
        buf->Lock(&p, nullptr, nullptr);
        memcpy(p, data, len);
        buf->Unlock();
        buf->SetCurrentLength((DWORD)len);
        IMFSample* in = nullptr;
        MFCreateSample(&in);
        in->AddBuffer(buf);
        in->SetSampleTime(ptsUs * 10);
        buf->Release();
        hr = mft->ProcessInput(0, in, 0);
        in->Release();
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return hr;

        for (;;)
        {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.pSample = outSample;
            DWORD status = 0;
            hr = mft->ProcessOutput(0, 1, &ob, &status);
            SafeRelease(ob.pEvents);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                hr = SetOutputType();
                if (FAILED(hr)) return hr;
                continue;
            }
            if (FAILED(hr)) return hr;

            IMFMediaBuffer* ob2 = nullptr;
            if (SUCCEEDED(outSample->GetBufferByIndex(0, &ob2)))
            {
                BYTE* d;
                DWORD cur = 0;
                if (SUCCEEDED(ob2->Lock(&d, nullptr, &cur)))
                {
                    if (cur >= (DWORD)(stride * frameH * 3 / 2))
                        onFrame(d, d + (size_t)stride * frameH, (int)stride, visW & ~1, visH & ~1);
                    ob2->Unlock();
                }
                ob2->SetCurrentLength(0);
                ob2->Release();
            }
        }
    }
};

// ---------------------------------------------------------------------------
// scrcpy session

static bool RecvAll(SOCKET s, void* buf, int len)
{
    char* p = (char*)buf;
    while (len > 0)
    {
        int n = recv(s, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t Be64(const uint8_t* p) { return ((uint64_t)Be32(p) << 32) | Be32(p + 4); }

static std::string FindDevice()
{
    std::string out;
    if (Run(Adb("", "devices"), &out) != 0) return "";
    size_t pos = out.find('\n'); // skip header
    while (pos != std::string::npos)
    {
        size_t next = out.find('\n', pos + 1);
        std::string line = out.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
        size_t tab = line.find('\t');
        if (tab != std::string::npos && line.compare(tab + 1, 6, "device") == 0)
            return line.substr(0, tab);
        pos = next;
    }
    return "";
}


// Returns true when the session should be restarted immediately (stream size change).
static bool RunSession(const std::string& serial, int id, Publisher& pub)
{
    StreamSource& src = g_sources[id];
    bool reconnectNow = false;
    char scid[16];
    sprintf_s(scid, "%08x", (unsigned)((GetTickCount64() + id) * 2654435761u) & 0x7fffffff);
    std::string port = std::to_string(src.port);

    if (Run(Adb(serial, "push \"" + Narrow(g_cfg.server) + "\" " + src.jar)) != 0)
    {
        Status("[%s] adb push failed", src.label);
        return false;
    }
    // Stop leftovers of a previous run. CLASSPATH is an environment variable, so the server's
    // command line only shows its arguments; "log_level=warn video_source=..." is our signature.
    // pkill exits with 0 if it killed something; Android needs a moment to release the camera.
    if (Run(Adb(serial, std::string("shell pkill -f 'log_level=warn video_source=") + (id == SRC_SCREEN ? "display" : "camera") + "'")) == 0)
        Sleep(1000);
    // Android only produces screen frames while the display is on. Both streams wake it before
    // starting, because waking the phone disconnects an already running camera capture.
    Run(Adb(serial, "shell input keyevent KEYCODE_WAKEUP"));
    if (Run(Adb(serial, "forward tcp:" + port + " localabstract:scrcpy_" + scid)) != 0)
    {
        Status("[%s] adb forward failed", src.label);
        return false;
    }

    // The screen stream also opens the control socket, only for keep_active: it keeps the
    // display on while we run, without changing any phone setting.
    const bool withControl = id == SRC_SCREEN;
    char sourceArgs[192];
    if (id == SRC_SCREEN)
        sprintf_s(sourceArgs, "video_source=display max_fps=%d control=true keep_active=true clipboard_autosync=false", g_cfg.maxFps);
    else
        sprintf_s(sourceArgs, "video_source=camera camera_facing=%s camera_ar=16:9 control=false", g_cfg.cameraFacing.c_str());
    char args[640];
    sprintf_s(args,
        "shell CLASSPATH=%s app_process / com.genymobile.scrcpy.Server %s scid=%s log_level=warn %s "
        "video_codec=h264 max_size=%d video_bit_rate=%d audio=false "
        "tunnel_forward=true cleanup=false send_device_meta=false",
        src.jar, g_cfg.version.c_str(), scid, sourceArgs, src.maxSize, g_cfg.bitRate);
    HANDLE server = nullptr;
    if (!Launch(Adb(serial, args), &server, nullptr))
    {
        Status("[%s] could not start scrcpy-server", src.label);
        Run(Adb(serial, "forward --remove tcp:" + port));
        return false;
    }

    // The forward tunnel accepts connections before the server listens; the dummy byte confirms it.
    SOCKET s = INVALID_SOCKET;
    for (int attempt = 0; attempt < 100 && !g_quit; attempt++)
    {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)src.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        uint8_t dummy;
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0 && recv(s, (char*)&dummy, 1, 0) == 1)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
        if (WaitForSingleObject(server, 100) == WAIT_OBJECT_0) break; // server died
    }

    SOCKET control = INVALID_SOCKET;
    if (s != INVALID_SOCKET && withControl)
    {
        // the server accepts the control socket right after the video socket
        control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)src.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(control, (sockaddr*)&addr, sizeof(addr)) != 0)
        {
            Status("[%s] control connection failed", src.label);
            closesocket(control);
            control = INVALID_SOCKET;
        }
    }

    if (s != INVALID_SOCKET)
    {
        src.sock = s;
        int rcvbuf = 4 << 20;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
        uint8_t codec[4];
        if (RecvAll(s, codec, 4) && Be32(codec) == 0x68323634)
        {
            Status("[%s] connected to %s", src.label, serial.c_str());
            src.live = true;
            Decoder* dec = nullptr;
            std::vector<uint8_t> config, packet;
            uint8_t hdr[12];
            int frames = 0;
            double decodeMs = 0;
            // arrival jitter: spread of (arrival time - phone capture time) within one second
            int64_t minSkew = INT64_MAX, maxSkew = INT64_MIN;
            ULONGLONG statTime = GetTickCount64();
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);

            while (!g_quit && RecvAll(s, hdr, 12))
            {
                if (hdr[0] & 0x80) // session packet: new capture size (e.g. rotation)
                {
                    int w = (int)Be32(hdr + 4), h = (int)Be32(hdr + 8);
                    // Let the phone scale to exactly fit the camera frame: cheaper decode, no PC-side scaling.
                    double scale = min((double)VCAM_WIDTH / w, (double)VCAM_HEIGHT / h);
                    int ideal = (int)(max(w, h) * scale) & ~7;
                    if (src.autoSize && (scale < 0.98 || scale > 1.02) && ideal != src.maxSize)
                    {
                        Status("[%s] %dx%d, restarting stream with max size %d", src.label, w, h, ideal);
                        src.maxSize = ideal;
                        reconnectNow = true;
                        break;
                    }
                    delete dec;
                    dec = new Decoder();
                    HRESULT hr = dec->Init(w, h);
                    if (FAILED(hr))
                    {
                        Status("[%s] H.264 decoder error 0x%08X", src.label, hr);
                        break;
                    }
                    continue;
                }
                uint64_t ptsFlags = Be64(hdr);
                uint32_t size = Be32(hdr + 8);
                bool isConfig = (ptsFlags >> 62) & 1;
                packet.resize(config.size() + size);
                if (!RecvAll(s, packet.data() + config.size(), (int)size)) break;
                if (isConfig)
                {
                    config.assign(packet.begin() + config.size(), packet.end());
                    continue;
                }
                if (!config.empty()) // SPS/PPS go in front of the next frame
                {
                    memcpy(packet.data(), config.data(), config.size());
                    config.clear();
                }
                if (!dec) continue;

                int64_t ptsUs = (int64_t)(ptsFlags & ((1ull << 61) - 1));
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                int64_t skew = t0.QuadPart * 1000000 / freq.QuadPart - ptsUs;
                minSkew = min(minSkew, skew);
                maxSkew = max(maxSkew, skew);
                HRESULT hr = dec->Decode(packet.data(), packet.size(), ptsUs,
                    [&](const uint8_t* y, const uint8_t* uv, int stride, int w, int h) {
                        pub.Publish(id, y, uv, stride, w, h);
                        frames++;
                        src.lastFrame = GetTickCount64();
                    });
                QueryPerformanceCounter(&t1);
                decodeMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
                if (FAILED(hr))
                    Status("[%s] decode error 0x%08X", src.label, hr);

                ULONGLONG now = GetTickCount64();
                if (now - statTime >= 1000)
                {
                    src.fps = frames;
                    src.decodeUs = frames ? (int)(decodeMs * 1000 / frames) : 0;
                    src.jitterUs = maxSkew > minSkew ? (int)(maxSkew - minSkew) : 0;
                    frames = 0;
                    decodeMs = 0;
                    minSkew = INT64_MAX;
                    maxSkew = INT64_MIN;
                    statTime = now;
                }
            }
            delete dec;
            src.live = false;
            src.fps = 0;
            if (!g_quit && !reconnectNow) Status("[%s] disconnected", src.label);
        }
        else
        {
            Status("[%s] unexpected stream (not H.264)", src.label);
        }
        SOCKET mine = s;
        src.sock.compare_exchange_strong(mine, INVALID_SOCKET);
        closesocket(s);
    }
    else if (!g_quit)
    {
        Status("[%s] scrcpy-server does not respond (version %s mismatch?)", src.label, g_cfg.version.c_str());
    }

    if (control != INVALID_SOCKET) closesocket(control);
    TerminateProcess(server, 0);
    CloseHandle(server);
    Run(Adb(serial, "forward --remove tcp:" + port));
    return reconnectNow;
}

static void StreamThread(int id, Publisher* pub)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Starting the screen capture disconnects a running camera capture on the phone,
    // so the camera starts once the screen stream is up.
    if (id == SRC_CAMERA)
        for (int i = 0; i < 100 && !g_quit && !g_sources[SRC_SCREEN].live; i++)
            Sleep(100);
    bool waiting = false;
    while (!g_quit)
    {
        std::string serial = FindDevice();
        if (serial.empty())
        {
            if (!waiting && id == SRC_SCREEN) Status("waiting for a phone (USB debugging enabled?) ...");
            waiting = true;
            Sleep(1000);
            continue;
        }
        waiting = false;
        if (!RunSession(serial, id, *pub) && !g_quit) Sleep(2000);
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// PowerPoint: switch the source by the shape names on the current slide.
// A shape whose name contains "camera" selects the phone camera, "screen" the phone screen
// (German "Kamera"/"Bildschirm" work too). Rename the cameo in the Selection Pane (Alt+F10).
// Slides without such a shape keep the current source.

static HRESULT DispInvoke(IDispatch* d, const wchar_t* name, WORD flags, VARIANT* result, VARIANT* arg = nullptr)
{
    DISPID id;
    LPOLESTR n = (LPOLESTR)name;
    HRESULT hr = d->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    DISPPARAMS params{ arg, nullptr, arg ? 1u : 0u, 0 };
    VariantInit(result);
    return d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, nullptr, nullptr);
}

static IDispatch* DispGet(IDispatch* d, const wchar_t* name)
{
    VARIANT v;
    if (!d || FAILED(DispInvoke(d, name, DISPATCH_PROPERTYGET, &v))) return nullptr;
    if (v.vt == VT_DISPATCH) return v.pdispVal;
    VariantClear(&v);
    return nullptr;
}

static IDispatch* DispItem(IDispatch* collection, long index)
{
    VARIANT arg, v;
    arg.vt = VT_I4;
    arg.lVal = index;
    if (!collection || FAILED(DispInvoke(collection, L"Item", DISPATCH_METHOD | DISPATCH_PROPERTYGET, &v, &arg))) return nullptr;
    if (v.vt == VT_DISPATCH) return v.pdispVal;
    VariantClear(&v);
    return nullptr;
}

static long DispLong(IDispatch* d, const wchar_t* name)
{
    VARIANT v;
    if (!d || FAILED(DispInvoke(d, name, DISPATCH_PROPERTYGET, &v))) return 0;
    VariantChangeType(&v, &v, 0, VT_I4);
    long r = v.vt == VT_I4 ? v.lVal : 0;
    VariantClear(&v);
    return r;
}

static std::wstring DispString(IDispatch* d, const wchar_t* name)
{
    VARIANT v;
    if (!d || FAILED(DispInvoke(d, name, DISPATCH_PROPERTYGET, &v))) return L"";
    std::wstring r = v.vt == VT_BSTR && v.bstrVal ? v.bstrVal : L"";
    VariantClear(&v);
    return r;
}

// Slide shown in the running slide show, otherwise the slide selected in the editor.
static IDispatch* CurrentSlide(IDispatch* app)
{
    IDispatch* slide = nullptr;
    IDispatch* windows = DispGet(app, L"SlideShowWindows");
    if (windows && DispLong(windows, L"Count") > 0)
    {
        IDispatch* win = DispItem(windows, 1);
        IDispatch* view = DispGet(win, L"View");
        slide = DispGet(view, L"Slide");
        SafeRelease(view);
        SafeRelease(win);
    }
    SafeRelease(windows);
    if (!slide)
    {
        IDispatch* win = DispGet(app, L"ActiveWindow");
        IDispatch* view = DispGet(win, L"View");
        slide = DispGet(view, L"Slide");
        SafeRelease(view);
        SafeRelease(win);
    }
    return slide;
}

// -1 = no marker on this slide
static int SourceForSlide(IDispatch* slide)
{
    int result = -1;
    IDispatch* shapes = DispGet(slide, L"Shapes");
    long count = DispLong(shapes, L"Count");
    for (long i = 1; i <= count && result < 0; i++)
    {
        IDispatch* shape = DispItem(shapes, i);
        std::wstring name = DispString(shape, L"Name");
        SafeRelease(shape);
        CharLowerW(name.data());
        if (name.find(L"kamera") != std::wstring::npos || name.find(L"camera") != std::wstring::npos)
            result = SRC_CAMERA;
        else if (name.find(L"bildschirm") != std::wstring::npos || name.find(L"screen") != std::wstring::npos)
            result = SRC_SCREEN;
    }
    SafeRelease(shapes);
    return result;
}

static void PowerPointThread()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CLSID clsid;
    if (FAILED(CLSIDFromProgID(L"PowerPoint.Application", &clsid)))
    {
        CoUninitialize();
        return;
    }
    while (!g_quit)
    {
        // Re-acquired on every poll: holding the object would keep PowerPoint alive after closing it.
        IUnknown* unk = nullptr;
        IDispatch* app = nullptr;
        if (SUCCEEDED(GetActiveObject(clsid, nullptr, &unk)) && unk)
        {
            unk->QueryInterface(IID_PPV_ARGS(&app));
            unk->Release();
        }
        if (!app)
        {
            g_slide = 0;
            Sleep(1000);
            continue;
        }
        IDispatch* slide = CurrentSlide(app);
        if (slide)
        {
            g_slide = DispLong(slide, L"SlideIndex");
            int wanted = SourceForSlide(slide);
            if (wanted >= 0 && wanted != g_active && (wanted == SRC_SCREEN || g_cfg.camera))
                g_active = wanted;
            slide->Release();
        }
        else
        {
            g_slide = 0;
        }
        app->Release();
        Sleep(100);
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------

static BOOL WINAPI CtrlHandler(DWORD type)
{
    g_quit = true;
    for (auto& src : g_sources)
    {
        SOCKET s = src.sock.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) closesocket(s); // unblocks recv
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        WaitForSingleObject(g_doneEvent, 4000); // let main remove the camera
    return TRUE;
}

static void Usage()
{
    printf("usage: scrcpy-vcam [--front] [--no-camera] [--start-camera] [--max-fps N] [--bit-rate N]\n"
           "  --front         use the front camera instead of the back camera\n"
           "  --no-camera     stream the screen only\n"
           "  --start-camera  start with the camera instead of the screen\n"
           "  --max-fps N     screen frame rate limit (default 60)\n"
           "  --bit-rate N    bits per second (default 16000000)\n"
           "Switching: name a shape on the slide \"camera\" or \"screen\" (Selection Pane, Alt+F10),\n"
           "or press C / S in this window.\n");
}

int wmain(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; i++)
    {
        std::wstring a = argv[i];
        if (a == L"--front") g_cfg.cameraFacing = "front";
        else if (a == L"--no-camera") g_cfg.camera = false;
        else if (a == L"--start-camera") g_active = SRC_CAMERA;
        else if (i + 1 < argc && a == L"--max-fps") g_cfg.maxFps = _wtoi(argv[++i]);
        else if (i + 1 < argc && a == L"--bit-rate") g_cfg.bitRate = _wtoi(argv[++i]);
        else
        {
            Usage();
            return 1;
        }
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleTitleW(L"scrcpy-vcam");
    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    timeBeginPeriod(1);

    // adb child processes die with us, even if we are killed
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job, GetCurrentProcess());

    if (!LocateTools()) return 1;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    IMFVirtualCamera* vcam = nullptr;
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser, VCAM_NAME, VCAM_CLSID_STR, nullptr, 0, &vcam);
    if (SUCCEEDED(hr)) hr = vcam->Start(nullptr);
    if (FAILED(hr))
    {
        Status("could not start the virtual camera (0x%08X).", hr);
        Status("Run install.cmd once (needs administrator rights).");
        SafeRelease(vcam);
        MFShutdown();
        return 1;
    }
    Status("camera \"%s\" is ready. C = camera, S = screen, Ctrl+C = quit.", Narrow(VCAM_NAME).c_str());

    Publisher pub;
    std::thread screen(StreamThread, SRC_SCREEN, &pub);
    std::thread camera;
    if (g_cfg.camera) camera = std::thread(StreamThread, SRC_CAMERA, &pub);
    std::thread powerpoint(PowerPointThread);

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    ULONGLONG lastPrint = 0;
    while (!g_quit)
    {
        // keys in the console window
        DWORD n = 0;
        if (GetNumberOfConsoleInputEvents(input, &n) && n)
        {
            INPUT_RECORD rec;
            DWORD read;
            if (ReadConsoleInputW(input, &rec, 1, &read) && rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
            {
                wchar_t c = towlower(rec.Event.KeyEvent.uChar.UnicodeChar);
                if (c == L'c' && g_cfg.camera) g_active = SRC_CAMERA;
                if (c == L's') g_active = SRC_SCREEN;
            }
            continue;
        }
        ULONGLONG now = GetTickCount64();
        if (now - lastPrint >= 1000)
        {
            lastPrint = now;
            char slide[32] = "-";
            if (g_slide) sprintf_s(slide, "slide %d", (int)g_slide);
            auto& a = g_sources[SRC_SCREEN];
            auto& b = g_sources[SRC_CAMERA];
            int fpsA = now - a.lastFrame < 1500 ? (int)a.fps : 0; // stats only update when frames arrive
            int fpsB = now - b.lastFrame < 1500 ? (int)b.fps : 0;
            printf("\r  active: %-6s | screen %3d fps %4.1f ms jitter %3d ms | camera %3d fps %4.1f ms | PowerPoint: %-9s %s ",
                g_sources[g_active].label, fpsA, a.decodeUs / 1000.0, a.jitterUs / 1000, fpsB, b.decodeUs / 1000.0, slide,
                pub.CameraWatched() ? "[in use]" : "        ");
        }
        Sleep(20);
    }

    Status("quitting ...");
    screen.join();
    if (camera.joinable()) camera.join();
    powerpoint.join();
    vcam->Remove();
    vcam->Release();
    MFShutdown();
    CoUninitialize();
    WSACleanup();
    SetEvent(g_doneEvent);
    return 0;
}
