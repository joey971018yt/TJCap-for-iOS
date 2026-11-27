# 版本更新日誌 (Changelog)

本文件記錄 **TJCap for iOS**（原 `JCast for iOS` / `iOS USB Capture Card`）各版本的功能新增、架構優化與問題修復。

---

## [v2.0.0] - 2026-11-27 — 正式更名「TJCap for iOS」全新品牌版本

### 🏷️ 品牌正式更名與升級相容 (Rebrand & Seamless Upgrade)
- **正式更名為「TJCap for iOS」**：
  - 全面更新主程式檔名（`TJCap for iOS.exe`）、Win32 檔案版本資源、Manifest 識別名稱、Windows 安裝程式（`TJCap-for-iOS-Setup-v2.0.0.exe`）、免安裝便攜包（`TJCap-for-iOS-Portable-v2.0.0.zip`）、自動更新引擎與 GitHub 專案連線位址（`joey971018yt/TJCap-for-iOS`）。
  - **DirectShow 虛擬擷取卡同步更新**：顯示名稱更新為 **`TJCap for iOS (Video)`** 與 **`TJCap for iOS (Audio)`**，並**嚴格保留原本的底層 COM `CLSID`**，無縫銜接既有 OBS 場景。
  - **診斷報錯、截圖與獨立視窗全面對齊**：錯誤碼更新為 **`TJCAP-E101` ～ `TJCAP-E503`**（內部解析器維持向下相容舊版 `JCAST-` 代碼與 `[JCAST-DIAG|` 日誌）、預設截圖路徑更新為 **`%USERPROFILE%\Pictures\TJCap Screenshots\TJCap_*.png`**、獨立外接螢幕視窗更新為 **`TJCap Live Monitor`**。
- **舊版偏好繼承與無殘留覆蓋升級**：
  - 語言設定登錄檔遷移至 `HKCU\Software\TJCapForiOS\Language`，首次讀取時若尚未建立會自動繼承舊版 `HKCU\Software\JCastForiOS\Language` 偏好。
  - Inno Setup 安裝程式保留同一 `AppId`，覆蓋升級時透過 `[InstallDelete]` 自動清理舊版 `JCast for iOS.exe` 與舊捷徑；便攜版自更新腳本改為自動偵測單一頂層目錄並支援清除舊版執行檔。

---

## [v1.9.0] - 2026-11-27 — 介面語言設定選項（繁體中文 / English 即時切換）

### ✨ 新增功能 (Added)
- **多語系偏好引擎與免管理員權限持久化 (`app_lang.h` / `app_lang.cpp`)**：
  - 支援 **「🌐 自動 (跟隨系統) / Auto (System)」**、**「繁體中文」** 與 **「English」** 三種設定模式；自動模式會依 Windows `GetUserDefaultUILanguage()` 智慧對應顯示語系，手動偏好安全儲存於 `HKCU\Software\JCastForiOS\Language`（安裝版與免安裝便攜版皆免 UAC 權限讀寫）。
- **控制台「🌐 語言 / Language」下拉選單與免重啟即時切換 (`ApplyLanguageToUi`)**：
  - 切換語言後立即無縫更新主控制台標題、所有按鈕／核取方塊、狀態橫幅、解析度／狀態列／音訊下拉選項（完整保留目前選取項目）、4 欄式串流健康度儀表板，以及已開啟的 **`JCast Live Monitor` 獨立大視窗**（標題列、全螢幕 HUD、右鍵選單），即使在投影擷取途中切換亦絕不中斷直播。
- **全介面 100% 雙語化（繁體中文 / English）**：
  - 包含 18 組故障錯誤碼 (`JCAST-E101` ～ `JCAST-E503`) 完整英文根因解析與修復步驟、8 項系統自檢報告、Windows 程序結束碼翻譯、GitHub Issue 一鍵報錯範本、OBS 設定教學與 GitHub 自動更新視窗 (`UpdateModalWindow`)。

---

## [v1.8.0] - 2026-11-27 — 智慧故障偵測、GPU 自動軟解自癒與一鍵隱私脫敏報錯中心

