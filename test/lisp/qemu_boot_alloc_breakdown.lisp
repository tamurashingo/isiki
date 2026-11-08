;; [計器] インタプリタ for の 1 反復あたりの確保の内訳(M8-T2)。
;; make test-qemu-alloc-breakdown から走る(監査ビルド専用)
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/alloc_breakdown.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
