# XM6 較長播放、手動碼率與重連驗證 — 2026-11-07

本輪完成三個獨立 60 秒實體播放案例，分別在約第 3 秒手動切換 990→660、660→330、330→990 kbps；worker 確認各套用一次並正常結束，使用者回報三段都雙耳可聽、沒有察覺異常。這是目前已完成的功能與主觀聆聽證據。

長播與重連尚未通過：600 秒固定 990 kbps 嘗試在送入約 93.39 秒來源資料後觸發既有 40 ms pacing 防護；耳機關機時收到正常 peer CLOSE，但再開機後兩次明確重播都在 CONNECT 以 Win32 71 失敗；之後外部新鮮 SDP 查詢再另開 worker，才完成 90 秒 660 kbps。這不是同一 worker 自動重連通過。MMCSS 與高解析度 timer 各曾完成 30 秒重測，也各保留後續失敗的反例，不能宣告排程問題已被修好。本頁記錄完整方法、失敗歸因、手動控制、CPU 觀察及延遲資源退休，不以較晚成功改寫較早證據。

日期以臺灣時間 2026-11-07 編排；本頁初次建置、自測及實體案例的原始時間為 UTC 2026-11-06。兩者是時區差異。

## 既有結果與本輪問題

[前一輪修正報告](XM6_CONNECT_FIX_2026-11-07.md) 已記錄真實 MediaTek → VM BthPort → 實驗性原生驅動 → XM6 的 990 kbps 短音傳送；使用者在後續較大聲的兩秒短音後回覆「兩耳都有聽到，音量合適」。這是該短音的主觀雙耳可聽確認，不能倒推為所有先前嘗試都可聽，也不是音樂保真度、聲學聲道分離、續航或長期穩定性測量。

本輪維持音質優先：48 kHz 來源預設 990 kbps，保留明確手動 660/330 kbps 控制，不因電池狀態降碼率。40 ms 排程遲到上限、嚴格 CONNECT 回傳長度與實際 MTU 驗證不因失敗而放寬。仍只在隔離 VM 載入測試簽章驅動；未將實驗性 LDAC 驅動安裝至日用主機，Windows 音訊端點也尚未完成。

## 可重現方法與證據層次

1. 以既有核心修正版與受限 VM INF 準備測試，驗證套件及目標服務，留下連線前狀態。USB 捕捉方法僅在已記錄的機器條件下成立，不視為通用解法。
2. 先執行不接觸 RF 的 SCM 自測，確認實際服務管理員能接收 HQ/SQ/MQ 控制、停止並移除暫時服務。
3. 在 VM 中驗證真實 CONNECT 與 DISCOVER，接著以獨立 SCM worker 執行固定 990 kbps 的 30 秒短關卡。只有成功才進入後續較長播放、手動切換和重連案例；前一關失敗即中止本次序列。
4. 保存 worker 最終 JSON、控制接收紀錄、CPU／記憶體樣本、前後狀態與核心診斷；每次完成或失敗都檢查連線／保留物件／未完成 I/O，並執行 USB filter 清除與主機控制器歸還。

來源為 48 kHz、雙聲道 PCM16 合成測試訊號，預先建立 30/60/90/600 秒檔案。每十秒含一段 0.6 秒提示音，其合成振幅係數為 −30 dBFS，另加 −48 dBFS 背景訊號與漸入漸出。這些數字是數位合成參數，不是耳邊聲壓；檔案不是代表性音樂。產生了較長檔案不表示它們已播放。

## 新增最終結果輸出

原生播放與 SCM 模式接受 `--result <new.json>`。程式在啟動原生播放前以 `CREATE_NEW` 建立檔案；路徑已存在或無法建立便提早失敗，不覆寫既有證據。worker 清理完成後才寫入及 flush 最終 JSON，因此檔案不是即時進度；異常中止仍可能留下不完整內容。`exit_code_before_report` 不包含稍後寫入／關閉檔案本身的失敗，必須一起保留程序或服務結果。

`hq_controls/sq_controls/mq_controls` 是服務回呼接受次數。獨立檢查指出，舊欄位 `latest_control_requested_kbps` 實際是 owner 以 atomic exchange 讀取並清為 0 的待處理槽，並非永久保留的最新請求；新版已更名為 `pending_control_requested_kbps`，歷史原始報告不改寫。0 可能代表請求已被取走，連續請求也可能在 owner 讀取前合併。必須再比對 `bitrate_changes`、`requested_kbps` 與 `actual_kbps`，不能把 `sc control` 成功當成耳機已完整播放該碼率。來源與封包計數範圍為 `last_attempt`；有限次重連的 `whole_source` 策略會從 WAV 開頭重新播放，並保留手動目標，並非無縫接回中斷取樣位置。

