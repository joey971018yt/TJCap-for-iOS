#include "dshow_base.h"
#include "ios_vcam/dshow_guids.h"
#include "ios_vcam/image_convert.h"
#include "ios_vcam/shared_ipc.h"

#include <chrono>
#include <thread>

namespace ios_vcam {

namespace {

enum class PixelSubtype {
    NV12,
    YUY2,
    RGB24,
};

struct VideoModeSpec {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    PixelSubtype pix;
};

// Static fallback presets exposed alongside dynamic device-native resolution
static constexpr VideoModeSpec kPresetVideoModes[] = {
    // 2K QHD Landscape (16:9)
    {2560, 1440, 60, PixelSubtype::NV12},
    {2560, 1440, 60, PixelSubtype::YUY2},
    {2560, 1440, 60, PixelSubtype::RGB24},
    // 2K QHD Portrait (9:16)
    {1440, 2560, 60, PixelSubtype::NV12},
    {1440, 2560, 60, PixelSubtype::YUY2},
    {1440, 2560, 60, PixelSubtype::RGB24},
    // 2K iPad 4:3 Landscape
    {2560, 1920, 60, PixelSubtype::NV12},
    {2560, 1920, 60, PixelSubtype::YUY2},
    {2560, 1920, 60, PixelSubtype::RGB24},
    // 1080p FHD Landscape (16:9)
    {1920, 1080, 60, PixelSubtype::NV12},
    {1920, 1080, 60, PixelSubtype::YUY2},
    {1920, 1080, 60, PixelSubtype::RGB24},
    // 1080p FHD Portrait (9:16)
    {1080, 1920, 60, PixelSubtype::NV12},
    {1080, 1920, 60, PixelSubtype::YUY2},
    {1080, 1920, 60, PixelSubtype::RGB24},
    // 1080p@30 & 720p@60 compatibility modes
    {1920, 1080, 30, PixelSubtype::NV12},
    {1920, 1080, 30, PixelSubtype::YUY2},
    {1920, 1080, 30, PixelSubtype::RGB24},
    {1280, 720, 60, PixelSubtype::NV12},
    {1280, 720, 60, PixelSubtype::YUY2},
    {1280, 720, 60, PixelSubtype::RGB24},
};

size_t CalcFrameByteSize(uint32_t w, uint32_t h, PixelSubtype pix) {
    switch (pix) {
        case PixelSubtype::NV12:
            return static_cast<size_t>(w) * h * 3ULL / 2ULL;
        case PixelSubtype::YUY2:
            return static_cast<size_t>(w) * h * 2ULL;
        case PixelSubtype::RGB24:
            return static_cast<size_t>(w) * h * 3ULL;
    }
    return static_cast<size_t>(w) * h * 2ULL;
}

// Queries live advertised device-native dimensions from ios_vcam_host.exe if active, else defaults to 2560x1440 (2K)
void QueryPreferredNativeDimensions(uint32_t& out_w, uint32_t& out_h) {
    SharedCaptureConsumer probe;
    if (probe.Open(kDefaultShmName) && probe.QueryAdvertisedDimensions(out_w, out_h)) {
        return;
    }
    out_w = 2560U;
    out_h = 1440U;
}

std::vector<VideoModeSpec> BuildAvailableModesList() {
    uint32_t dyn_w = 2560U, dyn_h = 1440U;
    QueryPreferredNativeDimensions(dyn_w, dyn_h);

    std::vector<VideoModeSpec> modes;
    modes.reserve(3 + std::size(kPresetVideoModes));
    // Dynamic slot #0..#2: exact native resolution of the currently connected iPhone / iPad!
    modes.push_back({dyn_w, dyn_h, 60, PixelSubtype::NV12});
    modes.push_back({dyn_w, dyn_h, 60, PixelSubtype::YUY2});
    modes.push_back({dyn_w, dyn_h, 60, PixelSubtype::RGB24});

    for (const auto& m : kPresetVideoModes) {
        if (m.width == dyn_w && m.height == dyn_h && m.fps == 60) {
            continue;
        }
        modes.push_back(m);
    }
    return modes;
}

void BuildVideoMediaType(const VideoModeSpec& spec, AM_MEDIA_TYPE& mt) {
    std::memset(&mt, 0, sizeof(mt));
    mt.majortype = MEDIATYPE_Video;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.formattype = FORMAT_VideoInfo;

    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    std::memset(vih, 0, sizeof(VIDEOINFOHEADER));

    REFERENCE_TIME avg_time = 10000000LL / static_cast<REFERENCE_TIME>(spec.fps);
    size_t frame_bytes = CalcFrameByteSize(spec.width, spec.height, spec.pix);

    vih->AvgTimePerFrame = avg_time;
    // Cap advertised dwBitRate inside DWORD range
    uint64_t raw_bps = frame_bytes * 8ULL * spec.fps;
    vih->dwBitRate = static_cast<DWORD>(std::min<uint64_t>(raw_bps, 0x7FFFFFFFULL));
    SetRect(&vih->rcSource, 0, 0, static_cast<int>(spec.width), static_cast<int>(spec.height));
    SetRect(&vih->rcTarget, 0, 0, static_cast<int>(spec.width), static_cast<int>(spec.height));

    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = static_cast<LONG>(spec.width);
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biSizeImage = static_cast<DWORD>(frame_bytes);

    if (spec.pix == PixelSubtype::NV12) {
        mt.subtype = MEDIASUBTYPE_NV12;
        vih->bmiHeader.biHeight = static_cast<LONG>(spec.height);
        vih->bmiHeader.biBitCount = 12;
        vih->bmiHeader.biCompression = MAKEFOURCC('N', 'V', '1', '2');
    } else if (spec.pix == PixelSubtype::YUY2) {
        mt.subtype = MEDIASUBTYPE_YUY2;
        vih->bmiHeader.biHeight = static_cast<LONG>(spec.height);
        vih->bmiHeader.biBitCount = 16;
        vih->bmiHeader.biCompression = MAKEFOURCC('Y', 'U', 'Y', '2');
    } else {
        mt.subtype = MEDIASUBTYPE_RGB24;
        vih->bmiHeader.biHeight = static_cast<LONG>(spec.height);
        vih->bmiHeader.biBitCount = 24;
        vih->bmiHeader.biCompression = BI_RGB;
    }

    mt.lSampleSize = static_cast<ULONG>(frame_bytes);
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = reinterpret_cast<BYTE*>(vih);
}

bool ExtractVideoSpecFromMediaType(const AM_MEDIA_TYPE* pmt, VideoModeSpec& out_spec) {
    if (!pmt || !IsEqualGUID(pmt->majortype, MEDIATYPE_Video) || !pmt->pbFormat) {
        return false;
    }

    PixelSubtype pix{};
    if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_NV12)) {
        pix = PixelSubtype::NV12;
    } else if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_YUY2)) {
        pix = PixelSubtype::YUY2;
    } else if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_RGB24)) {
        pix = PixelSubtype::RGB24;
    } else {
        return false;
    }

    LONG w = 0, h = 0;
    REFERENCE_TIME avg_time = 166667;
    if (IsEqualGUID(pmt->formattype, FORMAT_VideoInfo) && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
        w = vih->bmiHeader.biWidth;
        h = std::abs(vih->bmiHeader.biHeight);
        if (vih->AvgTimePerFrame > 0) avg_time = vih->AvgTimePerFrame;
    } else if (IsEqualGUID(pmt->formattype, FORMAT_VideoInfo2) && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(pmt->pbFormat);
        w = vih2->bmiHeader.biWidth;
        h = std::abs(vih2->bmiHeader.biHeight);
        if (vih2->AvgTimePerFrame > 0) avg_time = vih2->AvgTimePerFrame;
    } else {
        return false;
    }

    // Accept any 4x2-aligned native/custom resolution up to 2K+/4K (kMaxVideoPixels)
    if (w < 160 || h < 120 || w > static_cast<LONG>(kMaxFrameDimension) ||
        h > static_cast<LONG>(kMaxFrameDimension)) {
        return false;
    }
    if ((w % 4) != 0 || (h % 2) != 0) {
        return false;
    }
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxVideoPixels) {
        return false;
    }

    uint32_t fps = static_cast<uint32_t>(std::clamp<LONGLONG>((10000000LL + avg_time / 2) / avg_time, 15, 60));
    out_spec = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), fps, pix};
    return true;
}

