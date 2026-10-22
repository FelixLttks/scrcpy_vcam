// scrcpy-vcam-source.dll - Media Foundation virtual camera media source.
// Loaded by the Windows Camera Frame Server. Serves NV12 frames that scrcpy-vcam.exe
// writes into a shared memory double buffer.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <sddl.h>
#include <propvarutil.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <stdio.h>
#include "common.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")

static HMODULE g_module;
static std::atomic<long> g_objects{0};

static void Log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf) - 2, _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) n = (int)strlen(buf);
    buf[n++] = '\n';
    buf[n] = 0;
    OutputDebugStringA(buf);
    // install.cmd creates this directory writable for the frame server account
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%ProgramData%\\scrcpy-vcam\\source.log", path, MAX_PATH)) return;
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        char line[600];
        SYSTEMTIME t;
        GetLocalTime(&t);
        int len = sprintf_s(line, "%02d:%02d:%02d.%03d [%lu] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), buf);
        DWORD written;
        WriteFile(f, line, (DWORD)len, &written, nullptr);
        CloseHandle(f);
    }
}

template <class T> static void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

// Forwards IMFAttributes (or a derived interface such as IMFActivate) to an inner attribute store.
template <class Base> struct AttributesImpl : Base
{
    IMFAttributes* m_attr = nullptr;

    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return m_attr->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return m_attr->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return m_attr->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return m_attr->Compare(a, m, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return m_attr->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return m_attr->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return m_attr->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return m_attr->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return m_attr->GetStringLength(k, l); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 n, UINT32* l) override { return m_attr->GetString(k, s, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) override { return m_attr->GetAllocatedString(k, s, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return m_attr->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* r) override { return m_attr->GetBlob(k, b, n, r); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return m_attr->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID i, LPVOID* p) override { return m_attr->GetUnknown(k, i, p); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return m_attr->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return m_attr->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return m_attr->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return m_attr->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return m_attr->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return m_attr->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return m_attr->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return m_attr->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return m_attr->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return m_attr->SetUnknown(k, u); }
    STDMETHODIMP LockStore() override { return m_attr->LockStore(); }
    STDMETHODIMP UnlockStore() override { return m_attr->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return m_attr->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return m_attr->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return m_attr->CopyAllItems(d); }
};

#define IMPLEMENT_REFCOUNT                                                   \
    std::atomic<long> m_ref{1};                                              \
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }               \
    STDMETHODIMP_(ULONG) Release() override                                  \
    {                                                                        \
        long r = --m_ref;                                                    \
        if (r == 0) delete this;                                             \
        return r;                                                            \
    }

// ---------------------------------------------------------------------------
// Shared memory reader

struct FrameReader
{
    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    VcamShm* shm = nullptr;
    LONG64 lastSeq = 0;
    ULONGLONG lastAttempt = 0;

    bool Ensure()
    {
        if (shm) return true;
        ULONGLONG now = GetTickCount64();
        if (now - lastAttempt < 1000) return false;
        lastAttempt = now;

        // Everyone full access, low integrity label so any user process may write.
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr))
            return false;
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(VcamShm), VCAM_SHM_NAME);
        if (!mapping)
            Log("CreateFileMapping failed: %lu", GetLastError());
        event = CreateEventW(&sa, FALSE, FALSE, VCAM_EVENT_NAME);
        if (!event)
            Log("CreateEvent failed: %lu", GetLastError());
        LocalFree(sd);
        if (!mapping || !event)
        {
            Close();
            return false;
        }
        shm = (VcamShm*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VcamShm));
        if (!shm)
        {
            Log("MapViewOfFile failed: %lu", GetLastError());
            Close();
            return false;
        }
        Log("shared memory ready, seq=%lld", shm->seq);
        return true;
    }

    void Close()
    {
        if (shm) UnmapViewOfFile(shm);
        if (mapping) CloseHandle(mapping);
        if (event) CloseHandle(event);
        shm = nullptr;
        mapping = event = nullptr;
    }

    bool HasNewFrame()
    {
        return Ensure() && shm->seq != lastSeq;
    }

    // Waits up to timeoutMs for a frame newer than the last one delivered.
    void WaitForNewFrame(DWORD timeoutMs)
    {
        if (!Ensure())
        {
            Sleep(timeoutMs);
            return;
        }
        ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (shm->seq == lastSeq)
        {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            WaitForSingleObject(event, (DWORD)(deadline - now));
        }
    }

    // timing stats, logged every 5 s
    int statNew = 0, statRepeat = 0;
    LONGLONG statLatency = 0, statMaxLatency = 0;
    ULONGLONG statStart = 0;

    void Stats(bool fresh, LONGLONG latency)
    {
        ULONGLONG now = GetTickCount64();
        if (!statStart) statStart = now;
        if (fresh)
        {
            statNew++;
            statLatency += latency;
            statMaxLatency = max(statMaxLatency, latency);
        }
        else
        {
            statRepeat++;
        }
        if (now - statStart >= 5000)
        {
            double secs = (now - statStart) / 1000.0;
            Log("delivered %.1f new fps + %.1f repeats/s, publish->deliver avg %.1f ms max %.1f ms",
                statNew / secs, statRepeat / secs, statNew ? statLatency / 10000.0 / statNew : 0.0, statMaxLatency / 10000.0);
            statNew = statRepeat = 0;
            statLatency = statMaxLatency = 0;
            statStart = now;
        }
    }

    // Copies the latest frame into an NV12 destination with the given pitch.
    // Returns the time the frame was published, or 0 if there is none.
    LONGLONG CopyLatest(BYTE* dst, LONG pitch)
    {
        const int w = VCAM_WIDTH, h = VCAM_HEIGHT;
        if (shm && shm->magic == VCAM_MAGIC)
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                LONG64 s1 = shm->seq;
                if (s1 == 0) break;
                const BYTE* src = shm->frames[s1 & 1];
                LONGLONG published = shm->publishTime[s1 & 1];
                for (int y = 0; y < h * 3 / 2; y++)
                    memcpy(dst + (size_t)y * pitch, src + (size_t)y * w, w);
                LONG64 s2 = shm->seq;
                if (s2 - s1 < 2)
                {
                    Stats(s1 != lastSeq, MFGetSystemTime() - published);
                    lastSeq = s1;
                    shm->readerTick = (LONG64)GetTickCount64();
                    return published;
                }
            }
            if (shm->seq != 0)
                return shm->publishTime[shm->seq & 1]; // torn twice in a row, the copied frame is still mostly fine
        }
        // no producer: black frame
        for (int y = 0; y < h; y++)
            memset(dst + (size_t)y * pitch, 16, w);
        for (int y = 0; y < h / 2; y++)
            memset(dst + (size_t)(h + y) * pitch, 128, w);
        return 0;
    }
};

