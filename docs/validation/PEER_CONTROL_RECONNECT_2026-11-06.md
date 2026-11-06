# 耳機主動控制與有界重連：2026-11-06 驗證

## 本次結論與受測版本

從 `f43ee6009826b33a22910d15ff44235631e90bbc` 延伸的 user-mode 控制／重連實作，已完成本機與隔離 Windows VM 的指定測試。69 個 C/header/build 輸入和 8 個 EXE 的 SHA-256 在 VM 匯入前固定，執行後與本機重新比對；完整原始結果與對應二進位保留在專用資料夾。可公開的[結果與雜湊](peer-control-reconnect-2026-11-06.json)不含密碼、VM UUID、使用者路徑或原始音源。

這是單一 SEP AVDTP peer-control 和明確整首重播的 reconnect prototype。**未載入新的核心驅動、未接觸實體藍牙、未驗證 XM6 播放或 AVRCP 觸控按鍵；尚不符合日用筆電安裝閘門。**

## 實際執行

| 測試 | 本機 | 隔離 VM | 證據範圍 |
| --- | ---: | ---: | --- |
| AVDTP session | 9,381 | 9,381 | prepare/commit、SEID、狀態、過期 generation/serial |
| Signaling reassembly | 292,045 | 292,045 | 大量重複／canary CHECK，不是獨立情境數 |
| Playback integration | 927 | 927 | 真實 libldac/session 配合 fake peer，含重連整合 |
| Reconnect policy | 190 | 190 | attempt stub、重試順序與上限，非 RF |
| WAV source | 818 | 818 | 唯讀檔案、rewind、逐位元組一致 |
| Native I/O | 1,443 | 1,443 | 真實 Windows owned named-pipe、OVERLAPPED、事件與 QPC |
| CLI | 68 | — | 參數驗證，不註冊服務、不開藍牙 |
| WAV/CLI integration | 104 | — | LDAC header 檢查，不是解碼品質量測 |

既有本機回歸：core 5,805；lifecycle 18,976 checks／1,020 schedules；native host shim 7,532。這些仍然是 host-model 證據，不是新的 WDF／Driver Verifier 執行。

VM 實際經 SCM 啟動暫時 Manual `OpenLdacAudioLab` selftest，接收 128/129/130 手動碼率控制，再停止、等待擁有者離開、刪除服務。另實際確認 malformed WAV 的 Win32 11，以及沒有專案驅動介面的 Win32 259；失敗來源檔 SHA 不變，服務回報 STOPPED 後清理。SCM 成功與失敗判定使用結構化狀態，不把 sc start 返回 0 當成播放成功。

## 新增情境與發現

- 在讀 PCM 前、pacing 中、媒體完成後、EOF drain 中注入 SUSPEND/START，恢復後的 RTP/LDAC hash 與沒有暫停的基準一致；保留手動 660 kbps。
- 暫停沒有下一個 START 時只等待事件；stop 可退出，PCM/read 與媒體送出不繼續。
- 回覆傳送失敗不 commit 狀態；錯 SEID／錯狀態拒絕、控制分段重組、連續 65 指令洪流停止；CLOSE/ABORT 都按媒體先於 signaling 退休。
- 真實 pipe 背壓期間抵達控制，當前寫入先完成，不因 signaling 就取消並重送。
- 初始 offset 0 的 pacing 被控制打斷後，收取控制、實際暫停 60 ms、恢復，仍可用相同 offset 完成。測試 fixture 給 100 ms 未來 anchor 作為排程餘裕；正式 40 ms lateness guard 沒有放寬。
- 實際 playback/encoder + fake peer 注入第一次 media 失敗，清理後等待、rewind，第二個 epoch 完整重播；手動 660 kbps、fresh generation/SSRC、媒體數量與正常 SQ 基準相符。
- 獨立唯讀 review 促成兩項修正：安靜 suspended 等待後重設連續指令 budget；peer CLOSE 退休 media 後記錄 CLOSED，而非停留 WAIT_MEDIA_CLOSE。

完整原理、設計限制、重現選項與一手資料見[開發說明](../PEER_CONTROL_RECONNECT.md)。測試沒有量測耳機端每個 sample 的播放確認、RF 重傳率、glitch、聲音評分或電功率。

## 清理與交付

先匯出及稽核結果、保留 SHA 對應 EXE，再正常關機與還原既有 clean snapshot。剛開機第一次清潔檢查的傳檔階段回報 Guest Additions execution service not ready，沒有開始清潔檢查；待服務就緒後重試通過：Secure Boot 開啟、專案音訊／核心服務與憑證為零、BCD test/debug 關閉、Verifier 沒有受測驅動。最後確認正常關機及保留 clean snapshot。未強制關機、未改日用主機驅動或安全設定。

GitHub CI 執行結果以本次 commit 的檢查頁及 draft PR 為準；此文件中的 VM 計數來自上述固定套件，而非 GitHub CI。核心驅動原始碼本次未變；舊 WDF／Verifier 紀錄不得改稱本次耳機控制實測。

## 仍未通過的實機閘門

AVRCP 按鍵與音量、setup／同時 CLOSE 碰撞、完整 peer command 子集合、實體 PnP 裝置重新列舉、真正 BthPort BRB 關閉完成、XM6 重連與音訊播放、Windows 音訊端點、正式簽章與回復、休眠及射頻干擾、可重現的音質／耗電量測。重連沿用已選定介面路徑；路徑消失不保證恢復。整首重播可能重複已聽過的片段，並非無縫續播。
