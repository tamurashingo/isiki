(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
;; device.lisp/ide.lisp/partition.lisp/mount.lisp/fat16.lisp/fat32.lisp/
;; file-cmd.lispはM15でAOTトランスパイル対象に移動し、ブート時に既に
;; global_environmentへ登録・初期化済みのため、ここでのloadは不要になった
;; (このテストがAOT側の経路を実際に検証する)
(load "test/lisp/partition_test.lisp")
(load "test/lisp/file_cmd_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
