;; [#215] **初期アプリ: ページャ**の段。
;;
;; 2 本を**この順で**読む。順序に意味がある:
;;
;;   1. pager_app_disk_test.lisp … アプリ用の円盤(/app)を割り当てて
;;      **そこから apps/pager.lisp を読み込み**、実機で動かす。
;;      「読み込む前は未定義」という陽性対照を持つので、**先に走らなければならない**
;;   2. pager_test.lisp … 核(pager-run)を**文字列入力ストリームで**試験する。
;;      5 つの対照(空 / 1 行 / 頁ちょうど / 頁 + 1 行 / 打ち切り)はここ。
;;      ホスト共有から読み直すので、/app 側の定義には依らない
;;
;; device.lisp/ide.lisp/partition.lisp/mount.lisp/fat32.lisp/file-cmd.lisp は
;; M15 で AOT 対象へ移っており、ブート時に登録・初期化済みなので load は不要
;; (test/lisp/qemu_boot_load_disk.lisp と同じ)。
;; アプリのディスクは Makefile が **どの QEMU 起動にも** Primary の slave として
;; 付ける(QEMU_DISK_IMG の上書きは要らない)。
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/pager_app_disk_test.lisp")
(isiki-test-load "test/lisp/pager_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
