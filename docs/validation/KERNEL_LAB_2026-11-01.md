# 隔離核心實驗紀錄：2026-11-01

## 目的與結論

確認生產傳輸函式在真實 WDF 環境中，能完成指定的合成提供者案例，
並比較基準、標準 Driver Verifier 與卸載結果。

修正版 `36bc35f` 的**合成核心基準與標準 Verifier profile 通過本次案例**。
這是有範圍的工程驗證，不是整個原生 LDAC 驅動已可供日用安裝的結論。
真實 BthPort、MediaTek／XM6、音訊端點、低資源等額外故障注入、
實機休眠／移除、音質與電力仍未取得驗證。

## 可重現的版本資訊

| 項目 | 值 |
| --- | --- |
| 程式提交 | `36bc35fd0ef8563af7a7a05f2c55835d18553522` |
| Git tree | `2cb1c34be6e377f2e8a772680359927f44dea24f` |
| CI PR merge checkout | `192dfc7bdcfe29be102cad793055cf70ed63d5a3`，Git tree 與上述提交相同 |
| 測試 SYS SHA-256，簽章前 | `a329b7cc0f74f1586b5df084c7e4a2a08cf64ae397b78cbca46171185e9515d9` |
| 同次 PDB SHA-256 | `b61d230dffc466489e0eddb49ebfbf60f2ce58703359ff78d425fbe0d559cc1c` |
| SYS／PDB CodeView GUID | `529faa74-7a66-4a89-b827-a0891adb6400`，age 1 |
| Artifact ZIP SHA-256 | `8e7b8fac0067374d26446ed4ba9c52b55e93e1c070ecd8db37dc032976814bb6` |
| Artifact ID | `13052064184` |

CI artifact 保持未簽章。載入的是在客體中以私有測試憑證簽署的複本；
簽章會改變檔案雜湊，控制腳本分別記錄來源雜湊與已簽章雜湊。
沒有將測試私鑰或客體信任安裝到日用主機。

三組 CI：

