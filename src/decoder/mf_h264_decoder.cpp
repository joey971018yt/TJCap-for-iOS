#include "ios_vcam/mf_h264_decoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#endif

namespace ios_vcam {

#ifdef _WIN32
namespace {

// CLSID_CMSH264DecoderMFT {62CE7E72-4C71-4D20-B15D-452831A87D9D}
static const GUID kClsidCmsH264DecoderMft = {
    0x62ce7e72, 0x4c71, 0x4d20, {0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d}};

// MF_LOW_LATENCY / CODECAPI_AVLowLatencyMode {9C27891A-ED7A-40E1-88E8-B22727A024EE}
static const GUID kGuidMfLowLatency = {
    0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

// MF_SA_D3D11_AWARE {206B4FC8-FCF9-4C51-AFE3-9764369E33A0}
static const GUID kGuidMfSaD3D11Aware = {
    0x206b4fc8, 0xfcf9, 0x4c51, {0xaf, 0xe3, 0x97, 0x64, 0x36, 0x9e, 0x33, 0xa0}};

// Dynamically resolve MFCreateDXGIDeviceManager from mfplat.dll for broad MinGW/SDK compatibility
using PFN_MFCreateDXGIDeviceManager = HRESULT(STDAPICALLTYPE*)(UINT*, IMFDXGIDeviceManager**);

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace
#endif

struct MfH264Decoder::Impl {
#ifdef _WIN32
    bool mf_started = false;
    ID3D11Device* d3d_device = nullptr;
    ID3D11DeviceContext* d3d_ctx = nullptr;
    IMFDXGIDeviceManager* dxgi_mgr = nullptr;
    UINT dxgi_reset_token = 0;
    ID3D11Texture2D* staging_tex = nullptr;
    uint32_t staging_tex_w = 0;
    uint32_t staging_tex_h = 0;

    IMFTransform* decoder = nullptr;
    bool MftProvidesSamples = false;
    uint32_t out_buffer_size = 1920 * 1080 * 3 / 2;
    uint32_t coded_width = 1920;
    uint32_t coded_height = 1088;
    uint32_t crop_width = 1920;
    uint32_t crop_height = 1080;

    std::vector<uint8_t> cpu_scratch;

    bool NegotiateOutputType() {
        if (!decoder) {
            return false;
        }
        for (DWORD idx = 0;; ++idx) {
            IMFMediaType* out_type = nullptr;
            HRESULT hr = decoder->GetOutputAvailableType(0, idx, &out_type);
            if (FAILED(hr) || !out_type) {
                break;
            }
            GUID subtype{};
            if (SUCCEEDED(out_type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                IsEqualGUID(subtype, MFVideoFormat_NV12)) {
                hr = decoder->SetOutputType(0, out_type, 0);
                if (SUCCEEDED(hr)) {
                    UINT32 fw = 0, fh = 0;
                    if (SUCCEEDED(MFGetAttributeSize(out_type, MF_MT_FRAME_SIZE, &fw, &fh)) && fw > 0 && fh > 0) {
                        coded_width = fw;
                        coded_height = fh;
                        crop_width = fw;
                        crop_height = fh;
                    }
                    MFVideoArea aperture{};
                    UINT32 blob_size = 0;
                    if (SUCCEEDED(out_type->GetBlob(
                            MF_MT_MINIMUM_DISPLAY_APERTURE,
                            reinterpret_cast<UINT8*>(&aperture),
                            sizeof(aperture),
                            &blob_size))) {
                        if (aperture.Area.cx > 0 && aperture.Area.cy > 0) {
                            crop_width = static_cast<uint32_t>(aperture.Area.cx);
                            crop_height = static_cast<uint32_t>(aperture.Area.cy);
                        }
                    }

                    MFT_OUTPUT_STREAM_INFO osi{};
                    if (SUCCEEDED(decoder->GetOutputStreamInfo(0, &osi))) {
                        MftProvidesSamples =
                            (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                        out_buffer_size = std::max<uint32_t>(
                            osi.cbSize, coded_width * coded_height * 3U / 2U);
                    }
                    out_type->Release();
                    return true;
                }
            }
            out_type->Release();
        }
        return false;
    }

    bool ConfigureInputType(uint32_t w, uint32_t h) {
        IMFMediaType* in_type = nullptr;
        if (FAILED(MFCreateMediaType(&in_type)) || !in_type) {
            return false;
        }
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in_type, MF_MT_FRAME_SIZE, w > 0 ? w : 1920, h > 0 ? h : 1080);
        MFSetAttributeRatio(in_type, MF_MT_FRAME_RATE, 60, 1);
        MFSetAttributeRatio(in_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        HRESULT hr = decoder->SetInputType(0, in_type, 0);
        in_type->Release();
        return SUCCEEDED(hr);
    }

    bool ExtractSampleAndEmit(IMFSample* sample, uint64_t fallback_pts_ns, const OnDecodedFrameFn& on_frame) {
        if (!sample || !on_frame) {
            return false;
        }
        LONGLONG sample_hns = 0;
        uint64_t pts_ns = fallback_pts_ns;
        if (SUCCEEDED(sample->GetSampleTime(&sample_hns)) && sample_hns >= 0) {
            pts_ns = static_cast<uint64_t>(sample_hns) * 100ULL;
        }

        IMFMediaBuffer* media_buf = nullptr;
        if (FAILED(sample->GetBufferByIndex(0, &media_buf)) || !media_buf) {
            return false;
        }

        uint32_t active_w = std::min(crop_width, coded_width);
        uint32_t active_h = std::min(crop_height, coded_height);

        // Path A: D3D11 hardware texture via IMFDXGIBuffer
        IMFDXGIBuffer* dxgi_buf = nullptr;
        if (d3d_device && d3d_ctx &&
            SUCCEEDED(media_buf->QueryInterface(__uuidof(IMFDXGIBuffer), reinterpret_cast<void**>(&dxgi_buf))) &&
            dxgi_buf) {
            ID3D11Texture2D* gpu_tex = nullptr;
            UINT subresource = 0;
            dxgi_buf->GetSubresourceIndex(&subresource);
            if (SUCCEEDED(dxgi_buf->GetResource(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&gpu_tex))) &&
                gpu_tex) {
                D3D11_TEXTURE2D_DESC desc{};
                gpu_tex->GetDesc(&desc);
                if (!staging_tex || staging_tex_w != desc.Width || staging_tex_h != desc.Height) {
                    SafeRelease(staging_tex);
                    D3D11_TEXTURE2D_DESC st{};
                    st.Width = desc.Width;
                    st.Height = desc.Height;
                    st.MipLevels = 1;
                    st.ArraySize = 1;
                    st.Format = DXGI_FORMAT_NV12;
                    st.SampleDesc.Count = 1;
                    st.Usage = D3D11_USAGE_STAGING;
                    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (SUCCEEDED(d3d_device->CreateTexture2D(&st, nullptr, &staging_tex))) {
                        staging_tex_w = desc.Width;
                        staging_tex_h = desc.Height;
                    }
                }
                if (staging_tex) {
                    d3d_ctx->CopySubresourceRegion(
                        staging_tex, 0, 0, 0, 0, gpu_tex, subresource, nullptr);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(d3d_ctx->Map(staging_tex, 0, D3D11_MAP_READ, 0, &mapped))) {
                        const auto* base = static_cast<const uint8_t*>(mapped.pData);
                        DecodedNV12View view;
                        view.y_plane = base;
                        view.y_stride = mapped.RowPitch;
                        view.uv_plane = base + static_cast<size_t>(mapped.RowPitch) * desc.Height;
                        view.uv_stride = mapped.RowPitch;
                        view.width = std::min(active_w, static_cast<uint32_t>(desc.Width));
                        view.height = std::min(active_h, static_cast<uint32_t>(desc.Height));
                        view.pts_ns = pts_ns;
                        on_frame(view);
                        d3d_ctx->Unmap(staging_tex, 0);
                        gpu_tex->Release();
                        dxgi_buf->Release();
                        media_buf->Release();
                        return true;
                    }
                }
                gpu_tex->Release();
            }
            dxgi_buf->Release();
        }

        // Path B: Pitch-aware 2D buffer (IMF2DBuffer)
        IMF2DBuffer* buf_2d = nullptr;
        if (SUCCEEDED(media_buf->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&buf_2d))) &&
            buf_2d) {
            BYTE* scanline0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buf_2d->Lock2D(&scanline0, &pitch)) && scanline0 && pitch > 0) {
                uint32_t upitch = static_cast<uint32_t>(pitch);
                DecodedNV12View view;
                view.y_plane = scanline0;
                view.y_stride = upitch;
                view.uv_plane = scanline0 + static_cast<size_t>(upitch) * coded_height;
                view.uv_stride = upitch;
                view.width = active_w;
                view.height = active_h;
                view.pts_ns = pts_ns;
                on_frame(view);
                buf_2d->Unlock2D();
                buf_2d->Release();
                media_buf->Release();
                return true;
            }
            buf_2d->Release();
        }

