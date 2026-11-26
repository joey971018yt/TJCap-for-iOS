# iOS USB 通用有線虛擬擷取卡 (`ios-usb-vcam`) — Windows 10 / 11

純 C++20 打造的 **Windows 10 / 11 iPhone / iPad 有線 USB 低延遲虛擬擷取卡（視訊 + 音訊）**。  
透過 Apple 底層 QuickTime / Valeria (`SubClass 0x2A`) 有線螢幕鏡像協定，免購買實體擷取卡、免越獄、免安裝手機端 App、免開啟 Windows 測試模式，即可將 iPhone / iPad 畫面與 48kHz 立體聲音訊以 **1080p60** 直接輸入 **OBS Studio、Discord、Zoom、Teams、WebRTC 瀏覽器**等任何軟體。

---

## 一、核心架構亮點

1. **Apple QuickTime / Valeria 二進位協定引擎 (`src/protocol/`)**
   - `CoreMediaCodec`：完整實作 Little-Endian TLV 字典序列化（`DICT` / `KEYV` / `STRK` / `NMBV`）、`CMTime`、`AVCDecoderConfigurationRecord` (`avcC`) 解析、SPS 指數哥倫布（Exp-Golomb）解析度提取，以及 `AVCC` 轉 `Annex-B` NALU 封包轉換。
   - `QtStreamSession`：完整實作 `PING` → `SYNC CWPA` → `ASYN HPD1/HPA1` → `SYNC AFMT` → `SYNC CVRP` → `CLOK/TIME/SKEW` 時鐘同步與 `FEED`（H.264 視訊）/ `EAT!`（48kHz LPCM 音訊）串流狀態機，支援任意 USB Bulk 封包碎片重組。

2. **安全 USB 隱藏組態切換與過濾驅動掛載 (`src/usb/`、`src/driver_tool/`)**
   - **不破壞 iTunes / Apple 裝置信任配對**：不同於 Zadig 直接覆寫整個父裝置驅動（會導致 `usbmuxd` 斷線無法信任電腦），`ios_vcam_driver_setup.exe` 透過 Windows `SetupAPI` 將微軟 WHQL 簽章的 `libusb0.sys` 安全掛載為 Apple 複合父裝置的 `SPDRP_UPPERFILTERS`，完整保留原廠 `usbccgp` / `usbaapl64` 服務。若偵測到曾誤用 Zadig 覆蓋父驅動，亦會主動告警提示修復。
   - **自動啟閉 `SubClass 0x2A`**：`UsbIosTransport` 動態載入 `libusb-1.0.dll`，發送廠商控制指令 `(0x40, 0x52, 0x00, 0x02)` 觸發 iPhone 重新列舉出隱藏的影音 Bulk 端點；結束時自動發送 `HPA0`/`HPD0` 與 `(0x40, 0x52, 0x00, 0x00)` 切回標準 `USBMux` 組態。

3. **GPU 低延遲硬體解碼與橫豎屏無縫轉換 (`src/decoder/`、`src/common/`)**
   - `MfH264Decoder`：直接調用 Windows 10/11 內建 Media Foundation H.264 MFT，啟用 `D3D11VA` GPU 硬體加速與 `MF_LOW_LATENCY` 零 B-frame 超低延遲模式（無獨顯或虛擬機環境自動降級 CPU 解碼）。
   - `ScaleAndLetterboxNV12`：手機直橫屏即時旋轉（如 `1080x1920` ↔ `1920x1080`）時，自動等比縮放並補齊標準 BT.709 黑邊輸出至固定 `1920x1080` 畫布，徹底杜絕 OBS / Discord 因解析度突變而閃退、黑畫面或比例拉扯變形。

4. **無鎖共享記憶體 + 雙路通用虛擬擷取卡 (`src/dshow/`、`src/common/shared_ipc.cpp`)**
   - `SharedCaptureProducer` / `SharedCaptureConsumer`：採用三緩衝（Triple-Buffered）`NV12` 視訊槽與單寫多讀（SPMC）`48000 Hz` 立體聲環形緩衝區，允許 OBS、Discord、Zoom 同時讀取而不互相搶佔音訊樣本。
   - `ios_vcam_dshow.dll`（零外部執行期 DLL 依賴）：同時註冊兩張標準 DirectShow 擷取裝置：
     - **`iOS USB Capture Card (Video)`**：支援 `1920x1080@60fps` / `30fps` 與 `1280x720@60fps`，提供 `NV12`（OBS 最優）、`YUY2`（Discord / Zoom / WebRTC 最優）與 `RGB24` 相容格式，未連線時自動輸出動態待機彩條。
     - **`iOS USB Capture Card (Audio)`**：實作 `IAMBufferNegotiation` 10ms 超低延遲 `48kHz / 16-bit / Stereo PCM` 輸出，OBS 可直接掛載為擷取卡自訂音訊裝置。
   - `WasapiAudioRouter`：若電腦裝有 `VB-Audio Virtual Cable`，`ios_vcam_host.exe` 會自動偵測並將音訊低延遲路由至 `CABLE Input`，讓僅支援核心態麥克風的軟體也能直接收音。