## 本機與無 RF 驗證

初版記錄器以基底 commit `b7799e8068a986c5beeda288c0dc5fd5db1eb265` 加當時工作樹建置；建置結果保留所有輸入雜湊，不能把基底 commit 當成包含未提交修改的完整識別。初版播放器 SHA-256 為 `40b5314c0e9fa30c305569273955a49b26ee8b6edea9113eb2fd447237941d64`。

| 層次 | 已觀察結果 | 限制 |
| --- | --- | --- |
| Windows 編譯 | exit 0，嚴格警告 | 不是實體接受 |
| CLI／WAV 整合 | CLI 92、整合 104，exit 0 | 無 RF，包含結果檔案契約 |
| 既有完整本機套件 | lifecycle 18,976／1,020 schedules；adapter 8,738；core 5,805；session 9,381；reconnect 190；reassembly 292,045；playback 927；WAV 818；Windows owned-pipe I/O 1,471 | host model、fake peer 或 named pipe；不是真實 Verifier／耳機驗證 |
| VM SCM 自測 | HQ/SQ/MQ 各接受一次、拒絕 0；worker／Win32／service status error 皆 0，服務建立後已刪除 | 48 kHz 映射為 synthetic，`bluetooth_transmitted=false`、`codec_exercised=false` |

SCM 自測 UTC 16:58:26–16:58:35。外層控制器 `timed_out=false`；內層自測 `timed_out=true` 表示其有限自測時間到後主動停止，不能誤讀為實體傳輸逾時。該自測 CPU 0.03125 秒、外層經過 9.576 秒，只用來記錄無 RF 執行情況。

## 第一個實體案例：30 秒 HQ 未完成

實體序列 UTC 17:00:04–17:01:01。VM 實際取得控制器、安裝驗證通過，CONNECT 回傳 0 bytes、DISCOVER 通過。`ShortHQ` 原訂 30 秒，未送出任何手動控制或重連請求。

| 觀察 | 實測值 |
| --- | --- |
| worker 結果 | `result=-2`、Win32 `1460`、stage `7` |
| protocol／encoder error | 0／0 |
| 來源送入量 | 410,368 stereo frames，即約 8.549 秒來源資料 |
| 媒體成功寫入 | 1,602 packets、1,078,146 bytes |
| 碼率 | requested 990、actual 990、changes 0；未自動降級 |
| 嘗試／重新開始 | 1／0 |
| CPU／外層經過時間 | 0.5 秒／9.423802 秒 |
| 程序 peak working set | 8,028,160 bytes |

`last_attempt_media_write_completed=true` 只代表曾有媒體寫入完成，不代表整段 30 秒完成或 8.549 秒都已被耳機實際播出。初版報告尚無逾時來源細分，單憑 1460 不能確定是排程遲到、等待 I/O 或下層完成狀態。因此該初版案例不回填具體原因；下一節另記錄新增觀測後的重測。

失敗後 `connections=0`、`retained_connections=0`、`in_flight=0`；open completion 3、callback add/release 3/3、close completion 3、roots destroyed 3。contract fault、close failure、remote disconnect 與 cancel call 皆 0；transfer retirements 1,619。三次 open 包含先前 DISCOVER 與播放的 signaling/media，不能解釋成三條音訊媒體串流。暫時 SCM 服務已刪除；filter 已移除、detach exit 0，主機控制器已歸還。

## 前景程序對照與 pacing 歸因

先以非 SCM 的前景程序重播 30 秒來源，仍以 `result=-2`、Win32 `1460`、stage `7` 失敗：559,360 frames（約 11.653 秒來源量）、2,184 packets、1,469,832 bytes，requested／actual 990、changes 0。`service_mode=false` 且沒有 ServiceMain／handler，因此「只由 SCM 模式造成」不足以解釋這些觀察；它仍不是精確的排程原因證明。

接著新增只讀時間計數，播放器 SHA-256 `59a6384d863b84f19f16588c5a43846012e2edc013ba484ef4d0d49051abe10b`，編譯 exit 0，CLI 92、WAV 整合 104、Windows owned-pipe I/O 1,522 均通過。這個版本與初版分開保存輸入雜湊；沒有放寬 40 ms 上限。

UTC 17:08:03–17:08:25 的 SCM 重測仍失敗，但新增觀測指出此次失敗類別：

