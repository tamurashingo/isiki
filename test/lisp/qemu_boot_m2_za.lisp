(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "test/lisp/za_test.lisp")
(load "test/lisp/fn_cell_cache_test.lisp")
(load "test/lisp/frame_definition_test.lisp")
(load "test/lisp/frame_variable_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
