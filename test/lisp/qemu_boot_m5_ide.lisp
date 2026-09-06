(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
;; device.lisp/ide.lispはM15でAOTトランスパイル対象に移動し、
;; os_register_aot_init_functions/os_run_aot_toplevel_forms経由で
;; ブート時に既にglobal_environmentへ登録・初期化済みのため、ここでの
;; loadは不要になった(このテストがAOT側の経路を実際に検証する)
(load "src/lisp/partition.lisp")
(load "test/lisp/ide_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
