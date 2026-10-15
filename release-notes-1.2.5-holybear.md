# NitLink 1.2.5 HolyBear Edition

**非官方社群整合版（Unofficial Community Edition）。**

保留 HolyBear 的 HDMI／VRR、FPS／Present Cap、低延遲音訊與更新功能，選擇性移植官方 NitLink v1.2.4 修正；並非 nitlink-dev 官方發行版。

## 本次更新

### 遊戲、我的遊戲庫與 Discord

- 新增獨立「遊戲與 Discord」頁面，支援繁中、簡中與英文搜尋、完整結果捲動、真實 API 封面分頁及 Full／Docked 響應式介面。
- 收藏與最近遊玩保存在原生 Config；各遊戲獨立記住名稱、封面、來源與裁切，啟動直接還原。移除收藏不刪除畫質設定。
- 優先提供香港任天堂商店圖片，保留 Nintendo Taiwan 精選素材與 SteamGridDB 候選。圖片語言未確認時，不標示為繁中。
- 加入真正輸出正方形圖片的「完整顯示／填滿裁切／原圖」編輯器、拖曳縮放、84／64px 參考預覽與本機快取。
- 「調整封面」緊接「儲存此遊戲畫質」，兩個搜尋按鈕採用相同樣式；保留既有設定操作。
- 同一遊戲更換封面、語言或 HDMI 來源保留計時，切換遊戲才重新計時。預覽區分本機圖片與 Discord 實際可用狀態。

### 官方 1.2.4 修正

- 自適應學習重複影格雜訊，改善部分 30 FPS 內容經 60 Hz 擷取時被視為 60 個不同影格；保留 tile rescue 與平滑。
- Elgato 4K X 卡端 tone mapping 跟隨實際協商的 P010／SDR，涵蓋開啟、回退與重開，不對 GC553Pro 套用 Elgato 命令。
- F1 關閉後縮零並非同步隱藏 WebView drawing surface，觸發 composition refresh；保留 Full Wide／Left／Right 與安全恢復。
- 修復 About 貢獻者行內文字被分隔的問題；保留原有姓名、帳號、圖示與連結。

### 保留既有整合

- 保留 GC553Pro HDR10／P010／NV12、HDMI Timing／VRR、手動來源、No Signal 與訊號重新同步。
- 保留 Content FPS 主顯示、Tab 輔助資訊、非整數更新率與 Present Cap 排程；自動模式無法取得更新率時仍採 30 Hz fallback。
- CN_115 WASAPI、漂移補償、格式轉換、獨占／焦點回退不變；預設維持 FIFO 12 ms／Render 10 ms。
- 自動更新檢查預設關閉，啟用後每 24 小時最多一次，不自動下載或安裝；Setup／Portable、GPG／SHA256 流程保留。

## 測試與驗證

- 本地驗證涵蓋 x64 Release、完整 CTest、真正 FrameDiffer WARP、原生 WebView2、安全及三語資源。詳細紀錄見[移植盤點](https://github.com/HolyBearTW/nitlink/blob/holybear/game-search-presence/docs/UPSTREAM-1.2.4-PORT-AUDIT.md)，封裝時由流程附加實際結果，不預填成功數字。
- 歷史 GC553Pro／音訊紀錄來自既有 1.2.4 安裝版，不代表 1.2.4.1 正式產物，也不代表本次 1.2.5 硬體實測。
- 本次沒有自動發布、建立 Tag 或產生正式 GPG 分離式簽章；腳本測試與非發布封裝不等於正式簽章。

## 已知限制

- 4K X 的 4K120 HDR→NV12 色彩、P010 HDR，以及 GC553Pro HDR／SDR／VRR／訊號切換仍須硬體實測。
- F1 controller／bounds 自動化不等於每一種 GPU／桌面合成環境均沒有殘影。
- Discord IPC 接受網址不等於客戶端成功繪製。新增香港圖及加工圖仍須明確標示可用狀態；加工圖片沒有自動上傳，不傳本機路徑給 Discord。
- 瀏覽器 Host 的硬體／原生 Config 橋接是模擬，真實搜尋／圖片來自 API 或官方來源，不用 Fixture 代替硬體證據。
- Content FPS 是影格內容變化估計，不是遊戲引擎 FPS。沒有提供 Steam Frame Generation；EXE／Setup 沒有 Windows Authenticode 發行者簽章。

## 來源、授權與致謝

- 原始專案：[nitlink-dev/nitlink](https://github.com/nitlink-dev/nitlink)，固定來源 [v1.2.4](https://github.com/nitlink-dev/nitlink/tree/0a093cc8e41fd04ffe13196122dc1a03dc9a0d6a)。
- 保留 [#23](https://github.com/nitlink-dev/nitlink/pull/23)、[#25](https://github.com/nitlink-dev/nitlink/pull/25)、[#31](https://github.com/nitlink-dev/nitlink/pull/31) 的 HDMI／VRR、手動來源／Discord 與 FPS／Present Cap 修改。
- 低延遲音訊與簡中：[CN_115（@CN115）](https://github.com/CN115/nitlink-lowlatency/tree/v0.1.2)。整合：[聖小熊（@HolyBearTW）](https://github.com/HolyBearTW)，保留 Nathan K.（@n810K）等既有貢獻者致謝。
- [MIT 授權](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/LICENSE)、[第三方授權](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/LICENSES.md)、[致謝](https://github.com/HolyBearTW/nitlink/blob/holybear/integrated/ACKNOWLEDGMENTS.md)與[來源紀錄](https://github.com/HolyBearTW/nitlink/blob/holybear/game-search-presence/HOLYBEAR-SOURCES.json)完整保留。圖片來源與署名保留在詳情，不把加工圖標為官方原生素材。

## English Summary

This unofficial update adds multilingual game search, a persistent library, Nintendo HK/TW artwork and square-cover editing. It selectively ports upstream 1.2.4 adaptive duplicate detection, negotiated-format 4K X tone mapping and F1 composition refresh. HolyBear audio defaults remain 12/10 ms. Hardware and Discord client rendering still require validation. This local integration task does not publish a release or sign official artifacts.
