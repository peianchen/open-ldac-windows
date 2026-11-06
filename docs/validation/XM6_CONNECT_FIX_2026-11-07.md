# CONNECT completion 修正與重新驗證紀錄：2026-11-07

已修正原生 CONNECT IOCTL 回傳錯誤 bytes 數的問題，並完成可重現的 red／green 回歸、8,738 項使用者模式 adapter 檢查，以及三組 GitHub Actions 驗證。新原生套件已核實來源並完成 guest 測試簽章。**最終連續流程已在實體 XM6 通道觀察到 CONNECT bytes=0，並完成一份 2 秒來源的 990 kbps LDAC 發送、正常結束與資源退休。其後兩次重播也完成發送，使用者最後確認「兩耳都有聽到，音量合適」。這是短立體聲測試音的發送證據與主觀雙耳可聞性確認，尚不是音質、聲壓或耗電量測。**

檔名依臺灣日期 **2026-11-07（Asia/Taipei）**；本輪記錄的 UTC 日期仍是 **2026-11-06**。首次發送段落的原截止點仍為 `2026-11-06T16:27:38.2642967Z`；本次追加重播後，工具結果延伸至 `2026-11-06T16:35:19.7664988Z`，對應臺灣 11 月 7 日 00:35:19。使用者於最後一輪後另行確認雙耳聽到，本文未記錄該回覆的精確 UTC 時刻，也不回填改寫首次發送當時尚未確認的狀態。[JSON 摘要](xm6-connect-fix-2026-11-07.json) 與本文件使用相同驗證界線。

## 1. 從實體觀察重現錯誤

[前一階段報告](XM6_NATIVE_LOAD_2026-11-06.md) 已證實舊版真正原生驅動在 VM 載入、XM6 接受 DISCOVER，以及正常關閉時 callback／root 的退休。該版第一次 HQ 播放停在 stage 1，`win32_error=13`，尚無媒體封包。

加入觀察後發現，沒有輸出 buffer 的 CONNECT 應回傳 0 bytes，實際卻回傳 **368 bytes**。其後獨立 QUERY 可回傳正確的 64-byte／ABI 2／OPEN 狀態，outbound MTU 895、inbound MTU 1008，並不是這兩個 MTU 超出程式允許範圍。播放器在 CONNECT bytes 非零時正確拒絕繼續；原 DISCOVER probe 當時未檢查該 bytes 條件，所以能往下完成訊令交換。

錯誤位於下層 BRB 的 `Information` 被保留至對外 CONNECT completion。`WdfRequestComplete` 提供完成狀態；要明確設定完成資訊，可用 `WdfRequestCompleteWithInformation`，其效果相當於先設定 Information 再完成 request。[Microsoft WdfRequestComplete](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestcomplete)、[CompleteWithInformation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestcompletewithinformation)

