# 耳機主動控制與有界重連

這一階段研究「已建立的 AVDTP 串流如何接收耳機指令，以及連線損失後如何重建播放擁有權」。實作是單一 SEP 的實驗性子集合；AVRCP 觸控按鍵、音量與換曲、Windows 音訊端點、真實 XM6 射頻互通仍待完成。測試層級與數據見[驗證紀錄](validation/PEER_CONTROL_RECONNECT_2026-11-06.md)。

## 為什麼要拆開控制與媒體

AVDTP signaling 和 media 使用不同通道。媒體寫入進行中收到 signaling，不必然是斷線：可能是 SUSPEND。若此時直接取消 WriteFile，再重送相同 RTP，原寫入可能已經完成，造成重複音訊。現在由同一個 owner 完成有界寫入、記錄結果，再處理控制；真正的失敗不在原連線上盲目重送。CancelIoEx 仍只是取消請求，OVERLAPPED 完成必須收回後才能釋放資源。

控制封包先經固定 1024-byte／16-fragment 重組。已建立串流可接收 SUSPEND、START、CLOSE、ABORT。傳入 ACP SEID 指的是本地端點，不是先前 DISCOVER 找到的遠端 SEID。prepare 驗證狀態與封包並產生回覆，沒有狀態副作用；回覆寫入成功才 commit，generation／serial／原狀態阻止過期回覆重複提交。

```text
STREAMING -- SUSPEND / ACCEPT --> SUSPENDED
SUSPENDED -- START / ACCEPT   --> STREAMING
STREAMING or SUSPENDED -- CLOSE / ACCEPT --> retire media --> CLOSED
STREAMING or SUSPENDED -- ABORT / ACCEPT --> retire media + signaling
```

錯誤 SEID、錯誤狀態回覆對應拒絕；無效 SEID 的 ABORT 按規範不回覆。未知 signal identifier 使用 GeneralReject；已知但不支援的指令回覆 NOT_SUPPORTED_COMMAND。多 SEP 清單、malformed packet、setup 交易碰撞以及同時本地主動 CLOSE 的碰撞仍 fail closed。這些是明確邊界，並非完整 AVDTP 認證實作。

## 暫停期間的音訊與時間

SUSPEND 回覆前，既有媒體寫入已完成；擁有者凍結 QPC 播放時鐘。暫停時不讀取 PCM，不改碼率，使用 stop/signaling 事件等待，沒有忙碌輪詢或全域 1 ms timer。恢復時將 anchor 和最後媒體完成時刻一起加上暫停時間；原本 40 ms 遲到保護不變。被控制打斷的 pacing 保留同一 sample offset；RTP 只建一次，待送封包留在 owner 堆疊，沒有重編碼或重送已完成封包。

尚未完成的控制重組與回覆共用 5 秒 deadline，連續最多 64 個完整指令避免控制洪流餓死媒體。正常安靜暫停可持續等待，使用可取消的 5 秒事件等待；安靜等待後重設連續指令計數。完成 sender drain 前收到控制也可交接；sender drain 不等於耳機已播放完最後一個 sample。

## 重連政策：明確重播，保留音質

新增選項 `--reconnect-restart 0..3`，預設 0。啟用後只對列出的連線損失 Win32 錯誤進行最多三次重試，等待依序 1、2、4 秒，stop 可打斷。一般 timeout、資源不足、協定拒絕、PCM／codec 錯誤及耳機主動 CLOSE／ABORT 不重試。

```powershell
# 僅限已通過對應安裝閘門的實驗環境；這不是日用筆電安裝指令。
./build/wldac_player.exe --play source.wav --connect --bitrate 990 --reconnect-restart 2
```

每次重試先完全退休舊 handle、encoder、session，再從原先已開啟的唯讀 WAV 開頭讀起。generation 嚴格遞增，SSRC 更換，重新協商 capability／MTU。原先選定的介面路徑固定，避免 enumeration 順序改變時接到另一個裝置；介面被移除或改名可能需要重新啟動播放器。

沒有接收端播放確認時，無法精確知道斷線前哪個 sample 已聽到；因此目前明確重播整首，而不是宣稱無縫續播。JSON attempts/restarts 與 last-attempt 計數揭露此行為。手動碼率沿用最後一次有效選擇，待處理的新手動要求仍可由 owner 接收；48/96 kHz 家族為 990/660/330，44.1/88.2 kHz 為 909/606/303。沒有切到電池就降碼率、靜默 resampling 或因斷線自動降品質的策略。

## 研究方法與後續實測

比較四個控制到達點：讀 PCM 前、pacing 中、媒體完成後、EOF drain 中。主要觀察是不在暫停時讀 PCM、恢復後 RTP/LDAC 位元組與無暫停基準一致、正常關閉 ACK 先於本地媒體退休、取消後無未回收 I/O。重連分成協調器故障注入、實際 encoder/session 配合 fake peer、Windows named-pipe、隔離 VM 的 SCM 四層，不能互相取代。

後續真實 XM6 閘門需要檢查 AVDTP/AVRCP 行為、BthPort BRB 關閉完成、連線損失回報、射頻干擾與休眠恢復、重新協商後的尾音、glitch/延遲，以及同音源同音量下的電功率 A/B 測試。event wait 與有限重試是節能設計，不是實測續航改善。此次沒有新增 kernel 驅動版本或進行新的 Driver Verifier runtime；舊核心測試證據保持獨立。

## 一手資料

- [Bluetooth SIG AVDTP 1.3](https://www.bluetooth.org/DocMan/handlers/DownloadDoc.ashx?doc_id=260860)，§§8.13–8.18、8.20.6：START/SUSPEND/CLOSE/ABORT、錯誤處理與狀態。
- [Bluetooth SIG AVDTP Test Suite p22](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2025/02/AVDTP.TS_.p22.pdf)：控制與分段測試方法；未宣稱通過 SIG qualification。
- [Microsoft CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)：取消不代表 I/O 完成。
- [Microsoft Win32 errors 1000–1299](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--1000-1299-) 與 [0–499](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-)：重試分類依據。
