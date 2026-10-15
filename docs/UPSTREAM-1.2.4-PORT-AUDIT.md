# 官方 NitLink v1.2.4 選擇性移植盤點

本次由最新遠端 `holybear/integrated` 的 `25307cfde47cda5171fac94de326ebbb8b504283`
盤點，官方來源固定為 `v1.2.4` / `0a093cc8e41fd04ffe13196122dc1a03dc9a0d6a`。
以 `git fetch --no-tags` 取得獨立稽核 ref，未改動任何既有 Tag。

依使用者後續指示，實際整合位置改為目前的 `holybear/game-search-presence` 工作區。
先將原有 226 個未提交檔案完整備份，提交為 `c364c3daf11b813f6f20a1a225b2eb408392f2fb`，
再加入這次修正。未將新修改合併回 `holybear/integrated`，也未 Push、建立 PR 或發布。
獨立的原始 Games 基準 worktree 僅供重現既有編譯問題。

## A. 官方 1.2.4 差異盤點

分類 1：HolyBear 已具備；2：官方新增且適用；3：須保留 HolyBear 特有架構；4：版本／品牌／發布文件。
以兩者共同祖先 `01e822dffd48ff8dff80ba12c55522bb04ea19e9` 盤點官方新增內容，
共 62 個變動路徑，其中 23 個檔案已與 HolyBear 整合基準逐位元組相同。
完整檔案清單與每檔分類見本次驗證資料夾的 `upstream-file-inventory.tsv` / `.json`。

| 項目 | 分類 | 本次處理 |
| --- | --- | --- |
| NoiseFloorEstimator、FrameDiffer 接線及官方測試 | 2 | 移植；估計器保留官方原文；新增 GPU／pacing 回歸 |
| 4K X NV12 tone mapping／P010 passthrough | 2、3 | 移植語意，保留 GC553Pro、4K S 路徑；補首次辨識與實際格式確認 |
| F1 隱藏後桌面殘影 | 2、3 | 移植隱藏／縮零／composition refresh；加強重複隱藏及 HWND 生命週期 |
| #31 Content FPS、非整數更新率、Present Cap deadline、Unknown 分類 | 1 | 已具備，保留現有程式與所有既有斷言 |
| CN_115 WASAPI、低延遲／獨占／焦點回退、drift／conversion／recovery | 1 | `src/audio` 與官方 v1.2.4 完全相同，不重複移植 |
| 官方音訊預設 40／30 及對應預設值測試 | 3 | 不移植；保留 HolyBear 12／10 與既有測試 |
| 官方標題列以擷取／顯示模式標示 HDR／SDR | 3 | 不覆蓋；保留 HolyBear 已確認的 HDMI IN EOTF、Timing、VRR |
| 官方簡化的設定狀態、Discord idle／固定 PS5、來源名稱 | 3 | 不覆蓋；保留原生安全序列化、手動來源、Unicode、遊戲庫與計時 |
| 官方 Full 設定、About、貢獻者差異 | 3、4 | 保留 Full Wide、Docked、HolyBear About／完整致謝／更新檢查 |
| 官方版本號／RC 資源／HUD 版本字樣／Release Notes | 4 | 不套用官方版本字樣；依後續指示同步升為 HolyBear 1.2.5，未發布 |
| Steam 宣傳、商店連結與橫幅 | 4 | 不移植；未加入 Steam 商店導流或 Frame Generation 功能 |
| 官方封裝／CI、刪除音訊 fork 文件 | 3、4 | 保留 HolyBear Setup、Portable、簽章／SHA256／來源驗證流程與致謝 |

未發現上述三項以外尚缺的適用實質修正。CN_115 與 #31 已存在；其餘差異主要是 HolyBear
新增功能、保護條件、顯示語意及發布系統，不能藉由整檔替換抹除。

## B. 成功移植的功能

### 自適應重複影格雜訊門檻

- 保留官方 16 筆觀察視窗、每 8 筆重新估計、至少 13／15 次交替、2.5 倍高低帶分離、
  1.6 倍低帶寬度限制、0.002 雜訊帶上限，以及 1.4／2.0 倍上下界。