// ---------------------------------------------------------------------------
// Media stream

struct MediaSource;

struct MediaStream : AttributesImpl<IMFAttributes>, IMFMediaStream2, IKsControl
{
    IMPLEMENT_REFCOUNT

    std::mutex m_lock;
    std::condition_variable m_cv;
    std::deque<IUnknown*> m_tokens;
    std::thread m_worker;
    bool m_running = false;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    IMFMediaSource* m_source = nullptr; // weak, cleared on shutdown
    IMFMediaEventQueue* m_queue = nullptr;
    IMFStreamDescriptor* m_descriptor = nullptr;
    IMFVideoSampleAllocatorEx* m_allocator = nullptr;
    FrameReader m_reader;

    MediaStream() { g_objects++; }
    ~MediaStream()
    {
        Shutdown();
        if (m_timer) CloseHandle(m_timer);
        SafeRelease(m_attr);
        g_objects--;
    }

    HRESULT Initialize(IMFMediaSource* source)
    {
        m_source = source;
        HRESULT hr = MFCreateAttributes(&m_attr, 8);
        if (FAILED(hr)) return hr;
        m_attr->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        m_attr->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
        m_attr->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        m_attr->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

        hr = MFCreateEventQueue(&m_queue);
        if (FAILED(hr)) return hr;

        IMFMediaType* type = nullptr;
        hr = MFCreateMediaType(&type);
        if (FAILED(hr)) return hr;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        MFSetAttributeSize(type, MF_MT_FRAME_SIZE, VCAM_WIDTH, VCAM_HEIGHT);
        type->SetUINT32(MF_MT_DEFAULT_STRIDE, VCAM_WIDTH);
        MFSetAttributeRatio(type, MF_MT_FRAME_RATE, VCAM_FPS, 1);
        MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        type->SetUINT32(MF_MT_AVG_BITRATE, VCAM_FRAME_BYTES * 8 * VCAM_FPS);

        hr = MFCreateStreamDescriptor(0, 1, &type, &m_descriptor);
        if (SUCCEEDED(hr))
        {
            IMFMediaTypeHandler* handler = nullptr;
            hr = m_descriptor->GetMediaTypeHandler(&handler);
            if (SUCCEEDED(hr))
            {
                hr = handler->SetCurrentMediaType(type);
                handler->Release();
            }
        }
        type->Release();
        return hr;
    }

