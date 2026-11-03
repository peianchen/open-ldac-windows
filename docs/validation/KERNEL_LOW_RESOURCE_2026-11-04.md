# KMDF 低資源錯誤處理實驗：2026-11-04

## 問題與有限範圍結論

本輪研究的問題是：在真實 WDF 的物件配置開始持續失敗時，傳輸與合成提供者能否走過失敗路徑、完成取消與退場、正常卸載，並在移除低資源條件後恢復原本的功能？

**指定版本的 KMDF allocation-threshold 測試 gate 已通過。**
測試來源為 `969a9efc010e47d6f67f29f1fc7e9b29f29d5c0b`，在隔離 Windows VM 使用真實 WDF 與合成 Bluetooth 提供者。129 個配置門檻測試、必要的十個執行期站點正向覆蓋、FINAL 退場證據、正常卸載與取消低資源條件後的恢復測試均符合本輪接受條件。

這個結論允許接續播放協定開發，**不是日用筆電安裝或耳機播放驗收**。本輪沒有實體 Bluetooth passthrough，沒有驗證真實 BthPort、MediaTek／XM6、實體 PnP／休眠、Windows 音訊端點、完整 AVDTP 串流、聽感或耗電。HQ 音質優先、手動碼率與電池不自動降級的既有政策保持原樣。

可重算的公開資料見 [JSON 摘要與 FINAL 遙測](low-resource-2026-11-04.json)。私人原始紀錄、先前失敗、安裝資料與除錯資料另行保留，未將帳號、VM UUID、私人路徑、私鑰或原始 dump 放入本報告。

從開發分支的儲存庫根目錄可重新解碼公開數值：

```powershell
python tests/kernel/audit_low_resource.py
```

工具逐筆核對 FINAL 原始位元組、唯一 nonce、資源與根物件銷毀、十站點覆蓋及彙總。
它是已發布紀錄的一致性稽核，不會重跑 VM，也不能單靠公開檔案重新見證先前的執行。

## 官方機制與本輪採用的方法

KMDF Verifier 可檢查框架介面的狀態、參數、IRQL、取消與鎖定等契約，也能模擬配置失敗。本輪使用驅動專屬的 `Parameters\Wdf` 設定：`VerifierOn` 與 `VerifierAllocateFailCount=n`。官方定義是，在第 n 次配置之後，框架使後續物件記憶體配置失敗；計數也包含框架代表驅動進行的配置，且不同框架版本可能改變配置順序。因此 n 是測試門檻，不能直接當成原始碼的某個固定配置站點。[Microsoft：KMDF 除錯登錄值](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/registry-values-for-debugging-kmdf-drivers)、[Using KMDF Verifier](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-kmdf-verifier)

本輪**沒有使用或驗證** Driver Verifier 的 random low-resource simulation 或 systematic／DIF low-resource injection。後者依被攔截的呼叫堆疊進行可重現注入，具有自己的規則及注入統計；不能把本輪的 KMDF 配置門檻觀測改名成 DIF 結果。[Microsoft：Systematic Low Resources Simulation](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/systematic-low-resource-simulation)

### 觀測與故障來源分開

測試版加入 24 個觀測站點。`OlLabObserveResource` 記錄真正 WDF 呼叫的回傳 NTSTATUS、嘗試次數、失敗次數及資源類失敗次數；它不替換 WDF 方法，也不把成功狀態改造成失敗。資源類判定接受 `STATUS_INSUFFICIENT_RESOURCES`、`STATUS_NO_MEMORY`、`STATUS_QUOTA_EXCEEDED`、`STATUS_COMMITMENT_LIMIT`，但本輪正向命中實際只看到 `0xC000009A`（`STATUS_INSUFFICIENT_RESOURCES`）。

觀測介面只用於合成核心測試建置，詳見 [`lab_observer.h`](../../tests/kernel/lab_observer.h) 與 [`lab_driver.c`](../../tests/kernel/lab_driver.c)。這不是在正式傳輸路徑加入會隨機破壞使用者操作的注入器。

## 可重現的二進位身分

