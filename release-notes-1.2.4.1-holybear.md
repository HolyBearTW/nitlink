# NitLink 1.2.4.1 HolyBear Edition

**非官方社群整合版（Unofficial Community Edition）。**

以 NitLink 1.2.3 為基礎，由 HolyBearTW 整合社群修改，並非 nitlink-dev 官方發布的版本。

## 本次更新

### FPS 與 HUD

- 調整 FPS HUD 的資訊配置、文字與背景，主要數字顯示 Content FPS 估計值；無法估計時顯示未知，避免與擷取或提交幀率混淆。
- 新增輔助 FPS 顯示切換按鈕，可展開或隱藏 Capture／Present FPS。按下 **Tab** 也可切換，僅在 **Ctrl+F3** 開啟 HUD、F1 設定關閉且非子母畫面時生效。
- 改善自動 Present Cap 對顯示器更新率與切換顯示器的處理，保留 59.94 Hz 等非整數更新率；自動模式無法取得更新率時採用保守的 30 Hz fallback，避免意外無上限提交。
- 調整等待後的提交排程，避免延遲後連續追趕提交。

Content FPS 是 FrameDiffer 的估計值，不是直接讀取遊戲 FPS。Capture FPS 是實際收到的擷取影格速率；Present FPS 是成功提交至 DXGI 的速率，不等於螢幕實際更新次數。

### Discord 與 HDMI 來源

- 修正單一 Unicode 字元來源名稱造成訊號資訊重複的問題。未選擇遊戲時，例如來源「熊」，Details 顯示 `來源：熊`，訊號資訊集中在 State；英文及簡體中文會使用對應的來源前綴。
- 一般來源名稱如 `Switch 2`、`Steam Deck` 維持原本名稱，不加多餘前綴。沒有可用訊號資訊時仍保留有效來源名稱，不補造 HDR、SDR、VRR 或解析度資訊。
- 修正「其他」來源未填寫名稱時的處理，改為沿用自動來源辨識。保留既有遊戲名稱、圖片與時間資訊。

### 設定與關於頁面

- 修正 F1 設定狀態的 JSON 組合與序列化錯誤，讓原生狀態正確傳到 WebView2，恢復相關設定與資訊更新，並正確處理特殊字元。原有安全驗證繼續保留。
- 「技術資訊」與「貢獻者」改為兩個獨立區塊，均預設展開，可分別以滑鼠或鍵盤收合。
- 移除 About 內重複的版本欄位，將上次檢查時間集中至「軟體更新」，依所選語言顯示日期。新版本提示及正確的 GitHub Release 連結繼續保留。
- 更新檢查遇到服務限制或錯誤時，提供更明確的狀態；自動檢查仍預設關閉，啟用後每 24 小時最多一次，不自動下載或安裝更新。
- 音訊「重新整理」按鈕補上滑過與按下效果。點擊後顯示「重新整理中」，收到對應的診斷資料後才顯示「已更新」；未收到更新資料時顯示「暫時無法更新」。
- 移除貢獻者區塊下方多餘的分隔線。

### 安裝介面

- 改善安裝程式的品牌圖像、文字對齊與完成頁對比，讓三種語言與不同 DPI 下的資訊更容易閱讀。

## 相容性與使用方式

- 支援 Windows 10 1809 以上／Windows 11 x64，需要 DirectX 11 GPU、適當的擷取卡與音訊驅動，以及 Microsoft Edge WebView2 Evergreen Runtime。
- 提供完整免安裝 ZIP 與 Windows Setup。免安裝版請解壓縮至可寫入的資料夾，保留隨附資源後執行 `NitLink.exe`；Setup 升級及解除安裝會保留使用者設定，不自動遷移原版 NitLink 安裝。
- 本版沿用 1.2.4 的 GC553Pro／HDMI 整合與 CN_115 低延遲音訊功能，沒有新增音訊引擎移植。WASAPI 獨占輸出仍會在失去前景焦點時退回共享模式；FIFO、Sinc SRC 與自適應時鐘漂移補償維持既有行為。
- VC++ 執行階段與第三方資源保留原有授權及聲明。音訊獨占模式預設關閉，FIFO／播放目標維持 12／10 ms。