class VideoCaptureFilter;

class VideoOutputPin final : public IPin,
                             public IAMStreamConfig,
                             public IKsPropertySet,
                             public IQualityControl {
public:
    explicit VideoOutputPin(VideoCaptureFilter* parent);
    ~VideoOutputPin();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID, DWORD, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    // IQualityControl
    STDMETHODIMP Notify(IBaseFilter*, Quality) override { return S_OK; }
    STDMETHODIMP SetSink(IQualityControl*) override { return S_OK; }

    HRESULT StartStreaming();
    HRESULT StopStreaming();

private:
    void PushLoop();
    void RefreshDynamicPreferredSpecLocked();
    HRESULT NegotiateAllocator(IMemInputPin* mem_in);

    VideoCaptureFilter* filter_ = nullptr;
    std::mutex pin_mtx_;
    IPin* connected_pin_ = nullptr;
    IMemInputPin* mem_input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;

    bool user_explicit_format_ = false;
    VideoModeSpec active_spec_{2560, 1440, 60, PixelSubtype::NV12};
    AM_MEDIA_TYPE current_mt_{};

    std::atomic<bool> streaming_{false};
    std::thread worker_;
};

class VideoCaptureFilter final : public IBaseFilter, public IAMFilterMiscFlags {
public:
    VideoCaptureFilter() : pin_(this) {
        LockModule();
    }