| 觀察 | 實測值 |
| --- | --- |
| result／Win32／stage | −2／1460／7 |
| 來源 frames／封包／bytes | 406,272／1,586／1,067,378 |
| requested／actual／changes | 990／990／0 |
| pacing limit／peak lateness | 40,000／41,462 µs |
| pacing misses／最後 offset | 1／406,016 source samples |
| I/O wait timeout／completion timeout | 0／0 |
| media write calls／timing samples | 1,586／1,586 |
| 最長 media write | 2,508 µs |
| last timeout source | `pacing_lateness` |
| CPU／外層經過時間／peak working set | 0.40625 秒／9.3012 秒／8,015,872 bytes |

因此可以確認**這次**是 sample-deadline pacing 防護拒絕了 41.462 ms 的遲到；不能把它籠統寫成 RF I/O 逾時，也不能從這些計數推定全部遲到來自 hypervisor、計時器或某個排程競爭者。舊版沒有這些觀測，保留其歸因未知。

此次結束後連線／retained／in-flight 皆 0；該段累計 callback add/release 4/4、close completions 4、roots destroyed 4，contract faults、close failures、remote disconnects、cancel calls 皆 0。這些核心累計計數也涵蓋同一裝置生命週期內的前景對照，不當成單一重測全部新增量。

下一個實驗是在真正的 playback owner thread 使用 MMCSS `Audio` 排程登記，native cleanup 後由同一執行緒還原，不覆寫 task priority、registry、power plan 或全系統 timer resolution；登記失敗則保留原有排程路徑並記錄錯誤。播放器 SHA-256 `6b89a1619054a9b3b616996901cc5d9996315dbcdd6eecce29e91f2590925868` 編譯 exit 0，CLI 92／整合 104 通過。使用 production helper 的無 RF API fixture 確認同執行緒登記及還原成功、錯誤皆 0，且 SCM selftest 跳過 MMCSS。這是 API／本機驗證；其後的一次實體 30 秒結果另列如下，不將 API 成功直接當成實體接受。40 ms 上限與 HQ 保持不變，不以放寬防護、丟取樣或降碼率掩蓋失敗。在這段連續測試期間控制器暫留 VM，沒有保留捕捉 filter。前一節歸還僅屬第一次 campaign；本輪最後的實際歸還見收尾節，不能把兩者混為同一時間點。

## MMCSS 後的 30 秒 HQ 重測通過

同一受控 VM／耳機路徑，UTC 17:12:45–17:13:17，MMCSS 版首次完整讀入 30 秒來源並正常結束：`result=0`、Win32／protocol／encoder errors 皆 0、stage `15`（CLOSED），source frames 1,440,000、media packets 5,626、media bytes 3,785,968。requested／actual 均 990、bitrate changes 0、attempts 1、restarts 0，沒有降碼率。

pacing peak 15,341 µs、小於仍維持的 40,000 µs 上限；deadline misses、I/O wait timeout、completion timeout 皆 0。5,626 次媒體寫入皆有時間樣本，最長 4,295 µs。MMCSS `Audio` 登記與還原都成功，錯誤皆 0。CPU 1.171875 秒、外層經過 31.0235277 秒、peak working set 8,056,832 bytes，約為單一邏輯處理器時間的 3.8%，不是功耗。

結束後 connections／retained／in-flight 皆 0，累計 callback add/release 6/6、close completions 6、roots destroyed 6、contract faults／close failures 0，暫時服務已刪除。這是一次受控前後變更中觀察到的改善；沒有重複隨機化試驗，不能宣告所有 Windows／VM 排程問題已解決，也不是新一輪使用者主觀聆聽確認。在這個歷史時間點，後續三個 60 秒手動控制案例與 600 秒固定 HQ 尚待獨立執行；實際結果接續記錄於下節，當時控制器暫留 VM 供連續測試。

## MMCSS 的反例：Switch660 尚未送出控制即失敗

下一個 60 秒來源案例原訂播放中請求 660 kbps，但 UTC 17:13:57–17:14:07 已在送出控制前終止。`controls=[]`、HQ/SQ/MQ 接受計數皆 0，不能把它記為「660 kbps 播放失敗」或「切碼率功能已測完」。實際仍是 requested／actual 990、changes 0。

來源送入 380,672 frames（約 7.931 秒），1,486 packets、1,000,078 bytes；result −2、Win32 1460、stage 7。MMCSS 登記與還原皆成功，但 peak pacing lateness 57,649 µs、miss 1、offset 380,416；I/O wait／completion timeout 皆 0，最長 media write 4,647 µs。CPU 0.5 秒、外層經過 9.3337012 秒、peak working set 8,069,120 bytes。

結束後 connections／retained／in-flight 皆 0；裝置累計 callback add/release 8/8、close completions 8、roots destroyed 8，contract faults／close failures 0，暫時服務已刪除。此反例明確限制上一個 30 秒通過的結論：MMCSS 登記成功與一次完整播放並不能保證後續穩定。

