;; test/lisp/fat16_test.lisp
;;
;; src/lisp/fat16.lisp(FAT16-M1: BPBパース)の動作確認。
;; QEMU側はMakefileの$(FAT16_DISK_IMG)ターゲット(mkfs.vfat -F 16でフォーマットした
;; 16MBイメージ)をQEMU_DISK_IMG経由でbus=1,unit=0にアタッチしている。
;; 期待値はホスト上でxxd -g1 tmp/fat16_test.imgのセクタ0を目視確認して得たもの
;; (mkfs.fat 4.2、16MBイメージのデフォルト値)。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal)とsrc/lisp/ide.lisp
;; (*ide-device*)とsrc/lisp/fat16.lisp(fat16-read-bpb)を、本ファイルより先に
;; boot-entryスクリプトがloadしている前提で書かれている。

(defglobal fat16-test-bpb (fat16-read-bpb *ide-device*))

(assert-equal t (if fat16-test-bpb t nil))

(assert-equal 512 (slot-value fat16-test-bpb 'bytes-per-sector))
(assert-equal 4 (slot-value fat16-test-bpb 'sectors-per-cluster))
(assert-equal 4 (slot-value fat16-test-bpb 'reserved-sectors))
(assert-equal 2 (slot-value fat16-test-bpb 'num-fats))
(assert-equal 512 (slot-value fat16-test-bpb 'root-entry-count))
(assert-equal 32768 (slot-value fat16-test-bpb 'total-sectors))
(assert-equal 32 (slot-value fat16-test-bpb 'sectors-per-fat))