    ~VideoCaptureFilter() {
        pin_.StopStreaming();
        if (clock_) clock_->Release();
        UnlockModule();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPersist) ||
            IsEqualIID(riid, IID_IMediaFilter) || IsEqualIID(riid, IID_IBaseFilter)) {
            *ppv = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, IID_IAMFilterMiscFlags)) {
            *ppv = static_cast<IAMFilterMiscFlags*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    STDMETHODIMP_(ULONG) Release() override {
        long r = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    STDMETHODIMP GetClassID(CLSID* pClsID) override {
        if (!pClsID) return E_POINTER;
        *pClsID = CLSID_IosUsbVideoCapture;
        return S_OK;
    }

    STDMETHODIMP Stop() override {
        std::lock_guard<std::mutex> lk(state_mtx_);
        pin_.StopStreaming();
        state_ = State_Stopped;
        return S_OK;
    }

    STDMETHODIMP Pause() override {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (state_ == State_Stopped) {
            pin_.StartStreaming();
        }
        state_ = State_Paused;
        return S_OK;
    }

    STDMETHODIMP Run(REFERENCE_TIME tStart) override {
        std::lock_guard<std::mutex> lk(state_mtx_);
        run_start_ = tStart;
        if (state_ == State_Stopped) {
            pin_.StartStreaming();
        }
        state_ = State_Running;
        return S_OK;
    }

    STDMETHODIMP GetState(DWORD, FILTER_STATE* State) override {
        if (!State) return E_POINTER;
        std::lock_guard<std::mutex> lk(state_mtx_);
        *State = state_;
        return (state_ == State_Paused) ? VFW_S_CANT_CUE : S_OK;
    }

    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (clock_) clock_->Release();
        clock_ = pClock;
        if (clock_) clock_->AddRef();
        return S_OK;
    }

    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override {
        if (!pClock) return E_POINTER;
        std::lock_guard<std::mutex> lk(state_mtx_);
        *pClock = clock_;
        if (clock_) clock_->AddRef();
        return S_OK;
    }

    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new SinglePinEnumerator(&pin_);
        return S_OK;
    }

    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override {
        if (!Id || !ppPin) return E_POINTER;
        if (wcscmp(Id, L"Capture") == 0 || wcscmp(Id, L"Out") == 0) {
            *ppPin = static_cast<IPin*>(&pin_);
            pin_.AddRef();
            return S_OK;
        }
        *ppPin = nullptr;
        return VFW_E_NOT_FOUND;
    }

    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        wcsncpy(pInfo->achName, kVideoDeviceFriendlyName, MAX_FILTER_NAME - 1);
        pInfo->achName[MAX_FILTER_NAME - 1] = L'\0';
        pInfo->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }

    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR) override {
        std::lock_guard<std::mutex> lk(state_mtx_);
        graph_ = pGraph;
        return S_OK;
    }

    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override {
        if (!pVendorInfo) return E_POINTER;
        const wchar_t* v = L"iOS USB 2K+ Adaptive Capture Card";
        size_t bytes = (wcslen(v) + 1) * sizeof(wchar_t);
        *pVendorInfo = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!*pVendorInfo) return E_OUTOFMEMORY;
        std::memcpy(*pVendorInfo, v, bytes);
        return S_OK;
    }

    STDMETHODIMP_(ULONG) GetMiscFlags() override {
        return AM_FILTER_MISC_FLAGS_IS_SOURCE;
    }

    FILTER_STATE CurrentState() const { return state_; }
    REFERENCE_TIME ClockStreamTime() {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (clock_ && state_ == State_Running) {
            REFERENCE_TIME now = 0;
            if (SUCCEEDED(clock_->GetTime(&now))) {
                return (now >= run_start_) ? (now - run_start_) : 0;
            }
        }
        return -1;
    }

