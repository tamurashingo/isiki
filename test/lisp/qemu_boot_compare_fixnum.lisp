;; [#128 / M7] 型特化した比較(fixnum)の試験。make test-qemu-compare-fixnum から走る
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/compare_fixnum_test.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
