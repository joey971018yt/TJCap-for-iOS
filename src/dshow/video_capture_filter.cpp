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

static constexpr VideoModeSpec kSupportedVideoModes[] = {
    {1920, 1080, 60, PixelSubtype::NV12},
    {1920, 1080, 60, PixelSubtype::YUY2},
    {1920, 1080, 60, PixelSubtype::RGB24},
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
            return static_cast<size_t>(w) * h * 3U / 2U;
        case PixelSubtype::YUY2:
            return static_cast<size_t>(w) * h * 2U;
        case PixelSubtype::RGB24:
            return static_cast<size_t>(w) * h * 3U;
    }
    return static_cast<size_t>(w) * h * 2U;
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
    vih->dwBitRate = static_cast<DWORD>(frame_bytes * 8ULL * spec.fps);
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
        vih->bmiHeader.biHeight = static_cast<LONG>(spec.height);  // Positive = standard bottom-up DIB
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

    if (!((w == 1920 && h == 1080) || (w == 1280 && h == 720))) {
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

    // IUnknown (delegates lifetime to parent filter while pin is embedded)
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
    HRESULT NegotiateAllocator(IMemInputPin* mem_in);

    VideoCaptureFilter* filter_ = nullptr;
    std::mutex pin_mtx_;
    IPin* connected_pin_ = nullptr;
    IMemInputPin* mem_input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;

    VideoModeSpec active_spec_{1920, 1080, 60, PixelSubtype::NV12};
    AM_MEDIA_TYPE current_mt_{};
    std::vector<AM_MEDIA_TYPE> supported_mts_;

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

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClsID) override {
        if (!pClsID) return E_POINTER;
        *pClsID = CLSID_IosUsbVideoCapture;
        return S_OK;
    }

    // IMediaFilter
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

    // IBaseFilter
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
        graph_ = pGraph;  // Weak reference per DirectShow rules
        return S_OK;
    }

    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override {
        if (!pVendorInfo) return E_POINTER;
        const wchar_t* v = L"iOS USB Universal Capture Card";
        size_t bytes = (wcslen(v) + 1) * sizeof(wchar_t);
        *pVendorInfo = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!*pVendorInfo) return E_OUTOFMEMORY;
        std::memcpy(*pVendorInfo, v, bytes);
        return S_OK;
    }

    // IAMFilterMiscFlags
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
    for (const auto& spec : kSupportedVideoModes) {
        AM_MEDIA_TYPE mt{};
        BuildVideoMediaType(spec, mt);
        supported_mts_.push_back(mt);
    }
    BuildVideoMediaType(active_spec_, current_mt_);
}

VideoOutputPin::~VideoOutputPin() {
    Disconnect();
    FreeMediaTypeFormat(current_mt_);
    for (auto& mt : supported_mts_) {
        FreeMediaTypeFormat(mt);
    }
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

    bool connected = false;
    if (pmt && !IsEqualGUID(pmt->majortype, GUID_NULL)) {
        if (IsEqualGUID(pmt->subtype, GUID_NULL) || pmt->pbFormat == nullptr) {
            if (try_connect_mt(current_mt_)) {
                connected = true;
            } else {
                for (const auto& m : supported_mts_) {
                    if (try_connect_mt(m)) {
                        connected = true;
                        break;
                    }
                }
            }
        } else {
            connected = try_connect_mt(*pmt);
        }
    } else {
        if (try_connect_mt(current_mt_)) {
            connected = true;
        } else {
            for (const auto& m : supported_mts_) {
                if (try_connect_mt(m)) {
                    connected = true;
                    break;
                }
            }
        }
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
    return E_UNEXPECTED;  // Output pin
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
    std::vector<AM_MEDIA_TYPE> list;
    list.reserve(1 + supported_mts_.size());
    list.push_back(current_mt_);
    for (const auto& mt : supported_mts_) {
        list.push_back(mt);
    }
    *ppEnum = new MediaTypeEnumerator(list);
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
    active_spec_ = spec;
    FreeMediaTypeFormat(current_mt_);
    BuildVideoMediaType(active_spec_, current_mt_);
    return S_OK;
}

STDMETHODIMP VideoOutputPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    *ppmt = CloneMediaType(current_mt_);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP VideoOutputPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = static_cast<int>(std::size(kSupportedVideoModes));
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP VideoOutputPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || iIndex >= static_cast<int>(std::size(kSupportedVideoModes))) {
        return S_FALSE;
    }
    const auto& spec = kSupportedVideoModes[iIndex];
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
    caps->MinCroppingSize = caps->InputSize;
    caps->MaxCroppingSize = caps->InputSize;
    caps->CropGranularityX = 2;
    caps->CropGranularityY = 2;
    caps->MinOutputSize = caps->InputSize;
    caps->MaxOutputSize = caps->InputSize;
    caps->OutputGranularityX = 2;
    caps->OutputGranularityY = 2;
    caps->MinFrameInterval = 10000000LL / 60LL;
    caps->MaxFrameInterval = 10000000LL / 15LL;
    size_t fb = CalcFrameByteSize(spec.width, spec.height, spec.pix);
    caps->MinBitsPerSecond = static_cast<LONG>(fb * 8ULL * 15ULL);
    caps->MaxBitsPerSecond = static_cast<LONG>(fb * 8ULL * 60ULL);
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
    const REFERENCE_TIME frame_duration_hns = 10000000LL / static_cast<REFERENCE_TIME>(spec.fps);
    const auto frame_interval = std::chrono::nanoseconds(1000000000ULL / spec.fps);

    SharedCaptureConsumer shm_consumer;
    std::vector<uint8_t> shm_1080p_nv12(kNv12FrameBytes, 16);
    std::vector<uint8_t> scaled_nv12;
    if (spec.width != kCanvasWidth || spec.height != kCanvasHeight) {
        scaled_nv12.resize(static_cast<size_t>(spec.width) * spec.height * 3U / 2U);
    }

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

            uint64_t seq = 0, pts_ns = 0;
            bool got_live = shm_consumer.ReadLatestNV12Frame(shm_1080p_nv12.data(), seq, pts_ns);
            const uint8_t* active_nv12 = nullptr;

            if (got_live) {
                if (spec.width == kCanvasWidth && spec.height == kCanvasHeight) {
                    active_nv12 = shm_1080p_nv12.data();
                } else {
                    ResizeNV12Fast(
                        shm_1080p_nv12.data(), kCanvasWidth, kCanvasHeight,
                        scaled_nv12.data(), spec.width, spec.height);
                    active_nv12 = scaled_nv12.data();
                }
            } else {
                // Standby animated HD color bars when host daemon is offline
                uint8_t* bar_buf = (spec.width == kCanvasWidth && spec.height == kCanvasHeight)
                                       ? shm_1080p_nv12.data()
                                       : scaled_nv12.data();
                GenerateAnimatedColorBarsNV12(bar_buf, spec.width, spec.height, frame_num);
                active_nv12 = bar_buf;
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
            // Downstream stopped accepting samples
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
