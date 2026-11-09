(load "test/lisp/test_framework.lisp")
;; device.lisp/ide.lispはM15でAOTトランスパイル対象に移動し、
;; os_register_aot_init_functions/os_run_aot_toplevel_forms経由で
;; ブート時に既にglobal_environmentへ登録・初期化済みのため、ここでの
;; loadは不要になった(このテストがAOT側の経路を実際に検証する)
;; **src/lisp のライブラリは素の load のままでよい**(試験ファイルではないので
;; アサーションを 1 件も持たない)。試験ファイルだけが isiki-test-load で読まれる
;; —— 証人の経路を 1 本に寄せた理由は test_framework.lisp の isiki-test-load にある
(load "src/lisp/partition.lisp")
(isiki-test-load "test/lisp/ide_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
