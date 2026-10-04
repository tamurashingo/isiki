(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/jit_frame_test.lisp")
(isiki-test-load "test/lisp/jit_rsp_balance_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
