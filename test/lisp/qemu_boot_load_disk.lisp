;; [M2-T3 / #195] ディスク(アプリ専用の FAT32)に置いた `.lisp` を load する段。
;; device.lisp/ide.lisp/partition.lisp/mount.lisp/fat32.lisp/file-cmd.lisp は
;; M15 で AOT 対象へ移っており、ブート時に登録・初期化済みなので load は不要
;; (この試験が AOT 側の経路も実際に通る)。
;; アプリのディスクは Makefile が **どの QEMU 起動にも** Primary の slave として
;; 付ける(QEMU_DISK_IMG の上書きは要らない)。
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/load_disk_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
