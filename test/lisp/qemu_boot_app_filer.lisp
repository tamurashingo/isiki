;; [#217] **初期アプリ: ファイラ**の段。
;;
;; 2 本を**この順で**読む。順序に意味がある:
;;
;;   1. filer_app_disk_test.lisp … アプリ用の円盤(/app)を割り当てて
;;      **そこから apps/filer.lisp を読み込み**、実機の画面で動かす。
;;      「読み込む前は未定義」という陽性対照を持つので、**先に走らなければならない**。
;;      **格子を `term-read-cell` で読み返す検査もここにしかない**
;;      (位置指定が効いていることは文字列ストリームでは言えない)
;;   2. filer_test.lisp … 核(filer-run)を**文字列入力ストリームで**試験する。
;;      一覧・上下の移動・入る/戻る・ページャで開く・断る場合はここ。
;;      ホスト共有から読み直すので、/app 側の定義には依らない
;;
;; device.lisp/ide.lisp/partition.lisp/mount.lisp/fat32.lisp/file-cmd.lisp は
;; M15 で AOT 対象へ移っており、ブート時に登録・初期化済みなので load は不要
;; (test/lisp/qemu_boot_app_pager.lisp と同じ)。
;; アプリのディスクは Makefile が **どの QEMU 起動にも** Primary の slave として
;; 付ける(QEMU_DISK_IMG の上書きは要らない)。
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/filer_app_disk_test.lisp")
(isiki-test-load "test/lisp/filer_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
