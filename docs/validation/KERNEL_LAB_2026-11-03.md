# 延後連線、取消與資源回收實驗：2026-11-03

## 範圍與目前結論

本輪在隔離 Windows 虛擬機執行真實 WDF，使用合成 Bluetooth 提供者。
測試生產傳輸函式的要求／回呼／檔案生命週期，不使用實體藍牙 passthrough。
新基準、標準 Verifier、強制 pending I/O ＋ WDF 三輪指定案例均通過，正常卸載亦通過。
這是所選案例／profile 的有限範圍證據，不能當作完整 Verifier 或實機驗收。

本輪未驗證 MediaTek／XM6、真實 BthPort、Windows 音訊端點、AVDTP 完整狀態機、
實際播放、聽感或電池耗電。HQ 預設與手動碼率功能保持原樣，電池模式不自動降級。

## 可重現版本

| 項目 | 值 |
| --- | --- |
| 測試來源 | `4437db13d02e3d175fdec58ef82bd143b23862dd` |
| CI 實際 checkout | `093376d97fb274f348d65ba74b9dc0c35c8c9d7c` |
| CI／來源差異 | 僅兩個頂層 README 的 main 閱讀入口；32 個受查建置輸入相同，完整 Git tree 不同 |
| 測試 ABI | 3，48-byte lab status；與先前 40-byte ABI 2 區分 |
| 未簽章 SYS SHA-256 | `21da713f8d9fa04684f07155611fab678c882c38e264a286fa348a891bd7d63f` |
| 同次 PDB SHA-256 | `6d3e4ebf20ee026cd440171f9994628c9d4e69c85faf5e6d8d120d3457a0bcf2` |
| SYS／PDB GUID | `fa9754e0-5f38-4950-823b-68214b63096d`，age 1 |
| ZIP SHA-256 | `3a53b60038dd6e484fa1c2ad7a18b9bc91df07719e19aea027a79886208680ac` |
| Artifact ID | `13240017421` |
| 客體簽署 SYS SHA-256 | `d8b1e48bf066eae8f67591d13895db20c3c1004dd80fe9deda7892bb40321a82` |
| Runner SHA-256 | `572ee92237a359dbce80fc9b718b4c140019188b97ca771a69ca5b130910f28a` |

三組 CI 均通過：

