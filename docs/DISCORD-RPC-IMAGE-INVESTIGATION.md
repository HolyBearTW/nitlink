# Discord RPC 圖片與 WinRT JSON 修復紀錄

本次工作分支為 `holybear/game-search-presence`，修改前 HEAD 為
`d6da1077a1de31b2c8cdf2642edb68878cd2eeac`。產品版本維持 `1.2.5-holybear`。
本輪沒有執行完整 NitLink Build、CTest、Commit、Push 或發布。

## JSON 例外與最小解析修復

原本 `src/discord/discord_rpc.cpp` 的 `WorkerLoop()` 直接執行：

```cpp
bool rejected = reply.GetNamedString(L"evt", L"") == L"ERROR";
```

WinRT 的預設值不會將「存在、但型別為 Null」轉成字串。
用真實 WinRT JSON 實作與獨立 C++ 程式重現，結果為 `HRESULT 0x8000000E`。
這個例外原本被外層 catch 忽略，導致成功回應未更新狀態。

目前由 `src/discord/activity_reply.h` 集中解析。可選字串只有在型別為
`JsonValueType::String` 時才執行 `GetString()`；缺少欄位或允許的 Null
不會呼叫字串 getter。必要的 `cmd`、`nonce` 則拒絕缺少、Null 或非字串。

- `Accepted`：`cmd == SET_ACTIVITY`、nonce 符合最後送出的活動請求、evt 為
  缺少／Null／空字串，並具有合法 data。
- `Error`：相同 command 與 nonce，但 evt 為 `ERROR`，保存錯誤 code、message。
- `Unrelated`：其他 command、沒有待關聯請求或 nonce 不匹配，不更改活動狀態。
- `Malformed`：無效 JSON 或不相容型別，記錄診斷，不重連、不重送、不標示成功。

`data: null` 在清除活動的回應中合法；對有活動內容的更新，不會據此宣稱接受。
`data.name`、`data.assets`、`data.assets.large_image`、`data.code`、`data.message`
也都先檢查型別；錯誤代碼另外驗證整數範圍。

## 驗證

- `tests/discord_reply_tests.cpp`：30 個 JSON 案例全部通過；每個案例後追加合法
  回應以確認恢復能力，另重複解析 `evt: null` 1,000 次，未再拋出例外。
- `tests/discord_image_tests.cpp`：隔離 named pipe 使用真正 `DiscordRPC` Worker。
  驗證無效 JSON、錯誤 evt 型別、缺少／錯誤 nonce、data Null 後，Worker
  仍能接受後續合法回應；不產生多餘重連或重送。封面選取、代理失敗、名稱回退、
  重試均維持原本 start timestamp，重複相同活動不重送。
- 同一隔離程式亦涵蓋三語與日文／emoji 視窗標題組合、名稱優先順序、未知訊號、
  非整數 HDMI 更新率，以及長標題保留。
- 最新隔離 RPC 程式通過 215 項檢查，包含香港代理失敗後改送台灣圖片、
  HDMI 更新保留替代圖片、離開 Switch 停用 Nintendo 備援，以及本機缺少
  AVIF 解碼器時保留遠端網址，不誤判為代理失敗、不反覆重送。
- JavaScript 語法及三語資源一致性通過：433 個介面字串、86 個原生字串。
- 新測試已註冊於 CMake；本輪只直接編譯與執行小型測試程式，沒有執行 CTest。

## 三層狀態

1. `discordRpcStatus: accepted` 僅表示相符的 SET_ACTIVITY 請求得到成功回應。
2. `discordActualImageStatus: externalAvailable` 表示 Discord 代理的 HTTPS GET
   回傳成功，MIME、大小與實際影像解碼都通過；仍不是畫面驗證。
3. `discordClientDisplayVerified` 維持 `false`，沒有以 IPC 或代理成功假裝完成
   真實 Discord 客戶端畫面驗證。

背景代理檢查限於對應已核准封面 URL 的 `media.discordapp.net/external/` 位址，
不跟隨重新導向，限制 8 MiB、6 秒及可解碼點陣圖片，不放寬 WebView2 CSP。
文字、訊號與封面更新皆沿用原本活動起始時間。

