(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
;; device.lisp/ide.lisp/partition.lisp/fat32.lispはM15でAOTトランスパイル対象に
;; 移動し、ブート時に既にglobal_environmentへ登録・初期化済みのため、ここでの
;; loadは不要になった(このテストがAOT側の経路を実際に検証する)
(load "test/lisp/fat32_primary_boot_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
