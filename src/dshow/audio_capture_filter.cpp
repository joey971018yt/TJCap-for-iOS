#include "dshow_base.h"
#include "ios_vcam/dshow_guids.h"
#include "ios_vcam/shared_ipc.h"

#include <chrono>
#include <thread>

namespace ios_vcam {

namespace {

void BuildPcm48kMediaType(AM_MEDIA_TYPE& mt) {
    std::memset(&mt, 0, sizeof(mt));
    mt.majortype = MEDIATYPE_Audio;
    mt.subtype = MEDIASUBTYPE_PCM;
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = kAudioBytesPerFrame;  // 4 bytes (16-bit stereo)
    mt.formattype = FORMAT_WaveFormatEx;

    auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    std::memset(wfx, 0, sizeof(WAVEFORMATEX));
    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->nChannels = static_cast<WORD>(kAudioChannels);
    wfx->nSamplesPerSec = kAudioSampleRate;
    wfx->wBitsPerSample = 16;
    wfx->nBlockAlign = static_cast<WORD>(kAudioBytesPerFrame);
    wfx->nAvgBytesPerSec = kAudioSampleRate * kAudioBytesPerFrame;

    mt.cbFormat = sizeof(WAVEFORMATEX);
    mt.pbFormat = reinterpret_cast<BYTE*>(wfx);
}

bool IsAcceptablePcm48kType(const AM_MEDIA_TYPE* pmt) {
    if (!pmt || !IsEqualGUID(pmt->majortype, MEDIATYPE_Audio)) return false;
    if (!IsEqualGUID(pmt->subtype, MEDIASUBTYPE_PCM) && !IsEqualGUID(pmt->subtype, GUID_NULL)) {
        return false;
    }
    if (!IsEqualGUID(pmt->formattype, FORMAT_WaveFormatEx) ||
        pmt->cbFormat < sizeof(WAVEFORMATEX) || !pmt->pbFormat) {
        return false;
    }
    const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(pmt->pbFormat);
    return (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->nChannels == 2 &&
            wfx->nSamplesPerSec == 48000 && wfx->wBitsPerSample == 16);
}

class AudioCaptureFilter;

class AudioOutputPin final : public IPin,
                             public IAMStreamConfig,
                             public IAMBufferNegotiation,
                             public IKsPropertySet,
                             public IQualityControl {
public:
    explicit AudioOutputPin(AudioCaptureFilter* parent);
    ~AudioOutputPin();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
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

    // IAMBufferNegotiation (10ms low-latency 480-frame buffer default)
    STDMETHODIMP SuggestAllocatorProperties(const ALLOCATOR_PROPERTIES* pprop) override;
    STDMETHODIMP GetAllocatorProperties(ALLOCATOR_PROPERTIES* pprop) override;

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
    void AudioPushLoop();
    HRESULT NegotiateAllocator(IMemInputPin* mem_in);

    AudioCaptureFilter* filter_ = nullptr;
    std::mutex pin_mtx_;
    IPin* connected_pin_ = nullptr;
    IMemInputPin* mem_input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;

    AM_MEDIA_TYPE pcm_mt_{};
    // Default 10ms buffer: 480 stereo frames * 4 bytes = 1920 bytes
    ALLOCATOR_PROPERTIES suggested_props_{8, 480 * 4, 4, 0};
    ALLOCATOR_PROPERTIES negotiated_props_{8, 480 * 4, 4, 0};

    std::atomic<bool> streaming_{false};
    std::thread worker_;
};

class AudioCaptureFilter final : public IBaseFilter, public IAMFilterMiscFlags {
public:
    AudioCaptureFilter() : pin_(this) { LockModule(); }
    ~AudioCaptureFilter() {
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
        *pClsID = CLSID_IosUsbAudioCapture;
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
        if (state_ == State_Stopped) pin_.StartStreaming();
        state_ = State_Paused;
        return S_OK;
    }

