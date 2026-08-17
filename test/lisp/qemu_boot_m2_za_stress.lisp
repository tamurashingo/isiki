(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/za_test.lisp")
(load "test/lisp/za_test_stress.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
