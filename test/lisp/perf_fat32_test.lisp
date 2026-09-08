;; test/lisp/perf_fat32_test.lisp
;;
;; [ファイルI/O]#52(M9): perf_fat16_test.lispのFAT32版(コメントはそちら参照)。

(defglobal *perf-test-device* (%device-handle 'blk0))
(mount "/mnt" 'blk0 ':fat32)

;;; --- #39: 65536byte超の書き込み→再読み込み一致 ---
(defun %%perf-write-n-chars (stream ch n)
  (let ((i 0))
    (while (< i n)
      (progn
        (write-char ch stream)
        (setq i (+ i 1))))))

(defglobal perf-bigwrite-len 100000)
(defglobal perf-bigwrite-stream (open-output-file "/mnt/PERFBIG.TXT"))
(assert-equal t (if perf-bigwrite-stream t nil))
(%%perf-write-n-chars perf-bigwrite-stream #\Z perf-bigwrite-len)
(close perf-bigwrite-stream)

(defglobal perf-bigwrite-after (read-file-into-vector "/mnt/PERFBIG.TXT"))
(assert-equal t (if perf-bigwrite-after t nil))
(assert-equal perf-bigwrite-len (length perf-bigwrite-after))
(assert-equal 90 (elt perf-bigwrite-after 0))
(assert-equal 90 (elt perf-bigwrite-after (- perf-bigwrite-len 1)))

;;; --- #41: カーネル自身のブートバイナリ(約1.76MB)の読み込み性能 ---
(defglobal perf-kernel-bytes (read-file-into-vector "/9p/esp_dir/EFI/BOOT/BOOTX64.EFI"))
(assert-equal t (if perf-kernel-bytes t nil))
(defglobal perf-kernel-len (length perf-kernel-bytes))
(assert-equal t (> perf-kernel-len 1000000))

(assert-equal t (if (fat32-create-file *perf-test-device* "/KERNEL.BIN" perf-kernel-bytes) t nil))

(defglobal perf-read-t0 (get-internal-real-time))
(defglobal perf-kernel-readback (read-file-into-vector "/mnt/KERNEL.BIN"))
(defglobal perf-read-t1 (get-internal-real-time))
(assert-equal t (if perf-kernel-readback t nil))
(assert-equal perf-kernel-len (length perf-kernel-readback))
(assert-equal perf-kernel-bytes perf-kernel-readback)

(defglobal perf-read-seconds
  (/ (- perf-read-t1 perf-read-t0) (internal-time-units-per-second)))
(format *isiki-test-stream* "#41: KERNEL.BIN(~A byte)の読み込みに約~A秒かかった~%"
        perf-kernel-len perf-read-seconds)
;; 閾値の根拠はperf_fat16_test.lisp参照。FAT16実測(約2.0MBで約1063秒)に
;; M8で確認済みのFAT16/FAT32比(1.2MBで約14分/約25分、約1.79倍)を掛けた
;; 見込み値(約1900秒)へ安全マージンを載せ、3000秒とする。
(assert-equal t (< perf-read-seconds 3000))
