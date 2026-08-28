(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/bitwise_char_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
