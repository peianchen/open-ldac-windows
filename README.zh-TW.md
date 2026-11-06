# Open LDAC Windows

開發透過 Windows 原生藍牙堆疊播放 LDAC 的開源專案，將筆電的音質與耗電同時列為核心需求。

**目前包含 M0 離線核心與驗證中的 M1 原生傳輸。隔離 VM 的合成核心基準與標準 Verifier 案例已通過；尚未在日用 Windows 安裝 LDAC 驅動，也未完成實機耳機播放或 Windows 音訊端點。**

## 研究背景、開發教學與心得

- [研究背景與原理](docs/RESEARCH_BACKGROUND.zh-TW.md)：PCM／LDAC、藍牙協定、Windows 架構、研究問題與實驗設計，附主要來源。
- [完整開發教學](docs/DEVELOPMENT_GUIDE.zh-TW.md)：離線建置、原生傳輸、取消與釋放、VM／簽章及真實核心當機除錯。
- [開發紀錄與心得](docs/DEVELOPMENT_JOURNAL.zh-TW.md)：版本演進、失敗、修正理由與工程反思。
- [2026-11-01 核心實測報告](docs/validation/KERNEL_LAB_2026-11-01.md)：精確版本／雜湊、363 項案例、active Verifier 與未驗證部分。
- [2026-11-03 延後連線與故障驗證](docs/validation/KERNEL_LAB_2026-11-03.md)：三輪各6,343項檢查，含140個延後連線案例、標準與強制pending Verifier、實際失敗與回收觀測。

- [2026-11-04 低資源故障與恢復](docs/validation/KERNEL_LOW_RESOURCE_2026-11-04.md)：129 個門檻、124 次觀測失敗、10 個必測分支與正常恢復；附可讀取的原始數值。

- [原生通道與音訊服務整合報告](docs/validation/PLAYBACK_USERMODE_2026-11-04.md)：真 Windows I/O、VM SCM 控制／正常與錯誤退場，保留計時測試失敗與限制。

- [播放尾段與正常 CLOSE 驗證](docs/validation/GRACEFUL_CLOSE_2026-11-06.md)：等待最後成功傳送的封包時長、接收 CLOSE 回覆後才釋放本機媒體通道。

文件區分官方規格、廠商說明、實作、實測、推論與待驗證設計；沒有把模擬或離線結果當成音質／耗電實測。

## 目前功能與策略

已實作：

- 整合固定版本的 AOSP／Sony LDAC 編碼器，保留 Apache-2.0 授權與 NOTICE。
- 44.1／48／88.2／96 kHz 立體聲 WAV 離線編碼；支援 PCM16／24／32 與 float32。
- 基本 AVDTP 解析／設定封包、LDAC 能力選擇、RTP／LDAC 封包組裝。
- 有界 [AVDTP 協商模組](docs/AVDTP_SESSION.md)：逐端點查詢、精確採樣率、媒體 ready 與錯誤退場；目前僅離線測試。
- 音質／省電策略、核心與檔案整合測試、可重現的離線 CPU 比較工具。

筆電預設策略：

- 預設 HQ，保留可直接使用的 44.1／48 kHz 來源，不強制升頻至 96 kHz。
- 切換到電池不會自動降低音質；SQ 省電模式由使用者明確選擇。
- 後續即時處理採事件驅動、預先配置的有限緩衝區；停止播放時停止編碼。
- 不以忙碌迴圈或全系統 1 ms 計時器維持運作。
- 實際電力、續航、音質、藍牙重傳與穩定性仍待實測。CPU 計時不代表瓦數或續航。

詳見 [英文建置說明與里程碑](README.md)、[音質與耗電設計](docs/POWER_AND_QUALITY.md)、
[原生驅動架構](docs/NATIVE_ARCHITECTURE.md)、[驗證紀錄](docs/VERIFICATION.md)。

M1 已加入針對 XM6 音訊服務的 KMDF／L2CAP 傳輸、版本化介面與探索工具，
詳見 [原生傳輸說明](driver/README.md)。微軟範例衍生的傳輸原始碼採 MS-PL，其他原創核心採 Apache-2.0。

後續工作包括原生傳輸實機驗證、完整 AVDTP 狀態機、Windows 音訊端點、
正式端點／服務整合、實機播放、休眠恢復及筆電耗電驗證。WAV／SCM 工作服務原型已加入，
所列本機與 VM 結果不能當成上述實機工作已完成。
