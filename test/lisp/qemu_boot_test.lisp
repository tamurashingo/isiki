(load "src/lisp/init.lisp")
(load "test/lisp/isiki_test.lisp")
(load "test/lisp/za_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
