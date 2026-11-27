# 版本更新日誌 (Changelog)

本文件記錄 **JCast for iOS**（原 `iOS USB Capture Card` / `ios-usb-vcam`）各版本的功能新增、架構優化與問題修復。

---

## [v1.5.0] - 2026-11-26 — 串接 GitHub Releases API 內建一鍵軟體自動更新

### ✨ 新增功能 (Added)
- **GitHub Releases REST API 原生自動更新引擎 (`app_updater`)**：
  - 採用 Windows 內建 `WinHTTP`（支援 TLS 1.2/1.3 加密與系統自動 Proxy）直接串接 `https://api.github.com/repos/joey971018yt/iOS-usb-cam/releases/latest`（API 版本 `2022-11-28`），零外部執行期 DLL 依賴。
  - 實作完整 RFC 8259 遞迴下降 JSON 解析器與語意化版本（SemVer）比對器，支援 Unicode / Emoji 代理對解碼、Draft / Pre-release 過濾與防止舊版降級。
- **非干擾式背景更新檢查與互動式升級視窗 (`UpdateModalWindow`)**：
  - 啟動後待機時自動於背景檢查是否有新版本，發現新版時於狀態橫幅提示並將按鈕動態切換為 **`⬆️ 升級 vX.Y.Z`**（擷取傳輸期間完全停用更新輪詢與按鈕，絕不干擾直播）。
  - 內建高 DPI 更新專屬視窗：可直接捲動閱讀完整 GitHub Release Notes、即時顯示平滑進度條與已下載 `MB` 數，並支援隨時取消下載或一鍵開啟 GitHub 發布網頁。
- **串流下載 `SHA-256` 雜湊完整性驗證與雙模式自動替換**：
  - 下載過程同步透過 Windows `BCrypt` (`BCRYPT_SHA256_ALGORITHM`) 計算雜湊值，自動比對 GitHub Releases API 回傳之 `digest` (`sha256:...`) 與檔案大小，校驗通過後才允許執行。
  - **自動識別安裝模式**：若為 `Setup.exe` 正式安裝版（偵測 `unins000.exe`），自動啟動安裝升級精靈；若為 `Portable.zip` 免安裝便攜版，自動生成背景 PowerShell 腳本於主程式關閉後原地解壓替換（即使 `ios_vcam_dshow.dll` 正被 OBS 載入亦能透過 `.old_upd` 安全熱替換）並自動重啟新版 `JCast for iOS.exe`。

---

## [v1.4.0] - 2026-11-26 — 品牌升級「JCast for iOS」與 USB 接口速率智慧解析度分級

### ✨ 新增功能 (Added)
- **品牌正式更名為「JCast for iOS」**：
  - 全面升級桌面與開始選單 App 名稱、主程式（`JCast for iOS.exe`）、安裝程式、便攜包與 DirectShow 虛擬擷取卡（**`JCast for iOS (Video)`** / **`JCast for iOS (Audio)`**，保留相同底層 COM `CLSID` 無縫相容既有場景）。
- **USB 接口與線材速率自動辨識 × 可選最大解析度分級開放**：
  - 待機時透過 `libusb_get_device_speed` 非侵入式辨識連接埠速率（`USB 3.x 5~20Gbps` 解鎖最高 `3840p`、`USB 2.0 480Mbps` 最高開放 `2K 2560p`、`USB 1.1 12Mbps` 最高開放 `720p 1280p`），且**一旦開始傳輸即全程鎖定所選解析度，絕不在直播途中自動變動**。

---

## [v1.3.0] - 2026-11-26 — 正式版獨立 Windows App 圖示與安裝包封裝

### ✨ 新增功能 (Added)
- **專屬高解析度多尺寸 App 圖示 (`assets/app_icon.ico`)** 與 **Inno Setup 6 正式安裝程式 (`Setup.exe`) / 綠色便攜包 (`Portable.zip`)**。

---

## [v1.2.0] - 2026-11-26 — 2K+ 原生畫質與裝置螢幕比例自適應

### ✨ 新增功能 (Added)
- **2K+ 裝置原生解析度與真實長寬比自適應 (`--res native`)**、即時 `Mbps` 碼率監控與 `IosUsbVcamSharedMemV2` 動態高解析度共享記憶體。

---

## [v1.1.0] - 2026-11-26 — 一鍵智慧圖形化控制台與原生 MSVC 建置

### ✨ 新增功能 (Added)
- **Windows 原生一鍵智慧圖形化控制台**、電腦耳機零延遲音訊監聽選單，並改用微軟官方 Visual Studio (`MSVC` `/MT`) 原生編譯消除防毒誤報。

---

## [v1.0.0] - 2026-11-26 — 初版核心引擎發布

### ✨ 核心模組 (Initial Release)
- **Apple QuickTime / Valeria (`SubClass 0x2A`) 協定引擎**、**免測試模式安全 WHQL `libusb0` UpperFilter 掛載器**、**`D3D11VA` GPU 超低延遲硬解**與**雙路 DirectShow 視訊＋48kHz 音訊虛擬擷取卡**。