## 已取得的圖片證據及限制

使用自己的 PID 執行隔離 RPC，結束後清除該測試活動；沒有讀取或記錄 Discord
帳號資料。公開測試圖片、SteamGridDB、任天堂台灣兩張圖片、香港商店圖片，
各自比較有／無 `name: Switch 2` 的回應，共 10 次均回傳相符 nonce 的成功
SET_ACTIVITY 與 `mp:external/...` 位址。兩組 name 設定沒有造成解析差異。

直接用修改過的原生 RPC 程式做三來源隔離測試亦得到：

| 圖片 | 原始來源 | Discord 代理 | 原生狀態 |
| --- | --- | --- | --- |
| SteamGridDB `72654158f52bf25f9a8101f78aceaa2f.jpg` | HTTP 200、JPEG、1024×1024 | HTTP 200、有效 JPEG | externalAvailable |
| 任天堂台灣 `zelda-yakusai/img/product/package.jpg` | HTTP 200、JPEG、280×456 | HTTP 200、有效 JPEG | externalAvailable |
| 香港商店 `h/e/hero_nhl_v2.jpg` | HTTP 200、JPEG、272×153 | HTTP 415 | proxyFailed |

這證明目前香港圖片可在 NitLink 本機顯示，同時可能無法通過 Discord 代理。
也證明 IPC 接受網址不代表代理能取得圖片。尚未確認畫面上的問號圖示全部由
同一原因造成；不能據此宣稱所有 SteamGridDB／任天堂台灣圖片均已完成顯示實測。

另外，查詢目前預設 Discord application 的公開圖片資產清單，回傳 `[]`。
應用程式已停止自動將 currentGameId、SteamGridDB ID 或未上傳的 `nitlink-logo`
當作封面資產鍵值。持久化封面 URL 經原生載入驗證後，送出原始公開 HTTPS URL；
本機裁切檔案不會變成可公開使用的網址，也沒有上傳圖片。

實際 TX JSON、相符 nonce 的 RX 摘要及代理錯誤寫入
`%LOCALAPPDATA%\NitLink\logs\discord-rpc.log`，保留一份輪替檔案。
新 EXE 尚未重建；既有 1.2.5 測試包仍是修正前的執行檔。

參考官方協定：
[Discord RPC](https://docs.discord.com/developers/topics/rpc#set-activity)、
[Activity Asset Image](https://docs.discord.com/developers/events/gateway-events#activity-object-activity-asset-image)。

## 本輪範圍

JSON 修復完成後，依「其他呢？」繼續完成已授權的圖片來源與視窗標題整合，
詳見 [官方圖片來源整合紀錄](ARTWORK-SOURCE-INTEGRATION.md)。
未改動 Cloudflare Worker 或原生遊戲庫儲存格式。

追加的真實 Discord 隔離測試結果：

| 選擇的圖片 | 實際送出的最後 large_image | 代理／原生狀態 |
| --- | --- | --- |
| SteamGridDB 方形圖片 | 相同公開 HTTPS URL | externalAvailable，已取得代理圖片 |
| Nintendo Taiwan 災厄啟示錄 | 相同公開 HTTPS URL | externalAvailable，已取得代理圖片 |
| 香港商店災厄啟示錄 | 同款 Nintendo Taiwan package.jpg | 香港 HTTP 415 後自動備援；台灣 externalAvailable |
| PlayStation Taiwan 宇宙機器人 | 已核實的 image.api.playstation.com HTTPS URL | externalAvailable，已取得代理圖片 |

每個代理成功結果都產生可透過現有受控 WebView2 資源顯示的 PNG，
使 Discord 原圖參考預覽能反映實際代理取得的圖片。加工封面仍僅在本機使用，
沒有上傳，不將本機檔案當成公開 URL。切換備援沒有修改收藏的原始選圖、
裁切設定或起始時間。所有測試結束後清除自己的測試活動。

仍未驗證 Discord 客戶端實際畫面，也未重建完整 NitLink EXE。
