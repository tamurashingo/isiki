(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/za_test_ext5.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