        // Path C: Contiguous linear IMFMediaBuffer
        BYTE* raw_ptr = nullptr;
        DWORD cur_len = 0;
        if (SUCCEEDED(media_buf->Lock(&raw_ptr, nullptr, &cur_len)) && raw_ptr) {
            size_t min_needed = static_cast<size_t>(coded_width) * coded_height * 3U / 2U;
            if (cur_len >= min_needed) {
                DecodedNV12View view;
                view.y_plane = raw_ptr;
                view.y_stride = coded_width;
                view.uv_plane = raw_ptr + static_cast<size_t>(coded_width) * coded_height;
                view.uv_stride = coded_width;
                view.width = active_w;
                view.height = active_h;
                view.pts_ns = pts_ns;
                on_frame(view);
            }
            media_buf->Unlock();
            media_buf->Release();
            return true;
        }

        media_buf->Release();
        return false;
    }
#endif
};

MfH264Decoder::MfH264Decoder() : impl_(new Impl()) {}

MfH264Decoder::~MfH264Decoder() {
    Shutdown();
    delete impl_;
}

bool MfH264Decoder::Initialize(bool prefer_d3d11_hw, std::string* out_error) {
#ifdef _WIN32
    Shutdown();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        if (out_error) *out_error = "MFStartup failed.";
        return false;
    }
    impl_->mf_started = true;

    hr = CoCreateInstance(
        kClsidCmsH264DecoderMft,
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(IMFTransform),
        reinterpret_cast<void**>(&impl_->decoder));
    if (FAILED(hr) || !impl_->decoder) {
        if (out_error) *out_error = "Failed to instantiate Windows H.264 MFT (CLSID_CMSH264DecoderMFT).";
        Shutdown();
        return false;
    }

    IMFAttributes* attrs = nullptr;
    if (SUCCEEDED(impl_->decoder->GetAttributes(&attrs)) && attrs) {
        // Enable zero-B-frame low-latency mode
        attrs->SetUINT32(kGuidMfLowLatency, TRUE);

        if (prefer_d3d11_hw) {
            UINT32 d3d11_aware = 0;
            attrs->GetUINT32(kGuidMfSaD3D11Aware, &d3d11_aware);
            if (d3d11_aware != 0) {
                D3D_FEATURE_LEVEL levels[] = {
                    D3D_FEATURE_LEVEL_11_1,
                    D3D_FEATURE_LEVEL_11_0,
                    D3D_FEATURE_LEVEL_10_1,
                    D3D_FEATURE_LEVEL_10_0,
                };
                D3D_FEATURE_LEVEL chosen{};
                UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                if (SUCCEEDED(D3D11CreateDevice(
                        nullptr,
                        D3D_DRIVER_TYPE_HARDWARE,
                        nullptr,
                        flags,
                        levels,
                        4,
                        D3D11_SDK_VERSION,
                        &impl_->d3d_device,
                        &chosen,
                        &impl_->d3d_ctx))) {
                    ID3D10Multithread* mt = nullptr;
                    if (SUCCEEDED(impl_->d3d_device->QueryInterface(
                            __uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt))) &&
                        mt) {
                        mt->SetMultithreadProtected(TRUE);
                        mt->Release();
                    }

                    HMODULE mfplat_mod = GetModuleHandleW(L"mfplat.dll");
                    auto create_dxgi_mgr = mfplat_mod
                                               ? reinterpret_cast<PFN_MFCreateDXGIDeviceManager>(
                                                     GetProcAddress(mfplat_mod, "MFCreateDXGIDeviceManager"))
                                               : nullptr;
                    if (create_dxgi_mgr &&
                        SUCCEEDED(create_dxgi_mgr(&impl_->dxgi_reset_token, &impl_->dxgi_mgr)) &&
                        impl_->dxgi_mgr) {
                        if (SUCCEEDED(impl_->dxgi_mgr->ResetDevice(impl_->d3d_device, impl_->dxgi_reset_token)) &&
                            SUCCEEDED(impl_->decoder->ProcessMessage(
                                MFT_MESSAGE_SET_D3D_MANAGER,
                                reinterpret_cast<ULONG_PTR>(impl_->dxgi_mgr)))) {
                            hw_accelerated_ = true;
                        }
                    }
                }
            }
        }
        attrs->Release();
    }

    if (!impl_->ConfigureInputType(1920, 1080) || !impl_->NegotiateOutputType()) {
        if (out_error) *out_error = "Failed to configure H.264 -> NV12 media types on MFT.";
        Shutdown();
        return false;
    }

    impl_->decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