| 項目 | 值 |
| --- | --- |
| 測試來源 | `969a9efc010e47d6f67f29f1fc7e9b29f29d5c0b` |
| CI 實際 checkout | `09fbf605ccd9ea389e86612dde39f9068bacadb5` |
| CI／來源差異 | 只有 `README.md`、`README.zh-TW.md`；33 個受查建置輸入相同，完整 Git tree 不同 |
| 未簽章 SYS SHA-256 | `a6063c1498b165953bdd37380f98405d82b00b9917b647b74de4056334c27770` |
| 客體測試簽署 SYS SHA-256 | `eba1538a5957a804e40c99cd74d467fe6f5922b94ce9ddbfd53b06d3dbd1c675` |
| 同次 PDB SHA-256 | `eace3f5636d1951d9b632daf3fa772ac8ca51df3a0848dcf22fb6e858cf9215c` |
| SYS／PDB GUID、age | `c8195e4f-9c07-4a56-9461-61b98c9e8872`，age 1 |
| Runner SHA-256 | `aa4de179059e66f971e6f3c9d6d978c6f9a3fc26a0a04e16c47f25898010432a` |
| CI ZIP SHA-256 | `38c458d2ac5fae12100dba105ef2ddcfa50aca8726294db50e1a14bc6838f20c` |
| Artifact ID | `13244134204` |
| FINAL 遙測布局 | 464 bytes、version 2、24 sites、`snapshot_kind=2` |

這組結果使用的是 `969a9ef` 的產物與 runner，不把 [2026-11-03 的 `4437db1` 結果](KERNEL_LAB_2026-11-03.md)當作新版重測。三組建置 CI 均通過：

