# NitLink 1.2.5 正式驗收紀錄（2026-10-16）

本次交付為未發布的 Windows x64 Release 驗收包。沒有 Commit、Push、Tag、PR、
發布或正式 GPG 簽章，也沒有覆寫之前的 1.2.5 測試包。

## 執行檔與來源

- EXE：`C:\Users\HolyBear\Documents\NitLink-Validation-20261016\acceptance-v1.2.5-20261016-201240\NitLink-1.2.5-HolyBear-Acceptance-Win64\NitLink.exe`
- ZIP：`C:\Users\HolyBear\Documents\NitLink-Validation-20261016\acceptance-v1.2.5-20261016-201240\NitLink-1.2.5-HolyBear-Acceptance-Win64.zip`
- 版本：`1.2.5-holybear`；Windows 檔案版本 `1.2.5.0`；AMD64 PE。
- EXE SHA256：`8b0913a8ad0e9cc7d8edfb0bc12880ea54967d76a136bc55e56d025f63c968cb`
- ZIP SHA256：`cd9680ded1cf484659027611c1126edffaec49cc76ac3469e498985bf939f5f5`
- 來源：`C:\Users\HolyBear\.codex\worktrees\98c2\nitlink`，分支 `holybear/game-search-presence`。
- HEAD：`d6da1077a1de31b2c8cdf2642edb68878cd2eeac` **加上未提交的工作樹內容**，不是只建置該 Commit。
- 開始時保留 32 個修改／新增檔案，備份為 `uncommitted-source-backup.zip`
  與 `initial-worktree.patch`。503 個來源檔案的雜湊保存在 `source-manifest.json`。

請保留整個驗收資料夾，從該資料夾開啟 EXE。不能只複製 EXE 而遺漏
assets、locales、JSON 或 DLL。要沿用舊設定，先關閉舊版並備份，再複製一份
`nitlink.json` 到驗收資料夾；本次沒有自動修改使用者原本的設定檔。

## 建置與自動化測試

使用全新 `release-x64` 目錄，MSVC 19.44／Visual Studio 2022 x64、Ninja、
`CMAKE_BUILD_TYPE=Release`、`BUILD_TESTING=ON`、`NITLINK_TEST_WEBVIEW_RUNTIME=ON`。
`VsDevCmd.bat -arch=x64 -host_arch=x64` 與建置在同一 cmd 程序初始化。
主程式及所有測試共 196 個建置步驟完成，沒有編譯／連結錯誤。
有 3 筆 C4244 警告，位於既有 ASCII 儲存鍵／快取網址轉換；
繁中、簡中、日文及 emoji 名稱另由 Unicode／Config／標題測試驗證。

| 驗證 | 結果 | 證據 |
| --- | --- | --- |
| 全新 Windows x64 Release Build | 通過 | configure.log、release-build.log |
| 完整 CTest 首輪 | 50／51 通過，1 失敗，0 跳過 | ctest-results.xml、ctest-output.log |
| About 必要修正後單項 CTest | 1／1 通過 | layout-retest.xml、layout-retest.log |
| 最後結果（按項目採最新紀錄） | 51 項均通過，未解決失敗 0，跳過 0 | acceptance-test-summary.json、ctest-all-results.csv |
| 真實原生 GameService 整合 | 26 項通過 | native-live-checks.log |
| 本次 Release Discord IPC／代理 | 四來源皆取得有效代理圖片，香港成功轉台灣備援 | discord-live-proxy.log |
| 三語資源 | 433 個 menu、86 個 native 字串通過 | validate-locales.js |
| JavaScript 語法 | menu／preview-host／artwork-editor 通過 | node --check |
| Release Notes 修正後回歸 | 64 項通過 | release-notes-retest.log |
| 公鑰與簽章腳本非發布驗證 | 3 測試通過；未用正式私鑰簽章 | test-release-signing.py |
| 驗收資料夾與 ZIP | 44 個內容檔案＋SHA256SUMS；ZIP 逐檔雜湊通過 | package-validation.log、zip-verification.json |
| 建置與交付資源一致性 | 21 項 SHA256 一致 | resource-verification.json |