修正在 [commit dd653832](https://github.com/peianchen/open-ldac-windows/commit/dd653832751f6e2f52341f337fc9619c2be0f9e6)：CONNECT 的成功、失敗、取消與早期拒絕路徑明確完成為零 Information。這是對外 IOCTL 的契約，不是把下層內部 BRB 長度當成使用者收到的資料。播放器的嚴格 bytes 檢查予以保留，沒有為了讓播放通過而放寬 ABI。

## 2. 為何先前模擬沒抓到：修正測試替身

原 host shim 中的普通 Complete 行為遮蔽了殘留 Information。新的 [adapter 回歸測試](../../tests/native/native_adapter_tests.c) 改成讓普通 `WdfRequestComplete` 保留現有值，並在下層 OPEN completion 注入實測值 368。於是測試可以區分：程式是否真的把對外 bytes 清為 0，或只是測試替身替程式做了這件事。

| 階段 | 條件 | 結果 |
| --- | --- | --- |
| Red | 修正後的替身，加上仍使用普通 Complete 的舊 production 路徑 | exit 1；`r->information==0` 失敗 |
| Green | 同一替身，加上明確零 Information 的 CONNECT 修正 | exit 0；8,738 checks 通過 |

測試涵蓋 OPEN 成功／失敗、同步或稍後完成、已取消、配置失敗、format／send 失敗、與 file cleanup 交錯及裝置移除時的拒絕。原 red／green EXE 連同雜湊及輸出已保留，保存後重新執行所得結果相同；保存程序沒有把新編譯結果冒充成原實驗二進位檔。

這些是編入實際 adapter 程式碼的**使用者模式 host-shim 測試**，以 Zig 0.14.1、`-Wall -Wextra -Werror` 建置。沒有載入核心、接觸藍牙或執行 RF，也不構成真正 KMDF／Driver Verifier 驗收。8,738 是本次整組 adapter 檢查數，不是新增 8,738 個獨立實體耳機情境。

## 3. CI 與修正版套件的來源鏈

三組 run 已完成且相關 jobs 成功；「KernelLab build 成功」仍只代表 lab driver 可以建置，不等於在 guest 執行過 Verifier。

| CI | Run | 驗證範圍 |
| --- | --- | --- |
| Native transport | [42249367647](https://github.com/peianchen/open-ldac-windows/actions/runs/42249367647) | KMDF 原生建置、靜態分析、INF、套件完整性 |
| KernelLab | [42249367654](https://github.com/peianchen/open-ldac-windows/actions/runs/42249367654) | unsigned VM lab driver 建置 |
| Offline | [42249367224](https://github.com/peianchen/open-ldac-windows/actions/runs/42249367224) | Windows／Linux offline core 及 lifecycle sanitizers |

取回 native artifact `13519528675` 後，ZIP SHA-256 與 GitHub digest 一致，安全解壓只接受四個預期檔案。manifest 的 build SHA 是 `dd653832751f6e2f52341f337fc9619c2be0f9e6`，這次是 push run，沒有混用先前 PR merge artifact。23 項原生建置輸入與當時工作樹一致、八個 C 編譯單元、不含 KernelLab provider；原 INF 及 VM-only 衍生 INF 的本機 `InfVerif /w` 都回傳 0。

公開 INF 不擴大匹配範圍。隔離套件仍使用本機 `LOCALMFG` 變體，它不是 XM6 專屬 hardware ID，因此安裝工具保留唯一 Audio Sink、精確私有 XM6 instance／parent／位址後綴／container、guest 身分、簽章及重啟條件。新工具由既有工具建立副本，只更新修正版來源與雜湊固定值；先前工具與證據保留。

| 修正版材料 | SHA-256 |
| --- | --- |
| CI ZIP | `C9D576E2C259919BB06CAB4A098F30E284357DBE2DE7F2B2FD2E92372C473945` |
| unsigned SYS | `32891878F5B9199C8A4D9D3A678FA743B693D4CE0CEFD129AFAD08C9D236BF98` |
| VM-only INF | `0997A168598B3FE971352DC539D9E97AB3BDCB0CF4B451DA56C114C50533E167` |
| guest 簽署 SYS | `B19DA12DCE86810FF76597D343F223ED28039E48623C90053CDE6FFE6209E0C5` |
| guest 簽署 CAT | `286758AB9B90DE3FB688C5316A3E6CAB6A06702FB33996AB26D9B50F643AB91A` |

簽章維持 SYS → 重建 CAT → CAT → membership 驗證順序，簽章狀態已通過、私鑰沒有匯出。簽章套件及憑證只保存在本機／guest 實驗區域，沒有作為公開安裝檔發布。它不是正式發行簽章；其後已在下述穩定 filter 輪次核實 guest 載入。

## 4. 初期實體重測在新驅動安裝前停止

本次先回到 paired-stock VM 基準，再準備修正版。其後遇到的是 USB 移交與身分盤點的障礙，不能將這些安裝前的失敗歸因於尚未載入的新 SYS。

| 試驗 | 觀察 | 停止位置與回復 |
| --- | --- | --- |
| 16:13:53–16:14:13 UTC | inquiry exit 0；捕捉前電源狀態 D0；VM 實際附掛仍為 false | 尚未安裝新驅動；detach exit 1，host restored=true |
| 更新盤點後的 preflight | 同目標出現重複／殘留列舉，無法建立唯一身分 | 在提出新 attach 前拒絕；未安裝新驅動 |
| 16:18:50–16:18:54 UTC clean-server 對照 | 正常關閉 VM，等 VBoxSVC 自行結束後重新盤點；inquiry exit 0，但完整描述子身分不足 | `Refreshed full descriptor identity required`；未 attach，host restored=true |

第一輪紀錄中的舊快取描述標籤為 Foxconn，更新後為 MediaTek，VirtualBox 內部 UUID 也改變；同時有重複／殘留項目，VM 實際附掛清單沒有完成目標接管。這是身分映射與捕捉完成條件不一致的觀察，不代表實體控制器真的換成另一顆。UUID、序號與原始 USB 記錄不公開。

更新盤點後的 preflight 中止目前依控制程序觀察記錄，對應資料夾沒有完成的 summary；不能把它寫成一次已完成的 capture 試驗。clean-server 過程採正常 VM shutdown 與 VBoxSVC 自行退出，沒有把強制終止 VirtualBox 程序當作例行復原步驟。

首次失敗輪次的 detach exit 1 也照實保留。當 VM 從未確認附掛成功時，detach 命令不成功與主機 radio 健康狀態恢復可以同時存在；不能只留 `host_restored=true` 就把整輪改寫為成功。

### 穩定 filter 對照：移交與新驅動載入通過，CONNECT 失敗

**16:22:28–16:23:30 UTC** 的單次對照使用穩定身分 filter 與 D0 觀察，確認實際附掛為 true，並觀察到附掛使用的 VirtualBox UUID 已改變。新套件 Validate／Install 均回傳 0、DevCon exit 0；實際載入映像雜湊與修正版 guest SYS `B19DA12D…E0C5` 一致，無安裝後重啟要求。這是修正版核心已在 guest 載入的證據。

但後續 DISCOVER probe 在 CONNECT 就回報 **Windows error 121**，控制程序 exit 33，沒有輸出成功的 CONNECT completion bytes 或 DISCOVER accepted。不能因此宣稱已在實體上確認 bytes=0，更不能宣稱已播放。該輪保留的零狀態／診斷查詢發生在 DISCOVER **之前**；它們不能當成逾時後清理完成的證據。

此輪結束時臨時 filter 已移除、detach exit 0、host restored=true。使用者隨後確認耳機保持開機且其他裝置已斷開，但下一次 preflight 又遇到重複／殘留身分而在硬體動作前停止。其後連續單流程的成功結果分開記錄如下，不覆寫這次 CONNECT 失敗。

## 5. 首次修正版 990 kbps 真實發送成功

**16:27:15–16:27:38 UTC（臺灣 00:27:15–00:27:38）**，在使用者確認耳機開機、其他裝置已斷開後，以重新盤點的穩定身分與單次連續流程執行。fresh Audio Sink SDP 回傳 **1 筆、67 bytes、error 0、完整列舉**；接著 CONNECT 回傳 **0 bytes**，QUERY 的 size 64／ABI 2／state 2、MTU 895／1008 均正確，XM6 接受 DISCOVER，SEID 為 1。

這次直接觀察到修正後的 CONNECT 零輸出契約，因此不再只依靠 mock 或 CI 推定問題已修好。隨後的原生播放報告為：

| 欄位 | 實測報告 |
| --- | --- |
| result／Win32／protocol／encoder error | 0／0／0／0 |
| requested／actual kbps | 990／990 |
| source frames | 96,000，48 kHz stereo PCM16 的 2 秒來源 |
| media packets／media bytes | 376／252,718 |
| bitrate changes／attempts／restarts | 0／1／0 |
| battery downgrade | false |
| 最終 stage | 15，`WL_AVDTP_SESSION_CLOSED` |

stage 15 的意義核對自 [session 狀態定義](../../include/avdtp_session.h) 與 [播放結束路徑](../../src/playback.c)，是 session 已關閉，不能誤讀成「停在未完成的第 15 步」。990 kbps 是本次協商／編碼與發送報告值，不是額外量到的無線實體層速度；2 秒是來源長度，不是精密測得的耳機端延遲或播放時間。

播放後與最後一次 QUERY 均為 connections／retained connections／in-flight **0／0／0**，兩次 diagnostics 相同：open attempts／completions 各 3，callback add／release 各 3，close attempts／completions 各 3，roots destroyed 3，transfer retirements 394；open failures、close failures、contract faults 均為 0。

這些計數涵蓋**先前一次 DISCOVER signaling channel，加上播放的 signaling 與 media 兩條 channel**，不是三條音訊 media channel。累計 bytes sent 252,758、received 78 包含訊令，不應與 media bytes 252,718 混為一談。remote disconnects 與 cancel calls 為 0 也表示本輪沒有執行這些故障注入，不能宣稱故障條件全部通過。

在首次發送的原截止點，證據支持真實 radio／BthPort 路徑上的 LDAC 發送流程完成與正常資源退休；當時沒有耳機解碼輸出擷取或使用者「已聽到」確認，使用者先要求再播一次。這項歷史狀態保留；後續聆聽確認記錄於下一節，不回填為第一次已聽到。

本輪臨時 filter 已移除、detach exit 0、host restored=true。單次短片段成功不代表冷啟動、休眠、長時間播放、邊聽邊斷線或所有控制器的穩定性已驗證。

## 6. 重播、提高測試音振幅與使用者確認

第一次成功後另有三個輪次，失敗也一併保留：

| 輪次（UTC；臺灣時間另加 8 小時） | 來源與程序 | 結果 |
| --- | --- | --- |
| 16:28:35–16:29:04 | 播放前 fresh Audio Sink SDP | lookup_begin error 10108、0 筆、未完整列舉；在 CONNECT／播放前停止 |
| 16:30:14–16:30:47 | 直接 CONNECT／DISCOVER，2 秒、−40 dBFS 來源 | CONNECT bytes 0；DISCOVER accepted；990 kbps 發送成功並關閉 |
| 16:35:00–16:35:19 | 直接 CONNECT／DISCOVER，2 秒、−24 dBFS 來源 | CONNECT bytes 0；DISCOVER accepted；990 kbps 發送成功並關閉，使用者隨後確認雙耳聽到 |

第一輪 SDP error 10108 不被後來的成功覆蓋；它表示該次查詢未取得服務結果，不能推定 XM6 永久沒有 Audio Sink。該輪沒有提出 CONNECT，最終 open／callback／close／root 與 bytes 計數皆為 0。後兩輪未執行新的 fresh SDP 查詢，改由既有耳機服務的 CONNECT 與實際 DISCOVER 往返確認訊令路徑，再播放來源；因此不能將兩種程序當作完全相同的重複實驗。

保留的 tone manifest 指定兩個來源皆為 **48 kHz、PCM16、雙聲道、2 秒**，左聲道 440 Hz、右聲道 660 Hz，淡化參數 100 ms。兩次數位峰值分別為 **−40 dBFS** 與 **−24 dBFS**；後者提高來源振幅以利聆聽確認。dBFS 是相對數位滿刻度的來源設定，不是耳機聲壓級、環境噪音或系統音量量測，不能換算為本次耳內音量。

| 來源 | SHA-256 |
| --- | --- |
| −40 dBFS 測試音 | `e33f20a9e3d8a255927795d81c9b99bb82e909302d5284255e9e6fa4846fed6e` |
| −24 dBFS 測試音 | `5588646d735aedb07aa563116b2be4ac8916a89e0df325cf50a290d6c8e1b4a3` |

兩次成功重播的報告均為 result／Win32／protocol／encoder error **0／0／0／0**、requested／actual **990／990 kbps**、96,000 frames、376 media packets、252,718 media bytes、stage 15 CLOSED。attempts 1、restarts 0、bitrate changes 0、battery downgrade=false。每輪播放後與最終 status 都是 connections／retained／in-flight **0／0／0**；累計 diagnostics 仍各為 open completion 3、callback add/release 3／3、close completion 3、roots destroyed 3、transfer retirements 394，contract／open／close failure 都為 0。這個累計包含該輪先前的 DISCOVER，計數範圍與第 5 節相同。

使用者在最後一輪後明確回覆：

> 兩耳都有聽到，音量合適。

因此新增的驗收結論是：**這份短立體聲測試音在使用者雙耳可聞，且使用者主觀認為音量合適**。這不是麥克風／音訊分析儀量測，不證明頻率響應、失真、左右聲道隔離或完整頻率／聲道對應，也不是一般音樂品質與長時間無斷音驗收。660／330 kbps、能量耗用及日用主機安裝條件仍未由這次確認補足。

三輪均記錄臨時 filter 移除、detach exit 0、host restored=true。VM 後續是否還原到乾淨快照及 guest 套件清理另由後續狀態記錄，不把 radio 歸還主機當成所有清理已完成。

## 7. 歷史成功與本次失敗如何同時成立

[先前 USB 喚醒報告](USB_WARM_XM6_2026-11-06.md) 與原生載入報告保存了真正附掛、guest BTHUSB、原生載入與 XM6 DISCOVER 成功，這些歷史證據仍然有效。本次則觀察到：即使捕捉前讀到 D0，後續仍可能未完成附掛。因此「短 inquiry 加 D0 檢查」是曾成功的條件組合，並非對所有控制器狀態都有效的永久修復。

兩個問題要分開驗證：CONNECT 契約錯誤已有來源修正與可重現軟體回歸；USB capture 在這次重測中曾阻擋安裝，穩定 filter 輪次則讓修正版載入，最後連續流程完成 CONNECT 與 LDAC 發送；其間的 error 121 與重複身分中止仍保留。前者的 CI 成功不能補足後者的實體證據，後者的失敗也不能推翻先前已觀察到的舊版 DISCOVER 成功。

已取得本次短測試音的使用者雙耳聆聽確認；後續仍需測其他手動碼率、較長時間播放、斷線重連與睡眠復原。990 kbps 音質優先及不因電池自動降級的要求已保留在這次發送設定；660／330 kbps 尚未完成實體互通比較。音質、能量耗用、Windows 音訊端點與正式發行簽章仍未驗收，也沒有安裝實驗性驅動到日用筆電主機。


## 8. 試驗後還原與資料保留

另於 **16:40:39 UTC（臺灣 00:40:39）** 完成收尾確認。先匯出 41 個 guest 原生測試檔案並建立雜湊清單，核實保留的已簽署 SYS，再正常關閉 VM、還原到本次已配對且仍使用原廠驅動的 paired-stock 快照。重新啟動後於 16:39:35 UTC 實際檢查：Secure Boot=true；OpenLdac 系統驅動、服務、Driver Store 套件及測試憑證均為 0；testsigning/debug 未開啟，Driver Verifier 沒有驗證中驅動。檢查通過後再次正常關機，最終 VM 為 poweroff。

臨時 USB filter 為 0，VM 的原有 USB Tablet／OHCI 設定已恢復；筆電 MediaTek radio、XM6 Audio Sink 與 USB composite 三項原有驅動 INF／service／version 和基準一致、狀態 OK。這是主機綁定回復確認，沒有另行實測主機原廠音訊播放。虛擬機快照不會還原耳機內的配對金鑰，日用主機如無法連線可能仍須重新配對。

另已清除四個明確列出的本次建置／暫存目錄，合計 132,908,764 bytes（約 126.75 MiB）。原始碼、red/green 二進位檔、WAV 與 manifest、失敗／成功原始紀錄、已簽署測試套件及虛擬機均保留於專用資料夾。公開的是去識別化摘要；裝置位址、配對金鑰、憑證識別及原始 private 紀錄未發布。
