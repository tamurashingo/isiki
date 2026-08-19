(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/environment_pages_test.lisp")
(load "test/lisp/environment_literal_slots_test.lisp")
(load "test/lisp/environment_utilities_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