private:
    std::atomic<long> ref_count_{1};
    std::mutex state_mtx_;
    FILTER_STATE state_ = State_Stopped;
    IFilterGraph* graph_ = nullptr;
    IReferenceClock* clock_ = nullptr;
    REFERENCE_TIME run_start_ = 0;
    VideoOutputPin pin_;
};

VideoOutputPin::VideoOutputPin(VideoCaptureFilter* parent) : filter_(parent) {
    RefreshDynamicPreferredSpecLocked();
}

VideoOutputPin::~VideoOutputPin() {
    Disconnect();
    FreeMediaTypeFormat(current_mt_);
}

void VideoOutputPin::RefreshDynamicPreferredSpecLocked() {
    if (connected_pin_ || user_explicit_format_) {
        return;
    }
    uint32_t dyn_w = 2560U, dyn_h = 1440U;
    QueryPreferredNativeDimensions(dyn_w, dyn_h);
    active_spec_ = {dyn_w, dyn_h, 60, PixelSubtype::NV12};
    FreeMediaTypeFormat(current_mt_);
    BuildVideoMediaType(active_spec_, current_mt_);
}

STDMETHODIMP VideoOutputPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPin)) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IAMStreamConfig)) {
        *ppv = static_cast<IAMStreamConfig*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IKsPropertySet)) {
        *ppv = static_cast<IKsPropertySet*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IQualityControl)) {
        *ppv = static_cast<IQualityControl*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) VideoOutputPin::AddRef() { return filter_->AddRef(); }
STDMETHODIMP_(ULONG) VideoOutputPin::Release() { return filter_->Release(); }

HRESULT VideoOutputPin::NegotiateAllocator(IMemInputPin* mem_in) {
    IMemAllocator* alloc = nullptr;
    if (FAILED(mem_in->GetAllocator(&alloc)) || !alloc) {
        alloc = new FallbackMemAllocator();
    }

    ALLOCATOR_PROPERTIES req{};
    mem_in->GetAllocatorRequirements(&req);
    req.cBuffers = std::max<long>(4, req.cBuffers);
    req.cbBuffer = static_cast<long>(
        CalcFrameByteSize(active_spec_.width, active_spec_.height, active_spec_.pix));
    req.cbAlign = std::max<long>(16, req.cbAlign);
    req.cbPrefix = std::max<long>(0, req.cbPrefix);

    ALLOCATOR_PROPERTIES actual{};
    HRESULT hr = alloc->SetProperties(&req, &actual);
    if (FAILED(hr)) {
        alloc->Release();
        return hr;
    }
    hr = mem_in->NotifyAllocator(alloc, FALSE);
    if (FAILED(hr)) {
        alloc->Release();
        return hr;
    }
    if (allocator_) allocator_->Release();
    allocator_ = alloc;
    return S_OK;
}

