# iOS USB 通用有線虛擬擷取卡 (`ios-usb-vcam`) — Windows 10 / 11

純 C++20 打造的 **Windows 10 / 11 iPhone / iPad 有線 USB 低延遲虛擬擷取卡（視訊 + 音訊）**，附帶 **一鍵智慧圖形化控制台 (`ios_vcam_gui.exe`)**。  
透過 Apple 底層 QuickTime / Valeria (`SubClass 0x2A`) 有線螢幕鏡像協定，免購買實體擷取卡、免越獄、免安裝手機端 App、免開啟 Windows 測試模式、**全程免打指令**，即可將 iPhone / iPad 畫面與 48kHz 立體聲音訊以 **1080p60** 直接輸入 **OBS Studio、Discord、Zoom、Teams、WebRTC 瀏覽器**等任何軟體。

---

## 一、超簡單「一鍵啟動」教學（全程圖形介面、免打指令）

1. **前置準備**：
   - Windows 10 / 11 (x64) 電腦安裝 **Apple 裝置 (Apple Devices)** 或 **iTunes**（提供 Apple 原廠 USB 信任配對服務）。
   - 下載本專案右上角綠色 **`<> Code`** ➔ **`Download ZIP`** 並完整解壓縮。
2. **雙擊開啟控制台**：
   - 直接雙擊根目錄的 **`點我啟動_iOS擷取卡控制台.bat`**（或雙擊 `dist-win64\ios_vcam_gui.exe`）。
3. **點擊一顆按鈕自動完成所有步驟**：
   - 用 USB 線接上 iPhone / iPad，解鎖手機螢幕並按 **「信任這部電腦」**。
   - 在控制台點擊 **`🚀 智慧一鍵啟動`** 按鈕：
     - 程式會**自動檢測系統環境**：若為首次使用，會自動彈出管理員授權完成 DirectShow 視訊/音訊虛擬擷取卡註冊與微軟 WHQL 簽章 `libusb0` 過濾層掛載，接著**自動開始 1080p60 有線投影擷取**！
     - 下次使用時驅動已就緒，點擊按鈕即可秒速開始投影，結束時點擊 **`■ 停止投影擷取`** 即自動切回一般充電/傳輸模式。
   - 手邊暫時沒插手機？點擊 **`🧪 免插手機自測 (彩條+測試音)`** 即可一鍵發送 1080p60 動態彩條與雙聲道測試音測試 OBS！
4. **在 OBS Studio / Discord 選取擷取卡**：
   - **OBS Studio**：新增「視訊擷取裝置」➔ 裝置選擇 **`iOS USB Capture Card (Video)`**；往下捲動勾選「使用自訂音訊裝置」➔ 選擇 **`iOS USB Capture Card (Audio)`**（控制台內亦可點擊 `📋 OBS 設定教學` 一鍵複製裝置名稱）。
   - **想直接用電腦耳機／喇叭聽手機遊戲聲音？** 在控制台「電腦端音訊監聽／轉發」下拉選單直接選取您的耳機或喇叭即可同步零延遲收聽！

---

## 二、核心架構亮點

1. **一鍵智慧圖形化控制台 (`src/gui/gui_main.cpp` — `ios_vcam_gui.exe`)**
   - 自動解除瀏覽器下載 ZIP 的檔案封鎖標記 (`Zone.Identifier`)。
   - 即時偵測 DirectShow 註冊狀態（資料夾搬移亦會自動偵測並一鍵更新路徑）、`libusb0` 簽章服務與 iPhone USB 熱插拔狀態（若曾誤用 Zadig 覆蓋父驅動亦會即時紅字告警）。
   - 支援下拉切換「乾淨直播狀態列（固定 9:41 滿電量）」或「保留手機真實時間電量列」，以及選擇任意電腦喇叭／耳機／VB-Cable 同步播放音訊。

2. **Apple QuickTime / Valeria 二進位協定引擎 (`src/protocol/`)**
   - `CoreMediaCodec`：完整實作 Little-Endian TLV 字典序列化（`DICT` / `KEYV` / `STRK` / `NMBV`）、`CMTime`、`AVCDecoderConfigurationRecord` (`avcC`) 解析、SPS 指數哥倫布（Exp-Golomb）解析度提取，以及 `AVCC` 轉 `Annex-B` NALU 封包轉換。
   - `QtStreamSession`：完整實作 `PING` → `SYNC CWPA` → `ASYN HPD1/HPA1` → `SYNC AFMT` → `SYNC CVRP` → `CLOK/TIME/SKEW` 時鐘同步與 `FEED`（H.264 視訊）/ `EAT!`（48kHz LPCM 音訊）串流狀態機。

3. **安全 USB 過濾層與 GPU 低延遲硬解 (`src/usb/`、`src/driver_tool/`、`src/decoder/`)**
   - **不破壞 iTunes 信任配對**：透過 `SetupAPI` 將微軟 WHQL 簽章之 `libusb0.sys` 安全掛載為 Apple 複合父裝置的 `SPDRP_UPPERFILTERS`，完整保留原廠 `usbccgp` / `usbaapl64`。
   - **D3D11VA 硬體解碼與橫豎屏無縫轉換**：直接調用 Windows Media Foundation H.264 MFT (`MF_LOW_LATENCY`)，手機直橫屏旋轉時自動等比縮放並補齊標準 BT.709 黑邊輸出至 `1920x1080` 畫布，OBS / Discord 絕不閃退或變形。

4. **無鎖共享記憶體 + 雙路通用虛擬擷取卡 (`src/dshow/`、`src/common/shared_ipc.cpp`)**
   - 三緩衝 `NV12` 視訊槽 + 單寫多讀（SPMC）`48kHz` 立體聲環形緩衝區，支援 OBS、Discord、電腦耳機同時讀取。
   - `ios_vcam_dshow.dll` 同時註冊 **`iOS USB Capture Card (Video)`**（支援 `NV12` / `YUY2` / `RGB24`）與 **`iOS USB Capture Card (Audio)`**（10ms 超低延遲 `48kHz` PCM）。

---

## 三、進階命令列參數（選用，`ios_vcam_host.exe`）

若需整合至自動化腳本，亦可直接呼叫命令列背景程式：

| 參數 | 說明 |
| :--- | :--- |
| `--mock` | 免插手機輸出 `1920x1080@60fps` 動態彩條與 `48kHz` 雙聲道和弦測試音 |
| `--mode valeria` | （預設）啟用 Apple Valeria 乾淨狀態列（時間固定 9:41、滿電量訊號） |
| `--mode clean` | 保留 iPhone 真實時間、電量與通知狀態列 |
| `--udid <序號片段>` | 連接多台 iPhone / iPad 時，指定目標裝置的 Serial / UDID 片段 |
| `--audio-out <名稱>` | 指定 WASAPI 音訊轉發目標播放裝置名稱片段（預設自動尋找 `CABLE Input`） |
| `--no-audio-route` | 停用 WASAPI 額外音訊轉發（僅透過 DirectShow Audio 擷取卡輸出） |
| `--sw-decode` | 強制使用 CPU 軟體解碼 H.264（預設優先使用 `D3D11VA` GPU 硬體解碼） |

---

## 四、完整移除方式

開啟 `ios_vcam_gui.exe` 點擊 **`🗑 完整移除`** 按鈕（或執行 `dist-win64\uninstall_all_admin.ps1`），即可一鍵移除 Apple 複合裝置上的 `libusb0` UpperFilter 並反註冊 DirectShow 虛擬視訊與音訊擷取卡，不殘留任何設定。
