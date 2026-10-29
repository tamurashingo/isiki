(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/gc_debug_guard.lisp")
(isiki-test-load "test/lisp/stack_guard_gc_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
