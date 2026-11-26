#include "dshow_base.h"
#include "ios_vcam/dshow_guids.h"

#include <cstdio>
#include <cwchar>

namespace ios_vcam {

std::atomic<long> g_dll_obj_count{0};
HMODULE g_dll_module = nullptr;

namespace {

using CreateInstanceFn = HRESULT (*)(REFIID, void**);

class CaptureClassFactory final : public IClassFactory {
public:
    explicit CaptureClassFactory(CreateInstanceFn fn) : create_fn_(fn) {
        LockModule();
    }
    ~CaptureClassFactory() {
        UnlockModule();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
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

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;
        return create_fn_(riid, ppv);
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) {
            LockModule();
        } else {
            UnlockModule();
        }
        return S_OK;
    }

private:
    std::atomic<long> ref_count_{1};
    CreateInstanceFn create_fn_ = nullptr;
};

void FormatGuidWide(const GUID& g, wchar_t (&out)[64]) {
    std::swprintf(
        out,
        64,
        L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1,
        g.Data2,
        g.Data3,
        g.Data4[0],
        g.Data4[1],
        g.Data4[2],
        g.Data4[3],
        g.Data4[4],
        g.Data4[5],
        g.Data4[6],
        g.Data4[7]);
}

HRESULT RegisterComClsidInRegistry(const CLSID& clsid, const wchar_t* friendly_name, const wchar_t* dll_path) {
    wchar_t clsid_str[64] = {0};
    FormatGuidWide(clsid, clsid_str);

    wchar_t key_path[128] = {0};
    std::swprintf(key_path, 128, L"CLSID\\%s", clsid_str);

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(
            HKEY_CLASSES_ROOT, key_path, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) !=
        ERROR_SUCCESS) {
        return E_ACCESSDENIED;
    }
    RegSetValueExW(
        hKey,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(friendly_name),
        static_cast<DWORD>((wcslen(friendly_name) + 1) * sizeof(wchar_t)));

    HKEY hInproc = nullptr;
    if (RegCreateKeyExW(
            hKey, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hInproc, nullptr) !=
        ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return E_ACCESSDENIED;
    }

    RegSetValueExW(
        hInproc,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(dll_path),
        static_cast<DWORD>((wcslen(dll_path) + 1) * sizeof(wchar_t)));

    const wchar_t* threading = L"Both";
    RegSetValueExW(
        hInproc,
        L"ThreadingModel",
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(threading),
        static_cast<DWORD>((wcslen(threading) + 1) * sizeof(wchar_t)));

    RegCloseKey(hInproc);
    RegCloseKey(hKey);
    return S_OK;
}

void UnregisterComClsidFromRegistry(const CLSID& clsid) {
    wchar_t clsid_str[64] = {0};
    FormatGuidWide(clsid, clsid_str);

    wchar_t inproc_path[160] = {0};
    std::swprintf(inproc_path, 160, L"CLSID\\%s\\InprocServer32", clsid_str);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, inproc_path);

    wchar_t key_path[128] = {0};
    std::swprintf(key_path, 128, L"CLSID\\%s", clsid_str);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, key_path);
}

HRESULT RegisterFilterCategory(
    IFilterMapper2* mapper,
    const CLSID& filter_clsid,
    const CLSID& category_clsid,
    const wchar_t* friendly_name,
    const GUID& major_type,
    const GUID& sub_type) {
    REGPINTYPES pin_types{};
    pin_types.clsMajorType = &major_type;
    pin_types.clsMinorType = &sub_type;

    REGFILTERPINS2 reg_pin{};
    reg_pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    reg_pin.cInstances = 1;
    reg_pin.nMediaTypes = 1;
    reg_pin.lpMediaType = &pin_types;
    reg_pin.nMediums = 0;
    reg_pin.lpMedium = nullptr;
    reg_pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;

    REGFILTER2 rf2{};
    rf2.dwVersion = 2;
    rf2.dwMerit = MERIT_DO_NOT_USE;
    rf2.cPins2 = 1;
    rf2.rgPins2 = &reg_pin;

    IMoniker* moniker = nullptr;
    HRESULT hr = mapper->RegisterFilter(
        filter_clsid, friendly_name, &moniker, &category_clsid, nullptr, &rf2);
    if (moniker) {
        // Also store FriendlyName explicitly on the category property bag for WebRTC/Chromium enumeration
        IPropertyBag* bag = nullptr;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, reinterpret_cast<void**>(&bag))) &&
            bag) {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_BSTR;
            var.bstrVal = SysAllocString(friendly_name);
            bag->Write(L"FriendlyName", &var);
            VariantClear(&var);
            bag->Release();
        }
        moniker->Release();
    }
    return hr;
}

}  // namespace
}  // namespace ios_vcam

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        ios_vcam::g_dll_module = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

