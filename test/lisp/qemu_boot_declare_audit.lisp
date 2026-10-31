;; [D6] 監査ビルド専用。make test-qemu-declare-audit から走る
(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/declare_audit_test.lisp")
(isiki-test-report)
(close *isiki-test-stream*)