STDMETHODIMP VideoOutputPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (connected_pin_) return VFW_E_ALREADY_CONNECTED;

    RefreshDynamicPreferredSpecLocked();

    IMemInputPin* mem_in = nullptr;
    if (FAILED(pReceivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&mem_in))) || !mem_in) {
        return VFW_E_NO_TRANSPORT;
    }

    auto try_connect_mt = [&](const AM_MEDIA_TYPE& candidate) -> bool {
        VideoModeSpec spec{};
        if (!ExtractVideoSpecFromMediaType(&candidate, spec)) {
            return false;
        }
        if (FAILED(pReceivePin->ReceiveConnection(static_cast<IPin*>(this), &candidate))) {
            return false;
        }
        active_spec_ = spec;
        FreeMediaTypeFormat(current_mt_);
        CopyMediaType(&current_mt_, &candidate);
        if (FAILED(NegotiateAllocator(mem_in))) {
            pReceivePin->Disconnect();
            return false;
        }
        return true;
    };

    auto modes = BuildAvailableModesList();
    auto try_all_modes = [&]() -> bool {
        if (try_connect_mt(current_mt_)) return true;
        for (const auto& m : modes) {
            AM_MEDIA_TYPE tmp{};
            BuildVideoMediaType(m, tmp);
            bool ok = try_connect_mt(tmp);
            FreeMediaTypeFormat(tmp);
            if (ok) return true;
        }
        return false;
    };

    bool connected = false;
    if (pmt && !IsEqualGUID(pmt->majortype, GUID_NULL)) {
        if (IsEqualGUID(pmt->subtype, GUID_NULL) || pmt->pbFormat == nullptr) {
            connected = try_all_modes();
        } else {
            connected = try_connect_mt(*pmt);
        }
    } else {
        connected = try_all_modes();
    }

    if (!connected) {
        mem_in->Release();
        return VFW_E_NO_ACCEPTABLE_TYPES;
    }

    connected_pin_ = pReceivePin;
    connected_pin_->AddRef();
    mem_input_ = mem_in;
    return S_OK;
}

STDMETHODIMP VideoOutputPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) {
    return E_UNEXPECTED;
}

STDMETHODIMP VideoOutputPin::Disconnect() {
    StopStreaming();
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (!connected_pin_) return S_FALSE;
    if (allocator_) {
        allocator_->Decommit();
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (mem_input_) {
        mem_input_->Release();
        mem_input_ = nullptr;
    }
    connected_pin_->Release();
    connected_pin_ = nullptr;
    user_explicit_format_ = false;
    return S_OK;
}

STDMETHODIMP VideoOutputPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    *pPin = connected_pin_;
    if (!connected_pin_) return VFW_E_NOT_CONNECTED;
    connected_pin_->AddRef();
    return S_OK;
}

STDMETHODIMP VideoOutputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (!connected_pin_) return VFW_E_NOT_CONNECTED;
    return CopyMediaType(pmt, &current_mt_);
}

STDMETHODIMP VideoOutputPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = static_cast<IBaseFilter*>(filter_);
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcsncpy(pInfo->achName, L"Capture", MAX_PIN_NAME - 1);
    pInfo->achName[MAX_PIN_NAME - 1] = L'\0';
    return S_OK;
}

STDMETHODIMP VideoOutputPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP VideoOutputPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const wchar_t* name = L"Capture";
    size_t sz = (wcslen(name) + 1) * sizeof(wchar_t);
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sz));
    if (!*Id) return E_OUTOFMEMORY;
    std::memcpy(*Id, name, sz);
    return S_OK;
}

STDMETHODIMP VideoOutputPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    VideoModeSpec spec{};
    return ExtractVideoSpecFromMediaType(pmt, spec) ? S_OK : S_FALSE;
}

STDMETHODIMP VideoOutputPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    RefreshDynamicPreferredSpecLocked();

    auto modes = BuildAvailableModesList();
    std::vector<AM_MEDIA_TYPE> list;
    list.reserve(1 + modes.size());
    list.push_back(current_mt_);
    for (const auto& m : modes) {
        AM_MEDIA_TYPE mt{};
        BuildVideoMediaType(m, mt);
        list.push_back(mt);
    }
    *ppEnum = new MediaTypeEnumerator(list);
    for (size_t i = 1; i < list.size(); ++i) {
        FreeMediaTypeFormat(list[i]);
    }
    return S_OK;
}