- [離線與 host 測試](https://github.com/peianchen/open-ldac-windows/actions/runs/41656656785)
- [生產傳輸建置與分析](https://github.com/peianchen/open-ldac-windows/actions/runs/41656656535)
- [合成核心測試驅動建置與分析](https://github.com/peianchen/open-ldac-windows/actions/runs/41656656554)

Runner 使用既有 Zig 0.14.1，以 `-Wall -Wextra -Werror` 建置；未在日用主機執行。
簽章工具使用既有官方 SDK，私鑰與信任僅置於客體。載入前分別核對未簽章、
已簽章 SYS 與 Runner 的雜湊；baseline、unload、Verifier 設定記錄均綁定該組身分。

## 問題、方法與驗收

原合成提供者會立即完成 OPEN，因此先前 363 checks 並未證明「OPEN 尚未完成時
取消或關閉檔案」的真實 WDF 路徑。本輪增加兩種可控制的 lower request 所有權：

1. WDF manual queue 持有要求，由取消回呼或成功 retrieve 的控制命令取得完成權。
2. 提供者持有要求，即使收到取消仍保持 pending，再由命令完成成功或失敗。

七種排程各測回呼在 OPEN 完成前／後退休，重複十次，共 140 案例：
queued cancellation、queued success、取消後延後成功、file cleanup 後延後成功、
取消後失敗、file cleanup 後失敗，以及 CancelIoEx 與 queue retrieval 真正並行競爭。

每案例必須觀察到 pending，收齊同一 OVERLAPPED 的唯一 IOCP 完成封包，
沒有第二個封包；完成前保持輸入與 OVERLAPPED 存活，join worker 後才釋放堆疊。
逐案例比較 OPEN／CLOSE send/complete、callback add/release 與 root destroy。
取消本身不能替代完成，也不能替代 callback retirement；已退休的提供者不再接收 CLOSE。
取消回呼不取得被動層級 wait lock，OPEN 完成時亦不持有會被同步 CLOSE 重入的 PortGate。

IOCP／event／檔案 handle 關閉後，再觀察 `active_files` 回到只有 control handle 的狀態。
這測的是「最終回收後可重新開啟」，不是保證立即重開，也沒有放寬兩個檔案的上限。
最多 500 次輪詢、每次睡眠 10 ms；查詢和排程本身另耗時間，並非嚴格五秒牆鐘上限。
所有輪詢都在使用者態測試工具，未加入生產驅動定時器或忙等。

## 已執行結果

| 環境／profile | Runner | 回呼 add/release；root destroy | 正常卸載 |
| --- | --- | --- | --- |
| 無 Verifier 基準 | 6,343 checks；140 pending OPEN 案例 | 173/173；173 | 通過，Stopped |
| 標準 oneboot Verifier，runtime 0x001209bb | 6,343 checks；140 案例 | 173/173；173 | 通過，runtime load1/unload1 |
| 強制 pending I/O ＋ WDF，runtime 0x00120bbb | 6,343 checks；140 案例 | 173/173；173 | 通過，runtime load1/unload1 |

基準有 21 次檔案退休等待，最長客體等待 32 ms；標準 Verifier 為5次／31 ms，
強制 pending 為7次／31 ms。三輪各執行相同的140案例，不是420種不同排程。
這些是虛擬機內測試查詢與排程時間，
不能當作 Bluetooth 延遲、耳機播放延遲、日用驅動性能或電池耗電數字。

## 保留的失敗與修正

1. `c7d62d4` 核心 CI 的 C6011 擋下可能為 NULL 的 channel 路徑；
   `3c5b96f` 從已取得所有權的 request context 重新取得 channel，未關閉分析或壓掉警告。
2. `3c5b96f` 實際基準在完成 11 個新案例後，下一次 CreateFile 回報 Windows error 4
   （檔案數上限）。客體沒有當機。這次失敗保留為失敗，沒有拿先前 363 checks 代替。
3. IOCP 完成／root 歸零不代表 WDF 已執行最後的 EvtFileClose。新測試先關閉所有
   使用者態案例 handle，再以獨立 `active_files` 計數觀測最終退休；不藉由增加上限或
   無限延長等待讓案例通過。後續基準觀察到有限的非零等待，支持延後退休的解釋，
   不能據此宣稱已修正生產驅動的立即重開行為或證明所有引用不存在洩漏。
4. 失敗後另外以 inspect-only 查到 roots0、I/O0、回呼11/11，才正常卸載舊測試版本。
   inspect-only 是復原觀測，不能算通過整輪案例。

## 尚待的證據與限制

標準／強制 pending 均保留實際 runtime flags、選定模組、開機前 WDF rule class34、
runner 與 unload 證據。本次 oneboot 生效後清空了「下次開機」設定；因此以來源／雜湊與
開機時間連結開機前設定，再以 `/query` 的作用中 flags 和模組載入／卸載證明執行。
不能把 `/querysettings` 單獨當作作用中驗證，亦不能把 oneboot 清空設定誤判成沒有執行。
集區統計為零，仍不表示所有 WDF 配置都被追蹤。
低資源故障仍需能接受預期配置失敗
的 runner、實際注入統計及完整清理證據；未觀察到注入不能判通過。
現代 Windows 已棄用 kernel synchronization delay fuzzing；確定性案例不能替代
所有 IRQL、並行、實體 PnP／電源或 BthPort 路徑。

虛擬機已透過 VirtualBox 正式搬入目前專用資料夾，UUID、磁碟鏈與乾淨快照保留。
歷史原始證據不改寫；敏感資料不公開。結果、私人階段紀錄與雜湊符合的已簽章 SYS
均已匯出後，回復乾淨快照，實際重新開機檢查：Secure Boot 開啟、測試服務／標記／
三個存放區內的測試憑證不存在、BCD 無 testsigning/debug、Verifier 沒有作用中驅動。
最後正常關機並確認 poweroff；只保留一台 VM 與一個乾淨快照。
本輪客體測試憑證、註冊服務與大型傾印預留緩衝隨測試 delta 一起丟棄。
日用主機沒有載入實驗驅動；唯讀檢查亦沒有發現專案服務或測試憑證。
主機 BCD 唯讀查詢被拒絕，未修改或嘗試繞過該限制；客體 BCD 已實際檢查。

## 官方依據

- [WDF 取消回呼的 ownership 與 IRQL](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfio/nc-wdfio-evt_wdf_io_queue_io_canceled_on_queue)
- [ForwardToIoQueue](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestforwardtoioqueue)
- [RetrieveNextRequest](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfio/nf-wdfio-wdfioqueueretrievenextrequest)
- [EvtFileClose](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdevice/nc-wdfdevice-evt_wdf_file_close)
- [Verifier 命令與 rule classes](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/verifier-command-line)
- [Synchronization delay fuzzing 棄用說明](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/kernel-synchronization-delay-fuzzing)