注意：51 項最終結果來自「完整首輪＋失敗項目復測」，不是把首輪失敗報成
一次全套 51／51。原始失敗 XML 與輸出保留，不覆寫，也沒有停用任何測試。

唯一失敗的根因是 About 模擬資料使用產品版本 1.2.4，現在副標題也正確顯示
「基於 NitLink 1.2.4」，原測試仍只允許標題區出現一次 1.2.4。
修正 `tests/settings_layout_tests.cpp` 的精確出現次數，追加完整繁中副標題檢查，
保留所有版面、無障礙、操作與安全斷言；只重建該測試目標，未重建主程式。

歷史上的 `webview_runtime_security` 重啟斷言失敗，本次完整測試已通過；
本輪沒有將任何失敗當成可忽略的既有失敗。

## 正式 EXE 包含範圍與證據界線

| 功能 | 已包含及驗證 | 仍需使用者實測 |
| --- | --- | --- |
| 真實遊戲搜尋 | Production GameService 直接取得 Worker 27 筆薩爾達結果，加 1 筆誠實標示的王國之淚官方對應 | 主程式內的完整點選體驗 |
| 我的遊戲庫 | 原生 Config 保存／還原、Unicode 名稱、收藏、畫質關聯 | 主程式關閉／啟動與快速切換 |
| 官方來源優先 | Switch 台灣優先、無台灣對應才香港；PS5 不注入 Nintendo | 實體 HDMI 來源變動時的端到端同步 |
| 封面分頁與形狀 | Native Worker 下一頁通過；直式台灣封面不誤入正方形，也不復活香港備選 | 實際圖庫捲動與選圖 |
| PlayStation 官方圖 | 原生安全下載通過，英文素材不標繁中 | PS5 來源實際操作 |
| Discord RPC | evt/null／nonce／失敗恢復、實際公開 URL、重試、計時、代理與備援通過 | Discord 客戶端實際活動圖片 |
| 正方形加工 | 正式 WIC 完整顯示與裁切輸出真正 512×512 PNG；Config／檔案還原通過 | 調整視窗與 84／64px 本機顯示 |
| 動態視窗標題 | Application 已編入；多語與 Unicode 名稱優先順序、實際 HDMI 標籤組合測試通過 | 真正主視窗、工作列與 Alt+Tab |
| 三語與設定模式 | 三語資源、原生 Full／Left／Right、F1 與安全測試通過；About 基於 1.2.4 | 硬體輸入時的畫面與關閉 F1 是否有殘影 |
| 官方 1.2.4 選擇性移植及既有功能 | Noise Floor／FrameDiffer、Elgato policy、GC553Pro、FPS／Present Cap、CN_115 音訊等自動化通過 | 真實擷取卡 HDR／SDR／Timing／VRR 及音訊穩定性 |

原生整合小程式直接連結本次主程式的 GameService、ArtworkRenderer、Provider、
Config 與 JSON serializer 物件檔，使用真實 API／圖片，沒有以 Fixture 代替。
輸出包含 `fit-native.png`、`crop-native.png` 與隔離的設定檔；沒有開啟擷取硬體。
完整建置後，所有正式 src 程式碼雜湊均維持不變。

## Discord 圖片與已知限制

1. **自訂裁切 PNG 目前不能直接顯示於 Discord。** 檔案只存在本機，沒有公開
   HTTPS 位址，也沒有上傳。實際 RPC 沿用原始遠端圖片或同款官方備援圖，
   不傳 file://、快取路徑、SGDB 數字 ID 或 currentGameId。
2. Release 隔離 RPC 實測：SteamGridDB、Nintendo Taiwan、PlayStation 原圖
   都得到有效代理圖片；香港原圖仍遭代理 HTTP 415，最後送出同款台灣網址，
   也取得有效代理圖片，保留原本活動時間。