STDMETHODIMP VideoOutputPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (streaming_.load(std::memory_order_acquire)) {
        return VFW_E_WRONG_STATE;
    }
    VideoModeSpec spec{};
    if (!ExtractVideoSpecFromMediaType(pmt, spec)) {
        return VFW_E_INVALIDMEDIATYPE;
    }
    user_explicit_format_ = true;
    active_spec_ = spec;
    FreeMediaTypeFormat(current_mt_);
    BuildVideoMediaType(active_spec_, current_mt_);
    return S_OK;
}

STDMETHODIMP VideoOutputPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    RefreshDynamicPreferredSpecLocked();
    *ppmt = CloneMediaType(current_mt_);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP VideoOutputPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    auto modes = BuildAvailableModesList();
    *piCount = static_cast<int>(modes.size());
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP VideoOutputPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    auto modes = BuildAvailableModesList();
    if (iIndex < 0 || iIndex >= static_cast<int>(modes.size())) {
        return S_FALSE;
    }
    const auto& spec = modes[static_cast<size_t>(iIndex)];
    AM_MEDIA_TYPE tmp{};
    BuildVideoMediaType(spec, tmp);
    *ppmt = CloneMediaType(tmp);
    FreeMediaTypeFormat(tmp);
    if (!*ppmt) return E_OUTOFMEMORY;

    auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
    std::memset(caps, 0, sizeof(*caps));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = AnalogVideo_None;
    caps->InputSize.cx = static_cast<LONG>(spec.width);
    caps->InputSize.cy = static_cast<LONG>(spec.height);
    caps->MinCroppingSize = {160, 120};
    caps->MaxCroppingSize = {static_cast<LONG>(kMaxFrameDimension), static_cast<LONG>(kMaxFrameDimension)};
    caps->CropGranularityX = 4;
    caps->CropGranularityY = 2;
    caps->MinOutputSize = {160, 120};
    caps->MaxOutputSize = {static_cast<LONG>(kMaxFrameDimension), static_cast<LONG>(kMaxFrameDimension)};
    caps->OutputGranularityX = 4;
    caps->OutputGranularityY = 2;
    caps->MinFrameInterval = 10000000LL / 60LL;
    caps->MaxFrameInterval = 10000000LL / 15LL;
    size_t fb = CalcFrameByteSize(spec.width, spec.height, spec.pix);
    caps->MinBitsPerSecond = static_cast<LONG>(std::min<uint64_t>(fb * 8ULL * 15ULL, 0x7FFFFFFFULL));
    caps->MaxBitsPerSecond = static_cast<LONG>(std::min<uint64_t>(fb * 8ULL * 60ULL, 0x7FFFFFFFULL));
    return S_OK;
}

STDMETHODIMP VideoOutputPin::Get(
    REFGUID guidPropSet,
    DWORD dwPropID,
    LPVOID,
    DWORD,
    LPVOID pPropData,
    DWORD cbPropData,
    DWORD* pcbReturned) {
    if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pcbReturned) *pcbReturned = sizeof(GUID);
    if (!pPropData) return S_OK;
    if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
    *static_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP VideoOutputPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

HRESULT VideoOutputPin::StartStreaming() {
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (!connected_pin_ || !allocator_ || !mem_input_) {
        return S_OK;
    }
    if (streaming_.load(std::memory_order_acquire)) {
        return S_OK;
    }
    HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) return hr;

    streaming_.store(true, std::memory_order_release);
    worker_ = std::thread(&VideoOutputPin::PushLoop, this);
    return S_OK;
}

