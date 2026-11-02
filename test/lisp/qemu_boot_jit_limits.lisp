(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/jit_limits_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
