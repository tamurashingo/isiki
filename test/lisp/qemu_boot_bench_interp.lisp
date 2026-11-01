(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "src/lisp/bench_interp.lisp")
;; bench-jit-alloc-per-iter(1 反復あたりの確保量)を使うので読む。
;; **同じものを 2 箇所に書けば片方だけ直される**ので、JIT 側の計器を使い回す
(load "test/lisp/bench_jit_guard.lisp")
(load "test/lisp/bench_interp_guard.lisp")
(load "test/lisp/bench_interp_control.lisp")

(isiki-test-load "test/lisp/bench_interp_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
