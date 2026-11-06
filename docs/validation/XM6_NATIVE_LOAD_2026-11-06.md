# 真實原生通道載入、XM6 DISCOVER 與首次 HQ 播放嘗試

本階段已在隔離 Windows 虛擬機載入真正的 `OpenLdacTransport.sys`，經內建 MediaTek 藍牙與 WH-1000XM6 完成一次 AVDTP DISCOVER 往返。耳機接受查詢，回傳 Audio Sink SEID 1；關閉後連線與在途計數歸零，回呼取得／釋放及根物件銷毀都有對應紀錄。**首次 990 kbps 播放嘗試則在 stage 1 失敗，尚未傳送媒體資料，不能宣稱耳機已播放 LDAC。**

本文以 UTC 日期 **2026-11-06** 命名，觀察截止於 `2026-11-06T16:02:45.0567485Z`。載入與 DISCOVER 發生於臺灣 11 月 6 日深夜；首次 HQ 嘗試已跨至臺灣 **11 月 7 日 00:00**。這是同一工作階段的時區差異。後續修正或重跑需另列結果，不回填改寫本次失敗。[機器可讀摘要](xm6-native-load-2026-11-06.json) 保留已去識別化數據。

## 1. 研究問題與驗證層次

前一階段已取得實體 USB 移交、XM6 配對、Audio Sink SDP 與 stock `BthA2dp` 服務 PDO 的證據，詳見[喚醒與服務發現報告](USB_WARM_XM6_2026-11-06.md)。本次進一步回答：原生 production transport 能否在真正 BthPort 堆疊上載入、收發訊令，並在正常關閉或早期失敗時結束本次連線。

| 判定層次 | 本次證據 | 結論 |
| --- | --- | --- |
| 套件來源 | CI artifact、23 項原生建置輸入比對、SYS／INF／CAT 雜湊 | 可追溯至指定來源；沒有以 KernelLab 合成 provider 代替 |
| 核心載入 | 精確 guest 安裝成功、服務 Running、PnP 正常、載入 SYS 雜湊相符 | 真實原生驅動已在 VM 載入 |
| 訊令互通 | DISCOVER accepted；SEID 1；送出 2 bytes、收到 8 bytes | 本次 AVDTP discovery 往返成功 |
| 正常關閉 | callback add/release 各 1、close completion 1、roots destroyed 1 | 本次關閉與物件退休有配對證據 |
| HQ 播放 | requested 990；stage 1 失敗；媒體封包與 bytes 皆 0 | 未取得 LDAC 串流或聆聽證據 |
| 筆電日用驗收 | 無長時間音訊、耗電、音訊端點及正式簽章測試 | 尚未達到主機安裝條件 |

## 2. 原生二進位檔的來源與建置限制

