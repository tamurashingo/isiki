;; [#128 / M7-T1] `*` `/` のインライン展開の試験。make test-qemu-inline-muldiv から走る
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/inline_muldiv_test.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
