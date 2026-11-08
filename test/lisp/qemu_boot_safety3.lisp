;; [#148] safety 3 で宣言した型を検証する試験。make test-qemu-safety3 から走る
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/safety3_test.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