VirtualBox 記錄顯示本次使用 NEM／Windows Hypervisor 後端，這是環境背景，不是遲到來源的因果證明。下一個候選修改是高解析度 one-shot waitable timer；其建置與獨立重測接續記錄於下節，40 ms 防護與主機全域設定保持不變。此時控制器仍在 VM，最後歸還另見收尾節。

## 高解析度 one-shot timer：第二次 30 秒通過

候選播放器 SHA-256 `529a99dbc9a79aff50d3db6f8500e87c6f9937cd79450c040574c05a1dee0a8c`，編譯 exit 0、CLI 92／WAV 整合 104／Windows owned-pipe I/O 1,554 通過。新 timer 使用既有 one-shot 等待流程；僅在 high-resolution flag 不受支援、回傳 `ERROR_INVALID_PARAMETER` 時允許普通 timer fallback，其他建立失敗不被掩蓋。沒有要求修改全系統 timer resolution。新欄位另外記錄 timer wake 後與進入等待前的 lateness，屬觀測位置而非系統根因。

UTC 17:23:07–17:23:54 的 30 秒重測 result 0、Win32／protocol／encoder errors 0、stage 15；1,440,000 frames、5,626 packets、3,785,968 bytes，requested／actual 990、changes 0。high-resolution timer 實際啟用、fallback false、建立錯誤 0，MMCSS 登記／還原亦成功。

pacing peak 9,936 µs、miss 0、I/O wait／completion timeout 0、最長 media write 5,322 µs；timer wake 樣本 5,611，wake 後 peak 9,936 µs、等待前 peak 5,414 µs，兩類 miss 皆 0。CPU 2.15625 秒、外層經過 30.9053771 秒、peak working set 8,257,536 bytes；資源三項皆 0，暫時服務已刪除。外層 campaign 時戳包含取證等工作，與 worker 經過時間不同。

這是本機此組合的一次 30 秒成功，並非解決所有遲到的證明。CPU 時間也高於前一個 MMCSS-only 成功案例；兩次非配對穩態量測不足以歸因 timer 的耗電成本或宣稱省電。該次 30 秒成功不取代後續三個 60 秒手動案例、600 秒 HQ 或斷線重連；它們的後續結果分開記錄。

## 高解析度 timer 的反例與分開的早期控制測試

下一個 `Switch660` 仍在原訂 20 秒控制之前中止：381,440 frames、1,489 packets、1,002,097 bytes，result −2、Win32 1460、stage 7。requested／actual 990、changes 0、控制接受計數皆 0；同樣不能記為測過 660 kbps。

此次 high-resolution timer 與 MMCSS 都成功啟用，仍記錄 peak lateness 41,179 µs、miss 1；`pacing_misses_after_timer_wake=1`、before-wait miss 0，等待前 peak 2,647 µs、最長 media write 6,375 µs，I/O timeout 皆 0。這把觀測位置縮小到 timer wake 後的 deadline 檢查，仍不能識別究竟是哪個 host／guest 排程活動造成延遲。CPU 0.65625 秒、外層經過 9.1095657 秒、peak working set 8,257,536 bytes；資源三項皆 0、暫時服務已刪除。

因此 high-resolution timer 也不是完整修正。接下來把測試控制送出時間參數化，保留原預設 20 秒；另以第 3 秒送出控制、來源仍為 60 秒，隔離觀察「owner 是否能實際套用手動碼率」。這是新增的較早控制功能實驗，不取代原本 20 秒控制關卡，不能把其成功稱為 60 秒穩定或長期播放通過。600 秒 HQ 亦須保留實際嘗試與失敗，不能從短關卡推定。

## 早期控制的測試工具錯誤與 660 kbps 完整案例

第一次第 3 秒控制實驗遇到 harness 錯誤：在播放中另開 probe 查詢 status，回傳 Windows error 4；工具的通用文字誤稱需要 admin。harness 的 catch 路徑主動 STOP，尚未送出任何碼率控制。worker 留下 result −6／Win32 995、stop controls 1、136,192 frames、531 packets／357,363 bytes，pacing miss 0。這是測試工具觸發的取消，不記為 codec 或 RF 自發失敗。

立即拍下的 final status 為 connections 1／retained 1／in-flight 0，不能抹成當時已全數釋放；稍後下一個案例的 before-status 確認三項已為 0。修正 harness 後不再於播放中另開 probe，而以最終 `actual_kbps` 與 `bitrate_changes` 確認套用；最後提供最多 10 秒的退休狀態輪詢。這只增加收尾觀測，未把非零資源當成成功。