### ✨ 新增功能 (Added)
- **結構化全鏈路故障偵測引擎 (`diagnostics.h` / `diagnostics.cpp`, `JCAST-E101` ～ `JCAST-E503`)**：
  - **啟動前 8 項深度系統預檢 (`ProbeSystemHealth`)**：自動檢查 5 項核心檔案完整性 (`E101`)、Apple 配對驅動與服務 (`E102`)、Windows Media Foundation H.264 系統解碼器 (`E103`，防範 Windows N/KN 閃退)、DirectShow 鏡頭註冊狀態 (`E104`) 與 **Zadig 覆蓋父裝置原廠驅動衝突 (`E201`)**。
  - **USB 傳輸與 QuickTime 握手精準判讀**：將底層 `libusb` 錯誤與協議狀態轉譯為明確代碼，區分權限不足 (`E203`)、通道遭其他軟體佔用 (`E204`)、手機未解鎖／未點信任電腦 (`E301`)、USB 組態切換失敗 (`E302`)、8.5 秒零影格握手逾時看門狗 (`E303`) 與線材接觸不良 I/O 瞬斷 (`E304`)，並修復 iOS 靜態畫面長時間零封包時的心跳保活 (`session_watchdog`)。
  - **背景程序異常終止攔截 (`E503`)**：自動區分使用者手動停止與非預期結束，即時解析 Windows `NTSTATUS` 結束代碼（如 `0xC0000135` 缺系統 DLL、`0xC0000005` 存取違規、`0xC000007B` 位元不符）並主動彈出診斷中心。
- **🛡️ GPU `D3D11VA` 異常「零中斷自動降級 CPU 軟解自癒 (`E402`)」與音訊斷線保護 (`E501`)**：
  - 當顯示卡硬體解碼初始化失敗或直播途中發生顯示卡驅動重置 (`TDR`) 連續解碼錯誤時，自動無縫切換至 CPU 軟體解碼繼續輸出，直播不黑屏、不中斷；耳機中途拔除 (`E501`) 時發出溫和提示且不影響 OBS 音訊擷取。
