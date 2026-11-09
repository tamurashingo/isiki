(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/environment_pages_test.lisp")
(isiki-test-load "test/lisp/environment_literal_slots_test.lisp")
(isiki-test-load "test/lisp/environment_utilities_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
