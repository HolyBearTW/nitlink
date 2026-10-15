# 遊戲、官方封面與 Discord：介面審核版

最新的通用香港任天堂商店來源、任意商品搜尋與全目錄分頁，請先見
[`NINTENDO-STORE-REVIEW.md`](NINTENDO-STORE-REVIEW.md)。香港商店優先於台灣精選素材，
主流程不再受三款種子資料限制。台灣精選另加入《時之笛》原版無文字美術圖。

新增的「官方多尺寸素材／正方形製作」操作、實際檔案與安全發布方案，請見
[`ARTWORK-EDITOR-REVIEW.md`](ARTWORK-EDITOR-REVIEW.md)。三種模式、拖曳縮放、
84／64px 預覽及每遊戲儲存已接入正式介面；原生 WIC 路徑已納入 1.2.5 Release 編譯；完整原生操作仍待測試版驗收。

預覽入口：<http://127.0.0.1:8765/nitlink-menu.html?preview=games>。
建議使用 `scripts/open-game-preview.ps1` 啟動可操作的 Chrome／Edge 預覽。
這台電腦的網頁防護仍偶爾中斷普通網址的 HTTP 文件載入；本機服務已改為完整 gzip
回應，語系與腳本也不再快取。建議的啟動器會將
**實際來源檔的完整原始內容**提供給預覽瀏覽器；Bridge、API、圖片仍走本機伺服器。
重新整理會讀取最新檔案，不停用網頁防護，也不修改系統或正式程式 CSP。
預設為 Real API；頂部可切換 Fixture、Full、Docked 及三語。
預覽使用正式 HTML、CSS、menu.js 和語言資源，沒有另一份獨立 Mockup。

## 本次實作

- 保留 Worker v5 的全部搜尋結果；目前「薩爾達」為 27 筆 Worker 資料，
  加上 1 筆明確標示為 Nintendo Taiwan 官方對應資料的《王國之淚》。
- 搜尋卡片縮圖逐批處理所有結果，不再只處理前 12 筆（預覽）／8 筆（原生）。
  官方素材先載入；沒有正方形候選時嘗試其他比例，失效圖片可改試同批候選。
  載入中、暫停、無可用封面及載入失敗分別顯示狀態。實際預覽 28 筆均已載入縮圖。
  所有資料來源一同依名稱相關性排序；同級保留 Worker 順序，不宣稱熱門度排序。
- 官方來源由 `ArtworkProvider` 介面及 `NintendoTaiwanArtworkProvider` 實作，
  原生和瀏覽器 Host 共用 `official-artwork.json`，不用把作品清單編譯進 EXE。
- 官方候選先於社群圖，使用者手動選擇優先；沒有官方資料時沿用原流程。
  形狀篩選同時適用官方與社群圖片；官方素材獨立緊湊排列，不重複計入社群總頁數。
  官方盒裝圖屬於直式，Logo 為橫式，只在對應篩選或「全部」出現。
- 圖庫以圖片為主，選擇使用藍框與勾選。已驗證的官方圖有「官方繁中」角標；
  Logo 另行標記。作者、來源、語言、預覽尺寸與出處集中在資訊視窗。
  Full 約 5–6 欄、Docked 約 4 欄，依實際寬度調整，單頁不顯示分頁列。
- 選擇遊戲、同步收藏和重新命名沿用原本圖片節點；取消請求會結束載入狀態。
  圖片失敗可重試，有獨立逾時保護，不再因同款遊戲重選而重新請求整份圖庫。
  失效圖片隱藏並禁止選取，顯示失敗數量；原生 Bridge 也要求已驗證的本機圖片。
  `unavailable-artwork.json` 以精確 SHA-256 排除已實測的 SteamGridDB HTTP 200 移除通知圖，
  不會猜測其他圖片內容；未來新通知版本可能需要更新指紋。
- 收藏／最近遊玩、每遊戲畫質、自訂名稱、封面及自動／手動狀態由原生 Config
  保存。官方圖另外保存 provider/sourceLabel/language/region/isOfficial/kind。
  移除收藏保留最近遊玩與畫質；重新啟動不需要搜尋即可還原目前遊戲。

| SteamGridDB ID | 顯示名稱 | 真實官方素材 |
| --- | --- | --- |
| 5303117 | 薩爾達傳說 王國之淚 | 官方繁中盒裝圖 |
| 5267007 | ZELDA無雙 災厄啟示錄 | 官方繁中盒裝圖 |
| 5267007 | ZELDA無雙 災厄啟示錄 | Nintendo eShop CDN 1920×1080 繁中主視覺（使用者提供的香港候選） |
| 5483959 | ZELDA無雙 封印戰記 | 官方繁中 Logo，非完整封面 |