## 測試與驗證

- 發版工作流程會執行乾淨的 x64 Release 建置、完整 CTest（含 WebView2 Runtime）、Portable ZIP／NSIS 封裝，以及三種語言的安裝、升級與解除安裝自動化檢查。實際結果記錄於本文末尾。
- 既有 1.2.4 安裝版已完成 Switch 2 → GC553Pro 的五分鐘獨占音訊觀測，正式觀測期間沒有新增丟棄幀、重新同步或欠載。這項結果只適用於當次設備與設定，不代表 1.2.4.1 正式產物已完成完整硬體驗證。
- 正式發布時會從新 Tag 重新建置並執行完整測試、封裝及 GPG 驗證，將實際 Commit、Tag、CTest 成績與 Actions 連結自動附於本文末尾。不得略過測試；若命中既有、明確記錄的 WebView 基準失敗，必須如實列出，任何其他失敗均阻止發布。
- 最終 ZIP、Setup 與 `SHA256SUMS.txt` 提供 GPG 分離式簽章，並在發布前及上傳後驗證。請參閱[簽章驗證說明](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/docs/HOLYBEAR-RELEASE-SIGNING.md)。

## 已知限制

- 尚未完成整合版所有 GC553Pro HDR／SDR 切換、No Signal 與輸入至螢幕延遲的完整實機回歸。Discord 的真正 RPC JSON 已納入自動化驗證，但本次尚未取得新版 Activity 的實際畫面驗證。
- 五分鐘音訊觀測不涵蓋長時間運作、所有焦點切換、影音同步與音質。較低緩衝不適用於所有設備；FIFO 水位及 WASAPI 週期不等於端對端音訊延遲。
- 既有 WebView 重新啟動基準測試問題仍保留記錄。其他環境測試通過，不代表該問題已修復。
- 關閉 Low Latency 時曾觀察到的緩衝區覆寫原因仍未查明。FrameDiffer 的估計可能與遊戲實際節奏不同，本版不宣稱精確的效能提升或延遲降低數值。
- EXE 與 Setup 尚無受信任的 Windows Authenticode 發行者簽章；GPG 簽章不會消除 SmartScreen 警告或 Defender 誤判。長期升級與所有安裝互動頁面仍需更廣泛測試。

## 來源、授權與致謝

- **原始專案：**[nitlink-dev/nitlink](https://github.com/nitlink-dev/nitlink) 及 NitLink 的所有貢獻者。
- **社群修改來源：**[#23](https://github.com/nitlink-dev/nitlink/pull/23) 的部分 HDMI／VRR／重新同步改善，以及 [#31](https://github.com/nitlink-dev/nitlink/pull/31) 的 FPS／Present Cap 改善。與 [#25](https://github.com/nitlink-dev/nitlink/pull/25) 共用的手動來源及 Discord 功能僅整合一次。
- **低延遲音訊與簡體中文：**[CN_115（@CN115）](https://github.com/CN115)，來源為 [CN115/nitlink-lowlatency v0.1.2](https://github.com/CN115/nitlink-lowlatency/tree/v0.1.2)。
- **HolyBear Edition 整合與發布：**[聖小熊（@HolyBearTW）](https://github.com/HolyBearTW)。

原始 [MIT 授權](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/LICENSE)、[第三方授權](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/LICENSES.md)與[致謝](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/ACKNOWLEDGMENTS.md)均完整保留；Microsoft 元件遵循各自授權，未改為 MIT。確切來源 Commit、作者及音訊移植紀錄請參閱 [HOLYBEAR-SOURCES.json](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/HOLYBEAR-SOURCES.json) 與 [docs/cn115](https://github.com/HolyBearTW/nitlink/tree/holybear/integrated/docs/cn115)。

## English Summary

This unofficial maintenance update improves FPS reporting and controls, Present Cap scheduling, short HDMI source names in Discord, F1 settings, About, audio diagnostics refresh feedback, and installer readability. Existing CN_115 audio features are retained.
