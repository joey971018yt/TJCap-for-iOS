#include "ios_vcam/screenshot_util.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <wincodec.h>
#endif

namespace ios_vcam {

std::filesystem::path GetDefaultScreenshotDirectory(const std::filesystem::path& fallback_app_dir) {
    std::filesystem::path target_dir;
#ifdef _WIN32
    PWSTR pictures_path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures_path)) && pictures_path) {
        target_dir = std::filesystem::path(pictures_path) / L"TJCap Screenshots";
        CoTaskMemFree(pictures_path);
    }
#endif
    if (target_dir.empty()) {
        target_dir = fallback_app_dir / "Screenshots";
    }
    std::error_code ec;
    std::filesystem::create_directories(target_dir, ec);
    return target_dir;
}

std::filesystem::path BuildTimestampedScreenshotPath(
    const std::filesystem::path& dir,
    uint32_t width,
    uint32_t height) {
#ifdef _WIN32
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t fname[128] = {0};
    std::swprintf(
        fname,
        128,
        L"TJCap_%04u%02u%02u_%02u%02u%02u_%03u_%ux%u.png",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        width,
        height);
    return dir / fname;
#else
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
    localtime_r(&tt, &tm_val);
    char fname[128] = {0};
    std::snprintf(
        fname,
        sizeof(fname),
        "TJCap_%04d%02d%02d_%02d%02d%02d_%03lld_%ux%u.png",
        tm_val.tm_year + 1900,
        tm_val.tm_mon + 1,
        tm_val.tm_mday,
        tm_val.tm_hour,
        tm_val.tm_min,
        tm_val.tm_sec,
        static_cast<long long>(ms),
        width,
        height);
    return dir / fname;
#endif
}

bool SaveTopDownBGRA32AsPng(
    const uint8_t* bgra_topdown,
    uint32_t width,
    uint32_t height,
    const std::filesystem::path& file_path,
    std::string* out_error) {
    if (!bgra_topdown || width == 0 || height == 0) {
        if (out_error) *out_error = "無效的畫面緩衝區。";
        return false;
    }

#ifdef _WIN32
    std::error_code ec;
    if (!file_path.parent_path().empty()) {
        std::filesystem::create_directories(file_path.parent_path(), ec);
    }

    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool need_co_uninit = (hr_com == S_OK || hr_com == S_FALSE);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        if (need_co_uninit) CoUninitialize();
        if (out_error) *out_error = "無法初始化 Windows WIC 影像編碼元件。";
        return false;
    }

    IWICBitmap* src_bitmap = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    auto release_all = [&]() {
        if (props) props->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (src_bitmap) src_bitmap->Release();
        if (factory) factory->Release();
        if (need_co_uninit) CoUninitialize();
    };

    const UINT stride = width * 4U;
    const UINT buf_bytes = stride * height;
    hr = factory->CreateBitmapFromMemory(
        width,
        height,
        GUID_WICPixelFormat32bppBGRA,
        stride,
        buf_bytes,
        const_cast<BYTE*>(bgra_topdown),
        &src_bitmap);
    if (FAILED(hr) || !src_bitmap) {
        release_all();
        if (out_error) *out_error = "無法建立記憶體影像物件。";
        return false;
    }

    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr) && stream) {
        hr = stream->InitializeFromFilename(file_path.c_str(), GENERIC_WRITE);
    }
    if (FAILED(hr) || !stream) {
        release_all();
        if (out_error) *out_error = "無法建立截圖輸出檔案（請確認資料夾寫入權限）。";
        return false;
    }

    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr) && encoder) {
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr) && encoder) {
        hr = encoder->CreateNewFrame(&frame, &props);
    }
    if (SUCCEEDED(hr) && frame) {
        hr = frame->Initialize(props);
    }
    if (SUCCEEDED(hr) && frame) {
        hr = frame->SetSize(width, height);
    }
    if (SUCCEEDED(hr) && frame) {
        WICPixelFormatGUID pix_fmt = GUID_WICPixelFormat24bppBGR;
        frame->SetPixelFormat(&pix_fmt);
        hr = frame->WriteSource(src_bitmap, nullptr);
    }
    if (SUCCEEDED(hr) && frame) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr) && encoder) {
        hr = encoder->Commit();
    }

    release_all();
    if (FAILED(hr)) {
        std::filesystem::remove(file_path, ec);
        if (out_error) *out_error = "PNG 無損影像編碼寫入失敗。";
        return false;
    }
    return true;
#else
    (void)file_path;
    if (out_error) *out_error = "WIC PNG encoder is only active on Windows.";
    return false;
#endif
}

bool CopyTopDownBGRA32ToClipboard(
    void* owner_hwnd,
    const uint8_t* bgra_topdown,
    uint32_t width,
    uint32_t height) {
#ifdef _WIN32
    if (!bgra_topdown || width == 0 || height == 0) {
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(width) * 4ULL;
    const size_t img_bytes = row_bytes * height;
    const size_t total_bytes = sizeof(BITMAPINFOHEADER) + img_bytes;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, total_bytes);
    if (!hMem) {
        return false;
    }

    auto* raw = static_cast<uint8_t*>(GlobalLock(hMem));
    if (!raw) {
        GlobalFree(hMem);
        return false;
    }

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = static_cast<LONG>(width);
    bih.biHeight = static_cast<LONG>(height);  // Positive bottom-up DIB for universal Windows app compatibility
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(img_bytes);
    std::memcpy(raw, &bih, sizeof(bih));

    uint8_t* dst_pixels = raw + sizeof(BITMAPINFOHEADER);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src_row = bgra_topdown + static_cast<size_t>(height - 1U - y) * row_bytes;
        std::memcpy(dst_pixels + static_cast<size_t>(y) * row_bytes, src_row, row_bytes);
    }

    GlobalUnlock(hMem);

    if (!OpenClipboard(static_cast<HWND>(owner_hwnd))) {
        GlobalFree(hMem);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, hMem)) {
        CloseClipboard();
        GlobalFree(hMem);
        return false;
    }
    CloseClipboard();
    return true;
#else
    (void)owner_hwnd;
    (void)bgra_topdown;
    (void)width;
    (void)height;
    return false;
#endif
}

}  // namespace ios_vcam
