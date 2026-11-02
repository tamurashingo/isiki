(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "src/lisp/bench_jit.lisp")
(load "test/lisp/gc_debug_guard.lisp")

(isiki-test-load "test/lisp/for_gc_stress_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
