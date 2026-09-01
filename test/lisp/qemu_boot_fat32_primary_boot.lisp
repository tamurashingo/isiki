(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
(load "src/lisp/device.lisp")
(load "src/lisp/ide.lisp")
(load "test/lisp/fat32_primary_boot_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
