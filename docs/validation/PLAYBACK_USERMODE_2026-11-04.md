# 原生通道介面與音訊工作服務：2026-11-04

## 本輪問題與範圍

本輪在前一輪低資源 gate 通過後，實作真正呼叫 Windows 原生傳輸介面的使用者模式程式，
串起 AVDTP、LDAC、RTP、WAV 來源與 SCM 音訊工作服務。問題是：協商與媒體是否按正確
順序執行，停止是否等待 I/O 退休，以及服務在正常／錯誤輸入下能否正確退場。

這是**程式整合與所列客體測試通過**，不是已完成 XM6 實際播放。客體沒有專案的
Bluetooth 介面，也沒有實體藍牙 passthrough；真實 BthPort、MediaTek、XM6 聲音、
休眠共存、尾音完整性與電力尚未驗收。沒有把實驗驅動或服務裝到日用 Windows。

實作契約見[原生通道與服務](../PLAYBACK_SERVICE.md)。[公開數值與檔案身分](playback-usermode-2026-11-04.json)
保存執行檔 SHA-256、60 個編譯輸入雜湊、服務結果與失敗診斷。
來源基底為 `ac0fe0eecb2da1e6c28d47a3e2973e01ece5228c`；本輪新增來源在此基底後，
以同版公開檔案及實際編譯輸入辨識，不能把基底提交說成已包含新播放程式。
輸入雜湊計算工作樹位元組，包含換行狀態；它們不是 Git blob ID。

## 新增實作

| 層 | 新增工作 | 尚未證明 |
| --- | --- | --- |
| `src/playback.c` | 驗證輸入、協商、OPEN→媒體→START、編碼封包與收尾 | 耳機端互通／播放 |
| `src/win_native_io.c` | SetupAPI 選擇、雙 handle、真 IOCTL/ReadFile/WriteFile、取消退休 | 本輪沒有向真 BthPort 發出通道要求 |
| `src/wav_source.c` | 唯讀 RIFF 結構／長度預檢，讀取時拒絕非有限 float | 不是所有音訊格式或內容品質分析 |
| `tools/player.c` | Unicode CLI、SCM ServiceMain/HandlerEx、停止與手動碼率 | 不是 Windows 一般聲音輸出端點 |

核心傳輸、INF 與 ABI 本輪沒有改動。兩個既有 handle 分別承載 signaling/media，
媒體只在 OPEN 接受後開啟；使用實際 MTU，沒有把離線固定 MTU 偷帶進播放。
編碼仍在使用者模式，沒有將浮點 codec 加入核心。

## 方法與已執行結果

本機使用專案既有 Zig 0.14.1、C11、Windows x64 建置；播放器另以
`-Wall -Wextra -Werror` 核對。傳入客體前記錄每份 EXE 的 SHA-256，客體先核對
身分才執行。最終客體結果逐檔匯出並在本機稽核；程式不以自己的「測試成功」字串
代替服務外部狀態。三個新增執行測試分別觀察不同層：

| 驗證 | 最終結果 | 可支持的結論 |
| --- | --- | --- |
| Portable fake peer＋真 libLDAC/RTP | 510 CHECK，本機及 VM 通過 | 所列協商順序、精確格式／碼率、尾塊、MTU、錯誤／停止與拆除 |
| WAV reader | 689 CHECK，本機及 VM 通過 | 所列格式、邊界、Unicode、截斷與非有限 float 處理 |
| 真 Windows owned named pipes | 1,196 CHECK，本機及 VM 通過 | 正式 adapter 的 OVERLAPPED、取消退休、monitor 交接、錯誤邊界 |
| 無 RF CLI | 54 項，本機通過 | help/inspect、Unicode、無明確 connect 時拒絕、來源未改寫 |
| 既有 WAV/CLI | 104 項，本機通過 | 既有離線編碼與檔案保護未回歸 |
| 既有核心／協定 | 5,805／4,837 CHECK 通過 | 既有離線功能未回歸 |
| 既有 lifecycle／adapter host shim | 18,976／7,532 CHECK 通過 | host 模型／實際 wrapper seam 未回歸，仍非新核心測試 |

