# 版本更新日誌 (Changelog)

本文件記錄 **JCast for iOS**（原 `iOS USB Capture Card` / `ios-usb-vcam`）各版本的功能新增、架構優化與問題修復。

---

## [v1.4.0] - 2026-11-26 — 品牌升級「JCast for iOS」與 USB 接口速率智慧解析度分級

### ✨ 新增功能 (Added)
- **品牌正式更名為「JCast for iOS」**：
  - 全面升級桌面與開始選單 App 名稱、主程式（`JCast for iOS.exe`）、安裝程式（`JCast-for-iOS-Setup-v1.4.0.exe`）、綠色便攜包（`JCast-for-iOS-Portable-v1.4.0.zip`）與 Windows 檔案版本資訊。
  - DirectShow 視訊與音訊虛擬擷取卡同步命名為 **`JCast for iOS (Video)`** 與 **`JCast for iOS (Audio)`**，並保留完全相同的底層 COM `CLSID`，舊版使用者升級無須重新設定 OBS 來源。
- **USB 接口與線材速率自動辨識 (`UsbIosTransport::ProbeConnectedIosLinkSpeed`)**：
  - 待機狀態下透過 `libusb_get_device_speed` 與 USB 裝置描述符非侵入式偵測當前連接埠與線材之真實協商速率（`USB 3.x 5Gbps / 10Gbps / 20Gbps`、`USB 2.0 480Mbps`、`USB 1.1 12Mbps`），無須開啟或干擾手機現有連線狀態，並即時顯示於控制台頂部狀態橫幅。
- **依 USB 接口速率動態開放「可選最大解析度上限」（傳輸中嚴格鎖定不變動）**：
  - 僅於**開始擷取前**依據偵測到的 USB 接口速率動態生成下拉選單中的最大解析度選項：
    - **USB 3.x (`5～20 Gbps`)**：解鎖最高 **`3840p` 原生極限無上限**（支援 iPhone Pro Max `2868×1320`、iPad Pro `2752×2064` 原生滿版）及 `2560p` / `1920p` / `1280p` 各級選項。
    - **USB 2.0 (`480 Mbps`，如 Lightning 機型或 USB-C 充電線)**：自動隱藏超負荷選項，最高開放至 **`2K (2560p)`** 上限與 `1080p (1920p)` 穩定推薦選項。
    - **USB 1.1 (`12 Mbps`)**：最高開放至 **`720p (1280p)`** 與 `540p (960p)` 省頻寬選項。
  - **傳輸期間零動態切換保證**：一旦點擊啟動進入投影傳輸，即透過 `--max-dim <px>` 與 QuickTime `HPD1` 握手邊界**全程鎖定使用者選取的解析度上限**，絕不在直播傳輸途中自動縮放或變動解析度，確保 OBS / 直播畫面零閃爍、零中斷。

---

## [v1.3.0] - 2026-11-26 — 正式版獨立 Windows App 圖示與安裝包封裝

### ✨ 新增功能 (Added)
- **專屬高解析度多尺寸 App 圖示 (`assets/app_icon.ico`)**：
  - 內嵌 `16×16` ～ `256×256` 高清圓角玻璃質感專屬圖示與 Windows `VERSIONINFO` 應用程式資訊至主程式、背景服務與 DirectShow 模組，完整顯示於桌面捷徑、工作列、開始選單、視窗標題列與狀態橫幅。
- **正式版 Windows 安裝程式 (`Setup.exe`) 與免安裝綠色便攜版 (`Portable.zip`)**：
  - 採用 Inno Setup 6 封裝單一安裝檔，一鍵部署至 `Program Files`、建立桌面與開始選單捷徑，並支援從 Windows「設定 ➔ 應用程式」一鍵乾淨解除安裝。

---

## [v1.2.0] - 2026-11-26 — 2K+ 原生畫質與裝置螢幕比例自適應

### ✨ 新增功能 (Added)
- **2K+ 裝置原生解析度與真實長寬比自適應 (`--res native`)**：
  - 自動依據連接之 iPhone（如 `19.5:9` 最高達 `2868×1320` 2.8K@60fps、`16:9`）或 iPad（如 `4:3`、`1.44:1` 最高達 `2752×2064` @ 60fps）輸出 **1:1 點對點滿版無黑邊**原生畫面。
- **即時 USB 傳輸碼率 (`Mbps`) 與原生比例監控**：
  - 背景主程式與圖形控制台即時日誌新增每 4 秒統計之真實 H.264 輸入碼率（`Bitrate: XX.X Mbps`）與裝置長寬比資訊。
- **共享記憶體協定升級至 V2 (`IosUsbVcamSharedMemV2`)**：
  - 單槽緩衝區擴充支援最高 `3840×2560`（或直向 `2560×3840`）`NV12` 畫布，並實作 `4×2` 像素自動對齊。

---

## [v1.1.0] - 2026-11-26 — 一鍵智慧圖形化控制台與原生 MSVC 建置

### ✨ 新增功能 (Added)
- **Windows 原生一鍵智慧圖形化控制台 (`ios_vcam_gui.exe`)**：
  - 支援 PerMonitorV2 4K 高 DPI 縮放、`🚀 智慧一鍵啟動`、電腦端耳機／喇叭零延遲音訊監聽選單、自動清除 `Zone.Identifier` 與 `--stop-event` 優雅退場機制。
- **改用微軟官方 Visual Studio (`MSVC`) 原生靜態執行期 (`/MT`) 編譯**：
  - 大幅降低檔案體積並消除防毒軟體與雲端硬碟啟發式誤報。

---

## [v1.0.0] - 2026-11-26 — 初版核心引擎發布

### ✨ 核心模組 (Initial Release)
- **Apple QuickTime / Valeria (`SubClass 0x2A`) 協定引擎**、**免測試模式安全 WHQL `libusb0` UpperFilter 掛載器**、**`D3D11VA` GPU 超低延遲硬解**與**雙路 DirectShow 視訊＋48kHz 音訊虛擬擷取卡**。
