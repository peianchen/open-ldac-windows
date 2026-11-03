# Open LDAC Windows

## 研究背景、原理與開發紀錄

這個預設分支仍是 M0 離線里程碑；以下文件記錄 M1 開發分支的研究與實作，
並區分已完成的合成核心驗證，以及尚待驗證的實機、音訊與耗電工作。

- [研究背景、原理與實驗方法](https://github.com/peianchen/open-ldac-windows/blob/611c4bc5d31a04a6a26352ef2ea198260094093c/docs/RESEARCH_BACKGROUND.zh-TW.md)
- [完整開發教學與實際除錯案例](https://github.com/peianchen/open-ldac-windows/blob/611c4bc5d31a04a6a26352ef2ea198260094093c/docs/DEVELOPMENT_GUIDE.zh-TW.md)
- [開發歷程與心得](https://github.com/peianchen/open-ldac-windows/blob/896a3cab7478ad5d6e76806940000c625e290f11/docs/DEVELOPMENT_JOURNAL.zh-TW.md)
- [核心基準與標準 Verifier 實測報告](https://github.com/peianchen/open-ldac-windows/blob/611c4bc5d31a04a6a26352ef2ea198260094093c/docs/validation/KERNEL_LAB_2026-11-01.md)
- [2026-11-03：延後連線取消、標準與強制 pending Verifier 實測](https://github.com/peianchen/open-ldac-windows/blob/896a3cab7478ad5d6e76806940000c625e290f11/docs/validation/KERNEL_LAB_2026-11-03.md)

開發透過 Windows 原生藍牙堆疊播放 LDAC 的開源專案，將筆電的音質與耗電同時列為核心需求。

**目前是 M0：離線編碼與協定核心。尚無可安裝的 Windows 驅動，也尚未透過藍牙傳送音訊。**

已實作：

- 整合固定版本的 AOSP／Sony LDAC 編碼器，保留 Apache-2.0 授權與 NOTICE。
- 44.1／48／88.2／96 kHz 立體聲 WAV 離線編碼；支援 PCM16／24／32 與 float32。
- 基本 AVDTP 解析／設定封包、LDAC 能力選擇、RTP／LDAC 封包組裝。
- 音質／省電策略、核心與檔案整合測試、可重現的離線 CPU 比較工具。

筆電預設策略：

- 預設 HQ，保留可直接使用的 44.1／48 kHz 來源，不強制升頻至 96 kHz。
- 切換到電池不會自動降低音質；SQ 省電模式由使用者明確選擇。
- 後續即時處理採事件驅動、預先配置的有限緩衝區；停止播放時停止編碼。
- 不以忙碌迴圈或全系統 1 ms 計時器維持運作。
- 實際電力、續航、音質、藍牙重傳與穩定性仍待實測。CPU 計時不代表瓦數或續航。

詳見 [英文建置說明與里程碑](README.md)、[音質與耗電設計](docs/POWER_AND_QUALITY.md)、
[原生驅動架構](docs/NATIVE_ARCHITECTURE.md)、[驗證紀錄](docs/VERIFICATION.md)。

後續工作包括 KMDF／L2CAP 傳輸驅動、完整 AVDTP 狀態機、Windows 音訊端點、
事件驅動編碼服務、實機播放、休眠恢復及筆電耗電驗證。離線編碼成功不能當成這些工作已完成。
