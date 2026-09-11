;; test/lisp/perf_fat32_test.lisp
;;
;; [ファイルI/O]#52(M9): perf_fat16_test.lispのFAT32版(コメントはそちら参照)。
;;
;; [ABI刷新] 2026-09-10改訂: perf_fat16_test.lispと同じ理由で、テストデータを
;; 9P経由のホストファイル読み込みから、JITコンパイルされるwhileループでの
;; その場生成(%%perf-make-fill-vector)へ変更した。9Pはベアメタル実機では
;; 使われない暫定的な経路であり、このテストが計測すべきはFAT32自体の読み書き
;; 性能のみ。

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

;;; --- #41: 大きなファイル(旧カーネルバイナリ相当、約2MB)の読み込み性能 ---
;; (%%perf-make-fill-vector n) : perf_fat16_test.lispと同じ(コメントはそちら参照。
;; modを避けたincrement-and-wrap方式)
(defun %%perf-make-fill-vector (n)
  (let ((v (create-vector n 0)) (i 0) (b 0))
    (while (< i n)
      (progn
        (set-elt b v i)
        (setq b (if (>= b 255) 0 (+ b 1)))
        (setq i (+ i 1))))
    v))

(defglobal perf-kernel-len 2000000)
(assert-equal t (> perf-kernel-len 1000000))

(defglobal perf-fill-t0 (get-internal-real-time))
(defglobal perf-kernel-bytes (%%perf-make-fill-vector perf-kernel-len))
(defglobal perf-fill-t1 (get-internal-real-time))
(assert-equal t (%%za-compiled-p (function %%perf-make-fill-vector)))
(assert-equal perf-kernel-len (length perf-kernel-bytes))
(format *isiki-test-stream* "[参考] テストデータ~A byteの生成(JIT、FAT32とは無関係)に約~A秒かかった~%"
        perf-kernel-len (/ (- perf-fill-t1 perf-fill-t0) (internal-time-units-per-second)))

(defglobal perf-write-t0 (get-internal-real-time))
(assert-equal t (if (fat32-create-file *perf-test-device* "/KERNEL.BIN" perf-kernel-bytes) t nil))
(defglobal perf-write-t1 (get-internal-real-time))
(format *isiki-test-stream* "[参考] fat32-create-fileでの~A byte一括書き込みに約~A秒かかった~%"
        perf-kernel-len (/ (- perf-write-t1 perf-write-t0) (internal-time-units-per-second)))

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
;; [ABI刷新] 2026-09-10改訂: perf_fat16_test.lispと同じ理由で閾値を更新。
;; 旧閾値(FAT16実測1063秒にFAT16/FAT32比1.79倍を掛けた3000秒)は
;; B'/A/B適用後の実測には大幅に保守的すぎるため縮小する。
(assert-equal t (< perf-read-seconds 500))
