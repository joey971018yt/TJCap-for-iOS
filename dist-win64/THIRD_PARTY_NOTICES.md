# 第三方開源元件授權聲明 (Third-Party Open Source Notices)

**JCast for iOS** 隨附使用以下獨立預編譯之第三方開源元件（皆透過執行期動態載入或作業系統裝置驅動層隔離呼叫，未與主程式靜態連結）：

---

## 1. libusb (`libusb-1.0.dll`)
- **官方網站 / 原始碼**：https://github.com/libusb/libusb
- **授權條款**：GNU Lesser General Public License v2.1 (LGPL-2.1)
- **使用方式**：以獨立動態連結程式庫 (`libusb-1.0.dll`) 於執行期透過 `LoadLibraryExW` 載入，使用者可隨時自行替換相容版本之 `libusb-1.0.dll`。

---

## 2. libusb-win32 (`drivers/libusb0.sys`, `drivers/libusb0.dll`)
- **官方網站 / 原始碼**：https://github.com/mcuee/libusb-win32
- **數位簽章**：GlobalSign GCC R45 EV CodeSigning CA / Microsoft WHQL 相容簽章
- **授權條款**：
  - `libusb0.sys`（獨立核心態過濾驅動程式）：GNU General Public License v3.0 (GPL-3.0) — 見隨附檔案 `drivers/COPYING_GPL.txt`
  - `libusb0.dll`（使用者態介面程式庫）：GNU Lesser General Public License v2.1 (LGPL-2.1) — 見隨附檔案 `drivers/COPYING_LGPL.txt`