    STDMETHODIMP Run(REFERENCE_TIME tStart) override {
        std::lock_guard<std::mutex> lk(state_mtx_);
        run_start_ = tStart;
        if (state_ == State_Stopped) pin_.StartStreaming();
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
        wcsncpy(pInfo->achName, kAudioDeviceFriendlyName, MAX_FILTER_NAME - 1);
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
        const wchar_t* v = L"iOS USB Universal Capture Card (Audio)";
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
    AudioOutputPin pin_;
};

AudioOutputPin::AudioOutputPin(AudioCaptureFilter* parent) : filter_(parent) {
    BuildPcm48kMediaType(pcm_mt_);
}

AudioOutputPin::~AudioOutputPin() {
    Disconnect();
    FreeMediaTypeFormat(pcm_mt_);
}

STDMETHODIMP AudioOutputPin::QueryInterface(REFIID riid, void** ppv) {
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
    if (IsEqualIID(riid, IID_IAMBufferNegotiation)) {
        *ppv = static_cast<IAMBufferNegotiation*>(this);
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

STDMETHODIMP_(ULONG) AudioOutputPin::AddRef() { return filter_->AddRef(); }
STDMETHODIMP_(ULONG) AudioOutputPin::Release() { return filter_->Release(); }

HRESULT AudioOutputPin::NegotiateAllocator(IMemInputPin* mem_in) {
    IMemAllocator* alloc = nullptr;
    if (FAILED(mem_in->GetAllocator(&alloc)) || !alloc) {
        alloc = new FallbackMemAllocator();
    }

    ALLOCATOR_PROPERTIES req = suggested_props_;
    // Clamp buffer size to a clean multiple of 4 bytes between 5ms (960B) and 50ms (9600B)
    long cb = std::clamp<long>(req.cbBuffer, 960, 9600);
    cb = (cb / 4) * 4;
    req.cbBuffer = cb;
    req.cBuffers = std::max<long>(6, req.cBuffers);
    req.cbAlign = std::max<long>(4, req.cbAlign);
    req.cbPrefix = std::max<long>(0, req.cbPrefix);

    HRESULT hr = alloc->SetProperties(&req, &negotiated_props_);
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

STDMETHODIMP AudioOutputPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (connected_pin_) return VFW_E_ALREADY_CONNECTED;

    if (pmt && !IsEqualGUID(pmt->majortype, GUID_NULL) && pmt->pbFormat != nullptr) {
        if (!IsAcceptablePcm48kType(pmt)) {
            return VFW_E_NO_ACCEPTABLE_TYPES;
        }
    }

    IMemInputPin* mem_in = nullptr;
    if (FAILED(pReceivePin->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&mem_in))) || !mem_in) {
        return VFW_E_NO_TRANSPORT;
    }

    if (FAILED(pReceivePin->ReceiveConnection(static_cast<IPin*>(this), &pcm_mt_))) {
        mem_in->Release();
        return VFW_E_NO_ACCEPTABLE_TYPES;
    }

    if (FAILED(NegotiateAllocator(mem_in))) {
        pReceivePin->Disconnect();
        mem_in->Release();
        return VFW_E_NO_TRANSPORT;
    }

    connected_pin_ = pReceivePin;
    connected_pin_->AddRef();
    mem_input_ = mem_in;
    return S_OK;
}

STDMETHODIMP AudioOutputPin::Disconnect() {
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

STDMETHODIMP AudioOutputPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    *pPin = connected_pin_;
    if (!connected_pin_) return VFW_E_NOT_CONNECTED;
    connected_pin_->AddRef();
    return S_OK;
}

STDMETHODIMP AudioOutputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (!connected_pin_) return VFW_E_NOT_CONNECTED;
    return CopyMediaType(pmt, &pcm_mt_);
}

STDMETHODIMP AudioOutputPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = static_cast<IBaseFilter*>(filter_);
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcsncpy(pInfo->achName, L"Capture", MAX_PIN_NAME - 1);
    pInfo->achName[MAX_PIN_NAME - 1] = L'\0';
    return S_OK;
}

STDMETHODIMP AudioOutputPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP AudioOutputPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    const wchar_t* name = L"Capture";
    size_t sz = (wcslen(name) + 1) * sizeof(wchar_t);
    *Id = static_cast<LPWSTR>(CoTaskMemAlloc(sz));
    if (!*Id) return E_OUTOFMEMORY;
    std::memcpy(*Id, name, sz);
    return S_OK;
}

STDMETHODIMP AudioOutputPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    return IsAcceptablePcm48kType(pmt) ? S_OK : S_FALSE;
}

STDMETHODIMP AudioOutputPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::vector<AM_MEDIA_TYPE> list = {pcm_mt_};
    *ppEnum = new MediaTypeEnumerator(list);
    return S_OK;
}

STDMETHODIMP AudioOutputPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    return IsAcceptablePcm48kType(pmt) ? S_OK : VFW_E_INVALIDMEDIATYPE;
}

STDMETHODIMP AudioOutputPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = CloneMediaType(pcm_mt_);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP AudioOutputPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = 1;
    *piSize = sizeof(AUDIO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP AudioOutputPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex != 0) return S_FALSE;
    *ppmt = CloneMediaType(pcm_mt_);
    if (!*ppmt) return E_OUTOFMEMORY;

    auto* caps = reinterpret_cast<AUDIO_STREAM_CONFIG_CAPS*>(pSCC);
    std::memset(caps, 0, sizeof(*caps));
    caps->guid = MEDIATYPE_Audio;
    caps->MinimumChannels = 2;
    caps->MaximumChannels = 2;
    caps->ChannelsGranularity = 1;
    caps->MinimumBitsPerSample = 16;
    caps->MaximumBitsPerSample = 16;
    caps->BitsPerSampleGranularity = 16;
    caps->MinimumSampleFrequency = 48000;
    caps->MaximumSampleFrequency = 48000;
    caps->SampleFrequencyGranularity = 48000;
    return S_OK;
}