修正後的 60 秒 990→660 案例完整完成：第 3.2924433 秒送出 control 129，exit 0；worker SQ 接受次數 1、changes 1、requested／actual 660，pending slot 0，正好說明請求被取走後 pending=0 不表示沒有切換。result／Win32／protocol／encoder errors 皆 0、stage 15、source frames 2,880,000、media packets 7,639、media bytes 5,141,047、attempts 1／restarts 0。

pacing peak 16,309 µs、miss 0、I/O timeout 0，最長 write 5,023 µs；high-resolution timer 啟用且無 fallback，MMCSS 登記與還原成功。CPU 2.890625 秒、控制器記錄 elapsed 61.4107933 秒、peak working set 8,249,344 bytes；前後三項資源皆 0，服務刪除成功。

這是第 3 秒左右手動請求並完成 60 秒來源的實體傳送／encoder 套用證據，不是原第 20 秒案例通過，在此單例完成時尚未取得使用者聆聽回覆；後續針對三段的回覆另外記錄於下節。不同案例的日曆時間與程序 elapsed 分別保留，不以日曆時戳相減取代實際來源 frames 或程序計時。

## 三個獨立 60 秒手動碼率案例完成

同一播放器版本下，先後執行三個**各自重開播放**的 60 秒來源。控制時間是相對各案例的實際記錄值，並非保證精確 3.000 秒。每例 source frames 2,880,000、result／Win32／protocol／encoder errors 皆 0、stage 15、changes 1、attempts 1／restarts 0、pending slot 0；high-resolution timer 啟用且無 fallback，MMCSS 登記／還原成功。

| 來源初始→最終實際碼率 | 控制時間／code／接受次數 | packets／media bytes | peak lateness／最長 write (µs) | CPU／elapsed (秒) | peak working set (bytes) |
| --- | --- | --- | --- | --- | --- |
| 990→660 | 3.2924433 s／129／SQ 1 | 7,639／5,141,047 | 16,309／5,023 | 2.890625／61.4107933 | 8,249,344 |
| 660→330 | 3.5280523 s／130／MQ 1 | 3,923／2,639,959 | 6,256／5,297 | 1.984375／61.1674643 | 8,245,248 |
| 330→990 | 3.5171038 s／128／HQ 1 | 10,897／7,333,351 | 11,436／5,948 | 3.546875／61.2155697 | 8,261,632 |

三例 pacing miss、I/O wait／completion timeout 皆 0；各自結束後 connections／retained／in-flight 皆 0、暫時服務已刪除。這建立了請求被 owner 接收、codec 名目碼率改變及完整來源送出之間的證據鏈；沒有聲學解碼擷取或量化無縫切換測量，也不宣告長期可靠度。每例前三秒多仍為初始碼率，因此也不是固定 660／330／990 各跑一分鐘的功耗比較。

三段完成後，使用者針對「剛才三段各約 60 秒訊號」回覆：「兩耳都有聽到，沒有察覺異常」。本頁將它記為這三段的主觀雙耳可聽、未察覺異常回報；沒有聲學儀器量測，不能等同失真、丟包或無縫切換的客觀上限，也不延伸到正在進行的 600 秒或所有歷史案例。回覆沒有附精確 UTC 時間，因此不補造時間戳。

研究上的反例同樣重要：30 秒通過後仍可能在下一個 60 秒案例的第 8 秒左右失敗；後來早期控制的三例通過也不能抹去這些非確定性遲到。MMCSS 與 timer 修改沒有經過多次交錯 A/B 或負載控制，無法把單次改善歸因為 timer 的單一因果修復。原第 20 秒控制案例維持未通過，新增第 3 秒功能實驗明確另列。

本輪去識別結構化摘要見 [JSON](xm6-sustained-2026-11-07.json)；原始案例仍保留失敗、harness 取消與不同版本，不只收集成功案例。

## 600 秒固定 HQ：93.39 秒後停止，未通過長播

`EnduranceHQ` 使用原訂 600 秒來源、固定 990 kbps、無控制／無重連。UTC 17:31:02–17:32:50，送入 4,482,560 frames（約 93.3867 秒來源量）後以 result −2、Win32 1460、stage 7 停止；media packets 17,509、media bytes 11,783,557，requested／actual 990、changes 0。沒有自動降級，也沒有完成 600 秒。

high-resolution timer 與 MMCSS 仍啟用成功。pacing peak 40,566 µs、miss 1；這次 `pacing_misses_before_timer_wait=1`、after-wake miss 0，wake peak 27,039 µs、before-wait peak 40,566 µs，最後 offset 4,482,304。I/O wait／completion timeout 皆 0，最長 media write 28,900 µs。它是進入 timer 等待前已落後的觀測，與前一個 after-wake 反例不同，因此不能把所有遲到化約成 timer wake 單一問題。

