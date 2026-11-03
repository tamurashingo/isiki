;; [#128 / M7] 型特化した比較(single)の試験。make test-qemu-compare-single から走る
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/compare_single_test.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