STDMETHODIMP AudioOutputPin::SuggestAllocatorProperties(const ALLOCATOR_PROPERTIES* pprop) {
    if (!pprop) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (connected_pin_) return VFW_E_ALREADY_CONNECTED;
    if (pprop->cbBuffer > 0) suggested_props_.cbBuffer = pprop->cbBuffer;
    if (pprop->cBuffers > 0) suggested_props_.cBuffers = pprop->cBuffers;
    return S_OK;
}

STDMETHODIMP AudioOutputPin::GetAllocatorProperties(ALLOCATOR_PROPERTIES* pprop) {
    if (!pprop) return E_POINTER;
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (!connected_pin_) return VFW_E_NOT_CONNECTED;
    *pprop = negotiated_props_;
    return S_OK;
}

STDMETHODIMP AudioOutputPin::Get(
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

STDMETHODIMP AudioOutputPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

HRESULT AudioOutputPin::StartStreaming() {
    std::lock_guard<std::mutex> lk(pin_mtx_);
    if (!connected_pin_ || !allocator_ || !mem_input_) return S_OK;
    if (streaming_.load(std::memory_order_acquire)) return S_OK;

    HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) return hr;

    streaming_.store(true, std::memory_order_release);
    worker_ = std::thread(&AudioOutputPin::AudioPushLoop, this);
    return S_OK;
}

HRESULT AudioOutputPin::StopStreaming() {
    streaming_.store(false, std::memory_order_release);
    if (allocator_) allocator_->Decommit();
    if (worker_.joinable()) worker_.join();
    return S_OK;
}

void AudioOutputPin::AudioPushLoop() {
    timeBeginPeriod(1);

    const size_t chunk_frames = std::max<size_t>(
        240, static_cast<size_t>(negotiated_props_.cbBuffer) / kAudioBytesPerFrame);
    const size_t chunk_bytes = chunk_frames * kAudioBytesPerFrame;
    const REFERENCE_TIME chunk_hns =
        static_cast<REFERENCE_TIME>((chunk_frames * 10000000ULL) / kAudioSampleRate);
    const auto chunk_interval =
        std::chrono::nanoseconds((chunk_frames * 1000000000ULL) / kAudioSampleRate);

    SharedCaptureConsumer shm_consumer;
    uint64_t packet_index = 0;
    auto next_deadline = std::chrono::steady_clock::now();

    while (streaming_.load(std::memory_order_relaxed)) {
        IMediaSample* sample = nullptr;
        HRESULT hr = allocator_->GetBuffer(&sample, nullptr, nullptr, 0);
        if (FAILED(hr) || !sample) {
            break;
        }

        BYTE* dst = nullptr;
        if (SUCCEEDED(sample->GetPointer(&dst)) && dst) {
            size_t cap_frames = std::min<size_t>(
                chunk_frames, static_cast<size_t>(sample->GetSize()) / kAudioBytesPerFrame);
            size_t actual_bytes = cap_frames * kAudioBytesPerFrame;

            if (!shm_consumer.IsOpen()) {
                shm_consumer.Open(kDefaultShmName);
            }
            shm_consumer.ReadAudioPcmS16LE(reinterpret_cast<int16_t*>(dst), cap_frames);

            sample->SetActualDataLength(static_cast<LONG>(actual_bytes));
            REFERENCE_TIME clk_now = filter_->ClockStreamTime();
            REFERENCE_TIME start_t = (clk_now >= 0)
                                         ? clk_now
                                         : static_cast<REFERENCE_TIME>(packet_index) * chunk_hns;
            REFERENCE_TIME end_t = start_t + chunk_hns;
            sample->SetTime(&start_t, &end_t);
            sample->SetSyncPoint(TRUE);
            sample->SetDiscontinuity(packet_index == 0 ? TRUE : FALSE);

            hr = mem_input_->Receive(sample);
        }

        sample->Release();
        if (hr != S_OK && filter_->CurrentState() == State_Running && FAILED(hr)) {
            break;
        }

        ++packet_index;
        next_deadline += chunk_interval;
        auto now = std::chrono::steady_clock::now();
        if (next_deadline < now) {
            next_deadline = now;
        } else {
            std::this_thread::sleep_until(next_deadline);
        }
        (void)chunk_bytes;
    }

    timeEndPeriod(1);
}

}  // namespace

HRESULT CreateAudioCaptureFilter(REFIID riid, void** ppv) {
    auto* filter = new AudioCaptureFilter();
    HRESULT hr = filter->QueryInterface(riid, ppv);
    filter->Release();
    return hr;
}

}  // namespace ios_vcam
