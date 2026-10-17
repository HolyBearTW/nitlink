# NitLink 1.2.5 HolyBear Edition

**非官方社群整合版（Unofficial Community Edition）。**

保留 HolyBear Edition 的擷取、畫面、音訊與設定功能，並選擇性整合 NitLink v1.2.4 的修正；這不是 nitlink-dev 官方發行版。

## 本次更新

### 遊戲與 Discord

- 新增「遊戲與 Discord」頁面，可以依介面語言搜尋遊戲、收藏遊戲，並快速切換最近遊玩的作品。
- 收藏會保存在本機。重開 NitLink 後，遊戲名稱、封面和畫質設定都會保留；移除收藏不會刪除該遊戲的畫質設定。
- 封面會等候本頁圖片載入後再自動挑選：有合適官方圖片時優先使用，否則選擇方形圖片。英文、簡中或德文介面不會自動套用台灣封面；仍可自行選取。
- 封面圖庫會把官方圖片、方形圖片和其他圖片分開排列。圖片資訊可按開查看；台灣任天堂素材會標示「台灣任天堂」與「繁體中文」，社群圖片會以英文標示語言。
- Discord 活動預覽會跟隨目前選定的遊戲封面與 Discord 可取得的圖片。只更換封面、語言或 HDMI 來源時，遊玩時間會繼續累計。
- 遊戲名稱會依介面語言顯示，支援繁中、簡中、英文和德文；自訂名稱仍會優先顯示。
- 主視窗標題會顯示目前遊戲、訊號來源與已確認的訊號資訊，切換遊戲或訊號時同步更新。

### 畫面與擷取修正

- 改善重複影格判斷，降低部分 30 FPS 遊戲被顯示成 60 FPS 的情況。
- 修正 Elgato 4K X 在不同擷取格式下的 HDR 色彩處理，讓 HDR 與 SDR 輸出依實際使用的格式選擇處理方式。
- 修正關閉 F1 設定畫面後偶爾殘留在遊戲畫面上的問題，並保留 Full、Docked Left、Docked Right 和 Full Wide 使用方式。
- 關閉 F1 設定後即可返回遊戲；再次開啟時仍會使用原本的設定與安全檢查。

### 保留的 HolyBear Edition 功能

- 保留 GC553Pro 的 HDMI 來源、訊號同步、HDR、VRR、No Signal 畫面及手動來源名稱。
- 保留 FPS 顯示與畫面更新限制；自動模式無法取得更新率時仍使用 30 Hz fallback。
- 保留低延遲音訊、裝置切換回復與音訊診斷，預設音訊緩衝仍為 FIFO 12 ms、Render 10 ms。
- 保留更新檢查、Portable ZIP、Setup、檔案校驗及發布簽章流程。

## 測試與驗證

- 正式封裝會附上該次實際建置與測試結果；這份更新日誌不預填測試數字。
- NitLink 不會把本機圖片交給 Discord，也不會自動上傳使用者圖片。自訂裁切圖可在 NitLink 預覽；沒有公開圖片來源時，不會顯示成 Discord 已套用。
- Discord 活動能送出，不代表 Discord 桌面程式一定顯示了封面；GC553Pro／Switch 2 等硬體仍須實際測試。
- 歷史硬體紀錄使用既有 1.2.4 安裝版，不代表 1.2.4.1 正式產物，也不代表本次 1.2.5 實機驗收。

## 已知限制

- 本次沒有新增 PS5 官方封面搜尋；目前的 PS5 封面範圍不變。
- Nintendo Taiwan 的圖片只會在有對應素材時出現；圖片來源為官方網站，不代表每張圖都含繁體中文標題。
- Discord 主要封面顯示仍受 Discord 圖片代理與桌面程式實際讀取結果影響。NitLink 會顯示目前能確認的圖片狀態，不把本機預覽當成 Discord 已顯示。
- GC553Pro、Switch 2、HDR／VRR、音訊切換及 Discord 桌面圖片仍待使用者實機驗收。
- Content FPS 顯示的是畫面內容變化速度，不是遊戲內部 FPS。本版本沒有提供 Steam Frame Generation。

## 來源、授權與致謝

- 原始專案：[nitlink-dev/nitlink](https://github.com/nitlink-dev/nitlink)，本次上游修正來源為 [v1.2.4](https://github.com/nitlink-dev/nitlink/tree/0a093cc8e41fd04ffe13196122dc1a03dc9a0d6a)。
- 保留 [#23](https://github.com/nitlink-dev/nitlink/pull/23)、[#25](https://github.com/nitlink-dev/nitlink/pull/25)、[#31](https://github.com/nitlink-dev/nitlink/pull/31) 的 HDMI／VRR、手動來源與 FPS 顯示修改。
- 低延遲音訊與簡中：[CN_115（@CN115）](https://github.com/CN115/nitlink-lowlatency/tree/v0.1.2)。整合與維護：[聖小熊（@HolyBearTW）](https://github.com/HolyBearTW)，並保留 Nathan K.（@n810K）等既有貢獻者資訊。
- [MIT 授權](https://github.com/HolyBearTW/nitlink-holybear-edition/blob/holybear/integrated/LICENSE)、[第三方授權](https://github.com/HolyBearTW/nitlink-holybear-edition/blob/holybear/integrated/LICENSES.md)、[致謝](https://github.com/HolyBearTW/nitlink-holybear-edition/blob/holybear/integrated/ACKNOWLEDGMENTS.md)與[來源紀錄](https://github.com/HolyBearTW/nitlink-holybear-edition/blob/holybear/integrated/HOLYBEAR-SOURCES.json)均保留。圖片來源與署名可在封面資訊中查看。

## English Summary

This unofficial update adds a multilingual game library, localized titles, cover selection and Discord activity preview improvements. It also includes selected upstream v1.2.4 fixes while preserving HolyBear capture, FPS and audio features. Discord client artwork and capture hardware still require hands-on validation. No release was created by this change.
