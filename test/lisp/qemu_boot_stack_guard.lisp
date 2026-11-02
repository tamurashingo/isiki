(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/stack_guard_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
