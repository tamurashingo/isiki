;; [#216] **端末の土台**の段(カーソル位置指定とキー 1 つ待ち)。
;;
;; 1 本だけ読む: test/lisp/term_test.lisp。
;;
;; **なぜ段を分けたのか。** ページャの段(qemu_boot_app_pager.lisp)に混ぜず
;; 独立させたのは、落ちたときの切り分けを短くするためである:
;;   - ここが落ちたら「土台の口(位置指定・1 マス読み・行数・素の入力)」の問題
;;   - ページャの段が落ちたら「アプリ側」の問題
;; #216 はページャを 1 打鍵で動かす変更も含むので、両方が同時に落ちうる。
;; そのとき原因がどちらなのかを**段の名前で**言えるようにしてある。
;;
;; **ディスクは要らない。** term_test.lisp はホスト共有から apps/pager.lisp と
;; apps/inspect.lisp を読む(既定値が実画面に収まることを見るため)。
;; アプリ専用の円盤から読む側はページャ・inspect のそれぞれの段にある。
;;
;; device.lisp/ide.lisp/partition.lisp/mount.lisp/fat32.lisp/file-cmd.lisp は
;; M15 で AOT 対象へ移っており、ブート時に登録・初期化済みなので load は不要。
;;
;; **母艦側で守っているのは別の 2 本である**(こちらは実画面が要る分だけ):
;;   test/c/framebuffer_test.c … 位置指定・読み出し・消去の境界条件
;;   test/c/keymap_test.c      … キーの取り扱い(#216 まで 1 件も無かった)
(load "test/lisp/test_framework.lisp")

(isiki-test-load "test/lisp/term_test.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
