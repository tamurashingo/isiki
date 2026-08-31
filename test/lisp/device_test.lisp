;; test/lisp/device_test.lisp
;;
;; src/lisp/device.lispの*devices*登録内容とdescribeの動作確認。
;; qemu_boot_m6_fat16.lispがFAT16フォーマット済みディスクをSecondary masterに
;; アタッチした状態で、device.lisp→ide.lisp→fat16.lisp→fat16_test.lispの後に
;; 本ファイルをloadする前提で書かれている(*devices*にblk0が登録済み、かつ
;; セクタ0がFAT16のBPBであることを利用してUUID検出の経路も確認する)。

;;; --- 1. *devices*にblk0が登録されている ---

(defglobal device-test-blk0-entry (assoc 'blk0 (dynamic *devices*)))

(assert-equal t (if device-test-blk0-entry t nil))

(defglobal device-test-blk0-info (cdr device-test-blk0-entry))

(assert-equal 'blk (%plist-get device-test-blk0-info ':type nil))
(assert-equal t (if (%plist-get device-test-blk0-info ':handle nil) t nil))
(assert-equal t (> (length (%plist-get device-test-blk0-info ':model "")) 0))
(assert-equal t (> (%plist-get device-test-blk0-info ':total-sectors 0) 0))

;;; --- 2. (describe 'blk0)はinfoプリストを返し、エラーにならない ---

(assert-equal device-test-blk0-info (describe 'blk0))

;;; --- 3. (describe '未登録のデバイス名)はnilを返し、エラーにならない ---

(assert-equal nil (describe 'nosuchdevice))

;;; --- 4. FAT16フォーマット済みディスクのため、blk0のUUIDはXXXX-XXXX形式で検出できる ---

(defglobal device-test-uuid (%device-fat16-uuid (%plist-get device-test-blk0-info ':handle nil)))

(assert-equal t (if device-test-uuid t nil))
(assert-equal 9 (length device-test-uuid))
(assert-equal 45 (char-code (string-elt device-test-uuid 4))) ; '-'
