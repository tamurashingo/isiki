;; [計器] 確保の理由別の数え上げ(M8-T1)専用。make test-qemu-alloc-audit から走る
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/alloc_reason_test.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