HRESULT VideoOutputPin::StopStreaming() {
    streaming_.store(false, std::memory_order_release);
    if (allocator_) {
        allocator_->Decommit();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    return S_OK;
}

void VideoOutputPin::PushLoop() {
    timeBeginPeriod(1);

    const VideoModeSpec spec = active_spec_;
    const size_t out_bytes = CalcFrameByteSize(spec.width, spec.height, spec.pix);
    const size_t target_nv12_bytes = static_cast<size_t>(spec.width) * spec.height * 3ULL / 2ULL;
    const REFERENCE_TIME frame_duration_hns = 10000000LL / static_cast<REFERENCE_TIME>(spec.fps);
    const auto frame_interval = std::chrono::nanoseconds(1000000000ULL / spec.fps);

    SharedCaptureConsumer shm_consumer;
    std::vector<uint8_t> shm_raw_nv12(kMaxNv12FrameBytes, 16);
    std::vector<uint8_t> fitted_nv12(target_nv12_bytes, 16);

    uint64_t frame_num = 0;
    auto next_deadline = std::chrono::steady_clock::now();

    while (streaming_.load(std::memory_order_relaxed)) {
        IMediaSample* sample = nullptr;
        HRESULT hr = allocator_->GetBuffer(&sample, nullptr, nullptr, 0);
        if (FAILED(hr) || !sample) {
            break;
        }

        BYTE* dst_ptr = nullptr;
        if (SUCCEEDED(sample->GetPointer(&dst_ptr)) && dst_ptr &&
            static_cast<size_t>(sample->GetSize()) >= out_bytes) {
            if (!shm_consumer.IsOpen()) {
                shm_consumer.Open(kDefaultShmName);
            }

            uint32_t shm_w = 0, shm_h = 0;
            uint64_t seq = 0, pts_ns = 0;
            bool got_live = shm_consumer.ReadLatestNV12Frame(
                shm_raw_nv12.data(), shm_raw_nv12.size(), shm_w, shm_h, seq, pts_ns);
            const uint8_t* active_nv12 = nullptr;

            if (got_live && shm_w > 0 && shm_h > 0) {
                if (shm_w == spec.width && shm_h == spec.height) {
                    // 1:1 native pixel-for-pixel zero-black-bar output!
                    active_nv12 = shm_raw_nv12.data();
                } else {
                    // Aspect-ratio preserving fit if downstream pin locked onto a different resolution or rotated mid-stream
                    ResizeNV12Fast(shm_raw_nv12.data(), shm_w, shm_h, fitted_nv12.data(), spec.width, spec.height);
                    active_nv12 = fitted_nv12.data();
                }
            } else {
                GenerateAnimatedColorBarsNV12(fitted_nv12.data(), spec.width, spec.height, frame_num);
                active_nv12 = fitted_nv12.data();
            }

            if (spec.pix == PixelSubtype::NV12) {
                std::memcpy(dst_ptr, active_nv12, out_bytes);
            } else if (spec.pix == PixelSubtype::YUY2) {
                ConvertNV12ToYUY2(active_nv12, spec.width, spec.height, dst_ptr);
            } else {
                ConvertNV12ToRGB24BottomUp(active_nv12, spec.width, spec.height, dst_ptr);
            }

            sample->SetActualDataLength(static_cast<LONG>(out_bytes));
            REFERENCE_TIME clk_now = filter_->ClockStreamTime();
            REFERENCE_TIME start_t = (clk_now >= 0)
                                         ? clk_now
                                         : static_cast<REFERENCE_TIME>(frame_num) * frame_duration_hns;
            REFERENCE_TIME end_t = start_t + frame_duration_hns;
            sample->SetTime(&start_t, &end_t);
            sample->SetSyncPoint(TRUE);
            sample->SetDiscontinuity(frame_num == 0 ? TRUE : FALSE);

            hr = mem_input_->Receive(sample);
        }

        sample->Release();
        if (hr != S_OK && filter_->CurrentState() == State_Running) {
            if (FAILED(hr)) break;
        }

        ++frame_num;
        next_deadline += frame_interval;
        auto now = std::chrono::steady_clock::now();
        if (next_deadline < now) {
            next_deadline = now;
        } else {
            std::this_thread::sleep_until(next_deadline);
        }
    }

    timeEndPeriod(1);
}

}  // namespace

HRESULT CreateVideoCaptureFilter(REFIID riid, void** ppv) {
    auto* filter = new VideoCaptureFilter();
    HRESULT hr = filter->QueryInterface(riid, ppv);
    filter->Release();
    return hr;
}

}  // namespace ios_vcam
