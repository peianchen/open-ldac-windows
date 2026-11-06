# 分段協商與交易期限驗證（2026-11-06）

本輪在播放協商路徑加入有界的 AVDTP 控制訊息重組，以及整筆交易共用的逾時。
已通過本機及隔離 Windows VM 軟體／服務測試；尚未建立 MediaTek/XM6 實際互通。
來源基底為 `d10a1312b2d2f66aea9875ba509d69f6542d69ca`，本文件所在 commit 為本輪來源。
精確執行檔與建置輸入 SHA256 見[機器可讀紀錄](signaling-reassembly-2026-11-06.json)。
輸入雜湊是測試時工作樹位元組，可能與 Git 換行正規化後的 blob 不同。

## 實作與原理

原本 complete-message session 直接拒絕片段。本輪將 framing 與語意分開：
`receive → bounded reassembly → tuple matching → capability/session parsing`。
只有完整訊息能進入 session；部分片段不影響協商狀態。新狀態不配置 heap，
每則訊息最多16片、重組後1024bytes。錯序、錯誤label/type、錯誤count、超出容量
或中途插入另一訊息會讓重組器失效，必須reset才可重用。

每次送出命令前記錄單調時間。送出與全部接收共用5000ms；傳入 receive 的
timeout 隨剩餘時間遞減，進度不重設期限。完整回覆在期限上或之後才被觀察到
也不接受。另有64次wire接收與16個不相符完整回覆上限，避免假／故障時鐘不走
時無限迴圈。早於交易起點的時間讀值會失敗；adapter契約要求所有讀值單調。

這是應用層協商等待界線，不代表不合作的底層驅動必定在五秒內完成取消。
既有 CancelIoEx／GetOverlappedResult 退休等待仍然保留，以維持記憶體安全。

## 已執行結果

| 測試 | 本機 | 隔離 VM | 意義 |
|---|---:|---:|---|
| 分段重組 | 292,045 CHECK 通過 | 同數通過 | 分片邊界、標頭／計數、容量canary、固定seed亂數序列 |
| AVDTP session | 6,608 通過 | 同數通過 | 既有協商／CLOSE語意回歸 |
| 真編碼器＋假對端播放 | 867 通過 | 同數通過 | 分段接收、期限／取消、碼率與清理順序 |
| WAV reader | 689 通過 | 同數通過 | 格式、長度、來源保存 |
| Windows owned-pipe I/O | 1,303 通過 | 同數通過 | 真Windows I/O、取消／尾段等待、單調時鐘binding |
| 無RF CLI | 54＋104 通過 | 未在本輪VM執行此Python套件 | 命令列／檔案保存、碼率與LDAC標頭 |
| 既有 core／lifecycle／adapter shim | 5,805／18,976／7,532 通過 | 未重新載入kernel | 離線與模型回歸 |

CHECK數包含重複流程與每次push後的canary斷言，不是同等數量的獨立情境。
重組測試遍歷標籤／訊息類型／訊號值、多種二片／三片切割，並使用2000組
固定seed的隨機序列；這不是無界fuzzing或協定認證。

整合測試使用62個不同且未占用的audio-sink SEID，組成126-byte DISCOVER，
經三片跨48-byte MTU傳送。正常LDAC capabilities維持有效且不擴充假能力。
重組後媒體封包雜湊與未分段流程一致；這只表示測試來源／假對端的位元組一致。
亦涵蓋先重組不相符回覆再接受正確回覆、缺END、早END、錯transaction、插入SINGLE、
錯CONTINUE、64次接收上限、逐次故障／停止、期限邊界及停止優先。

虛擬機另實際註冊臨時手動 SCM 服務：RUNNING、三種品質控制、STOP_PENDING、
STOPPED與移除皆通過。壞WAV與無原生介面案例回報錯誤11／259，來源雜湊不變。
SCM控制映射自測沒有編碼或發送；真編碼器由假對端播放測試覆蓋。

## 審查與效度

獨立唯讀審查未發現阻擋性生產程式錯誤，但發現首版大型能力fixture使用不當的
Delay Reporting長度。已在最終本機／VM測試前換成合法大型DISCOVER，避免把
容忍不明TLV誤當成有效能力互通。另修正文檔中過時的片段限制及過廣的時鐘描述。

所有控制封包都在測試來源產生，沒有取得真耳機封包追蹤。重組層允許有界的
空payload片段，但這是相容策略，不宣稱規格要求。同標頭CONTINUE的內容重複／
交換無法單靠此格式辨認，仍依賴L2CAP有序傳送與後續語意檢查。

目前只在主動協商交易內接收分段回覆。串流期間的未預期控制訊息仍fail closed，
耳機主動SUSPEND／RECONFIGURE、交錯交易、重連、delay reporting、實際BRB關閉、
Windows音訊端點與正式簽章／回復安裝仍待完成。沒有改動kernel、INF或原生ABI，
沒有在日用筆電安裝實驗驅動／服務，也沒有聲稱真實藍牙、音質或耗電測量成功。

HQ預設、手動碼率、電池不降級、40ms媒體遲到保護與前輪尾段等待均保留。
固定緩衝與事件等待是耗電設計取向，不是實測節電數字。

## 可重現性與來源

沿用 Zig 0.14.1；新增模組與播放測試另以C11 `-Wall -Wextra -Werror`建置。
CMake／Zig／Windows及Linux CI與ASan/UBSan目標均加入重組測試。
所有最終EXE與63個C/header/player輸入，另加CMake、Zig helper與CI設定三檔，
共66個輸入雜湊留存。VM無實體藍牙直通，證據與原始碼在專用資料夾保留。

[設計契約與官方／主要實作來源](../SIGNALING_REASSEMBLY.md)說明標頭格式、
限制及來源範圍。SIG TS是官方測試套件，不能取代完整核心規格或資格認證。

## 測後還原與保留的工具失敗

完整結果與SHA256固定執行檔已匯出／核對，測試VM正常關機後還原原有乾淨快照。
第一次fresh-clean檢查在45秒GuestControl時限失敗；當時沒有結果JSON，且已無
active guest process，因此沒有將其計為通過，也無法確認原始卡住階段。
保留逾時紀錄，加入階段輸出、CIM操作10秒時限，controller改120秒後重跑通過。
這是測後環境檢查，不是播放測試期限；正式協商5000ms與媒體40ms限制未放寬。

新結果確認Secure Boot開啟，專案核心／音訊服務、測試憑證／標記皆為零，
BCD test/debug關閉、Verifier無執行中選定驅動。最後正常關機確認poweroff，
沒有強制關機，VM及乾淨快照保留。日用筆電沒有載入實驗驅動／服務。
