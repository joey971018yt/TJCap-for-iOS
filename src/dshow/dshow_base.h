#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <strmif.h>
#include <amvideo.h>
#include <dvdmedia.h>
#include <uuids.h>
#include <vfwmsgs.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace ios_vcam {

extern std::atomic<long> g_dll_obj_count;
extern HMODULE g_dll_module;

inline void LockModule() {
    g_dll_obj_count.fetch_add(1, std::memory_order_relaxed);
}

inline void UnlockModule() {
    g_dll_obj_count.fetch_sub(1, std::memory_order_relaxed);
}

inline void FreeMediaTypeFormat(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0 && mt.pbFormat != nullptr) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

inline void DeleteMediaType(AM_MEDIA_TYPE* pmt) {
    if (pmt != nullptr) {
        FreeMediaTypeFormat(*pmt);
        CoTaskMemFree(pmt);
    }
}

inline HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
    if (!dst || !src) {
        return E_POINTER;
    }
    if (dst == src) {
        return S_OK;
    }
    *dst = *src;
    dst->pUnk = nullptr;
    if (src->cbFormat > 0 && src->pbFormat != nullptr) {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
        if (!dst->pbFormat) {
            dst->cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        std::memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    } else {
        dst->cbFormat = 0;
        dst->pbFormat = nullptr;
    }
    if (src->pUnk != nullptr) {
        dst->pUnk = src->pUnk;
        dst->pUnk->AddRef();
    }
    return S_OK;
}

inline AM_MEDIA_TYPE* CloneMediaType(const AM_MEDIA_TYPE& src) {
    auto* pmt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!pmt) {
        return nullptr;
    }
    std::memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
    if (FAILED(CopyMediaType(pmt, &src))) {
        CoTaskMemFree(pmt);
        return nullptr;
    }
    return pmt;
}

// Generic IEnumMediaTypes implementation
class MediaTypeEnumerator final : public IEnumMediaTypes {
public:
    explicit MediaTypeEnumerator(const std::vector<AM_MEDIA_TYPE>& types, size_t cursor = 0)
        : cursor_(cursor) {
        LockModule();
        types_.resize(types.size());
        for (size_t i = 0; i < types.size(); ++i) {
            std::memset(&types_[i], 0, sizeof(AM_MEDIA_TYPE));
            CopyMediaType(&types_[i], &types[i]);
        }
    }

    ~MediaTypeEnumerator() {
        for (auto& t : types_) {
            FreeMediaTypeFormat(t);
        }
        UnlockModule();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumMediaTypes)) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
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
        if (r == 0) {
            delete this;
        }
        return static_cast<ULONG>(r);
    }

    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        if (cMediaTypes > 1 && !pcFetched) return E_INVALIDARG;

        ULONG fetched = 0;
        while (fetched < cMediaTypes && cursor_ < types_.size()) {
            AM_MEDIA_TYPE* cloned = CloneMediaType(types_[cursor_]);
            if (!cloned) {
                for (ULONG i = 0; i < fetched; ++i) DeleteMediaType(ppMediaTypes[i]);
                return E_OUTOFMEMORY;
            }
            ppMediaTypes[fetched++] = cloned;
            ++cursor_;
        }
        if (pcFetched) {
            *pcFetched = fetched;
        }
        return (fetched == cMediaTypes) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        cursor_ = std::min(types_.size(), cursor_ + static_cast<size_t>(cMediaTypes));
        return (cursor_ <= types_.size()) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        cursor_ = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new MediaTypeEnumerator(types_, cursor_);
        return S_OK;
    }

private:
    std::atomic<long> ref_count_{1};
    std::vector<AM_MEDIA_TYPE> types_;
    size_t cursor_ = 0;
};

// Single-pin IEnumPins implementation for single-output capture filters
class SinglePinEnumerator final : public IEnumPins {
public:
    explicit SinglePinEnumerator(IPin* pin, size_t pos = 0) : pin_(pin), pos_(pos) {
        LockModule();
        if (pin_) pin_->AddRef();
    }