extern "C" STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    ios_vcam::CreateInstanceFn fn = nullptr;
    if (IsEqualCLSID(rclsid, ios_vcam::CLSID_IosUsbVideoCapture)) {
        fn = &ios_vcam::CreateVideoCaptureFilter;
    } else if (IsEqualCLSID(rclsid, ios_vcam::CLSID_IosUsbAudioCapture)) {
        fn = &ios_vcam::CreateAudioCaptureFilter;
    } else {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new ios_vcam::CaptureClassFactory(fn);
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" STDAPI DllCanUnloadNow() {
    return (ios_vcam::g_dll_obj_count.load(std::memory_order_acquire) == 0) ? S_OK : S_FALSE;
}

extern "C" STDAPI DllRegisterServer() {
    wchar_t dll_path[MAX_PATH * 2] = {0};
    if (!GetModuleFileNameW(ios_vcam::g_dll_module, dll_path, MAX_PATH * 2 - 1)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT hr = ios_vcam::RegisterComClsidInRegistry(
        ios_vcam::CLSID_IosUsbVideoCapture, ios_vcam::kVideoDeviceFriendlyName, dll_path);
    if (FAILED(hr)) return hr;

    hr = ios_vcam::RegisterComClsidInRegistry(
        ios_vcam::CLSID_IosUsbAudioCapture, ios_vcam::kAudioDeviceFriendlyName, dll_path);
    if (FAILED(hr)) return hr;

    bool co_init = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IFilterMapper2* mapper = nullptr;
    hr = CoCreateInstance(
        CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, reinterpret_cast<void**>(&mapper));
    if (SUCCEEDED(hr) && mapper) {
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, ios_vcam::CLSID_IosUsbVideoCapture);
        mapper->UnregisterFilter(&CLSID_AudioInputDeviceCategory, nullptr, ios_vcam::CLSID_IosUsbAudioCapture);

        hr = ios_vcam::RegisterFilterCategory(
            mapper,
            ios_vcam::CLSID_IosUsbVideoCapture,
            CLSID_VideoInputDeviceCategory,
            ios_vcam::kVideoDeviceFriendlyName,
            MEDIATYPE_Video,
            MEDIASUBTYPE_NV12);

        if (SUCCEEDED(hr)) {
            hr = ios_vcam::RegisterFilterCategory(
                mapper,
                ios_vcam::CLSID_IosUsbAudioCapture,
                CLSID_AudioInputDeviceCategory,
                ios_vcam::kAudioDeviceFriendlyName,
                MEDIATYPE_Audio,
                MEDIASUBTYPE_PCM);
        }
        mapper->Release();
    }
    if (co_init) CoUninitialize();
    return hr;
}

extern "C" STDAPI DllUnregisterServer() {
    bool co_init = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IFilterMapper2* mapper = nullptr;
    if (SUCCEEDED(CoCreateInstance(
            CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, reinterpret_cast<void**>(&mapper))) &&
        mapper) {
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, ios_vcam::CLSID_IosUsbVideoCapture);
        mapper->UnregisterFilter(&CLSID_AudioInputDeviceCategory, nullptr, ios_vcam::CLSID_IosUsbAudioCapture);
        mapper->Release();
    }
    if (co_init) CoUninitialize();

    ios_vcam::UnregisterComClsidFromRegistry(ios_vcam::CLSID_IosUsbVideoCapture);
    ios_vcam::UnregisterComClsidFromRegistry(ios_vcam::CLSID_IosUsbAudioCapture);
    return S_OK;
}