#else
    (void)prefer_d3d11_hw;
    if (out_error) *out_error = "MfH264Decoder is only available on Windows.";
    return false;
#endif
}

void MfH264Decoder::Shutdown() {
#ifdef _WIN32
    if (impl_->decoder) {
        impl_->decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        SafeRelease(impl_->decoder);
    }
    SafeRelease(impl_->staging_tex);
    SafeRelease(impl_->dxgi_mgr);
    SafeRelease(impl_->d3d_ctx);
    SafeRelease(impl_->d3d_device);
    if (impl_->mf_started) {
        MFShutdown();
        impl_->mf_started = false;
    }
    hw_accelerated_ = false;
#endif
}

bool MfH264Decoder::DecodeAnnexB(
    std::span<const uint8_t> annexb_nalus,
    uint64_t pts_ns,
    uint32_t hint_width,
    uint32_t hint_height,
    const OnDecodedFrameFn& on_frame) {
#ifdef _WIN32
    if (!impl_->decoder || annexb_nalus.empty()) {
        return false;
    }
    if (hint_width > 0 && hint_height > 0) {
        impl_->crop_width = hint_width;
        impl_->crop_height = hint_height;
    }

    IMFMediaBuffer* in_buf = nullptr;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(annexb_nalus.size()), &in_buf)) || !in_buf) {
        return false;
    }
    BYTE* dst = nullptr;
    if (FAILED(in_buf->Lock(&dst, nullptr, nullptr))) {
        in_buf->Release();
        return false;
    }
    std::memcpy(dst, annexb_nalus.data(), annexb_nalus.size());
    in_buf->Unlock();
    in_buf->SetCurrentLength(static_cast<DWORD>(annexb_nalus.size()));

    IMFSample* in_sample = nullptr;
    if (FAILED(MFCreateSample(&in_sample)) || !in_sample) {
        in_buf->Release();
        return false;
    }
    in_sample->AddBuffer(in_buf);
    in_buf->Release();
    in_sample->SetSampleTime(static_cast<LONGLONG>(pts_ns / 100ULL));
    in_sample->SetSampleDuration(166667LL);  // 60fps nominal 100-ns units

    auto drain_outputs = [&]() {
        while (true) {
            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.dwStreamID = 0;

            IMFSample* alloc_sample = nullptr;
            if (!impl_->MftProvidesSamples) {
                IMFMediaBuffer* out_buf = nullptr;
                if (FAILED(MFCreateMemoryBuffer(impl_->out_buffer_size, &out_buf))) {
                    break;
                }
                if (FAILED(MFCreateSample(&alloc_sample))) {
                    out_buf->Release();
                    break;
                }
                alloc_sample->AddBuffer(out_buf);
                out_buf->Release();
                odb.pSample = alloc_sample;
            }

            DWORD status = 0;
            HRESULT phr = impl_->decoder->ProcessOutput(0, 1, &odb, &status);
            if (odb.pEvents) {
                odb.pEvents->Release();
            }

            if (phr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                SafeRelease(alloc_sample);
                break;
            }
            if (phr == MF_E_TRANSFORM_STREAM_CHANGE) {
                SafeRelease(alloc_sample);
                if (!impl_->NegotiateOutputType()) {
                    break;
                }
                continue;
            }
            if (FAILED(phr)) {
                SafeRelease(alloc_sample);
                break;
            }

            if (odb.pSample) {
                impl_->ExtractSampleAndEmit(odb.pSample, pts_ns, on_frame);
                if (odb.pSample != alloc_sample) {
                    odb.pSample->Release();
                }
            }
            SafeRelease(alloc_sample);
        }
    };

    HRESULT hr = impl_->decoder->ProcessInput(0, in_sample, 0);
    if (hr == MF_E_NOTACCEPTING) {
        drain_outputs();
        hr = impl_->decoder->ProcessInput(0, in_sample, 0);
    }
    in_sample->Release();

    if (FAILED(hr)) {
        return false;
    }
    drain_outputs();
    return true;
#else
    (void)annexb_nalus;
    (void)pts_ns;
    (void)hint_width;
    (void)hint_height;
    (void)on_frame;
    return false;
#endif
}

}  // namespace ios_vcam
