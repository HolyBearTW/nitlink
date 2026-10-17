# 官方圖片來源、Discord 與動態標題整合紀錄

工作目錄：`C:\Users\HolyBear\.codex\worktrees\98c2\nitlink`。
分支：`holybear/game-search-presence`。
HEAD：`d6da1077a1de31b2c8cdf2642edb68878cd2eeac`，本次未提交。
產品版本維持 `1.2.5-holybear`；About 三語基底為 NitLink 1.2.4。

## 完成範圍與驗證層級

| 項目 | 正式程式碼 | 已完成驗證 | 尚未驗證 |
| --- | --- | --- | --- |
| WinRT evt/null、欄位型別與 nonce | 完成 | 獨立 WinRT JSON／RPC Worker 測試 | 新版完整 EXE |
| 遊戲與來源動態視窗標題 | 完成，沿用原有更新／快取流程 | Unicode／本地化／未知狀態／HDMI 非整數更新率測試、Application 語法檢查 | Windows 工作列與 Alt+Tab 的新版實機畫面 |
| Nintendo Taiwan 優先、Hong Kong 備援 | 完成原生與瀏覽器 Bridge | 原生政策測試、真實 API／圖庫操作 | 尚未映射的全部台灣遊戲 |
| Switch／PS5 平台隔離 | 完成，平台由原生有效 HDMI 來源推導 | 手動來源優先、未知／PC／Xbox 不注入 Nintendo、切換來源的瀏覽器檢查 | 實體 HDMI 自動偵測切換 |
| 台灣 PlayStation 官方來源 | 完成兩款核實映射 | 官方圖片下載、真實 UI、原生 Config 保存 | 全站自動目錄；未實作爬蟲 |
| Discord 圖片與台灣備援 | 完成 | 真實 IPC／圖片代理、named-pipe 隔離測試 | 真實 Discord 客戶端像素顯示確認 |
| 正方形加工與收藏還原 | 沿用既有原生及預覽流程 | 真實 512×512 PNG、拖曳、84／64px、切換遊戲與重新載入還原 | 新版完整 NitLink EXE |

## 來源規則

保留使用者已選的手動／本機圖片。自動候選僅依目前 HDMI 主機平台處理：

1. Switch／Switch 2：該遊戲存在 Nintendo Taiwan 素材時，只列台灣候選，
   不查詢或混入香港商店候選；沒有台灣對應時，沿用香港商店一般查詢。
2. PS5／PS4：僅加入已核實的 PlayStation Taiwan 官方候選；不執行 Nintendo 商店查詢。
3. 其他或未知來源：保留一般 Worker／SteamGridDB 搜尋，不自動加入 Nintendo 素材。

優先規則在形狀篩選之前套用，所以台灣直式封面切到「正方形」時，
不會把已隱藏的香港圖片重新帶回。使用者仍可保留既有手動收藏封面；
切換主機不會刪除收藏、畫質設定、自訂名稱或重設 Discord 計時。

頁面上方新增來源提示與「選擇訊號來源」按鈕，開啟原本來源選單。
圖庫維持圖片式操作，不恢復每張卡片的技術資訊堆疊。

## Provider 與映射

`official-artwork.json` 是共用來源資料，原生 `CreateOfficialArtworkProvider()`
與瀏覽器受控 Bridge 讀取相同內容，沒有把遊戲名單寫入 EXE 邏輯。
原有 `provider/sourceLabel/language/region/isOfficial/kind` 欄位沿用既有 Config
結構，不需升級資料庫或修改 Worker v5。

目前 Nintendo Taiwan 對應 4 款：王國之淚、災厄啟示錄、封印戰記、時之笛。
時之笛是官方歷史頁的無文字美術，不冒稱繁中盒裝封面。
災厄啟示錄原有香港 eShop 素材仍保留在來源資料，但台灣主來源存在時不顯示。
其他 Switch 遊戲可用香港一般商店查詢與社群圖；這不等於已收錄全部台灣官網圖片。

新增 PlayStation 官方對應：

