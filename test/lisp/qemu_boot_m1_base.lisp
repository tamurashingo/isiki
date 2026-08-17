(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/init_test.lisp")
(load "test/lisp/isiki_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