CPU 5.9375 秒、程序 elapsed 94.3368366 秒、peak working set 8,241,152 bytes，約為單一邏輯處理器時間的 6.3%，但仍是未完成的案例，不能當作十分鐘穩態平均或筆電功耗。

收尾有重要的延遲退休觀察：最多 20 次、每次間隔 500 ms（約 10 秒）的狀態輪詢後，最後仍為 connections 1／retained 1／in-flight 0；歷史 report 的 `resource_retirement_checks=21` 是 harness 耗盡迴圈後以 index+1 記錄所造成的 off-by-one，並非實際查了 21 次。工具後續修正計數上限為 20，原始報告不改寫；暫時服務已刪除，但當時不能宣告所有核心物件皆已退休。後續獨立只讀 `after-endurance-status.json` 才確認 0/0/0，診斷累計 opens 22、callback add/release 22/22、close completions 22、roots destroyed 22、faults 0。保留兩個不同時間的觀測，不把後來歸零回填成即時歸零，也不從最終歸零推定收尾延遲已符合需求。

600 秒 gate 維持未通過。下一個有限次重連使用獨立 90 秒、660 kbps 耳機關機／開機案例；是另一項功能實驗，不能補成高碼率長播接受。此時控制器暫留 VM 等待這項測試與最終收尾；最終結果另列。

## 耳機關機／開機：正常 peer CLOSE，重新連線未通過

下一個獨立案例使用 90 秒來源、手動 660 kbps，設定 `reconnect_max_retries=3`。使用者實際將 XM6 關機再開機。關機時觀察到一個 peer command，worker 以 `result=-9`（PEER_CLOSED）、Win32／protocol／encoder errors 0、stage 15 收尾；送入 1,486,848 frames（30.976 秒來源量）、3,871 packets／2,605,183 bytes，requested／actual 660、changes 0，pacing miss／I/O timeout 0。

這是耳機主動正常 CLOSE 的實體觀察，不是測得強制 RF 消失或 automatic reconnect 成功。該政策將正常 peer CLOSE 視為終止而非可重試的傳輸錯誤，因此 attempts 1、restarts 0、retry_exhausted false；不能因設定最大三次就稱實際重試三次。服務刪除成功、三項資源皆 0。

使用者確認重新開機後，另起兩個明確播放嘗試，均在 CONNECT stage 1 以 result −2／Win32 71 結束，來源 frames／媒體 packets／bytes 皆 0；requested 660、actual 0、attempts 1／restarts 0、pending slot 660。這表示尚未建立成功串流，也不等於降成零碼率；Win32 71 的更深層狀態／重新連線策略原因仍待診斷。兩次服務都已刪除、三項資源皆 0。

第一次明確重播另有 harness 觀測競態：播放器快速結束後，GetProcess 查找已不存在的 PID 失敗。原始 worker JSON 已留下 Win32 71，不能把 harness 例外遮掉真實 CONNECT 結果。測量程序修正為程序已結束時將 CPU 等指標標示不可用；未修改播放器，再次測試仍為 Win32 71，沒有 harness error。不可用的 CPU 數值不補成 0。

此處只建立 peer 正常關閉與本地收尾的有限實體證據；整個 90 秒來源未完成，重新開機後的播放未恢復，重連 gate 不通過。使用者三段 60 秒無異常回報不延伸到這次案例。控制器歸還與 VM 最終狀態由最後收尾另記，尚不能因此安裝到日用主機。

## 外部新鮮 SDP 後另開 worker：90 秒恢復案例

兩次 CONNECT 71 之後，另外執行一次針對既定 AudioSink 的新鮮 SDP 查詢，error 0、1 record、67 blob bytes、enumeration complete。接著新開 worker，完成原訂 90 秒、固定 660 kbps 來源：result／Win32／protocol／encoder errors 皆 0、stage 15，4,320,000 frames、11,251 packets、7,571,483 bytes，requested／actual 660、changes 0、attempts 1／restarts 0。

pacing peak 9,276 µs、miss 0、I/O timeout 0、最長 write 11,560 µs；high-resolution timer 啟用且無 fallback，MMCSS 登記／還原成功。CPU 3.609375 秒、elapsed 91.1167028 秒、peak working set 8,249,344 bytes，結束後三項資源皆 0、服務已刪除。

這是**外部服務探索後新 worker 的恢復觀察**，不是同一 worker 保持狀態、重試或從中斷位置接續。單次先 SDP 再成功的時間關係，不能證明 SDP 是修復 Win32 71 的充分原因，亦不能抹去前兩次失敗。現有可見報告沒有保留足以還原下層確切拒絕原因的完整 NTSTATUS／BRB Response；下一階段需先補具體診斷，再決定恢復策略，不能只擴大錯誤重試清單。三段 60 秒的聆聽回覆不延伸為此 90 秒的新聆聽確認。

