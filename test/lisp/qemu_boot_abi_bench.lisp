(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/abi_bench.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