- [離線與 host 測試](https://github.com/peianchen/open-ldac-windows/actions/runs/41666545659)
- [生產傳輸建置／分析](https://github.com/peianchen/open-ldac-windows/actions/runs/41666545641)
- [合成核心驅動建置／分析](https://github.com/peianchen/open-ldac-windows/actions/runs/41666545738)

簽署後 SYS 的雜湊與未簽章 artifact 分開記錄；不能只核對來源提交就假設客體載入的是同一個檔案。

## 實驗程序與接受條件

### 1. 建立基準

先用同一新版套件執行無 allocation poisoning 的基準與正常卸載。基準通過 6,343 checks，包含 140 個延後 OPEN 案例；callback add／release 各 173，roots destroyed 為 173。這是本輪 `969a9ef` 的基準，與先前版本分開保存。

### 2. 每個配置門檻使用全新 load

依序測試 `n=0..128`，共 129 個門檻。每一輪先卸載舊測試服務，再配置門檻、建立新的一次載入身分、嘗試載入，並依結果執行 workload 或檢查初始化失敗。每輪都須取得該次 load 的最終證據，不能沿用上一輪 registry 資料。

- 80 輪啟動失敗，仍須以該次失敗進入／清理的 FINAL 記錄驗證。
- 49 輪載入成功，執行各一輪低資源 workload，再正常卸載。
- 每個 threshold 的 load nonce 都不同，且必須與 FINAL `load_id` 相符。
- 初始／進度記錄與 FINAL 記錄分開。FINAL 寫入若失敗，不接受舊進度資料作為成功證據。

低資源條件是「門檻後持續失敗」，所以同一門檻可能使幾個不同呼叫或重試失敗。它不是每輪只打一次、相互獨立的單點隨機事件。

### 3. 最終資源條件

所有 threshold 都須有完整 464-byte version 2、24-site FINAL 記錄，匹配本輪 load 身分；其中 `connections`、`in_flight`、`active_files`、`pending_open` 必須歸零，callback add／release 平衡，`contract_faults=0`，服務完成對應的退場狀態。

根物件銷毀計數依各次實際路徑判讀，不能要求每個門檻的 roots destroyed 都等於 callback add：物件可能在註冊回呼之前就配置失敗或被清理。這也說明只看某一組相等計數不足以驗收整條生命週期。

接受條件另外要求十個指定執行期站點至少各有一次**實際資源失敗**。預測某個 n「應該會打到」某站點、或只看到 API 回傳資源不足，都不能替代正向站點證據。控制器結果與主機側逐 byte 解碼的 FINAL 原始資料交叉核對，兩者皆通過；另有獨立審查核對統計口徑與邊界。

### 4. 移除低資源條件後恢復

關閉本輪 allocation-threshold 條件後，用同一套件再次載入、執行完整 runner 並卸載。恢復輪再次通過 6,343 checks／140 個延後 OPEN 案例，173 次 callback add、173 次 release、173 個 root destroy；FINAL 四項資源皆為零，contract faults 為零。

這個恢復輪證明同一受測套件在取消該條件後可恢復所測功能，不等於已驗證任何實體藍牙重連或耳機播放。

## 已觀察到的結果

### 統計口徑

| 數值 | 口徑與意義 |
| --- | --- |
| 129 | 門檻 `0..128`，每個門檻各一次全新 load |
| 80／49 | 啟動失敗／啟動成功後進入 workload 的輪數 |
| 24／18 | 觀測站點總數／至少一次資源失敗的站點數 |
| 102 | 至少有一個觀測站點資源失敗的 threshold 數 |
| 124 | 觀測到的資源失敗**呼叫次數總和**，分布於上述 18 站點、102 thresholds |
| 80＋44 | 初始化站點／十個必需執行期站點的資源失敗呼叫數 |
| 25 | workload 在使用者 API 層觀察到的預期資源錯誤，與 124 不同口徑 |
| 130／130 | Verifier runtime 對目標模組記錄的載入／卸載次數 |

124 不是 124 次獨立注入、124 個唯一 fault sites，或 124 個 runner API 失敗。runtime 的 130／130 也不是 129 個 threshold 全都成功初始化：門檻中的 80 次啟動失敗另有明確記錄；載入／卸載計數需與整個執行序列一起判讀。

### 24 個站點的實際覆蓋

下表的「命中數」是 `resource_failures` 的總和。正值站點本輪所記錄的資源失敗 NTSTATUS 都是 `0xC000009A`；「—」表示沒有觀測到該站點的資源失敗，不表示它沒有被呼叫，也不代表已完成其失敗路徑驗證。

| 站點 | 分類 | 資源失敗呼叫數 | 實際失敗 NTSTATUS | 命中的門檻 |
| --- | --- | ---: | --- | --- |
| `driver_create` | 初始化 | 9 | `0xC000009A` | 0–8 |
| `init_lock` | 初始化 | 1 | `0xC000009A` | 9 |
| `init_alloc` | 初始化 | 7 | `0xC000009A` | 10–12、42–45 |
| `init_name` | 初始化 | 4 | `0xC000009A` | 13–14、46–47 |
| `init_device` | 初始化 | 37 | `0xC000009A` | 15–29、48–69 |
| `init_queue` | 初始化 | 18 | `0xC000009A` | 30–41、70–75 |
| `init_target` | 初始化 | 2 | `0xC000009A` | 76–77 |
| `init_link` | 初始化 | 2 | `0xC000009A` | 78–79 |
| `file_lock` | 必需執行期 | 3 | `0xC000009A` | 81–83 |
| `root_object` | 必需執行期 | 2 | `0xC000009A` | 84–85 |
| `root_lock` | 必需執行期 | 2 | `0xC000009A` | 86–87 |
| `close_request` | 必需執行期 | 1 | `0xC000009A` | 88 |
| `close_memory` | 必需執行期 | 2 | `0xC000009A` | 89–90 |
| `open_memory` | 必需執行期 | 1 | `0xC000009A` | 91 |
| `open_format` | 必需執行期 | 1 | `0xC000009A` | 92 |
| `transfer_memory` | 必需執行期 | 10 | `0xC000009A` | 93–99 |
| `transfer_format` | 必需執行期 | 2 | `0xC000009A` | 96、100 |
| `close_format` | 必需執行期 | 20 | `0xC000009A` | 93–102 |
| `close_reuse` | 其他觀測 | 0 | — | — |
| `open_send` | 其他觀測 | 0 | — | — |
| `transfer_send` | 其他觀測 | 0 | — | — |
| `close_send` | 其他觀測 | 0 | — | — |
| `provider_final_retirement` | 退場觀測 | 0 | — | — |
| `init_target_open` | 其他初始化觀測 | 0 | — | — |

十個必需執行期站點全部正向命中，合計 44 次；八個初始化站點合計 80 次。六個零命中站點保持明確的覆蓋缺口。特別是 `provider_final_retirement` 的資源失敗為零，不表示沒有退休事件；它是退場時的觀測站點，必須另看事件與最終平衡。

### API 觀測與核心站點觀測不同

49 個成功啟動的 workload 合計完成 436 checks。使用者 API 層記錄 25 次預期資源錯誤，分別為開檔 4、連線 9、寫入 4、讀取 8；成功開檔 45、成功連線 36、成功寫入 32、取消讀取 28。這些計數各有自己的操作邊界，不能彼此當成同一組分母。

兩個具體例子說明為何必須同時保存不同觀測層：

- **n=80：**使用者層的 CreateFile 資源錯誤為 1，但 24 個站點的資源失敗總和為 0。框架內部未被此組站點直接觀測的配置可能先擋住開檔；只能記為 API 層的資源失敗，不能自行替某個站點增加命中數。
- **n=101／102：**runner 的使用者 API 資源錯誤均為 0，但各自的 `close_format` 真正回傳資源失敗兩次。最終檔案關閉的 provider retirement 觀測、FINAL 的資源歸零與 callback 平衡均完成。若只看 runner API 是否報錯，便會漏掉關閉時的錯誤處理。

### 基準與恢復

| 輪次 | Checks／延後 OPEN 案例 | Callback add／release | Roots destroyed | FINAL |
| --- | --- | --- | ---: | --- |
| `969a9ef` 無 poisoning 基準 | 6,343／140 | 173／173 | 173 | 四項資源歸零、contract faults 0 |
| 門檻測試後取消 poisoning 的恢復輪 | 6,343／140 | 173／173 | 173 | 四項資源歸零、contract faults 0 |

恢復輪有 2 次檔案退休等待，最大觀測等待值為 16 ms。這是該測試工具的一次觀測，不是立即重開保證、音訊延遲指標或通用的關閉時間上限。

## Verifier 證據與限制

同一次受測啟動中，Driver Verifier runtime 的旗標為 `0x001209bb`，模組清單只列 `OpenLdacKernelLab.sys`，最後記錄 load 130／unload 130。本輪使用此標準 profile 配合驅動專屬的 KMDF allocation threshold；並未把其他日用主機驅動加入測試。

Verifier runtime 集區配置／失敗統計仍為零。本輪真正的低資源命中證據來自 WDF 回傳值與每次 FINAL 遙測，不是從零值集區統計反推「沒有配置」或「沒有記憶體洩漏」。本輪的零資源與平衡條件也不能代替所有 WDF 內部物件的完整配置追蹤。

## 收集工具的失敗與修正

前置收集曾卡在 `Win32_SystemDriver` 的 WMI 查詢，因此該次流程沒有完成，也沒有被當作通過結果。後續改用 native SCM 查詢服務狀態、registry 讀取既有資料，在相同受測套件與驗收輸入下重新執行，才取得完整結果。

這個改動修正的是測試收集器對單一查詢介面的依賴；沒有修復或重設 WMI，沒有把日用主機的服務／驅動當作實驗對象，也沒有因為環境問題就放寬十站點正向覆蓋或 FINAL 規則。未完成與失敗的原始證據仍保留。

## 效度威脅與未回答的問題

1. **配置次序依版本而變。**本輪 n 與站點的對應只適用於受測 OS／框架／二進位與 workload。不能把 81 永久當成 file lock 的注入編號。
2. **門檻後持續失敗有相關性。**124 次失敗呼叫可能包括同一次門檻造成的多個失敗及重試，不是 124 個獨立統計樣本。
3. **站點不涵蓋全部框架內部。**n=80 已顯示 API 層可以失敗卻沒有本輪站點命中。零命中不能推為不存在低資源風險。
4. **合成提供者不是 BthPort。**本輪 final-file-close retirement 是測試提供者的可控制行為，仍須以實體提供者驗證真實契約及電源影響。
5. **快照／VM 時鐘不提供音訊效能結論。**VM 的 hypervisor 與除錯時序可能影響牆鐘，本輪主要驗證功能與生命週期，不推導音訊延遲或筆電能效。
6. **未測機制保持未測。**random／systematic DIF、更多並行與 IRQL／CPU 情況、實體 PnP／睡眠及播放鏈路均須依各自的接受條件補證據。

## 測試後還原與後續工作

先匯出來源、結果、八份測試 stage JSON、公開測試憑證及完整已簽署 SYS，再還原乾淨快照。還原後重新啟動客體進行 fresh runtime recheck，於台北日期 2026-11-04 核對下列狀態，**本輪清理與還原檢查通過**：

| 還原後檢查 | 結果 |
| --- | --- |
| 客體管理員執行能力 | 已確認 |
| 客體 Secure Boot | 開啟 |
| 專案服務／測試標記／測試憑證 | 0／不存在／0 |
| 客體 BCD 測試簽章或核心除錯 | 未啟用 |
| Verifier runtime | 沒有被驗證的驅動 |
| 新當機 dump | 未發現 |
| 最後關機狀態 | 正常關機，`poweroff` |

乾淨 VM 與快照保留供後續測試；原始碼、驗證結果、既有失敗紀錄及匯出套件也保留。這個還原結果只描述隔離客體的專案測試狀態，不是把其他程式、研究資料或筆電主機當作清理對象。

低資源 gate 通過後，可接續完整 AVDTP／播放協定實作與回歸測試，再處理 Windows 音訊端點與事件驅動編碼服務。實體 XM6 播放、休眠共存、音質與電力、正式簽章及筆電安裝復原仍是獨立里程碑。任何後續資源恢復策略都不能默默降低 HQ／使用者指定的碼率。