以上四張均來自任天堂官網／官方 eShop CDN。圖片內容及實際尺寸已檢查；香港商品頁
引用關係未另外驗證，來源標示為 Nintendo eShop CDN。Fixture 使用真實素材快取，合成語言示例必須另外
勾選才出現，也不會成為預設封面。Real API 不會混入 Fixture 合成圖。

## 邊界與已完成檢查

原生下載限制 HTTPS、兩個任天堂網站及固定 eShop CDN 路徑，且網址必須存在於已驗證的官方對應資料。
保留 MIME、大小、圖片解碼、逾時與快取限制，WebView2 仍只有受控本機圖片資源，
沒有放寬 CSP。前端只能提交遊戲／封面 ID，不能提交任意外站網址。

官方圖已完成 NitLink 顯示、選擇、收藏與還原。原本只允許 SteamGridDB 的 RPC 圖片
限制已修正；原生下載／驗證成功後，傳送已選的原始官方網址。四張官方圖均經獨立
Discord IPC 實測接受並回傳 `mp:external/...`，證據在 `out/discord-artwork-probe.json`。
這不是新 EXE 或 Discord 畫面的驗證。預覽使用當前選圖，區分「網址已實測接受」
與「瀏覽器本地預覽」；原生依實際 ACK 更新狀態，被拒絕或本機載入失敗時顯示 N
及原因。RPC ACK 變更會同步回 UI，不必重開設定或重送活動。
《王國之淚》原圖只裁掉左右空白，保留完整盒裝圖；Discord 仍收到原始官方網址。
本機 84／64px 正方形以留白保留比例，Discord 的裁切與像素尺寸尚未視覺確認。
動態 HDMI 名稱亦有獨立 IPC 接受證據。
同款遊戲更換來源、封面、語言或訊號資訊保留遊玩計時。

收藏不會附加文字到遊戲標題。已移除預覽測試寫入的已知「（我的收藏）」自訂名稱，
並改用中性的測試名稱、測試後還原；正式使用者的自訂名稱不會被任意清理。
三個語系的遊戲功能鍵與實際頁面標籤均有對應翻譯。固定腳本版本與 no-store
避免舊語系快取；固定版本參數只對打包腳本生效，沒有開放任意資源網址。

輕量檢查包含真實 API／官方圖片下載與解碼、URL 拒絕案例、互動瀏覽器流程、
延遲圖片回應的取消行為、收藏還原、三語資源完整性及 JavaScript/Python 語法。
瀏覽器 Host 的 Config 與硬體狀態仍是模擬；Windows 原生程式碼已通過 1.2.5 Release 編譯及完整 48／48 CTest。
**本機功能已獨立提交；沒有 Push、PR、Tag 或發布。**
完整來源、基準對照與硬體待測項目見 [`UPSTREAM-1.2.4-PORT-AUDIT.md`](UPSTREAM-1.2.4-PORT-AUDIT.md)。

## 主要修改檔案

- `nitlink-menu.html`、`assets/menu/menu.js`、`locales/{zh-TW,zh-CN,en-US}.js`
- `src/app/{game_identity.h,game_service.h,game_service.cpp,nintendo_tw_artwork.cpp}`
- `src/app/{artwork_renderer.h,artwork_renderer.cpp}`、`assets/menu/artwork-editor.js`
- `src/app/{application.cpp,application.h,config.cpp,config.h}`
- `src/app/{settings_message.cpp,settings_message.h,settings_state_json.cpp,settings_state_json.h}`
- `src/app/{WebViewSettings.cpp,WebViewSettings.h,webview_policy.h}`
- `src/discord/{discord_rpc.cpp,discord_rpc.h}`
- `official-artwork.json`、`unavailable-artwork.json`、`game-search-expansions.json`、`CMakeLists.txt`
- `assets/menu/preview-host.js`、`preview/fixtures.js` 與預覽圖片
- `scripts/{nintendo_tw_artwork.py,preview_game_bridge.py,serve-game-preview.py,prepare-game-preview.py,preview-browser.cjs,open-game-preview.ps1}`
- `scripts/{check-artwork-preview.cjs,check-game-live-preview.cjs,check-game-preview.cjs,probe-discord-activity-name.ps1,validate-package.ps1}`
- `scripts/{check-game-i18n.cjs,check-game-artwork-failures.cjs}`
- `scripts/{artwork_processing.py,check-artwork-editor.cjs}`
- `tests/{game_selection_tests.cpp,settings_layout_tests.cpp,webview_runtime_tests.cpp}`（已隨完整 CTest 執行）
- `docs/game-search-provider-contract.md`、本文件

完整修改／新增清單：`out/game-preview/modified-files.txt`。
瀏覽器證據：`out/game-preview/official-artwork-checks.json`、`live-browser-checks.json`。
