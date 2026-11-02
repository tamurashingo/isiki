(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/jit_frame_test.lisp")
(isiki-test-load "test/lisp/jit_rsp_balance_test.lisp")
;; **この順でなければならない。** jit_mixed_frame_test.lisp は上の 2 つが置く
;; 読み手(%jit-frame-size / %jrb-fsz・%jrb-audit)と上限(jit-frame-size-cap)を使う
(isiki-test-load "test/lisp/jit_mixed_frame_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
