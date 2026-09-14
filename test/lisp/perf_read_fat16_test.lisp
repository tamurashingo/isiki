;; test/lisp/perf_read_fat16_test.lisp
;;
;; [性能測定] test-qemu-perf(perf_fat16_test.lisp)は#39(書き込み)と#41(読み込み)
;; を1マイルストーンに同居させており、documents/performance-measurement.mdの
;; 調査で判明した「書き込みには未解明の遅さ・再現する劣化がある」ノイズが
;; 読み込み計測に混入してしまう。読み込み単体の性能を追うにはこの2つを分離
;; する必要があるため、書き込みを一切行わない専用マイルストーンとして新設した。
;;
;; テストデータはゲスト内で生成・書き込みするのではなく、Makefile側で
;; ホストがmkfs.vfat+mount+cpして「あらかじめFAT16イメージに焼き込んだ」
;; ファイル(PERF_READ_FAT16_IMG、READ100K.BIN/READ1M.BIN/READ2M.BIN)を
;; そのままread-file-into-vectorで読むだけにする。これによりゲスト内での
;; 書き込みコスト・JITでのテストデータ生成コストの両方をゼロにでき、
;; read-file-into-vector単体の命令数・実時間だけを計測できる
;; (documents/performance-measurement.mdで確立した「ホスト側事前書き込み」手法の
;; 恒久化)。データパターンはホスト側のpython3生成と同じ「i mod 256の反復」
;; (i=0から始まる)なので、ゲスト側では期待値をmodで再計算して照合する。

(defglobal *perf-test-device* (%device-handle 'blk0))
(mount "/mnt" 'blk0 ':fat16)

;; (%%perf-read-check path expected-len) : pathをread-file-into-vectorで読み、
;; 長さとi mod 256パターン(先頭・中間・末尾)を検証し、所要秒数を返す。
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

;; #41の元の閾値(perf_fat16_test.lisp、300秒/2MB)を踏襲するが、このテストは
;; 書き込みを含まない分明確に速いはずなので同じ上限で十分に安全
(assert-equal t (< perf-read-2m-seconds 300))