至此本輪停止 RF 操作；最後無 RF 驗證、控制器歸還與 VM 收尾記錄於下節。600 秒 HQ 與 automatic reconnect gate 仍未通過。

## 最後無 RF 驗證與控制器歸還

UTC 17:40:30 在 VM 執行 Windows owned-pipe native I/O fixture，1,554 checks、exit 0，high-resolution timer observed true、fallback false、error 0。該套件明確 `real_bluetooth_tested=false`、`headset_audio_tested=false`；它驗證本機 OS I/O／timer 行為，不增加耳機或長播接受。

歸還前最後查詢 connections／retained／in-flight 為 0。UTC 17:40:42 的 `radio-return.json` 記錄 detach exit 0、temporary filters 0、host restored true；MediaTek、XM6 與 USB Composite 三項原有 binding 屬性吻合且狀態 OK。這證實控制器已歸還日用主機，不能延伸成實驗性驅動已安裝到主機或主機端已做 LDAC 播放。

VM 最後恢復原已配對的 stock snapshot 後，第一次 clean boot 的 GuestControl 逾時，ACPI 關機要求在 30 秒內也未完成；先保存停滯狀態與紀錄，再從同一 clean snapshot 重新開機。第二次檢查成功並正常關機。不能僅因清理命令曾涉及驅動清冊就認定首次停滯由 DISM 造成，原因沒有被證實。

UTC 17:49:30 的 elevated clean 檢查確認 Secure Boot true、OpenLdac 系統驅動／服務／測試憑證／Driver Store 套件數各 0、test-signing/debug 皆未啟用，Verifier runtime 無目標驅動。Driver Store 使用 `pnputil /enum-drivers` 枚舉第三方套件，涵蓋本專案第三方套件範圍；未執行 DISM `-All`，不能寫成所有 inbox 驅動完整審計。

UTC 17:50:09 最終記錄確認 clean runtime passed、VM 已 power off、paired-stock snapshot、temporary filters 0、attached USB 0、三項主機原有 binding 正常，且最後為正常關機。原始碼與驗證證據保留。另存 `cleanup.json` 已記錄移除本輪四組建置暫存與停滯 saved-state 共 21 項、1,989,914,541 bytes（約 1.85 GiB）；四版 EXE 雜湊仍一致，PDB、合成訊號／產生器、紀錄、VM 及共用工具鏈均保留。這個收尾結果不改變 600 秒 HQ／automatic reconnect 未通過的結論。

## CPU、耗電與接受門檻