    HRESULT Start(IMFMediaType* type)
    {
        Log("stream start");
        {
            std::lock_guard<std::mutex> g(m_lock);
            if (!m_queue || !m_allocator) return MF_E_SHUTDOWN;
            if (m_running) return S_OK;
            IMFMediaType* current = type;
            IMFMediaTypeHandler* handler = nullptr;
            if (!current && SUCCEEDED(m_descriptor->GetMediaTypeHandler(&handler)))
            {
                handler->GetCurrentMediaType(&current);
                handler->Release();
            }
            HRESULT hr = m_allocator->InitializeSampleAllocator(10, current);
            if (current != type) SafeRelease(current);
            if (FAILED(hr))
            {
                Log("InitializeSampleAllocator failed 0x%08X", hr);
                return hr;
            }
            m_running = true;
            m_state = MF_STREAM_STATE_RUNNING;
        }
        m_worker = std::thread([this] { WorkerLoop(); });
        return m_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    }

    HRESULT Stop()
    {
        Log("stream stop");
        StopWorker();
        std::lock_guard<std::mutex> g(m_lock);
        if (!m_queue) return MF_E_SHUTDOWN;
        if (m_allocator) m_allocator->UninitializeSampleAllocator();
        m_state = MF_STREAM_STATE_STOPPED;
        return m_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }

    void StopWorker()
    {
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_running = false;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
        std::lock_guard<std::mutex> g(m_lock);
        for (auto t : m_tokens)
            if (t) t->Release();
        m_tokens.clear();
    }

    void Shutdown()
    {
        StopWorker();
        std::lock_guard<std::mutex> g(m_lock);
        if (m_queue) m_queue->Shutdown();
        SafeRelease(m_queue);
        SafeRelease(m_descriptor);
        SafeRelease(m_allocator);
        m_source = nullptr;
        m_reader.Close();
    }

    // Behave like a real camera: one sample per frame interval, always the newest phone frame.
    // Delivering faster than the app renders only piles frames up in the app (latency, judder).
    // The slot waits a little for a fresh frame, which locks its phase to a ~30 fps source.
    HANDLE m_timer = nullptr;
    LONGLONG m_nextDue = 0;

