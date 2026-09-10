;; test/lisp/perf_fat16_test.lisp
;;
;; [ファイルI/O]#52(M9): 性能・切り詰め検証。QEMU(TCG、KVM無し)環境では
;; 1MB超のファイルI/Oが現実的な時間で終わらないため、CI向けのqemu_boot_m6_
;; fat16.lisp(timeout-minutes: 10)には含めず、ローカル専用マイルストーン
;; (qemu_boot_perf_fat16.lisp、test-qemu-perfターゲット、test-qemu-stressと
;; 同じ位置づけ)として独立させた。
;;
;; #41(カーネルバイナリ約1.76MBの読み込みが遅い)の解消確認と、#39(65536byte
;; 超の書き込みが無音で切り詰められる)の解消確認を1つのマイルストーンにまとめる。
;;
;; [ABI刷新] 2026-09-10改訂: 以前は#41のテストデータを9P経由(/9p、ホスト上の
;; 実ビルド成果物esp_dir/EFI/BOOT/BOOTX64.EFI)から読んでいたが、9Pは
;; ベアメタル実機では使われない暫定的なファイル読み込み経路であり、9P自体が
;; 遅くてもこのテストの評価対象ではない(9Pの遅さはこのテストの合否・所要
;; 時間に影響すべきでない)。かつ9P経由の読み込み自体がこのマイルストーンの
;; 完走時間を大きく引き延ばしていた。そのため、テストデータは9Pを一切使わず
;; JITコンパイルされるwhileループ(%%perf-make-fill-vector)でその場生成する
;; ことにし、このテストがFAT16自体の読み書き性能だけを計測するようにした。
(defglobal *perf-test-device* (%device-handle 'blk0))
(mount "/mnt" 'blk0 ':fat16)

;;; --- #39: 65536byte超の書き込み→再読み込み一致 ---
;; M6のBIGWR.TXT(fat16_test.lisp、70000byte)で既に検証済みだが、#52のDoD
;; 通り専用のマイルストーンとしても独立に確認する。write-charループはza.cで
;; JITコンパイルされるようdefunにする(fat16_test.lispのBIGWRITEテストと
;; 同じ理由、トップレベルのwhileは非JITのツリーウォーク評価になり大量反復で
;; スタックを壊しうる)。
(defun %%perf-write-n-chars (stream ch n)
  (let ((i 0))
    (while (< i n)
      (progn
        (write-char ch stream)
        (setq i (+ i 1))))))

(defglobal perf-bigwrite-len 100000) ;; 65536byteの旧上限を明確に超える
(defglobal perf-bigwrite-stream (open-output-file "/mnt/PERFBIG.TXT"))
(assert-equal t (if perf-bigwrite-stream t nil))
;; [ABI刷新] 2026-09-10追記: この1文字ずつのwrite-charループ(バッファが
;; write_buf一杯になるたび=約98回、flush_write_buf_fat経由でwrite-from!
;; メソッドを呼びFATクラスタを拡張する)は、fat16-create-file(1回で全体を
;; 書く一括API、#41側で使用)より本質的に呼び出し回数が多く、このマイルストーン
;; 全体の完走時間に無視できない影響がある。参考値として計測する
(defglobal perf-bigwrite-t0 (get-internal-real-time))
(%%perf-write-n-chars perf-bigwrite-stream #\Z perf-bigwrite-len)
(close perf-bigwrite-stream)
(defglobal perf-bigwrite-t1 (get-internal-real-time))
(format *isiki-test-stream* "[参考] write-charでの1文字ずつの~A byte書き込みに約~A秒かかった~%"
        perf-bigwrite-len (/ (- perf-bigwrite-t1 perf-bigwrite-t0) (internal-time-units-per-second)))

(defglobal perf-bigwrite-after (read-file-into-vector "/mnt/PERFBIG.TXT"))
(assert-equal t (if perf-bigwrite-after t nil))
(assert-equal perf-bigwrite-len (length perf-bigwrite-after))
(assert-equal 90 (elt perf-bigwrite-after 0))
(assert-equal 90 (elt perf-bigwrite-after (- perf-bigwrite-len 1)))

;;; --- #41: 大きなファイル(旧カーネルバイナリ相当、約2MB)の読み込み性能 ---
;; (%%perf-make-fill-vector n) : 0〜255を繰り返すn byteのgeneral-vectorをその場で
;; 作る。JITコンパイルされるようdefunにする(%%perf-write-n-charsと同じ理由)。
;; 9Pを介さずFAT16の読み書き性能だけを計測するためのテストデータ生成。
;; [ABI刷新] 2026-09-10改訂: 当初(mod i 256)で0〜255を循環させていたが、modは
;; za.cのfixnum高速pathの対象外(+/-のみインライン化されている)のため、2,000,000
;; 反復のうち大半がこの生成ループ自体のコストになってしまっていた(実測57.44秒)。
;; 同じ0〜255の循環パターンを、インライン化済みの+と比較(>=)だけで組み立てる
;; increment-and-wrap方式に変更し、23.32秒まで短縮した(生成データはFAT16の
;; 測定対象ではないため、意図した循環パターン自体は変えずコストだけ削る)
(defun %%perf-make-fill-vector (n)
  (let ((v (create-vector n 0)) (i 0) (b 0))
    (while (< i n)
      (progn
        (set-elt b v i)
        (setq b (if (>= b 255) 0 (+ b 1)))
        (setq i (+ i 1))))
    v))

(defglobal perf-kernel-len 2000000) ;; 旧カーネルバイナリ(約1.76〜2.8MB)と同程度の桁数
(assert-equal t (> perf-kernel-len 1000000))

(defglobal perf-fill-t0 (get-internal-real-time))
(defglobal perf-kernel-bytes (%%perf-make-fill-vector perf-kernel-len))
(defglobal perf-fill-t1 (get-internal-real-time))
(assert-equal t (%%za-compiled-p (function %%perf-make-fill-vector)))
(assert-equal perf-kernel-len (length perf-kernel-bytes))
(format *isiki-test-stream* "[参考] テストデータ~A byteの生成(JIT、FAT16とは無関係)に約~A秒かかった~%"
        perf-kernel-len (/ (- perf-fill-t1 perf-fill-t0) (internal-time-units-per-second)))

(defglobal perf-write-t0 (get-internal-real-time))
(assert-equal t (if (fat16-create-file *perf-test-device* "/KERNEL.BIN" perf-kernel-bytes) t nil))
(defglobal perf-write-t1 (get-internal-real-time))
(format *isiki-test-stream* "[参考] fat16-create-fileでの~A byte一括書き込みに約~A秒かかった~%"
        perf-kernel-len (/ (- perf-write-t1 perf-write-t0) (internal-time-units-per-second)))

(defglobal perf-read-t0 (get-internal-real-time))
(defglobal perf-kernel-readback (read-file-into-vector "/mnt/KERNEL.BIN"))
(defglobal perf-read-t1 (get-internal-real-time))
(assert-equal t (if perf-kernel-readback t nil))
(assert-equal perf-kernel-len (length perf-kernel-readback))
(assert-equal t (equal perf-kernel-bytes perf-kernel-readback))

(defglobal perf-read-seconds
  (/ (- perf-read-t1 perf-read-t0) (internal-time-units-per-second)))
(format *isiki-test-stream* "#41: KERNEL.BIN(~A byte)の読み込みに約~A秒かかった~%"
        perf-kernel-len perf-read-seconds)
;; [ABI刷新] 2026-09-10改訂: os_make_symbolの線形走査除去(B')・AOT whileループの
;; go直接goto化(A)・quoteシンボル/クロージャ捕捉名のコールサイト単位キャッシュ化
;; (B)により、read-file-into-vectorのAOT whileループ自体のコストは
;; documents/abi-redesign.md 2026-09-10調査の通り約17分の1に短縮された。
;; 旧実測(本サンドボックス、KVM無しTCG、9P経由2.0MBで約1063秒≒583秒/MB)を
;; そのまま踏襲していた旧閾値(1800秒)は大幅に保守的すぎるため、新しい安全
;; マージン付きの値に更新する。絶対時間の根拠は上のformat出力
;; (test-results.txt経由)で確認する。
(assert-equal t (< perf-read-seconds 300))
