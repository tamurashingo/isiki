(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "src/lisp/ide.lisp")
(load "src/lisp/fat16.lisp")
(load "test/lisp/fat16_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
