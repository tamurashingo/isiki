(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/read_file_native_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