- 只在實際 GPU readback 成功後學習；原有非阻塞比較、hysteresis、tile rescue、三影格平滑不變。
- `FrameDiffer::Reset()` 同時清除估計器及記錄狀態；既有 capture reopen 同時重設 differ 與 SourceCadence。
- 官方測試完整保留並註冊 CTest，另加非法輸入、分組／寬雜訊帶、慢動作及重設後重新取樣。
- WARP 測試經真正 FrameDiffer shader／readback，使用等效 NV12 R8、P010 R16 與 SDR BGRA 輸入，
  檢查 noisy／exact 30 FPS、真 60 FPS、小幅慢動作、靜態、tile rescue、Content HUD 與 Source pacing。
- R16 測試驗證的是 P010 在 differ 使用的單通道 luma 介面；不代表實體 HDR 擷取或 HDR 螢幕驗證。

### Elgato 4K X HDR／SDR

- 開啟前按嘗試格式設定、P010 失敗重試時切回 SDR，開啟成功後再按 `GetOutputFormat().subtype` 確認。
- 兩條流程 `Initialize`、`ReconcileCaptureFormat` 都涵蓋；P010 關閉卡端 tone mapping，非 P010 開啟。
- 4K X 辨識移到第一次 Open 之前，採不分大小寫、同時具備 Elgato 與 4K X 名稱的既有安全條件。
- 重用原有 `SetElgatoTonemap`，沒有新增 XU 命令或改動 GC553Pro mailbox／4K S HID。
- 命令不支援／失敗會記錄硬體狀態未確認，並不把函式呼叫當成硬體成功。
- 上游固定來源說明 4K X 的 4K P010 為 30 FPS，4K120 是 8-bit；這是上游格式限制說明，並非本次實機測量。
- policy 測試以注入 sender 驗證首次嘗試、成功但降級、失敗重試、重新開啟與其他裝置隔離。

### F1 設定殘影

- 隱藏控制項後將 bounds 縮零，非同步隱藏 WebView drawing surface，於視窗模式依官方做法觸發 composition refresh。
- 保留 fullscreen 不移動、hidden resize／dock 不展開、重新開啟恢復 bounds；未加入 `DwmFlush` 或跨程序等待。
- 重複 Show(false) 保留待還原清單；還原前比對子視窗、class、PID、TID，Shutdown 清除舊 handle。
- 原生 WebView 測試檢查 Full／Left／Right、6 次開關、縮放、fullscreen、銷毀／替代／重新設 parent、Shutdown。
- 此為隱藏且軟體繪製的原生 fixture，檢查控制項與視窗狀態；實際桌面合成殘影仍須可見視窗實測。

## C. 尚未移植及原因

- 40／30 音訊預設、Steam 宣傳、官方品牌與發布版本：按需求排除。
- HolyBear 專有 HDMI、VRR、來源 EOTF、訊號保留畫面、Discord／更新、安全序列化：保留現有實作。
- Steam Frame Generation：未提供於此次免費版來源，沒有實作，也沒有宣稱已具備。
- 4K X 實體卡、GC553Pro HDMI 實體鏈路與可見桌面 F1 殘影：自動化無法取代，列入待實測。

## D. 來源 Commit 對照

| 修正 | 上游 Commit | 本地處理 |
| --- | --- | --- |
| 自適應雜訊門檻 | `f49ecd0123d4756d61cb130f1e6e332205e2feb8` | 估計器原文＋窄幅 FrameDiffer 接線＋擴充回歸 |
| 4K X tone mapping | `05ad330ff5146146f4c5f8f378859e94f271692a` | 語意移植＋提前辨識＋實際格式確認 |
| F1 composition refresh | `4267ef61661084e55861e0e5e50a7114ffb1d55c` | 原修正＋HolyBear 啟動／重啟／非同步 handle 保護 |
| #31 | `e2c7be920b5a935f4324591a2ea14412c08863fb` | HolyBear 基準已具備，不重複套用 |
| CN_115 官方合併 | `17346e70fba891c58c52736d6bc215a6a6563a53` | HolyBear 已整合 `240f1b0a91620bdf287eb57863136624a6b87b14` 核心 |
| 官方來源快照 | `0a093cc8e41fd04ffe13196122dc1a03dc9a0d6a` | 僅選擇性移植，不整體 merge |

