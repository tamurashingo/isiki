;; test/lisp/perf_read_fat32_test.lisp
;;
;; [性能測定] perf_read_fat16_test.lispのFAT32版(コメントはそちら参照)。
;; ホスト側で事前にmkfs.vfat -F 32+mount+cpしたPERF_READ_FAT32_IMG
;; (READ100K.BIN/READ1M.BIN/READ2M.BIN)を読み込むだけで、書き込みを一切
;; 行わない。

(defglobal *perf-test-device* (%device-handle 'blk0))
(mount "/mnt" 'blk0 ':fat32)

(defun %%perf-read-check (path expected-len)
  (let ((t0 (get-internal-real-time)) (v nil) (t1 nil))
    (setq v (read-file-into-vector path))
    (setq t1 (get-internal-real-time))
    (assert-equal t (if v t nil))
    (assert-equal expected-len (length v))
    (assert-equal (mod 0 256) (elt v 0))
    (assert-equal (mod (div expected-len 2) 256) (elt v (div expected-len 2)))
    (assert-equal (mod (- expected-len 1) 256) (elt v (- expected-len 1)))
    (/ (- t1 t0) (internal-time-units-per-second))))

(defglobal perf-read-100k-seconds (%%perf-read-check "/mnt/READ100K.BIN" 100000))
(format *isiki-test-stream* "[読み込みのみ] READ100K.BIN(100000 byte)の読み込みに約~A秒かかった~%"
        perf-read-100k-seconds)

(defglobal perf-read-1m-seconds (%%perf-read-check "/mnt/READ1M.BIN" 1000000))
(format *isiki-test-stream* "[読み込みのみ] READ1M.BIN(1000000 byte)の読み込みに約~A秒かかった~%"
        perf-read-1m-seconds)

(defglobal perf-read-2m-seconds (%%perf-read-check "/mnt/READ2M.BIN" 2000000))
(format *isiki-test-stream* "[読み込みのみ] READ2M.BIN(2000000 byte)の読み込みに約~A秒かかった~%"
        perf-read-2m-seconds)

;; perf_fat32_test.lispの元の閾値(500秒/2MB)を踏襲するが、書き込みを含まない
;; 分明確に速いはずなので同じ上限で十分に安全
(assert-equal t (< perf-read-2m-seconds 500))
