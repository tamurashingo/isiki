(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "src/lisp/ide.lisp")
(load "test/lisp/ide_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
