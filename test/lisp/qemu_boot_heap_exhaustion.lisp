;; [可用性/#110] ヒープ枯渇が <storage-exhausted> になることを見る milestone。
;; make test-qemu-heap-exhaustion。設計は documents/heap-exhaustion.md。
;;
;; **通しの試験(qemu_boot_test.lisp)に入れていない。** 枯渇の試験が万一ヒープを
;; 壊した場合に、5000 件超ある通しの試験を道連れにしないため
;; (スタックガードの qemu_boot_stack_guard.lisp と同じ扱い)。
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/heap_exhaustion_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
