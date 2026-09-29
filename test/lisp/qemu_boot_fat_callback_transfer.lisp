;; [P1] C→Lisp 呼び戻しの非局所脱出検査。**write-from! / read-into! を差し替える**ので
;; 専用のブートで単独に走らせる(他のテストと混ぜない)。
;; QEMU_DISK_IMG には FAT16 イメージ(tmp/fat16_test.img)を渡すこと。
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/fat_callback_transfer_test.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