CHECK 包括 helper、重複組合與逐位元組 canary，不是這麼多個獨立故障情境。
named pipe 是真 Windows I/O，但不是 Bluetooth 或音訊裝置。短完成案例使用實際短讀取
及正式 send 的長度驗證器，沒有宣稱成功製造短 WriteFile。

### 實際 SCM 與錯誤退場

客體以一次性、手動啟動的 `OpenLdacAudioLab` 執行三種服務情境，每種完成後刪除：

1. **SCM 無 RF 自測：**外部查到 RUNNING；控制碼 128/129/130 各接受一次，合成
   48 kHz 映射為 990/660/330 kbps，最後請求為330。STOP 接受一次，程式成功報告
   STOP_PENDING，worker 清理後外部查到 STOPPED；服務狀態及 worker 錯誤均0。
   這項控制測試沒有呼叫 codec，不能當成耳機已切換碼率。
2. **壞 WAV 的真正服務路徑：**預檢失敗，結構化 SCM 最終為 STOPPED、Win32 exit11
   （ERROR_BAD_FORMAT）；來源前後 SHA-256 相同。
3. **合法 WAV、沒有原生介面：**真服務 worker 執行介面探索後退出，SCM 為 STOPPED、
   exit259（ERROR_NO_MORE_ITEMS）；沒有 fallback 到其他音訊裝置，來源雜湊相同。

`sc start` 兩個錯誤情境都曾回傳0，因為啟動要求已受理；它不證明 worker 已成功。
驗收讀取 QueryServiceStatusEx 的實際最終狀態／錯誤碼，不從 STOPPED 時無效的 PID
推論程序身分。[Microsoft：QueryServiceStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-queryservicestatusex)

## 保留的失敗與診斷

初版 named-pipe 測試在第一輪通過，第二輪的「10 ms 最終計時等待必須成功」失敗。
舊 CHECK 只印 GetLastError=0，沒有印 adapter 保存的錯誤碼，**首次失敗原因未確證**。
增加診斷、保持原期待後，20 次自然重跑均未重現，不能抹除原失敗。

另以測試專用的60 ms延遲輸入做受控重現，同一類等待得到
`ERROR_TIMEOUT=1460`，觀測相對期限晚56.660 ms。這證明既定延遲保護能造成該類
失敗，與排程假說相符；它不是首次自然失敗的確證。早期測試 EXE 曾被後續重建
覆寫，保留其 SHA-256 與原始輸出；診斷、受控重現及最終 EXE 另行保留。

最後只重構功能測試：monitor 交接直接驗證 pending read／冪等性，停止測試使用
遠期期限加 stop event；另保留強制100 ms延遲必須 ERROR_TIMEOUT 的負例。沒有將
TIMEOUT 當成成功的準時播放，也沒有放寬正式40 ms限制。最終通過的是功能／錯誤
所有權案例，**本輪沒有取得 VM 即時排程性能驗收**。

## 音質優先與效度限制

HQ 預設與手動碼率保留，電池不自動降級。拒絕隱藏採樣率轉換、超 MTU、部分送出
與超過容許的排程延遲。背壓使用事件與有界 I/O，沒有增加系統全域1 ms計時器。
這些是實作政策，不是已測得音質或能效改善。

EOF 只等待編碼後的**發送端名義時間軸**。若最後一包送出完成晚於名義起點，
剩餘等待可能短於該包時長；即使在40 ms容許內，也不能保證耳機尾端已播放。
耳機 jitter buffer、delay reporting、graceful CLOSE 與尾音捕捉仍是音質驗證門檻，
不能把 fake peer 的通過寫成最後一個可聽 sample 已完整播放。

streaming 收到任何非預期 signaling／斷線會停止此 subset；遠端主動命令、分段重組、
重新協商與恢復仍待開發。正式 Windows PCM 端點、簽章／還原包及可復原的實體
BthPort/XM6 測試亦未完成，因此本輪不安裝到日用 Windows。

## 保留與清理

完整結果、失敗與診斷、編譯輸入、符號及精確最終 EXE 已保存在專用資料夾。
客體暫時服務皆已刪除；測試後還原乾淨快照並重查安全開機、服務、憑證與 Verifier。
最終清理狀態以外部保存的還原／fresh runtime 記錄為準；原始碼及證據不隨快照丟棄。
