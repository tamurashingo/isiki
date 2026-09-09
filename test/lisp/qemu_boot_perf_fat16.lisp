(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
;; device.lisp/ide.lisp/partition.lisp/mount.lisp/fat16.lispはM15でAOT
;; トランスパイル対象に移動し、ブート時に既にglobal_environmentへ登録・
;; 初期化済みのため、ここでのloadは不要。
(load "test/lisp/perf_fat16_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