    void WaitForFrameSlot()
    {
        const LONGLONG interval = 10000000 / VCAM_FPS;
        if (!m_timer)
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LONGLONG now = MFGetSystemTime();
        if (m_timer && m_nextDue > now)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(m_nextDue - now);
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(m_timer, (DWORD)((m_nextDue - now) / 10000) + 50);
        }
        if (!m_reader.HasNewFrame())
            m_reader.WaitForNewFrame((DWORD)(interval / 2 / 10000));
        m_nextDue = MFGetSystemTime() + interval;
    }

    void WorkerLoop()
    {
        for (;;)
        {
            {
                std::unique_lock<std::mutex> g(m_lock);
                m_cv.wait(g, [this] { return !m_running || !m_tokens.empty(); });
                if (!m_running) return;
            }

            WaitForFrameSlot();

            IUnknown* token = nullptr;
            IMFVideoSampleAllocatorEx* allocator = nullptr;
            IMFMediaEventQueue* queue = nullptr;
            {
                std::lock_guard<std::mutex> g(m_lock);
                if (!m_running) return;
                token = m_tokens.front();
                allocator = m_allocator;
                queue = m_queue;
                if (allocator) allocator->AddRef();
                if (queue) queue->AddRef();
            }
            // Every request must be answered with a sample, otherwise the frame server has one
            // request less in flight and the stream gets choppier over time. So when all samples
            // are still held by the app, keep the request and try again shortly.
            bool done = !allocator || !queue || DeliverSample(allocator, queue, token);
            SafeRelease(allocator);
            SafeRelease(queue);
            if (done)
            {
                std::lock_guard<std::mutex> g(m_lock);
                if (!m_tokens.empty() && m_tokens.front() == token)
                {
                    m_tokens.pop_front();
                    SafeRelease(token);
                }
            }
            else
            {
                Sleep(2);
            }
        }
    }

    int m_allocFailures = 0;

    // Returns false if no sample was available (the request stays pending).
    bool DeliverSample(IMFVideoSampleAllocatorEx* allocator, IMFMediaEventQueue* queue, IUnknown* token)
    {
        IMFSample* sample = nullptr;
        HRESULT hr = allocator->AllocateSample(&sample);
        if (hr == MF_E_SAMPLEALLOCATOR_EMPTY)
        {
            if (m_allocFailures++ % 500 == 0)
                Log("all samples in use, waiting (%d)", m_allocFailures);
            return false;
        }
        if (FAILED(hr))
        {
            Log("AllocateSample failed 0x%08X", hr);
            return true; // allocator unusable (stopping), drop the request
        }
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer)))
        {
            IMF2DBuffer2* buffer2d = nullptr;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))))
            {
                BYTE* scanline = nullptr;
                BYTE* start = nullptr;
                LONG pitch = 0;
                DWORD length = 0;
                if (SUCCEEDED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length)))
                {
                    m_reader.CopyLatest(scanline, pitch);
                    buffer2d->Unlock2D();
                }
                buffer2d->Release();
            }
            else
            {
                BYTE* data = nullptr;
                DWORD max = 0;
                if (SUCCEEDED(buffer->Lock(&data, &max, nullptr)))
                {
                    if (max >= VCAM_FRAME_BYTES)
                        m_reader.CopyLatest(data, VCAM_WIDTH);
                    buffer->Unlock();
                    buffer->SetCurrentLength(VCAM_FRAME_BYTES);
                }
            }
            buffer->Release();
        }
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetSampleDuration(10000000 / VCAM_FPS);
        if (token) sample->SetUnknown(MFSampleExtension_Token, token);
        hr = queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
        if (FAILED(hr) && hr != MF_E_SHUTDOWN)
            Log("QueueEvent(MEMediaSample) failed 0x%08X", hr);
        sample->Release();
        return true;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2))
            *ppv = static_cast<IMFMediaStream2*>(this);
        else if (riid == __uuidof(IMFAttributes))
            *ppv = static_cast<IMFAttributes*>(this);
        else if (riid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_queue ? m_queue->BeginGetEvent(cb, state) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_queue ? m_queue->EndGetEvent(result, ev) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override
    {
        IMFMediaEventQueue* queue;
        {
            std::lock_guard<std::mutex> g(m_lock);
            queue = m_queue;
            if (!queue) return MF_E_SHUTDOWN;
            queue->AddRef();
        }
        HRESULT hr = queue->GetEvent(flags, ev); // may block, so outside the lock
        queue->Release();
        return hr;
    }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_queue ? m_queue->QueueEventParamVar(met, ext, status, value) : MF_E_SHUTDOWN;
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override
    {
        if (!source) return E_POINTER;
        std::lock_guard<std::mutex> g(m_lock);
        if (!m_source) return MF_E_SHUTDOWN;
        *source = m_source;
        m_source->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** desc) override
    {
        if (!desc) return E_POINTER;
        std::lock_guard<std::mutex> g(m_lock);
        if (!m_descriptor) return MF_E_SHUTDOWN;
        *desc = m_descriptor;
        m_descriptor->AddRef();
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* token) override
    {
        {
            std::lock_guard<std::mutex> g(m_lock);
            if (!m_queue) return MF_E_SHUTDOWN;
            if (!m_running) return MF_E_INVALIDREQUEST;
            if (token) token->AddRef();
            m_tokens.push_back(token);
        }
        m_cv.notify_one();
        return S_OK;
    }

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override
    {
        Log("SetStreamState %d -> %d", m_state, value);
        if (value == m_state) return S_OK;
        switch (value)
        {
        case MF_STREAM_STATE_RUNNING: return Start(nullptr);
        case MF_STREAM_STATE_STOPPED: return Stop();
        case MF_STREAM_STATE_PAUSED:
            if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
            m_state = value;
            return S_OK;
        default: return MF_E_INVALID_STATE_TRANSITION;
        }
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override
    {
        if (!value) return E_POINTER;
        *value = m_state;
        return S_OK;
    }

    // IKsControl: no properties exposed
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
};

// ---------------------------------------------------------------------------
// Media source

struct MediaSource : AttributesImpl<IMFAttributes>, IMFMediaSourceEx, IMFGetService, IKsControl, IMFSampleAllocatorControl
{
    IMPLEMENT_REFCOUNT

    std::mutex m_lock;
    MediaStream* m_stream = nullptr;
    IMFMediaEventQueue* m_queue = nullptr;
    IMFPresentationDescriptor* m_descriptor = nullptr;

    MediaSource() { g_objects++; }
    ~MediaSource()
    {
        SafeRelease(m_stream);
        SafeRelease(m_queue);
        SafeRelease(m_descriptor);
        SafeRelease(m_attr);
        g_objects--;
    }

    HRESULT Initialize(IMFAttributes* attributes)
    {
        HRESULT hr = MFCreateAttributes(&m_attr, 8);
        if (FAILED(hr)) return hr;
        if (attributes) attributes->CopyAllItems(m_attr);

        IMFSensorProfileCollection* collection = nullptr;
        if (SUCCEEDED(MFCreateSensorProfileCollection(&collection)))
        {
            IMFSensorProfile* profile = nullptr;
            if (SUCCEEDED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile)))
            {
                profile->AddProfileFilter(0, L"((RES==;FRT<=60,1;SUT==))");
                collection->AddProfile(profile);
                profile->Release();
            }
            m_attr->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection);
            collection->Release();
        }

        m_stream = new MediaStream();
        hr = m_stream->Initialize(static_cast<IMFMediaSourceEx*>(this));
        if (FAILED(hr)) return hr;

        IMFStreamDescriptor* sd = nullptr;
        hr = m_stream->GetStreamDescriptor(&sd);
        if (FAILED(hr)) return hr;
        hr = MFCreatePresentationDescriptor(1, &sd, &m_descriptor);
        sd->Release();
        if (FAILED(hr)) return hr;
        return MFCreateEventQueue(&m_queue);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx))
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        else if (riid == __uuidof(IMFAttributes))
            *ppv = static_cast<IMFAttributes*>(this);
        else if (riid == __uuidof(IMFGetService))
            *ppv = static_cast<IMFGetService*>(this);
        else if (riid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else if (riid == __uuidof(IMFSampleAllocatorControl))
            *ppv = static_cast<IMFSampleAllocatorControl*>(this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_queue ? m_queue->BeginGetEvent(cb, state) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_queue ? m_queue->EndGetEvent(result, ev) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override
    {
        IMFMediaEventQueue* queue;
        {
            std::lock_guard<std::mutex> g(m_lock);
            queue = m_queue;
            if (!queue) return MF_E_SHUTDOWN;
            queue->AddRef();
        }
        HRESULT hr = queue->GetEvent(flags, ev);
        queue->Release();
        return hr;
    }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT status, const PROPVARIANT* value) override
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_queue ? m_queue->QueueEventParamVar(met, ext, status, value) : MF_E_SHUTDOWN;
    }

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override
    {
        if (!pd) return E_POINTER;
        std::lock_guard<std::mutex> g(m_lock);
        return m_descriptor ? m_descriptor->Clone(pd) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP GetCharacteristics(DWORD* c) override
    {
        if (!c) return E_POINTER;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override
    {
        Log("source shutdown");
        MediaStream* stream;
        {
            std::lock_guard<std::mutex> g(m_lock);
            if (!m_queue) return MF_E_SHUTDOWN;
            m_queue->Shutdown();
            SafeRelease(m_queue);
            SafeRelease(m_descriptor);
            stream = m_stream;
            m_stream = nullptr;
        }
        if (stream)
        {
            stream->Shutdown();
            stream->Release();
        }
        return S_OK;
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT*) override
    {
        Log("source start");
        if (!pd) return E_POINTER;
        if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        MediaStream* stream;
        IMFMediaEventQueue* queue;
        BOOL selected = FALSE;
        IMFMediaType* type = nullptr;
        {
            std::lock_guard<std::mutex> g(m_lock);
            if (!m_queue || !m_stream) return MF_E_SHUTDOWN;
            IMFStreamDescriptor* sd = nullptr;
            HRESULT hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
            if (FAILED(hr)) return hr;
            IMFMediaTypeHandler* handler = nullptr;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)))
            {
                handler->GetCurrentMediaType(&type);
                handler->Release();
            }
            sd->Release();
            stream = m_stream;
            queue = m_queue;
            stream->AddRef();
            queue->AddRef();
            if (selected) m_descriptor->SelectStream(0);
            else m_descriptor->DeselectStream(0);
        }

        HRESULT hr = S_OK;
        if (selected)
        {
            IUnknown* unk = static_cast<IMFMediaStream2*>(stream);
            hr = queue->QueueEventParamUnk(stream->m_running ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, unk);
            if (SUCCEEDED(hr)) hr = stream->Start(type);
        }
        else if (stream->m_running)
        {
            hr = stream->Stop();
        }
        if (SUCCEEDED(hr))
        {
            PROPVARIANT time;
            InitPropVariantFromInt64(MFGetSystemTime(), &time);
            hr = queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
            PropVariantClear(&time);
        }
        SafeRelease(type);
        stream->Release();
        queue->Release();
        return hr;
    }
    STDMETHODIMP Stop() override
    {
        Log("source stop");
        MediaStream* stream;
        IMFMediaEventQueue* queue;
        {
            std::lock_guard<std::mutex> g(m_lock);
            if (!m_queue || !m_stream) return MF_E_SHUTDOWN;
            stream = m_stream;
            queue = m_queue;
            stream->AddRef();
            queue->AddRef();
            m_descriptor->DeselectStream(0);
        }
        HRESULT hr = stream->m_running ? stream->Stop() : S_OK;
        PROPVARIANT time;
        InitPropVariantFromInt64(MFGetSystemTime(), &time);
        queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
        PropVariantClear(&time);
        stream->Release();
        queue->Release();
        return hr;
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attrs) override
    {
        if (!attrs) return E_POINTER;
        return QueryInterface(IID_PPV_ARGS(attrs));
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** attrs) override
    {
        if (!attrs) return E_POINTER;
        std::lock_guard<std::mutex> g(m_lock);
        if (!m_stream) return MF_E_SHUTDOWN;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        return m_stream->QueryInterface(IID_PPV_ARGS(attrs));
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override
    {
        // Frames are produced on the CPU; the allocator keeps handing out system memory buffers.
        return S_OK;
    }

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* ppv) override
    {
        if (ppv) *ppv = nullptr;
        return MF_E_UNSUPPORTED_SERVICE;
    }

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD id, IUnknown* allocator) override
    {
        if (!allocator) return E_POINTER;
        std::lock_guard<std::mutex> g(m_lock);
        if (!m_stream) return MF_E_SHUTDOWN;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard<std::mutex> gs(m_stream->m_lock);
        SafeRelease(m_stream->m_allocator);
        return allocator->QueryInterface(IID_PPV_ARGS(&m_stream->m_allocator));
    }
    STDMETHODIMP GetAllocatorUsage(DWORD id, DWORD* inputId, MFSampleAllocatorUsage* usage) override
    {
        if (!inputId || !usage) return E_POINTER;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        *inputId = id;
        *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
        return S_OK;
    }

    // IKsControl: no properties exposed
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
};