3. 以上是 IPC Accepted 與 Image Proxy Accessible；**Discord Client Display
   Verified 仍為 false**。沒有把代理成功或本機 84／64px 預覽當作客戶端畫面證據。
4. Nintendo Taiwan 目前 4 款核實映射，PlayStation Taiwan 2 款；不是全站覆蓋。
   其他 Switch 遊戲仍使用香港一般商店查詢與社群來源。區域 TW 不代表圖片有繁中標題。
5. GC553Pro／Elgato 4K X、HDR／VRR、No Signal、F1 GPU 合成殘影及實體音訊
   尚未完成本版硬體驗收；自動化與軟體 WebView2 測試不替代硬體實測。
6. 此驗收 EXE 沒有 Windows Authenticode 發行者簽章；沒有製作正式 GPG 簽章或發版。

## 使用者實機驗收步驟

1. 關閉舊版，開啟 Discord 桌面版，再從驗收資料夾啟動 NitLink.exe。
   在 About 確認 1.2.5 與「基於 NitLink 1.2.4」，遊戲頁不應顯示 Fixture 橫幅。
2. 選 Switch 2 來源，搜尋「薩爾達」，確認多筆可捲動結果；選災厄啟示錄
   或王國之淚，看官方台灣候選與社群圖。已有台灣對應時不應混入香港備選。
   再切 PS5 搜尋 Astro Bot，應出現 PlayStation Taiwan，沒有自動 Nintendo 候選。
3. 收藏遊戲，重新命名為「驗收 🎮 ゼルダ」，選官方直式圖，使用完整顯示／
   填滿裁切，拖曳後儲存。切換遊戲再切回、關閉並重開，確認名稱、收藏與裁切還原。
   移除收藏後，確認原有每遊戲畫質設定仍存在。
4. 在真正 Discord 個人檔案／活動面板，依序測 SteamGridDB 原圖及台灣官方原圖，
   每次留 10～20 秒讓活動與圖片更新。確認名稱、圖片、訊號與目前遊戲一致。
   更換封面、HDMI 來源、語言或重開 F1 時，計時應延續；切到另一款遊戲才重新開始。
   本機裁切後，Discord 顯示原始遠端圖屬目前限制，不能要求它已顯示裁切結果。
5. 在 Windows 工作列縮圖或 Alt+Tab 檢查真正主視窗標題。例如確認來源後應為
   `NitLink - 驗收 🎮 ゼルダ - Switch 2 - 1920x1080 @ 60Hz`；HDR／VRR 只在
   來源確認啟用時出現。改名、清除遊戲、切來源與訊號變化應同步，不能使用顯示器 FPS。
6. Full／Docked、全螢幕／視窗、繁中／簡中／英文各操作一次；重複開關 F1，
   確認遊戲畫面繼續更新、沒有設定殘影、導航與長名稱未超出邊界。
   GC553Pro 實際切換 HDR／SDR、VRR、解析度與 No Signal，並確認音訊無異常。

請提供：NitLink 目前遊戲與來源畫面、Discord 真實活動圖片，以及工作列縮圖或
Alt+Tab 標題截圖。若仍出現問號，附測試遊戲、圖片來源、操作時間與
`%LOCALAPPDATA%\NitLink\logs\discord-rpc.log` 對應片段，便能對照 nonce、
實際 large_image、代理狀態；不需要帳號資料或 Token。

## 完整 CTest 項目

