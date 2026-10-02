(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "src/lisp/bench_jit.lisp")
(load "test/lisp/bench_jit_guard.lisp")
(load "test/lisp/bench_jit_eval_control.lisp")

(isiki-test-load "test/lisp/bench_jit_test.lisp")
(isiki-test-load "test/lisp/bench_jit_guard_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
