(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/bitwise_char_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