    ~SinglePinEnumerator() {
        if (pin_) pin_->Release();
        UnlockModule();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumPins)) {
            *ppv = static_cast<IEnumPins*>(this);
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

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (cPins > 1 && !pcFetched) return E_INVALIDARG;

        ULONG fetched = 0;
        if (cPins >= 1 && pos_ == 0 && pin_) {
            ppPins[0] = pin_;
            pin_->AddRef();
            fetched = 1;
            pos_ = 1;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == cPins) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(ULONG cPins) override {
        pos_ += cPins;
        return (pos_ <= 1) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        pos_ = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new SinglePinEnumerator(pin_, pos_);
        return S_OK;
    }

private:
    std::atomic<long> ref_count_{1};
    IPin* pin_ = nullptr;
    size_t pos_ = 0;
};

// Self-contained IMemAllocator + IMediaSample fallback when downstream filter does not supply one
class FallbackMemAllocator final : public IMemAllocator {
public:
    class Sample final : public IMediaSample {
    public:
        Sample(FallbackMemAllocator* owner, size_t size)
            : owner_(owner), buffer_(size, 0), actual_len_(static_cast<LONG>(size)) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IMediaSample)) {
                *ppv = static_cast<IMediaSample*>(this);
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
            if (r == 0) {
                owner_->ReturnSample(this);
            }
            return static_cast<ULONG>(r);
        }

        STDMETHODIMP GetPointer(BYTE** ppBuffer) override {
            if (!ppBuffer) return E_POINTER;
            *ppBuffer = buffer_.data();
            return S_OK;
        }
        STDMETHODIMP_(LONG) GetSize() override { return static_cast<LONG>(buffer_.size()); }
        STDMETHODIMP GetTime(REFERENCE_TIME* pStart, REFERENCE_TIME* pEnd) override {
            if (!pStart || !pEnd) return E_POINTER;
            if (!has_time_) return VFW_E_SAMPLE_TIME_NOT_SET;
            *pStart = start_time_;
            *pEnd = end_time_;
            return S_OK;
        }
        STDMETHODIMP SetTime(REFERENCE_TIME* pStart, REFERENCE_TIME* pEnd) override {
            if (!pStart) {
                has_time_ = false;
                return S_OK;
            }
            start_time_ = *pStart;
            end_time_ = pEnd ? *pEnd : (*pStart + 1);
            has_time_ = true;
            return S_OK;
        }
        STDMETHODIMP IsSyncPoint() override { return sync_point_ ? S_OK : S_FALSE; }
        STDMETHODIMP SetSyncPoint(BOOL bIsSyncPoint) override {
            sync_point_ = (bIsSyncPoint != FALSE);
            return S_OK;
        }
        STDMETHODIMP IsPreroll() override { return preroll_ ? S_OK : S_FALSE; }
        STDMETHODIMP SetPreroll(BOOL bIsPreroll) override {
            preroll_ = (bIsPreroll != FALSE);
            return S_OK;
        }
        STDMETHODIMP_(LONG) GetActualDataLength() override { return actual_len_; }
        STDMETHODIMP SetActualDataLength(LONG len) override {
            if (len < 0 || static_cast<size_t>(len) > buffer_.size()) return VFW_E_BUFFER_OVERFLOW;
            actual_len_ = len;
            return S_OK;
        }
        STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** ppMediaType) override {
            if (!ppMediaType) return E_POINTER;
            *ppMediaType = nullptr;
            return S_FALSE;
        }
        STDMETHODIMP SetMediaType(AM_MEDIA_TYPE*) override { return S_OK; }
        STDMETHODIMP IsDiscontinuity() override { return discont_ ? S_OK : S_FALSE; }
        STDMETHODIMP SetDiscontinuity(BOOL bDiscont) override {
            discont_ = (bDiscont != FALSE);
            return S_OK;
        }
        STDMETHODIMP GetMediaTime(LONGLONG*, LONGLONG*) override { return VFW_E_MEDIA_TIME_NOT_SET; }
        STDMETHODIMP SetMediaTime(LONGLONG*, LONGLONG*) override { return S_OK; }

        void PrepareForCheckout() {
            ref_count_.store(1, std::memory_order_relaxed);
            actual_len_ = static_cast<LONG>(buffer_.size());
            has_time_ = false;
            sync_point_ = true;
            preroll_ = false;
            discont_ = false;
        }

    private:
        FallbackMemAllocator* owner_ = nullptr;
        std::atomic<long> ref_count_{0};
        std::vector<BYTE> buffer_;
        LONG actual_len_ = 0;
        REFERENCE_TIME start_time_ = 0;
        REFERENCE_TIME end_time_ = 0;
        bool has_time_ = false;
        bool sync_point_ = true;
        bool preroll_ = false;
        bool discont_ = false;
    };

    FallbackMemAllocator() { LockModule(); }
    ~FallbackMemAllocator() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto* s : all_samples_) delete s;
        UnlockModule();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IMemAllocator)) {
            *ppv = static_cast<IMemAllocator*>(this);
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

    STDMETHODIMP SetProperties(ALLOCATOR_PROPERTIES* pRequest, ALLOCATOR_PROPERTIES* pActual) override {
        if (!pRequest || !pActual) return E_POINTER;
        std::lock_guard<std::mutex> lk(mtx_);
        if (committed_) return VFW_E_ALREADY_COMMITTED;
        props_.cBuffers = std::max<long>(2, pRequest->cBuffers);
        props_.cbBuffer = std::max<long>(64, pRequest->cbBuffer);
        props_.cbAlign = std::max<long>(1, pRequest->cbAlign);
        props_.cbPrefix = std::max<long>(0, pRequest->cbPrefix);
        *pActual = props_;
        return S_OK;
    }

    STDMETHODIMP GetProperties(ALLOCATOR_PROPERTIES* pProps) override {
        if (!pProps) return E_POINTER;
        std::lock_guard<std::mutex> lk(mtx_);
        *pProps = props_;
        return S_OK;
    }

    STDMETHODIMP Commit() override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (committed_) return S_OK;
        if (all_samples_.empty()) {
            for (long i = 0; i < props_.cBuffers; ++i) {
                auto* s = new Sample(this, static_cast<size_t>(props_.cbBuffer));
                all_samples_.push_back(s);
                free_samples_.push_back(s);
            }
        }
        committed_ = true;
        return S_OK;
    }

    STDMETHODIMP Decommit() override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            committed_ = false;
        }
        cv_.notify_all();
        return S_OK;
    }

    STDMETHODIMP GetBuffer(IMediaSample** ppBuffer, REFERENCE_TIME*, REFERENCE_TIME*, DWORD dwFlags) override {
        if (!ppBuffer) return E_POINTER;
        *ppBuffer = nullptr;
        std::unique_lock<std::mutex> lk(mtx_);
        while (committed_ && free_samples_.empty()) {
            if (dwFlags & AM_GBF_NOWAIT) {
                return VFW_E_TIMEOUT;
            }
            cv_.wait(lk);
        }
        if (!committed_) {
            return VFW_E_NOT_COMMITTED;
        }
        Sample* s = free_samples_.back();
        free_samples_.pop_back();
        s->PrepareForCheckout();
        *ppBuffer = s;
        return S_OK;
    }

    STDMETHODIMP ReleaseBuffer(IMediaSample*) override { return S_OK; }

    void ReturnSample(Sample* s) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            free_samples_.push_back(s);
        }
        cv_.notify_one();
    }

private:
    std::atomic<long> ref_count_{1};
    std::mutex mtx_;
    std::condition_variable cv_;
    ALLOCATOR_PROPERTIES props_{4, 1920 * 1080 * 2, 16, 0};
    bool committed_ = false;
    std::vector<Sample*> all_samples_;
    std::vector<Sample*> free_samples_;
};

// Factory functions implemented in video_capture_filter.cpp & audio_capture_filter.cpp
HRESULT CreateVideoCaptureFilter(REFIID riid, void** ppv);
HRESULT CreateAudioCaptureFilter(REFIID riid, void** ppv);

}  // namespace ios_vcam