本次採用 [GitHub Actions run 42182756018](https://github.com/peianchen/open-ldac-windows/actions/runs/42182756018) 的原生 transport artifact `13485688126`。實際建置來源是 PR merge commit `91ed0ecc1e0695eaad0d7c734b38f41fab782761`；workflow head 為 `f43ee6009826b33a22910d15ff44235631e90bbc`。不能只用分支名稱推定二進位檔來源。

來源稽核以 `c88e74b027ccdf70903df25ffae6c9827ce4c5a3` 為當時工作樹基準，比對八個原生 C 編譯單元及其標頭、專案、INF、建置設定與 workflow，共 **23 項輸入一致**。這個比對範圍是核心 transport，不代表所有使用者模式程式與後續修正都等同該 CI revision。PE 檢查為 x64 Native，連結正式 Windows 核心／WDF 元件，沒有合成 `lab_driver.c` 或 KernelLab provider。

CI 原生建置、靜態分析及 INF 檢查成功；本機對取回的 INF 執行 `InfVerif /w` 也回傳 0。本機另一次 MSBuild 因 `MSB8020`、缺少 `WindowsKernelModeDriver10.0` 的 VS 整合而停止，因此**不宣稱本機原生編譯成功**。本次使用的是已核實來源相同的 CI 成品，不是略過編譯錯誤後自行假定可用。

## 3. 為何需要僅供 VM 的 INF 變體

公開 [OpenLdacTransport.inf](../../driver/OpenLdacTransport.inf) 只匹配 XM6 機型 VID／PID 形式。guest 實際列出的 Audio Sink hardware ID 卻是 `LOCALMFG` 形式；因此原公開 INF 無法直接匹配本次 guest 裝置。

實驗在獨立的本機套件目錄建立 INF 變體，只替換唯一的 model hardware ID，並加上 VM-only 註解。**LOCALMFG 不是 XM6 專屬識別**，所以公開 INF 與原始 production 套件都維持不變，變體也不是供一般使用者安裝的版本。變體 `InfVerif /w` 回傳 0。

安裝前必須同時滿足：

1. 精確 VM 身分、VirtualBox 型號及 guest 管理員權限；禁止在主機執行。
2. 簽章後 SYS、CAT、衍生 INF 與保留 stock inventory 的雜湊一致。
3. 包含 nonpresent 裝置在內只有一個 Audio Sink；該實例、耳機名稱、私有位址後綴與 parent container 均對應原 XM6。
4. target 原本仍由 `BthA2dp` 與 `microsoft_bluetooth_a2dp_src.inf` 服務，問題碼為 0；唯一 guest radio 身分也相符。
5. 保留原 stock INF、其 DriverStore 目錄檔案的有界且逐檔雜湊驗證副本，以及回復命令與驅動排名記錄。

原 stock 套件目錄副本不等於可移植、獨立完整的還原媒體；inbox 共用元件及 VM 還原點仍是回復條件。不能把只保存 INF 的結果寫成已匯出所有 Windows 音訊相依元件。

最終使用精確 hardware ID 的 `devcon update`，沒有綁定 radio，也沒有自動重啟。DevCon 會強制採用指定 INF，可能覆蓋一般驅動排名；PnPUtil 的 `/install` 則不會強制選用較低排名驅動。這是需要先確認唯一受影響裝置的原因，不代表新驅動品質高於 inbox driver。[Microsoft DevCon Update](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/devcon-update)、[PnPUtil 排名行為](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax)

測試結束後，VM-only 的寬匹配變體需要依回復計畫處理，避免將來在此 guest 加入其他耳機時意外匹配；本報告不預先宣稱已完成移除。

## 4. guest 測試簽章與開機條件

測試憑證與信任匯入僅限 guest。私鑰以不可匯出的 CNG RSA 金鑰建立；留存紀錄顯示 `private_key_exported=false`，公開文件不包含憑證指紋、私鑰或憑證檔。

順序為：**簽署 SYS → 依已簽署 SYS 與衍生 INF 重建 CAT → 簽署 CAT → 驗證 SYS 與 INF 的 catalog membership**。原始 CAT 不涵蓋修改後的 INF，也不應在 SYS 改變後繼續使用。此次 Inf2Cat 記錄為無 errors／warnings，簽章狀態與兩項 membership 驗證皆通過。這是開發用測試簽章，並非 WHQL 或正式發行簽章。[Microsoft 測試簽章流程](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing)

| 檔案／版本 | SHA-256 |
| --- | --- |
| 原始 unsigned SYS | `D47A327F5623C2EBA7D2DC620D7ACC02D4EB4CCF4E8BB9FD79E68E482DAC870F` |
| VM 衍生 INF | `0997A168598B3FE971352DC539D9E97AB3BDCB0CF4B451DA56C114C50533E167` |
| guest 簽署 SYS | `7E9089F5D41DEDCD5E1AE4F79A7CA3D91D0466CC1117EDB33A094F5FBD8B7AA8` |
| guest 簽署 CAT | `7723B617E6B5EB00119C20D75025305F142DD85EF447D55DD4AF8B8A766F6326` |

載入前檢查 guest Secure Boot 已關閉、TESTSIGNING 已設定，且開機時間確實晚於設定時間，再以執行中的 Code Integrity TESTSIGN flag 確認模式生效。不能只看一次 `bcdedit` 成功或桌面浮水印就略過重新啟動條件。Windows 官方亦說明 TESTSIGNING 變更需重啟才生效。[Microsoft 載入測試簽署程式碼](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)

主機沒有安裝本次實驗性 LDAC 驅動、匯入此測試憑證或變更其 Secure Boot／TESTSIGNING。主機藍牙控制器仍曾暫時移交 VM，因此不把「主機未安裝驅動」寫成「主機完全沒有狀態變化」。

## 5. 安裝前失敗：PowerShell 5.1 的 JSON 陣列語意

第一次 Validate 停在 `Exact stock XM6 identity required`，尚未安裝。問題是檢查工具對 Windows PowerShell 5.1 的 JSON pipeline 行為作了錯誤假設：`@(Get-Content ... | ConvertFrom-Json)` 的第一層長度為 1，而第一項本身是 `System.Object[]`。後續篩選看似各得到一筆，實際選到的卻是整個巢狀陣列。

具體失敗的 INF 判定因此同時包含 Audio Sink 的 `microsoft_bluetooth_a2dp_src.inf` 與 parent 的 `bth.inf`。這是工具資料形狀錯誤，不是 XM6 身分真的改變，也不是應移除身分保護的理由。

修正為先把 `ConvertFrom-Json` 結果賦值，再以 `@($stock)` 正規化，並拒絕非預期的巢狀資料。使用真正的 Windows PowerShell **5.1.26100.9711** 和固定雜湊的原 stock inventory 執行純資料 fixture，八項判定全部通過：兩筆平坦紀錄、唯一 sink、唯一 parent、兩者名稱、stock INF、stock service，以及私有位址後綴。fixture 未呼叫 radio 或安裝 API。修正後另以實際 guest Validate 通過，才進入 Install。

## 6. 原生載入與 DISCOVER 的實測結果

原生載入輪次為 **15:57:02–15:57:35 UTC**：Validate 與 Install 控制程序皆回傳 0，DevCon 回傳 0，沒有要求安裝後重啟。服務狀態 Running、PnP 狀態 OK／problem 0，DriverStore 中實際載入 SYS 的雜湊與上表簽署 SYS 相同。

其後 `--list`、`--status`、`--diagnostics` 均回傳 0。初始 status 的 connections、retained connections、in-flight、bytes sent／received 均為 0，diagnostics 計數亦為 0。這些查詢本身不開 Bluetooth channel，故僅證明裝置介面與診斷路徑，不提前當成傳輸成功。

DISCOVER 輪次為 **15:58:16–15:58:35 UTC**，結果如下：

| 欄位 | 實測值 |
| --- | --- |
| `avdtp_discover_accepted` | true |
| `audio_sink_seid` | 1 |
| 傳送／接收 bytes | 2／8 |
| open attempts／completions／failures | 1／1／0 |
| callback add／release | 1／1 |
| close attempts／completions／failures | 1／1／0 |
| transfer retirements／roots destroyed | 2／1 |
| contract faults／remote disconnects／cancel calls | 0／0／0 |
| 結束後 connections／retained connections／in-flight | 0／0／0 |
| LDAC negotiated／audio streamed | false／false |

這建立真正訊令通道的往返與正常關閉證據；DISCOVER 只發現端點，沒有協商 LDAC、設定串流或建立 media channel。本輪也沒有觸發取消、低資源或遠端斷線，所以不能用零錯誤計數宣稱這些故障路徑均已在實體耳機上驗證。

## 7. 首次 990 kbps 播放失敗的保留紀錄

播放輪次為 **16:00:25–16:00:37 UTC**，來源結構檢查是 PCM16、48 kHz、雙聲道、96,000 frames／384,000 data bytes，對應 2 秒。結構檢查不掃描樣本值，因此本文不據此聲稱振幅、音訊品質或內容已驗證。

請求保留手動 **990 kbps HQ**，`battery_downgrade=false`、attempts 1、restarts 0。結果為 `result=-2`、`win32_error=13`、`stage=1`；`protocol_error=0` 與 `encoder_error=0` 不能單獨證明協商或編碼已執行。實際報告 `actual_kbps=0`、source frames 0、media packets 0、media bytes 0，表示尚未建立可證明的串流。

失敗後 open completion 1、callback add/release 各 1、close completion 1、roots destroyed 1；contract faults 與 close failures 為 0，status 的 connections、retained connections、in-flight 及傳輸 bytes 都回到 0。這支持**本次早期失敗仍有關閉與資源退休證據**；不是媒體傳送中失敗的復原測試，也不是整個核心記憶體零洩漏的證明。

隨後於 **16:02:30–16:02:45 UTC** 加入 completion 觀察，定位到 CONNECT IOCTL 的輸出長度契約錯誤：

| 觀察項目 | 實際回傳 | 判讀 |
| --- | --- | --- |
| CONNECT completion bytes | 368 | 此 IOCTL 沒有輸出 buffer，預期應為 0 |
| QUERY 結構大小／ABI／state | 64／2／2 | 結構與 OPEN 狀態正確 |
| max packet／max channels | 1008／2 | 符合既定 ABI |
| outbound／inbound MTU | 895／1008 | 位於使用者模式允許範圍內 |
| 再次 DISCOVER | accepted、SEID 1 | 訊令通道仍可往返 |

來源檢查與觀察相符：原生 CONNECT completion 保留了下層 BRB 的 `Information`，導致沒有輸出資料的 IOCTL 卻向上層報告 368 bytes。播放器的 [native open 檢查](../../src/win_native_io.c) 在 CONNECT 成功但 bytes 非零時立即回報 `ERROR_INVALID_DATA`，尚未執行 QUERY；此處的嚴格拒絕是正確行為。原 [DISCOVER probe](../../tools/native_probe.c) 只檢查 CONNECT error，沒有檢查這個 bytes 契約，因此能繼續 DISCOVER，兩種結果並不矛盾。

這輪證據排除了「觀察到的 MTU 本身不合格」這個推測，也沒有證明耳機不支援 LDAC。修正方向是讓核心 CONNECT completion 明確帶回零 `Information`，新增回歸檢查，並保留播放器的 bytes 驗證。本文截止點尚未包含修正後原生套件重新建置及 guest 實體驗證；後續必須用新結果建立證據，不能把本次失敗改寫成成功。

## 8. 回復範圍、工程心得與下一步

載入、DISCOVER、第一次 HQ 嘗試與 CONNECT completion 觀察四個輪次均記錄 detach exit 0、`host_restored=true`。這是控制器移回主機的結果，不等於已移除 guest 驅動或回復先前耳機配對金鑰；VM snapshot 也不能還原耳機本體曾變更的配對狀態。

本次進展的關鍵是保留每個層次的獨立判定：來源可追溯不等於可載入，可載入不等於可互通，DISCOVER 成功也不等於音樂已播放。另一方面，腳本的 JSON 形狀錯誤與驅動本身錯誤必須分開處理；修正資料解析後重跑精確身分檢查，比直接放寬保護更能維持實驗可解釋性。

下一步先重建並驗證 CONNECT completion 修正，再依序驗證 codec 能力、SET_CONFIGURATION、OPEN、media L2CAP、START 及實際音訊，再測正常停止與斷線重連。手動碼率仍保留；本次只有請求 990 kbps 的證據，尚未完成 990／660／330 kbps 的耳機端互通比較。音質與電池優先順序是產品要求，尚無聲學或電池能量量測可供比較。Windows 音訊端點、正式發行簽章與日用筆電安裝均仍待後續門檻通過。

公開材料只保留方法、二進位雜湊與去識別化統計。原始診斷、裝置位址／實例、VM 識別碼、憑證指紋、還原資料及配對資訊不放入 GitHub；本報告也不附可直接套用到其他電腦的 VM-only 安裝包。

## 後續修正與實際聆聽

CONNECT 完成長度的原因、回歸測試、修正版原生套件及後續 XM6 990 kbps 短音播放，另見 [CONNECT 修正與實際播放紀錄](XM6_CONNECT_FIX_2026-11-07.md)。本文件保留首次失敗時的觀察，不以後來成功覆寫。