`HOLYBEAR-SOURCES.json` 保留原始整合基底來源，另以 `selectivePorts` 紀錄本次固定來源與三項修正。

本地按功能提交（均未 Push）：

| 提交 | 功能 |
| --- | --- |
| `c364c3d` | 原始 Games／Discord／收藏／官方素材 checkpoint |
| `ee7a15c` | fix(games): compile native artwork bridge on Windows |
| `4b32ad4` | fix(capture): port adaptive duplicate noise floor from upstream 1.2.4 |
| `427d51b` | fix(capture): pair 4K X tone mapping with negotiated output |
| `4de7e02` | fix(settings): clear hidden WebView surfaces after closing F1 |
| `932257a` | style(games): keep cover actions together and match search buttons |
| `80c70aa` | chore(version): prepare 1.2.5 HolyBear test build |
| `bcc8d12` | docs(integration): record upstream 1.2.4 audit and test boundaries |

## E. 修改檔案及摘要

| 檔案 | 摘要 |
| --- | --- |
| `src/capture/noise_floor.h` | 新增官方估計器，與固定上游內容相同 |
| `src/capture/frame_differ.{cpp,h}` | 學習／上下界／reset／診斷；比較、rescue 與平滑保留 |
| `tests/noise_floor_tests.cpp` | 官方 CPU 測試與額外誤判／非法樣本保護 |
| `tests/frame_differ_noise_tests.cpp` | 實際 shader、三種輸入、HUD／Source pacing／reset |
| `src/capture/elgato_tonemap_policy.h` | 選定裝置 guard 與可自動化的 sender 邊界 |
| `src/app/application.cpp` | 六個 tone mapping 配對位置及首次 4K X 辨識 |
| `tests/elgato_tonemap_policy_tests.cpp` | 回退／重開／失敗／GC553Pro 等裝置隔離 |
| `src/app/WebViewSettings.{cpp,h}` | 隱藏、縮零、composition refresh、handle 生命週期 |
| `tests/settings_layout_tests.cpp` | 新增獨立 `--visibility-only`，保留所有既有 layout 斷言 |
| `CMakeLists.txt` | 註冊 4 項新增 CTest；保留 Games 與全部既有測試 |
| `nitlink-menu.html` | 修復既有 About 姓名／帳號文字被 inline-flex 分隔的問題，保留原斷言 |
| `src/app/game_service.{cpp,h}` | 修復 Games 基準既有的 Windows `LoadImage` 巨集命名衝突 |
| `src/app/{game_service,nintendo_tw_artwork}.cpp`、`src/discord/discord_rpc.cpp` | 補 WinRT Collections 定義標頭 |
| `VERSION`、`src/app.rc`、`holybear_version.h`、HUD、README、封裝範例與新版 Release Notes | 依後續要求同步升為 1.2.5；版本 fixture 跟隨 VERSION，保留既有更新拒絕斷言 |
| `assets/menu/preview-host.js`、`scripts/holy-bear-common.ps1` | 讓預覽 About 讀取 HTML 版本，原生／封裝比對該版本；不更動 CSP |
| `HOLYBEAR-SOURCES.json`、本報告及 Games review 文件 | 選擇性來源、驗證結果與邊界 |

原先 Games 功能 226 檔已獨立提交；本表只列該 checkpoint 之後的移植及首次原生建置相容修正。

## F. 建置與驗證結果

