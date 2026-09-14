(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/perf_read_fat16_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
