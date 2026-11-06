# 實體 MediaTek 通道準備與斷線判斷：2026-11-06

## 問題與實驗邊界

目標是讓保留的 Windows VM 使用同一顆內建 MediaTek 藍牙，再逐步驗證 WH-1000XM6 的 L2CAP、AVDTP 和 LDAC 播放。主機與 VM 無法同時獨占同一 USB 藍牙控制器。操作者已確認可使用內建／有線輸入，因此先做一次不配對、不安裝實驗性驅動的 USB 列舉測試。

本次程式以 `ee9b27d7868efe9c5e6c4e3d709d9c722a649627` 為基底。HQ 優先、手動碼率與現有 40 ms pacing 防護未變；目前沒有新的音質、電功率或耳機播放結果。

## 首次實體移交的失敗證據

1. 對唯一符合 VID/PID 的 MediaTek USB 裝置執行單次移交，沒有建立自動捕捉所有 USB 的 filter。
2. `usbattach` 回傳 0，主機 USB 清單一度顯示 Captured；但 VM 的實際附掛裝置清單為空。**命令成功及 Captured 都不足以證明 guest 能使用藍牙。**
3. Guest 列舉程序逾時，沒有取得 guest 藍牙可用的證據。USB 重新列舉使 UUID 改變，對原始與更新 UUID 的 detach 都回報未附掛。
4. 正常關閉 VM 後，主機 MediaTek 與 XM6 音訊服務重新列為 OK；驅動 INF、服務與版本和基準一致。沒有強制關機、重啟主機、重新配對或安裝 LDAC 驅動。初次失敗判定和後續恢復紀錄分別保留，沒有以恢復成功覆寫失敗。

主機唯讀盤點進一步確認：原先最小安裝只選 VBoxApplication，現用 MSI 的 VBoxUSB 功能狀態為 absent；USB 驅動檔與服務也缺少。因此 USB bridge 是已確認缺少的前置條件，仍需安裝後重測才能判定完整原因。

## 經授權的環境修正

使用本機保留、Oracle America 簽章有效的 VirtualBox 7.2.22 MSI，只新增 VBoxUSB。SHA-256：`67875d993fe2460f4515fc4571feeffe0b3542f2ac450920332f8a1d53d566e8`。安裝回傳 0；VBoxUSB 由 absent 變成 local，VBoxUSBMon 為 Running；其餘網路與 Python 功能仍 absent。未自動重啟主機，未更改 Secure Boot、BCD 或主機 LDAC 驅動。

注意：繁體中文 MSI 內嵌語言轉換會改變 ProductCode。原始 MSI 的 ProductCode 不能直接視為目前安裝產品的識別碼；先查目前產品及 FeatureState，再核對歷史安裝 log 的語言轉換，避免修復錯誤產品。

一次乾淨 VM 開機後 Guest Control 卡在 starting，連短指令也失敗，ACPI 正常關機未完成；此時尚未傳入測試程式。保留畫面、log 和狀態，再保存執行狀態、還原既有 clean snapshot 冷開機。後續測試結果另列，這次卡住不算軟體測試通過。

## 補齊元件後的第二次實體測試

重新啟動 VirtualBox 後，在 xHCI 已開啟的乾淨 VM 重試同一顆控制器。`usbattach` 再次回傳 0，USB host 清單顯示 Captured，但 VM 實際附掛清單仍為空；另行執行 guest 裝置盤點，目標 USB 裝置數為 **0**。這不是 XM6 互通成功。

主機 MediaTek 的 PnP 問題碼為 0，仍由原 oem5.inf / BTHUSB 驅動。未配對、未載入實驗性核心驅動、未嘗試耳機播放。已確認新增 USB 元件**不足以單獨解決問題**；仍需辨識 host USB 重列舉／filter 啟用與此控制器的 passthrough 相容性。沒有為排除問題而重啟整個 USB hub、停用主機安全功能、強制拔除裝置或自動重開主機。可在使用者方便正常重啟主機後重測，但目前沒有證據能保證重啟會解決。

## 重連判斷修正與限制

`OlBeginTransfer` 在已斷線生命週期回傳 `STATUS_CONNECTION_DISCONNECTED`，Windows 將其映射為 `ERROR_NETNAME_DELETED`（64）。既有重連白名單漏掉 64，現在由共用純函式 `wl_win_native_retryable_error` 判定；播放器仍先要求 `WL_PLAY_TRANSPORT`，並遵守原有 stop、重試上限與整首 WAV 重播規則。

新增 28 項正反向檢查：接受明確斷線碼，拒絕 timeout、資源不足、無效參數及取消 995。**995 不能單獨證明遠端斷線**：遠端 disconnect 回呼取消未完成 BRB，以及本機 stop／D0 取消都可能回報它。這次不把 995 加入白名單，也不宣稱所有真實斷線都能自動恢復。後續需要有來源依據的 provider 斷線狀態與實體驗證。

本機完整回歸：native Windows owned-pipe 1,471 checks；session 9,381；reassembly 292,045（重複 CHECK，非獨立情境數）；playback fake-peer 927；reconnect attempt-stub 190；WAV 818；CLI 68；WAV/CLI integration 104；core 5,805；lifecycle 18,976／1,020 schedules；host shim 7,532。這些不是耳機 RF 或新的核心 Verifier 結果。

## 隔離 VM 回歸

乾淨冷開機後，指定測試已通過：native I/O 1,471、session 9,381、reassembly 292,045、playback 927、reconnect 190、WAV 818。實際經 SCM 執行手動 128/129/130 控制、stop/delete，並確認壞 WAV 回報 11、缺少原生介面回報 259。69 個建置輸入與 8 個執行檔雜湊和本機一致；沒有載入新的核心驅動或使用 RF。[可公開結果與雜湊](physical-disconnect-2026-11-06.json)。

## 後續驗證順序

USB Support / guest control 正常 → guest 真實藍牙列舉 → 合法簽章與 guest 驅動載入 → 有界 DISCOVER 與 root/IRP 退休計數 → 雙 L2CAP 通道 → LDAC 協商與手動碼率 → 實際聆聽、斷線／重連、休眠與功耗。每一步分別保留失敗與恢復結果，前置條件未過就不往主機安裝。

## 原始資料與參考

完整私有 USB/PnP、VM UUID、控制器輸出與畫面保留在本機專用結果目錄，公開報告不包含 MAC、配對金鑰、密碼或原始裝置識別碼。

- [Oracle VirtualBox 7.2 VM 與 USB 說明](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/working-with-vms.html)
- [Microsoft L2CAP open-channel BRB](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ns-bthddi-_brb_l2ca_open_channel)
- [Microsoft Bluetooth indication callback codes](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthddi/ne-bthddi-_indication_code)
- [前階段 peer-control 與重連實驗](PEER_CONTROL_RECONNECT_2026-11-06.md)



## 最後狀態

原始碼、受測 EXE 與完整私有結果已留存。最後乾淨 VM 執行期檢查通過（Secure Boot、BCD、Verifier、專案服務／憑證），正常關機後還原同一 clean snapshot 與原 USB 設定，確認 poweroff。主機目標裝置均為 OK，原 INF／服務／版本一致。Oracle USB Support 保留供後續測試，實驗性 LDAC 驅動未安裝到主機。[實體測試摘要](physical-radio-2026-11-06.json)。

後續：[主機重啟後的 USB 直通對照實驗](POST_REBOOT_USB_2026-11-06.md)。重啟及穩定識別規則仍未讓控制器進入 VM。