0.5 秒 CPU／9.424 秒外層經過時間約相當於 5.3% 的單一邏輯處理器時間，但它包含本次程序生命週期及失敗路徑，並非穩態十分鐘平均。CPU 取自 `Process.TotalProcessorTime`，包含歸帳於該程序的 user 與 privileged/kernel 時間；不是排除所有 guest kernel 工作。[Microsoft 定義](https://learn.microsoft.com/en-us/dotnet/api/system.diagnostics.process.totalprocessortime?view=netframework-4.7.2)。它不涵蓋整個 guest／host、其他程序、hypervisor 或控制器等硬體的完整成本，不能轉換成電池續航、瓦數或不同碼率的省電比例。

目前已有三個獨立 60 秒案例的 990→660、660→330、330→990 套用及傳送完成證據；不能合併寫成一條不中斷的三次切換串流。三段已有使用者雙耳可聽、未察覺異常回報；600 秒固定 HQ 已嘗試但失敗，耳機關機後的 peer CLOSE 已觀察到，但開機後兩次明確重新播放都未連上；之後外部 SDP 再新開 worker 完成 90 秒，automatic reconnect 接受仍未通過。應保留每次失敗與重測的獨立版本，不將後來成功回填舊案例；不能在這些門檻尚未通過時宣告可安裝日用主機。音樂品質、長期丟包／斷續、睡眠恢復與實際電池功耗仍需各自設計量測。

## 本機原始證據索引

公開頁面只列相對證據名稱與去識別結果；原始清冊、裝置識別碼與簽章私密資訊不公開。

- `work/build-sustained-report-20261107/build-result.json`：初版播放器輸入／輸出雜湊與 CLI／整合結果。
- `work/build-sustained-report-build.log`：完整本機套件輸出。
- `work/xm6-sustained-signal-20261107/manifest.json`：合成訊號與檔案雜湊。
- `xm6-sustained-20261107/selftest-controller.txt`：無 RF 自測控制器輸出。
- `xm6-sustained-physical-20261107/summary.json`：捕捉、案例中止、移除 filter 與歸還主機。
- 同目錄 `results/*-ShortHQ/` 的 `campaign.json`、`playback.json`、`final-status.json`、`final-diagnostics.json`：實體失敗與清理證據。

- `xm6-sustained-20261107/foreground30-report.json`：非 SCM 前景對照失敗。
- `work/build-sustained-timing-20261107/build-result.json`：時間歸因版雜湊與無 RF 檢查。
- `xm6-sustained-20261107/20261107-010754-058-ShortHQ-timing/results/20261107-010802-979-ShortHQ/`：該次 pacing 歸因報告與前後資源狀態；此容器也保存較早案例，勿當成全是新重測。

- `work/build-sustained-mmcss-20261107/build-result.json`：MMCSS owner-thread 版建置與 API fixture；實體接受另列。

- `xm6-sustained-20261107/20261107-011243-580-ShortHQ-mmcss/results/20261107-011245-306-ShortHQ/`：MMCSS 後第一次 30 秒完整 HQ 的 worker、CPU 與清理證據。

- `xm6-sustained-20261107/20261107-011355-360-Switch660-mmcss/results/20261107-011357-067-Switch660/`：控制尚未送出便 pacing 逾時的反例、CPU 與釋放證據。

- `work/build-sustained-hires-20261107/build-result.json`：高解析度 timer 版輸入／輸出雜湊及無 RF 驗證。
- `xm6-sustained-20261107/20261107-012309-419-ShortHQ-hires/results/20261107-012307-625-ShortHQ/`：high-resolution timer 的第一次 30 秒 HQ 重測。

- `xm6-sustained-20261107/20261107-012405-274-Switch660-hires/results/20261107-012410-811-Switch660/`：high-resolution timer 啟用後仍在 20 秒控制前停止的反例。

- `xm6-sustained-20261107/20261107-012524-694-Switch660-hires-early3/results/20261107-012519-899-Switch660/`：harness 額外 probe 導致 STOP，保留即時 1/1/0。
- `xm6-sustained-20261107/20261107-012639-680-Switch660-hires-early3-fixed/results/20261107-012645-049-Switch660/`：修正後 60 秒 990→660 完成，before-status 同時證實上一案例稍後已退休至 0/0/0。

- `xm6-sustained-20261107/20261107-012755-258-Switch330-hires-early3-fixed/results/20261107-012755-726-Switch330/`：獨立 60 秒 660→330 完成。
- `xm6-sustained-20261107/20261107-012913-873-Switch990-hires-early3-fixed/results/20261107-012911-300-Switch990/`：獨立 60 秒 330→990 完成。

- `xm6-sustained-20261107/20261107-013101-011-EnduranceHQ-hires/results/20261107-013102-965-EnduranceHQ/`：600 秒 HQ 未完成、時間歸因與收尾仍 1/1/0。
- `xm6-sustained-20261107/after-endurance-status.json` 與 `after-endurance-diagnostics.json`：稍後獨立只讀觀察已 0/0/0、累計 add/release／close／root 22。

- `xm6-sustained-20261107/20261107-013513-012-Reconnect-hires-user-power-cycle/results/20261107-013514-800-Reconnect/`：90 秒案例中耳機關機，peer CLOSE、attempt 1／restart 0。
- `xm6-sustained-20261107/20261107-013616-553-Reconnect-hires-explicit-after-power-cycle/results/20261107-013618-049-Reconnect/`：第一次明確重播 CONNECT 71 與另存的程序觀測競態。
- `xm6-sustained-20261107/20261107-013651-825-Reconnect-hires-explicit-after-power-cycle-retry/results/20261107-013653-473-Reconnect/`：修正測量競態後仍 CONNECT 71、無媒體、資源歸零。

- `xm6-sustained-20261107/post-powercycle-sdp.json`：外部新鮮 AudioSink SDP，1 record／67 bytes。
- `xm6-sustained-20261107/20261107-013817-324-Reconnect-hires-after-fresh-sdp/results/20261107-013825-072-Reconnect/`：外部探索後另開 worker 完整 90 秒 660 kbps，attempt 1／restart 0。

- `xm6-sustained-20261107/hires-native-io-vm.json`：VM 無 RF owned-pipe 1,554 checks。
- `xm6-sustained-20261107/final-before-return-status.json`、`radio-return.json`：歸還前資源歸零、detach 0、零 temporary filters、三項原有 binding 正常。

- `xm6-sustained-restore-20261107/clean-boot-retry.json`：首次 clean boot 停滯、保存狀態及同 snapshot 重試。
- `xm6-sustained-restore-20261107/xm6-final-clean/clean.json`、`final-state.json`：第二次 clean 檢查成功、VM 正常關機、零 USB／filter 及原主機 binding 確認。
