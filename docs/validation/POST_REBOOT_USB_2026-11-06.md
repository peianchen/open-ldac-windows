# 主機重啟後 USB 直通對照實驗：2026-11-06

## 結論與邊界

主機正常重啟後，Oracle USB 監控驅動正在執行，原 MediaTek／XM6 綁定正常，但內建控制器仍無法實際附掛到 Windows VM。第二個對照實驗確認 guest 目標 USB 數為 **0**。**沒有進行 XM6 配對、L2CAP／AVDTP 協商或 LDAC 播放；未安裝實驗性主機驅動。**

本輪原始碼維持 `b90af29da9fb8c3411c7eeebd60b91e5600312c8`。音質優先、手動碼率與 40 ms pacing 防護沒有修改；前輪通過的軟體回歸不等於這輪實體互通成功。[機器可讀結果](post-reboot-usb-2026-11-06.json)與[前輪修正紀錄](PHYSICAL_TRANSPORT_2026-11-06.md)分別保留。

## 方法與可觀察結果

主機開機時間讀值為 2026-11-06 13:49:03 UTC。先確認 VBoxUSBMon 為 Running、原裝置綁定一致、指定 VM 為關閉的 clean snapshot，再使用 xHCI。Guest Additions 尚未就緒時第一次短指令失敗，未移交 USB；就緒後再開始實驗。

| 實驗 | 操作差異 | VM 實際附掛 | Guest 盤點 | 主機恢復 |
| --- | --- | --- | --- | --- |
| 重啟後單次捕捉 | 唯一匹配 VID/PID 的裝置；最多約 30 秒等待 | 未出現 | 程序逾時，不能推定數量 | 原 MediaTek 為 OK |
| 穩定比對對照 | 暫時 VM-only 規則：VID/PID＋port 3＋local；不依賴 manufacturer/product/serial 字串 | 未出現 | 正常完成，目標 USB 0、Bluetooth class 0 | 原 MediaTek 為 OK |

兩次 `usbattach` 都回傳 0，host 清單曾顯示 Captured；但實際 VM 裝置清單為空。`usbdetach` 回報未附掛，不能當成成功解除通道。獨立 host PnP 檢查及最終綁定比對用來確認主機恢復，而不是只看 detach 的返回碼。

## 假說、反證與仍未解決的原因

1. **新安裝 USB 元件尚未在主機重啟後生效。** 重啟時間與 Running 狀態已確認，但仍失敗。因此單靠重啟沒有解決問題。
2. **USB 重列舉改變描述字串，造成捕捉條件失配。** 先前同 VID/PID 曾顯示 Foxconn 與 MediaTek 描述，而單次捕捉 log 含描述條件。對照規則只保留穩定識別條件，結果仍失敗；不能把字串失配當成已確認根因。
3. **Guest 缺少 MediaTek 驅動。** 尚不能證實或排除，但在實際 USB 裝置尚未進入 guest 前，安裝 guest 驅動不能算是已對準問題的修復。

目前可定位的失敗層是 host USB proxy／裝置重新列舉到 VM 附掛之間。沒有足夠證據區分控制器韌體行為、host stack 捕捉、或 VirtualBox 實作相容性。不要因為一般 hub 描述讀取錯誤就把未對應到目標的 port 錯誤當成根因。

## 精確單裝置重新列舉對照

使用者確認只有內建 MediaTek。進一步檢查最近的相關 Windows 系統事件，沒有取得足以定位根因的訊號；不把缺少事件當成不存在故障。

下一個有差異的實驗使用 Microsoft PnPUtil `/restart-device`，只傳入這顆複合 USB 裝置的精確 instance ID；執行前驗證 usbccgp 服務及唯一預期的 Bluetooth 子裝置。沒有使用 `/deviceid` 廣泛匹配、`/remove-device`、`/force`、`/reboot` 或重啟整個 hub。UAC 由操作者處理。

PnPUtil 回傳 **3010**，原始輸出表示要重新開機才可完成裝置重啟。它不是已完成的熱重啟，也不證明直通成功；runner 以非成功驗證結果停止，不繼續 guest 測試，最後移除暫時規則。主機 MediaTek 恢復檢查為 OK。沒有替使用者重開主機；Windows 回報的待重新開機條件仍須保留。不能宣稱再次重開就必然修好。

## 恢復與後續路徑

對照規則已在 finally 區段移除，沒有留下全域或全裝置自動捕捉規則。兩輪均未配對耳機、未開啟音訊播放、未改 host Secure Boot／BCD／Verifier，未重新啟動 USB hub。私有 VM／USB 原始 log 與控制器結果留在專用結果目錄。

後續先建立可實際進入 guest 的藍牙控制器路徑。使用者目前沒有其他藍牙介面，因此外接裝置對照未執行，也不要求購買。若仍使用內建控制器，需要更深入的 host USB 捕捉追蹤，不能重複要求重開機或跳過驗證直接安裝 LDAC 到日用主機。

## 一手參考

[Oracle VirtualBox 7.2 USB 說明](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/working-with-vms.html)分別說明虛擬 USB 控制器、裝置篩選條件與 Windows host proxy。它提供機制背景，沒有證明這顆 MediaTek 裝置可直通；相容性結論仍以本次觀察為準。

[Microsoft PnPUtil 命令說明](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax)提供精確 instance ID 與重啟旗標的定義。本次沒有使用自動重啟主機旗標。

## 最終安全狀態

最後 guest 的 Secure Boot／服務／憑證／BCD／Verifier 清潔檢查通過，正常關機後還原同一 clean snapshot 與原 USB 設定，VM 保持 poweroff。主機 MediaTek、XM6 音訊服務及複合裝置狀態均 OK，原 INF／服務／版本一致；配對未修改。PnPUtil 回報的需重新開機條件仍保留，不把狀態 OK 解讀成該操作已完成。

## 後續紀錄

第二次使用者重啟後的有界追蹤與結果另見 [USB ETW 實驗](USB_ETW_2026-11-06.md)。
