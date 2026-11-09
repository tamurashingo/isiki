;; [GCデバッグ / M2-T0 / #196] 記憶域の整理を強制した状態で、ディスク経由の
;; 読み取りと読み込みが壊れないことを確かめる専用のブート。
;; **GC_DEBUG=1 で走らせること**(test/lisp/gc_debug_guard.lisp が落とす)。
;; データディスクは FAT16 の試験用イメージを使う(Makefile の
;; test-qemu-load-gc-stress が QEMU_DISK_IMG で渡す)。
(load "test/lisp/test_framework.lisp")
(load "test/lisp/gc_debug_guard.lisp")

(isiki-test-load "test/lisp/load_gc_stress_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