- **🩺 互動式「診斷自檢與一鍵報錯中心 (`DiagnosticsModalWindow`)」＋ 🔒 隱私自動脫敏 (`RedactSensitiveReportText`)**：
  - 主視窗新增 **`🩺 診斷報錯`** 按鈕（偵測到異常時動態顯示 **`⚠️ 診斷報錯 (N)`** 計數），儀表板第 4 欄亦支援點擊直達診斷中心並即時高亮錯誤碼。
  - 自動遮罩報告中的電腦使用者帳號路徑 (`Users\<USER>\`) 與 iPhone / iPad 裝置序號 (`Serial` / `UDID`)，提供 **`🔧 一鍵修復驅動`**、**`📋 複製脫敏報告`**、**`💾 匯出報錯檔 (.txt)`** 與 **`🐞 開啟 GitHub 回報問題`**（自動預填版本與錯誤碼開啟 GitHub New Issue）。

---

## [v1.7.0] - 2026-11-27 — 即時串流健康度儀表板（FPS / 碼率 Mbps / 解碼延遲 ms）與全螢幕 HUD

### ✨ 新增功能 (Added)
- **無鎖零拷貝即時串流健康度遙測管線 (`SharedCaptureConsumer::ReadStreamHealth` & `EvaluateStreamHealthGrade`)**：
  - 於不變動 `SharedCaptureLayout` / `VideoSlotHeader` (`64 bytes`) 二進位佈局與相容性的前提下，擴充微秒級 H.264 解碼耗時 (`decode_latency_us`)、整體管線延遲 (`pipeline_latency_us`)、`0.5s` 高靈敏度滑動視窗幀率 (`fps_x100`)、累計封包流量與音訊封包計數。
- **控制台內建「📊 四欄式即時串流健康度儀表板 (`PaintTelemetryDashboard`)」**：
  - **🎞 即時幀率 (`FPS`)**：即時顯示 `FPS`、28 點即時折線圖 (`Sparkline`)、累計影格與異常掉幀數。
  - **📡 傳輸碼率 (`Mbps`)**：即時顯示 `Mbps`、動態峰值 `Mbps`、累計傳輸流量（`MB` / `GB`）與即時折線圖。
  - **⚡ 解碼與管線延遲 (`ms`)**：精確至 `0.1 ms` 顯示端到端處理延遲、純 H.264 解碼耗時、60fps 影格預算占比（`%`）與硬體加速標籤（`D3D11VA` / `CPU軟解`）。
  - **🩺 智慧串流健康度診斷**：自動判讀 **`🟢 極佳 · 低延遲`**、**`🔵 靜態省電 VFR`**（智慧識別 iOS 畫面靜止時的正常 VFR 省電降頻）與 **`🟡 解碼負載偏高／頻寬受限`**。
- **獨立大視窗／無邊框全螢幕「即時健康度 HUD 資訊列 (`Tab` / `F3`)」**：
  - `LiveMonitorWindow` 新增可隨時按 **`Tab`** 或 **`F3`**（及右鍵選單）切換顯示／隱藏的即時 `FPS / Mbps / 延遲 ms` HUD 懸浮資訊膠囊。

---

## [v1.6.0] - 2026-11-26 — 內建低負載即時預覽、獨立全螢幕外接螢幕視窗與一鍵 2K+ 原畫質無損截圖

### ✨ 新增功能 (Added)
- **控制台內建「極低負載即時預覽視窗 (`👁 內建即時預覽`)」**：
  - 直接從三重緩衝共享記憶體（`IosUsbVcamSharedMemV2`）零中繼拷貝雙線性取樣至顯示區塊，並在 `frame_seq` 未變動時完全略過像素轉換（待機／靜態畫面 `0.00%` CPU 負載）；提供一鍵勾選開關，隨時可暫停預覽以保留 100% 系統資源給遊戲與 OBS。
- **獨立大視窗／無邊框全螢幕模式 (`🖥 獨立大視窗`, `LiveMonitorWindow`)**：
  - 點擊「`🖥 獨立大視窗`」或雙擊預覽畫面即可彈出獨立外接螢幕視窗，自動依 iPhone / iPad 直橫屏比例調整初始視窗尺寸，支援任意拖曳縮放、右鍵選單「依手機原生比例自動貼合視窗」、`F11` / `Alt+Enter` / 雙擊切換無邊框全螢幕（`Esc` 退出）與 `Ctrl+T` 視窗永遠置頂，免開 OBS 即可將電腦當作 iOS 有線零延遲監視器。
- **📸 一鍵 1:1 原畫質無損截圖 (`F12` / `Ctrl+S`) + 自動複製剪貼簿**：
  - 無論預覽視窗縮放大小或是否暫停內建預覽，點擊「`📸 原畫質截圖`」（或於主視窗／獨立大視窗按下 `F12`）皆直接擷取共享記憶體中 **1:1 原始 2K+ 解析度影格**（如 `2868×1320`）。
  - 透過 Windows 原生 `WIC` (`WindowsCodecs`) 編碼為**無損 PNG** 儲存至 `%USERPROFILE%\Pictures\JCast Screenshots\JCast_YYYYMMDD_HHMMSS_mmm_WxH.png`（點擊「`📂 圖庫`」即可一鍵開啟資料夾），並**同步寫入 Windows 剪貼簿 (`CF_DIB`)**，截圖後立即可於 Discord、LINE、Photoshop 直接 `Ctrl+V` 貼上。

---

## [v1.5.1] - 2026-11-26 — Windows 系統安全性全面稽核與加固

### 🔒 安全性強化 (Security)
- **防範 DLL 搜尋順序劫持 (`CWE-427`)**：
  - 主程式 (`JCast for iOS.exe`)、背景擷取服務 (`ios_vcam_host.exe`) 與高權限驅動設定工具 (`ios_vcam_driver_setup.exe`) 啟動時全面呼叫 `SetDllDirectoryW("")` 與 `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` 移除工作目錄搜尋，並改用 `LoadLibraryExW` 鎖定應用程式目錄絕對路徑載入 `libusb-1.0.dll` 與 `ios_vcam_dshow.dll`。
- **更新器網域白名單、防路徑穿越 (`CWE-22`) 與執行檔絕對路徑 (`CWE-426`)**：
  - `DownloadReleaseAssetHttps` 除強制 HTTPS 與 `SHA-256` 雜湊校驗外，新增 `github.com` / `*.githubusercontent.com` 官方網域白名單限制；暫存檔名強制經 `.filename()` 純化；開啟外部網頁強制驗證 `https://github.com/` 前綴；便攜版自動替換強制鎖定 `System32` 絕對路徑啟動 PowerShell。
- **啟用 Windows 10 / 11 硬體級記憶體防護旗標**：
  - MSVC 建置全面啟用控制流防護 (`/guard:cf` CFG)、硬體強制堆疊保護 (`/CETCOMPAT` Intel CET / AMD Shadow Stack)、高熵 64 位元位址空間配置隨機載入 (`/HIGHENTROPYVA` `/DYNAMICBASE`) 與資料執行防止 (`/NXCOMPAT` DEP)。
- **開源合規性聲明 (`THIRD_PARTY_NOTICES.md`)**：
  - 新增第三方開源元件授權說明文件，明確標示動態載入之 `libusb-1.0.dll` (`LGPL-2.1`) 與獨立簽章過濾驅動 `libusb0.sys` (`GPL-3.0`)。

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