| 遊戲／SteamGridDB ID | 圖片類型／尺寸 | 內容與來源 |
| --- | --- | --- |
| 宇宙機器人／5454475 | squareIcon，1024×1024 | 英文 ASTRO BOT 標題；[台灣官方產品頁](https://www.playstation.com/zh-hant-tw/games/astro-bot/) |
| God of War Ragnarök／5296432 | keyArt，1024×1024 | Kratos／Atreus 20 週年英文主視覺；[台灣官方產品頁](https://www.playstation.com/zh-hant-tw/games/god-of-war-ragnarok/) |

兩張均標示 `Official PlayStation Taiwan`、`region: TW`、`language: en-US`。
不因頁面為繁體中文而標示圖片包含繁中標題，也不把週年主視覺當成原版盒裝封面。

## 原生網路與 Discord

沒有放寬 CSP。圖片由原生或本機預覽 Bridge 下載、檢查 MIME／實際格式／
大小／尺寸／逾時後轉為受控 PNG。Sony 限於已核實 seed 的確切 HTTPS URL，
主機為 `image.api.playstation.com`、路徑為 `/vulcan/`；不允許任意 URL 注入。

`assets.large_image` 使用已選原始公開 HTTPS URL，不使用 SGDB ID、
`currentGameId`、WebView2 資源 URI 或本機檔案路徑。若同款 Switch 遊戲的
選圖確定遭 Discord 代理拒絕，而已知有台灣來源，則只對 RPC 改送台灣圖片。
收藏原選圖與本機裁切仍保留，重試也不更動起始時間。

原生取得代理圖片後，將驗證過的實際代理像素提供給 Discord 原圖參考預覽。
本機加工圖維持本機，沒有上傳第三方服務。IPC Accepted、Image Proxy Accessible
與 Client Display Verified 分開，最後一項仍為未驗證。

不同 Accept 與 User-Agent 的 24 次 HTTP 檢查確認：

- 香港原始圖片始終為 JPEG、HTTP 200、無重新導向，但 Discord 代理回 HTTP 415。
  因此屬代理相容性問題，已驗證可改送台灣同款圖片。
- 台灣災厄啟示錄圖片為 JPEG、280×456、HTTP 200、無重新導向。
- Sony 的 `.png` URL 可能依 Accept 回 JPEG 或 AVIF；不能從副檔名推定實際格式。
  原生 PNG／JPEG／WebP Accept 得到 JPEG，已成功經 Discord 代理取得圖片。
- 代理回 AVIF／GIF 而本機無相應解碼器時，僅標記尚無法驗證，保留遠端圖片，
  不誤判 HTTP 415，不無限重送。

## 輕量驗證與預覽

預覽：[實際介面與 Real API](http://127.0.0.1:8765/nitlink-menu.html?preview=games)。
原始 URL 另以未攔截文件的 Chrome 驗證，HTTP 200、28 筆結果、無 JS 例外。

- 真實搜尋「薩爾達」：27 筆 Worker 回應，加 1 筆明確標示官方素材對應的王國之淚。
- `scripts/check-artwork-integration.cjs`：39 項瀏覽器整合檢查通過。
  使用真正 HTML／JS、真實 Worker／官方圖片，涵蓋三語 Full／Left／Right、
  420px、來源切換、官方候選互斥、收藏與裁切 PNG 保存／重啟還原。
  只使用並於結束時還原 `checks` 預覽資料，不修改使用者 default 收藏。
- `tests/artwork_policy_tests.cpp`：36 項原生來源／優先順序／URL／Config 檢查通過。
- `scripts/check-artwork-policy.py`：4 項隔離政策測試通過，Worker 與香港網路僅在這組測試中模擬。
- `tests/discord_image_tests.cpp`：215 項 RPC／代理／重試／計時／標題檢查通過。
- JavaScript／Python 語法檢查及三語 433 個 menu、86 個 native 字串檢查通過。
- MSVC `/Zs`：`application.cpp`、`game_service.cpp`、`settings_state_json.cpp`
  語法檢查通過；有既存 ASCII 儲存鍵轉換的 C4244 警告。
- 真實隔離 RPC：SteamGridDB、Nintendo Taiwan、PlayStation 與香港→台灣備援
  都取得有效代理圖片，沒有宣稱完成 Discord 客戶端畫面驗證。

證據位於 `out/game-preview/artwork-integration-checks.json`、
`out/game-preview/normal-url-check.json`、`out/artwork-http-matrix.json`。
截圖有 `taiwan-priority-full.png`、`playstation-taiwan-docked.png`、
`playstation-taiwan-docked-cover.png` 與 `official-source-crop.png`。

## 修改與新增檔案

| 範圍 | 檔案 |
| --- | --- |
| 原生 Application／視窗標題 | `src/app/application.cpp`、`application.h`、`hdmi_source_display.h` |
| 來源政策／模型／Bridge | `src/app/game_identity.h`、`game_service.cpp`、`game_service.h`、`nintendo_tw_artwork.cpp`、`settings_state_json.cpp`、`official-artwork.json` |
| Discord | `src/discord/discord_rpc.cpp`、`discord_rpc.h`、新增 `activity_reply.h`、`image_proxy.cpp`、`image_proxy.h` |
| 真正 UI／預覽 | `nitlink-menu.html`、`assets/menu/menu.js`、`assets/menu/preview-host.js` |
| 三語與 About | `locales/zh-TW.js`、`locales/zh-CN.js`、`locales/en-US.js` |
| 受控瀏覽器 Bridge | `scripts/nintendo_tw_artwork.py`、`scripts/preview_game_bridge.py` |
| 測試與診斷 | `CMakeLists.txt`、新增 `tests/discord_reply_tests.cpp`、`tests/discord_image_tests.cpp`、`tests/artwork_policy_tests.cpp`、`scripts/check-artwork-http.py`、`scripts/check-artwork-policy.py`、`scripts/check-artwork-integration.cjs`、`scripts/probe-discord-images.ps1` |
| 報告 | 本文件、`docs/DISCORD-RPC-IMAGE-INVESTIGATION.md` |

未執行完整 Windows Build、CTest、Commit、Push、PR、發版或簽章。
既有 1.2.5 測試包未覆寫，仍不包含本輪未建置的修改。