| 驗證 | 結果與範圍 |
| --- | --- |
| Windows x64 Release | 通過；新輸出目錄、集中 `--clean-first`，實際 EXE 為 1.2.5-holybear／AMD64 |
| 完整 CTest | **48／48 通過，失敗 0、略過 0**；最終完整執行 89.99 秒 |
| Noise Floor／FrameDiffer／FPS／Present Cap | CPU、WARP 三路徑、source cadence、availability／reset／HUD／deadline 均通過 |
| GC553Pro／HDR／SDR／Timing／VRR／resync | policy／parser／negotiation／poller／狀態機通過，非硬體鏈路實測 |
| HDMI 手動來源／Discord RPC | 原生欄位、Unicode、安全與 mock pipe 測試通過，非真實客戶端畫面 |
| WebView2／F1／Full Wide／Left／Right | 原生 runtime／安全／layout／missing assets 及新增 visibility 測試通過 |
| CN_115 音訊 | conversion／drift／packet feedback／clock recovery／media policy 自動化通過，預設仍為 12／10 |
| 三語與 UI | 420 個 menu、86 個 native 字串完整；173 個 game key 檢查；按鈕／About 版本 Full／Docked × 三語通過 |
| 簽章腳本非發布檢查 | 公開金鑰 preflight 3／3、PowerShell 語法、CTest 拋棄式 Tag／發布拒絕檢查通過 |
| Portable／Setup 非發布封裝 | 通過原有嚴格白名單、Microsoft CRT 簽章、SHA256、ZIP 全新解壓、來源／EXE 版本及 NSIS Setup payload 比對；未執行安裝／解除安裝 |

原始證據位置：`Documents/NitLink-Validation-20261016/official-v1.2.4-port/`。
包含 `clean-release-build.log`、`full-ctest.xml`、`full-ctest.log`、`focused-ctest.xml`、
`baseline-games-build.log`、`baseline-layout-ctest.xml`、`about-layout-fixed.xml`、`v125-ui-checks.json`、
`package-validation.log`、`verified-test-artifacts.json`、`preservation-verification.json`。
完整軟體測試通過不代表硬體／客戶端驗證完成。

測試包位於同一驗證資料夾的 `review-package/`：

- Portable：`NitLink-1.2.5-HolyBear-Win64.zip`（1,374,609 bytes）。
- Setup：`NitLink-1.2.5-HolyBear-Setup-x64.exe`（1,224,360 bytes）。
- 已解壓可直接測試的程式：`review-package/install/bin/NitLink.exe`，資源位於同一資料夾。
- 套件來源提交為 `bcc8d120da62e7bd47b63087509aeda05f632a93`；之後僅補本報告的封裝結果。
- `BUILD.txt` 明確記錄 `tag_source_verified=false`，標籤只是預定名稱；未建立 1.2.5 Tag，未 Push／發布。
- 原本 226 個開發檔案全部存在，212 個位元組未變，14 個僅有上述明列的後續修正；原始備份仍保留。

| 檔案 | SHA256 |
| --- | --- |
| `NitLink-1.2.5-HolyBear-Win64.zip` | `a251d5da3292b6512e06a3dde36ef824745b19c905a1f8d917720cf50530e47c` |
| `NitLink-1.2.5-HolyBear-Setup-x64.exe` | `d4b82d7195d95761b6b32425043b00c962aba6d195191c7356416bfcfa379f84` |


使用 Visual Studio 2022 x64 工具鏈，於同一個 `cmd.exe` 初始化 `VsDevCmd.bat -arch=x64 -host_arch=x64`，
Ninja／Release／C++20，開啟 `BUILD_TESTING` 與原生 WebView runtime 測試；新建獨立輸出目錄並集中清除重建。

原有 Games checkpoint 的編譯問題已在未移植的 `c364c3d` worktree 重現：WinRT Collections 定義缺失、
`GameService::LoadImage` 被 Windows 巨集展開。修正沒有更改 API、封面優先順序或收藏模型。
新增 F1 測試的 LONG／int `std::min` 型別錯誤也已修正，沒有降低斷言。

第一輪完整 CTest 為 47／48：既有 About 內容斷言失敗。未移植的 `c364c3d` 基準也重現同一斷言；
瀏覽器確認 inline-flex 連結使姓名與帳號之間的 `innerText` 出現換行。正式 HTML 改用 inline 連結與
相同尺寸／間距的行內 SVG；保留所有既有斷言、姓名、帳號與來源連結。Full／Docked、三語瀏覽器檢查均通過。

升為 1.2.5 後，更新檢查測試原有的「未來 1.2.5」資料需改為 1.2.6；
目前版本 fixture 改讀 VERSION，並補 1.2.4.1→1.2.5 的更新檢查。沒有改變 Available／Current
或取消／安全拒絕條件的預期，最終重新執行完整 CTest。

非發布封裝檢查不等於正式發布／簽章。正式私鑰簽章、GitHub 發布、實際安裝／移除均不在本次執行範圍。
簽章驗證測試只使用公開金鑰或拋棄式測試金鑰，沒有產生正式產物的 GPG 分離簽章。