---

## 二、快速啟用流程（使用預編譯 `dist-win64`）

1. **前置準備**：
   - Windows 10 / 11 (x64) 安裝 **Apple 裝置 (Apple Devices)** 或 **iTunes**（確保具備 Apple Mobile Device USB 信任配對服務）。
   - `dist-win64\` 已內建預編譯好的主程式與官方簽章之 `libusb-1.0.dll`、`drivers\libusb0.sys`、`drivers\libusb0.dll`（無須開啟 Windows 測試模式）。
2. **插上 iPhone 並執行管理員安裝**：
   - 使用 USB 傳輸線連接 iPhone / iPad，解鎖手機螢幕並點擊 **「信任這部電腦」**。
   - 對 `dist-win64\install_all_admin.ps1` 按右鍵 → **「使用 PowerShell 執行」**（腳本會自動請求系統管理員權限，完成 DirectShow 擷取卡註冊與安全 `libusb0` UpperFilter 掛載）。
3. **啟動擷取背景主程式**：
   - **無手機自測模式**（驗證 OBS / Discord 視訊與音訊管線）：
     ```powershell
     .\ios_vcam_host.exe --mock
     ```
   - **正式 iOS 有線擷取**（預設啟用 QuickTime Valeria 乾淨狀態列 `9:41` 滿格電量）：
     ```powershell
     .\ios_vcam_host.exe
     ```
   - **保留手機真實時間與電量列**：
     ```powershell
     .\ios_vcam_host.exe --mode clean
     ```
4. **在 OBS Studio / Discord / Zoom 選取擷取卡**：
   - **OBS Studio**：新增「視訊擷取裝置」→ 裝置選擇 **`iOS USB Capture Card (Video)`**；往下捲動勾選「使用自訂音訊裝置」→ 選擇 **`iOS USB Capture Card (Audio)`**。
   - **Discord / Zoom**：視訊鏡頭選擇 **`iOS USB Capture Card (Video)`**；若安裝了 VB-Audio Virtual Cable，麥克風可選擇 `CABLE Output (VB-Audio Virtual Cable)`。

---

## 三、命令列參數一覽 (`ios_vcam_host.exe`)

| 參數 | 說明 |
| :--- | :--- |
| `--mock` | 免插手機輸出 `1920x1080@60fps` 動態彩條與 `48kHz` 雙聲道和弦測試音 |
| `--mode valeria` | （預設）啟用 Apple Valeria 乾淨狀態列（時間固定 9:41、滿電量訊號） |
| `--mode clean` | 保留 iPhone 真實時間、電量與通知狀態列 |
| `--udid <序號片段>` | 連接多台 iPhone / iPad 時，指定目標裝置的 Serial / UDID 片段 |
| `--audio-out <名稱>` | 指定 WASAPI 音訊轉發目標播放裝置名稱片段（預設自動尋找 `CABLE Input`） |
| `--no-audio-route` | 停用 WASAPI 自動音訊轉發（僅透過 DirectShow Audio 擷取卡輸出） |
| `--list-audio` | 列出目前 Windows 所有可用音訊播放端點名稱並離開 |
| `--sw-decode` | 強制使用 CPU 軟體解碼 H.264（預設優先使用 `D3D11VA` GPU 硬體解碼） |

---

## 四、自行從原始碼編譯

### 方法 A：於 Windows 使用 Visual Studio 2022 編譯
安裝 Visual Studio 2022（勾選「使用 C++ 的桌面開發」）與 CMake 後，直接執行：
```bat
scripts\build_windows.bat
```
編譯完成之執行檔與單元測試會自動輸出至 `dist-win64\` 並執行自動化測試驗證。

### 方法 B：於 Linux 使用 MinGW-w64 交叉編譯 Windows x64 成品
```bash
cmake -S . -B build-win64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win64 -j$(nproc)
```

---

## 五、完整移除方式

執行 `dist-win64\uninstall_all_admin.ps1`（或以管理員執行 `ios_vcam_driver_setup.exe --uninstall`），即可一鍵移除 Apple 複合裝置上的 `libusb0` UpperFilter 並反註冊 DirectShow 虛擬視訊與音訊擷取卡，不殘留任何登錄檔設定。
