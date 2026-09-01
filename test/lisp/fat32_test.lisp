;; test/lisp/fat32_test.lisp
;;
;; src/lisp/fat32.lisp(FAT32-M1: BPBパース)の動作確認。
;; QEMU側はMakefileの$(FAT32_DISK_IMG)ターゲット(mkfs.vfat -F 32でフォーマットした
;; 40MBイメージ)をQEMU_DISK_IMG経由でbus=1,unit=0にアタッチしている。
;; 期待値はホスト上でxxd -g1 tmp/fat32_test.imgのブートセクタを目視確認して得たもの
;; (mkfs.fat 4.2、40MBイメージのデフォルト値)。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal)とsrc/lisp/device.lisp+
;; src/lisp/ide.lisp(*devices*)とsrc/lisp/fat32.lisp(fat32-read-bpb)を、本ファイルより
;; 先にboot-entryスクリプトがloadしている前提で書かれている。

;; *devices*からblk0(Secondary IDEチャネルのデバイス)のハンドルを取り出す。
(defglobal *fat32-test-device* (%device-handle 'blk0))

(defglobal fat32-test-bpb (fat32-read-bpb *fat32-test-device*))

(assert-equal t (if fat32-test-bpb t nil))

(assert-equal 512 (slot-value fat32-test-bpb 'bytes-per-sector))
(assert-equal 1 (slot-value fat32-test-bpb 'sectors-per-cluster))
(assert-equal 32 (slot-value fat32-test-bpb 'reserved-sectors))
(assert-equal 2 (slot-value fat32-test-bpb 'num-fats))
(assert-equal 81920 (slot-value fat32-test-bpb 'total-sectors))
(assert-equal 630 (slot-value fat32-test-bpb 'fat-size-32))
(assert-equal 2 (slot-value fat32-test-bpb 'root-cluster))