## G. 待實測清單

| 裝置／情境 | 自動化覆蓋 | 尚待實體／可見客戶端確認 |
| --- | --- | --- |
| GC553Pro | HDR policy、P010 selector／negotiation、Timing／VRR parser／poller、No Signal／resync | HDR10↔SDR、熱插拔、訊號重同步、VRR 開關、長時間畫面／聲音 |
| Elgato 4K X | 裝置隔離、P010→NV12、重試／重開、P010 passthrough 命令狀態 | HDR 主機 4K120 NV12 實際色彩；P010 HDR；驅動實際接受 tone mapping |
| FPS／Source pacing | CPU 軌跡及 WARP 真正 shader／readback；真 60／30、noise、static、slow／tile | 真實擷取卡雜訊與實際遊戲、HUD／Present pacing 體感 |
| F1／DWM | 真正 WebView controller、bounds／visibility、三布局、重複開關／resize／fullscreen | 視窗模式最後畫面是否清除、跨螢幕／DPI／GPU compositor 行為 |
| Discord | RPC／欄位／Unicode／fallback 自動化與 Games 持久化 | 真實 Discord 客戶端第一行及每張遠端圖顯示；加工圖沒有上傳 |
| 遊戲／官方圖片 | 原生 Config、URL／選圖驗證、Mock Provider 取消與 worker 邊界；瀏覽器真實 API 與官方圖 | Windows 原生真實搜尋、選圖、WIC 互動與重啟還原的完整手動流程 |
| 音訊 | 漂移、format conversion、packet window、欠載恢復與 focus／exclusive policy | 實際音訊裝置、主觀延遲、長時間漂移與焦點切換聽感 |

按使用者追加要求，封面調整按鈕與儲存畫質成對排列，兩個搜尋按鈕採相同樣式；
選取遊戲／圖片後的 Full／Docked × 三語共六組檢查，驗證按鈕啟用、同列、順序、樣式與無橫向溢出。
瀏覽器版面截圖為 Fixture 原生橋接模擬，不代表 HDMI 或 Discord 客戶端硬體畫面。

## H. HolyBear 既有功能影響

沒有刪除功能或放寬 WebView CSP、trusted URI、外連白名單及原生驗證。
保持 12／10 預設、三語、GC553Pro／#23／#25／#31、Present Cap、Tab HUD、Full Wide、About／貢獻者／更新、
Discord 計時與手動來源、原生 Config 收藏、Nintendo HK 優先、台灣素材、封面製作與安全快取。
音訊、renderer／present scheduler、GC553Pro parser／poller 與更新檢查核心沒有因本次移植修改。
上述「保留」由差異比對及自動化支持；不是全部硬體組合的無回歸保證。

## I. 建議版本與 Release Notes 重點

原建議為新功能一併發布時使用 `v1.2.5-holybear`。依使用者後續要求，已同步修改 `VERSION`、
CMake、Windows RC、HUD、原生更新檢查、HTML／瀏覽器 About、README 與封裝驗證範例為 1.2.5。
Windows 數值版本為 1.2.5.0，產品字串為 1.2.5-holybear。沒有改動既有 Release Tag 或已發布的 v1.2.4.1 產物。
新增 `release-notes-1.2.5-holybear.md`，保留歷史版本說明；目前只交付本機測試包，發版由使用者另行決定。

建議新版說明：

- 自動學習擷取卡重複影格雜訊，改善部分 30 FPS 內容被顯示為 60 FPS 的情況。
- 4K X tone mapping 跟隨實際協商的 SDR／P010，改善 HDR 訊號降級為 NV12 時的色彩。
- 修復 F1 隱藏／縮放後殘留設定畫面的處理，保留 Full Wide 與 Docked 模式。
- 加入真實遊戲搜尋、我的遊戲庫、Nintendo HK／TW 官方素材與正方形封面製作。
- 保留 HolyBear 低延遲音訊 12／10 與既有 HDMI／VRR／FPS／Discord 功能。
- 仍須明列硬體與 Discord 客戶端驗證邊界；不宣稱提供 Steam Frame Generation。