- [離線核心、Windows／Linux 與 sanitizer](https://github.com/peianchen/open-ldac-windows/actions/runs/41227111388)：通過。
- [原生傳輸建置、分析與套件檢查](https://github.com/peianchen/open-ldac-windows/actions/runs/41227111394)：通過。
- [合成核心驅動建置、分析與符號](https://github.com/peianchen/open-ldac-windows/actions/runs/41227111410)：通過。

## 環境與設計

- VirtualBox 7.2.22，x64 Windows 11 Enterprise Evaluation，回報 build 26300。
- 4 vCPU、4 GiB RAM、執行上限 100%、動態 80 GiB 磁碟。
- 真實 KMDF／WDF 物件與要求；下層 Bluetooth 提供者為專案自製合成實作。
- 沒有實體藍牙／USB passthrough，也沒有 Windows 音訊端點。
- 乾淨、關機狀態的快照在測試憑證、專案服務與 Verifier 之前建立。
- 客體管理員與 GuestControl 經實際驗證；服務為 demand-start。
- 測試簽章、必要的虛擬 Secure Boot 變更與虛擬 COM 核心除錯只作用於客體。
- 使用 NEM；除錯與時鐘 catch-up 會影響客體時間，未以本輪推算音訊延遲或實體電力。

合成提供者會參照必要的 WDM 物件、產生 AddReference／ReleaseReference，
並讓讀取實際進入 WDF pending／取消流程。它模擬提供者契約，不能證明真實 BthPort 的事件順序。

## 基準案例

runner 會檢查每個案例的 API 結果與退場條件，包含：

1. 25 次連線、寫入、取消 pending read、關閉與根物件歸零。
2. OPEN 失敗與清理。
3. CLOSE 失敗、延後提供者退場，以及兩個保留根物件的容量上限。
4. 釋放保留回呼後根物件歸零。
5. 四種遠端中斷原因與重設。
6. 合成移除／target stop 與延後退場。
7. 診斷 ABI、沒有 contract-fault、回呼建立／釋放平衡。

合成移除呼叫傳輸的移除路徑，不是實體 PnP 拔除測試。
runner 的原始 JSON 如下；基準與 Verifier 執行各自輸出相同結果：

```json
{"kind":"real_wdf_synthetic_provider_kernel_lab","checks":363,"callback_add":33,"callback_release":33,"roots_destroyed":33,"real_bluetooth_tested":false,"headset_audio_tested":false,"verifier_status_requires_external_evidence":true}
```

## 結果與證據對照

| 測試 | 觀察 | 判定 |
| --- | --- | --- |
| 原始 `f57107c` 基準 | 連線時觸發 0x3B／C0000005 | 失敗，保留失敗紀錄 |
| live KD 重現 | NULL BRB；主要求實際為 8-byte LAB_PORT_REQUEST；IFR 記錄型別不符 | 定位至測試初始化的多敘述巨集 |
| `36bc35f` 基準 | 363 checks、33 add、33 release、33 roots destroyed，exit 0 | 本次合成案例通過 |
| 基準後卸載 | 服務狀態 Stopped | 通過 |
| 標準 Verifier 啟動 | 實際 `verifier /query` flags `0x001209bb`，目標模組 load 1 | 已確認作用於目標模組 |
| 標準 Verifier runner | 相同 363 checks 與平衡計數，exit 0，未觸發新的當機 | 此 profile／案例通過 |
| Verifier 下卸載 | runtime load 1／unload 1，服務 Stopped | 通過 |
| 測試後設定 | 執行客體 Verifier reset，保存證據並正常關機 | 清除未來設定；本次 active 狀態隨關機結束 |

Verifier 設為 **standard oneboot**、只選擇 `OpenLdacKernelLab.sys`。
設定清單包含特殊集區、強制 IRQL、集區追蹤、I/O、鎖死、DMA、安全性、其他檢查、
DDI 與 WDF 規則類別 34。不是只查看待啟用設定：有重啟後 runtime 模組與載入次數，
以及同次啟動完成測試／卸載的紀錄。

此次 runtime 集區配置統計全部為零。WDF 內部配置的歸屬與追蹤範圍不能由此推定；
**零計數不等於已證明所有記憶體配置沒有洩漏。**

## 失敗處理與資料完整性

第一次當機沒有成功建立預定的 Windows dump，系統出現 volmgr 161。
後續使用虛擬 COM 連線與正確 PDB，重現相同錯誤並成功產生 843204-byte 小型核心 dump。
WDF context 的實際大小與類型，讓根因可以和修正直接對應；細節見
[教學中的除錯案例](../DEVELOPMENT_GUIDE.zh-TW.md#6-實例一個少了大括號的巨集如何造成核心當機)。

當機前部分 JSON 尚未完整落盤，讀回時包含 NUL，已標記無效。
修正後的基準與 Verifier 通過紀錄另外保存並解析檢查，沒有拿損壞檔案當通過證據。
公開報告只包含去除識別資訊的摘要；原始 dump、私有憑證與安裝帳密沒有公開。

## 未完成的驗證

- 低資源、強制 pending I/O、同步延遲模糊測試，以及實際觸發的故障點統計。
- 核心中 pending OPEN、每個 Send 邊界的取消、回呼在不同 IRQL／CPU 的競爭。
- 真實 BthPort 的參照／退場行為與正式傳輸裝置的 PnP／電源流程。
- 完整 AVDTP、真實 XM6 協商與音訊、Windows 端點／服務。
- 感知音質、glitch、端到端延遲、RF 共存、AC／電池與實際瓦數／續航。
- 正式發行簽章、安裝與原始服務綁定復原。

請以[核心測試矩陣](../KERNEL_TEST_MATRIX.md)持續補齊；本報告不解除日用筆電安裝門檻。