| 測試 | 最後結果 | 秒數 | 原始紀錄 |
| --- | --- | --- | --- |
| artwork_platform_policy | 通過 | 0.387 | ctest-results.xml |
| audio_clock_drift | 通過 | 0.965 | ctest-results.xml |
| audio_clock_recovery | 通過 | 1.192 | ctest-results.xml |
| audio_native_conversion | 通過 | 0.391 | ctest-results.xml |
| audio_packet_drift | 通過 | 1.925 | ctest-results.xml |
| capture_device_names_cache | 通過 | 0.384 | ctest-results.xml |
| capture_output_policy | 通過 | 0.383 | ctest-results.xml |
| config_rate_migration | 通過 | 0.383 | ctest-results.xml |
| config_round_trip | 通過 | 0.361 | ctest-results.xml |
| discord_images | 通過 | 49.384 | ctest-results.xml |
| discord_presence | 通過 | 33.69 | ctest-results.xml |
| discord_reply_json | 通過 | 0.345 | ctest-results.xml |
| display_refresh | 通過 | 0.38 | ctest-results.xml |
| elgato_4kx_tonemap_policy | 通過 | 1.548 | ctest-results.xml |
| frame_buffer_activity | 通過 | 0.401 | ctest-results.xml |
| frame_differ_adaptive_pacing | 通過 | 4.582 | ctest-results.xml |
| frame_differ_availability | 通過 | 0.416 | ctest-results.xml |
| frame_rate_stats | 通過 | 0.38 | ctest-results.xml |
| gc553pro_hdr_source | 通過 | 0.377 | ctest-results.xml |
| gc553pro_raw_timing | 通過 | 0.688 | ctest-results.xml |
| gc553pro_source_metadata_poller | 通過 | 5.289 | ctest-results.xml |
| gc553pro_source_vrr | 通過 | 0.69 | ctest-results.xml |
| hdr_tone_map_reference | 通過 | 0.371 | ctest-results.xml |
| holybear_release_notes | 通過 | 0.578 | ctest-results.xml |
| holybear_tag_security | 通過 | 26.519 | ctest-results.xml |
| holybear_update_checker | 通過 | 0.416 | ctest-results.xml |
| hud_fps_details_toggle | 通過 | 0.378 | ctest-results.xml |
| manual_hdmi_source | 通過 | 0.692 | ctest-results.xml |
| media_metadata_security | 通過 | 0.712 | ctest-results.xml |
| native_input_security | 通過 | 2.001 | ctest-results.xml |
| no_signal_cache | 通過 | 0.379 | ctest-results.xml |
| no_signal_layout | 通過 | 0.382 | ctest-results.xml |
| noise_floor | 通過 | 1.964 | ctest-results.xml |
| online_game_selection | 通過 | 0.411 | ctest-results.xml |
| p010_format_selector | 通過 | 0.339 | ctest-results.xml |
| p010_native_negotiation | 通過 | 0.384 | ctest-results.xml |
| packaged_shader_security | 通過 | 2.153 | ctest-results.xml |
| pipe_security | 通過 | 40.808 | ctest-results.xml |
| placeholder_detector | 通過 | 0.342 | ctest-results.xml |
| playback_power | 通過 | 0.478 | ctest-results.xml |
| present_cap_policy | 通過 | 0.378 | ctest-results.xml |
| present_wait_paths | 通過 | 0.596 | ctest-results.xml |
| presentation_state | 通過 | 0.339 | ctest-results.xml |
| settings_security | 通過 | 2.449 | ctest-results.xml |
| signal_resync_presentation | 通過 | 0.338 | ctest-results.xml |
| vsync_config_persistence | 通過 | 0.645 | ctest-results.xml |
| webview_f1_visibility | 通過 | 0.463 | ctest-results.xml |
| webview_missing_assets | 通過 | 17.474 | ctest-results.xml |
| webview_runtime_security | 通過 | 4.621 | ctest-results.xml |
| webview_settings_layout | 通過（修正後復測） | 32.49 | layout-retest.xml |
| window_size_persistence | 通過 | 6.63 | ctest-results.xml |

完整證據目錄：`C:\Users\HolyBear\Documents\NitLink-Validation-20261016\acceptance-v1.2.5-20261016-201240`。
本輪正式程式碼沒有新增功能；必要變更為 About 測試期望與 Release Notes 的
過時香港優先敘述，另新增本驗收報告。