// ---------------------------------------------------------------------------
// Activator (what the frame server instantiates through the class factory)

struct Activator : AttributesImpl<IMFActivate>
{
    IMPLEMENT_REFCOUNT

    MediaSource* m_source = nullptr;

    Activator() { g_objects++; }
    ~Activator()
    {
        SafeRelease(m_source);
        SafeRelease(m_attr);
        g_objects--;
    }

    HRESULT Initialize()
    {
        HRESULT hr = MFCreateAttributes(&m_attr, 4);
        if (FAILED(hr)) return hr;
        m_attr->SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_VcamSource);
        return S_OK;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate))
        {
            *ppv = static_cast<IMFActivate*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override
    {
        Log("ActivateObject");
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!m_source)
        {
            m_source = new MediaSource();
            HRESULT hr = m_source->Initialize(m_attr);
            if (FAILED(hr))
            {
                Log("MediaSource::Initialize failed 0x%08X", hr);
                SafeRelease(m_source);
                return hr;
            }
        }
        return m_source->QueryInterface(riid, ppv);
    }
    STDMETHODIMP ShutdownObject() override { return S_OK; }
    STDMETHODIMP DetachObject() override
    {
        SafeRelease(m_source);
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// COM plumbing

struct ClassFactory : IClassFactory
{
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Activator* activator = new Activator();
        HRESULT hr = activator->Initialize();
        if (SUCCEEDED(hr)) hr = activator->QueryInterface(riid, ppv);
        activator->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) g_objects++;
        else g_objects--;
        return S_OK;
    }
};

static ClassFactory g_factory;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return g_objects == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_VcamSource) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

// The frame server only looks in HKEY_LOCAL_MACHINE, so registration needs admin rights.
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    HKEY key;
    LONG err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" VCAM_CLSID_STR L"\\InprocServer32",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
    RegSetValueExW(key, nullptr, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    const wchar_t model[] = L"Both";
    RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, (const BYTE*)model, sizeof(model));
    RegCloseKey(key);
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    LONG err = RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" VCAM_CLSID_STR);
    return err == ERROR_SUCCESS || err == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(err);
}
