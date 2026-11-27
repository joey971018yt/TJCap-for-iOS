# 版本更新日誌 (Changelog)

本文件記錄 `iOS-usb-cam` (`ios-usb-vcam`) 各版本的功能新增、架構優化與問題修復。

---

## [v1.2.0] - 2026-11-26 — 2K+ 原生畫質與裝置螢幕比例自適應

### ✨ 新增功能 (Added)
- **2K+ 裝置原生解析度與真實長寬比自適應 (`--res native`)**：
  - 預設開放最高 `3840×3840` 對稱握手邊界，自動依據連接之 iPhone（如 `19.5:9` 最高達 `2868×1320` 2.8K@60fps、`16:9`）或 iPad（如 `4:3`、`1.44:1` 最高達 `2752×2064` @ 60fps）輸出 **1:1 點對點滿版無黑邊**原生畫面。
- **圖形控制台 4 段畫質與比例切換**：
  - 於 `ios_vcam_gui.exe` 新增「輸出畫質與比例模式」下拉選單，支援：
    1. `🌟 2K+ 原生自適應比例`（預設推薦，依機型滿版無黑邊）
    2. `✨ 1080p 自適應比例`（維持機型真實比例滿版無黑邊、輕量低負載）
    3. `🖥 固定 2K 16:9 畫布 (2560×1440)`（直橫屏旋轉自動等比補黑邊）
    4. `🖥 固定 1080p 16:9 畫布 (1920×1080)`（傳統會議軟體相容模式）
- **即時 USB 傳輸碼率 (`Mbps`) 與原生比例監控**：
  - 背景主程式與圖形控制台即時日誌新增每 4 秒統計之真實 H.264 輸入碼率（`Bitrate: XX.X Mbps`）與裝置長寬比資訊。
- **DirectShow 2K 與直向預設集擴充**：
  - `iOS USB Capture Card (Video)` 除動態回報當前連接裝置之原生解析度外，另新增 `2560×1440`、`1440×2560`（2K 直向）、`2560×1920`（2K 4:3 iPad）、`1080×1920`（1080p 直向）等預設格式。

### 🔧 優化與改進 (Changed)
- **共享記憶體協定升級至 V2 (`IosUsbVcamSharedMemV2`)**：
  - 單槽緩衝區擴充支援最高 `3840×2560`（或直向 `2560×3840`）`NV12` 畫布，並實作 `4×2` 像素自動對齊，確保 `NV12`、`YUY2` 與 `RGB24` 於 OBS / Discord / WebRTC 零跨距錯位。
- **硬體解碼器高解析度預配置**：
  - `MfH264Decoder` 支援於首幀 `>1080p`（2K+ / 4K）SPS 到達時即時動態擴充 Media Foundation 輸入／輸出緩衝區，首幀零掉幀。

---

## [v1.1.0] - 2026-11-26 — 一鍵智慧圖形化控制台與原生 MSVC 建置

### ✨ 新增功能 (Added)
- **Windows 原生一鍵智慧圖形化控制台 (`ios_vcam_gui.exe`)**：
  - 純 Win32 + `ComCtl32 v6` 視覺樣式打造，支援 PerMonitorV2 4K 高 DPI 縮放，體積僅約 213 KB、零外部 DLL 依賴，並附帶根目錄雙擊啟動器 `點我啟動_iOS擷取卡控制台.bat`。
  - **`🚀 智慧一鍵啟動`**：自動檢測 DirectShow 虛擬卡註冊狀態（若資料夾曾搬移位置亦會自動偵測並修復路徑）、`libusb0` 核心服務與當前插入之 iPhone 過濾層，一鍵完成 UAC 授權與投影啟動。
  - **電腦端音訊同步監聽選單**：自動列舉 Windows 所有播放端點（耳機／喇叭／VB-Cable），方便遊戲直播主直接透過電腦耳機零延遲收聽手機遊戲音效與隊友語音。
  - **自動解除下載封鎖**：啟動時自動清除瀏覽器下載 ZIP 附帶之 `Zone.Identifier` (Mark of the Web) 標記。
  - 內建一鍵 `🧪 免插手機自測`、`📋 OBS 設定教學`（自動複製擷取卡名稱）、`🔧 安裝／修復驅動` 與 `🗑 完整移除`。
- **無縫安全停止事件 (`--stop-event`)**：
  - 透過跨行程 Win32 命名事件通知 `ios_vcam_host.exe` 優雅結束，確保每次點擊停止或關閉視窗皆會發送 `HPA0`/`HPD0` 並將 iPhone 恢復為標準 `USBMux` 充電傳輸模式。

### 🛡️ 安全性與相容性優化 (Security & Build)
- **改用微軟官方 Visual Studio (`MSVC`) 原生靜態執行期 (`/MT`) 編譯**：
  - 新增 GitHub Actions (`windows-msvc-build.yml`) 自動於 Windows 環境建置並執行單元測試，全面取代 MinGW 交叉編譯產物，大幅降低檔案體積並消除防毒軟體／雲端硬碟啟發式誤報。

---

## [v1.0.0] - 2026-11-26 — 初版核心引擎發布

### ✨ 核心模組 (Initial Release)
- **Apple QuickTime / Valeria (`SubClass 0x2A`) 協定引擎**：
  - 完整實作 Little-Endian TLV 字典編解碼、`CMTime`、`avcC` / SPS 指數哥倫布解析、`AVCC` 轉 `Annex-B`，以及 `PING` / `CWPA` / `HPD1` / `HPA1` / `AFMT` / `CVRP` / `CLOK` / `TIME` / `SKEW` / `FEED` / `EAT!` 完整串流狀態機。
- **免測試模式安全 USB 過濾層安裝器 (`ios_vcam_driver_setup.exe`)**：
  - 採用微軟 WHQL 簽章之 `libusb0.sys` 掛載為 Apple 複合父裝置 `SPDRP_UPPERFILTERS`，完整保留原廠 `usbccgp` / `usbaapl64` 信任配對服務，並內建 Zadig 驅動覆寫衝突偵測告警。
- **GPU 低延遲硬解與雙路 DirectShow 虛擬擷取卡 (`ios_vcam_dshow.dll`)**：
  - 支援 `D3D11VA` + `MF_LOW_LATENCY` 硬體解碼、BT.709 等比縮放、無鎖三緩衝視訊＋單寫多讀（SPMC）`48kHz` 立體聲共享記憶體，同時註冊 **`iOS USB Capture Card (Video)`** 與 **`iOS USB Capture Card (Audio)`**，並內建 WASAPI `VB-Audio Virtual Cable` 自動音訊路由。
