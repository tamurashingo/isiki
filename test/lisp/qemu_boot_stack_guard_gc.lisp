(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
;; **gc_debug_guard.lisp は素の load で読む。** このファイルは関数を 2 つ
;; 定義するだけで、assert を 1 つも持たない。isiki-test-load で読むと
;; isiki-file-end-check の「このファイルは検査を走らせたか(n > 0)」が
;; 必ず落ちる(2026-11-01 に実測。10 passed / 1 failed)。
;; **qemu_boot_for_gc_stress.lisp は最初から素の load で読んでいる。**
;; こちらだけが isiki-test-load になっていた。
;; このミルストンは test-qemu-all に入っていないので、誰も気づいていなかった
(load "test/lisp/gc_debug_guard.lisp")
(isiki-test-load "test/lisp/stack_guard_gc_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
