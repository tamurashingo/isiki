;; [#197] **陽性対照。この段は落ちるのが正しい。**
;;
;; 読み込みの打ち切りが母艦に残るようになったことを、実機で確かめるための台本である。
;; **make test-qemu-all には入れていない**(落ちる段を 1 周に混ぜない)。
;;
;;     make test-qemu-milestone MILESTONE=test/lisp/qemu_boot_load_abort_positive.lisp
;;
;; 期待する結果:
;;   - 合否の一行は**通る**(検査は 1 件も落ちない)
;;   - そのあとで tools/check_load_abort_line.py が**落ちる**
;;     = 「合否の一行だけを見ていたら気づけなかった」ことの対照である
;;   - 直列線に [load] abort の行が**構文ごとに 1 本ずつ**出る
;;
;; **同時に構文ごとの実測を兼ねている。** 単体試験(test/c/load_test.c)では
;; 条件クラスを 1 つも登録していないので signal の経路が測れない。
;; C と I はここでしか測れない。結果の表は documents/load-abort-visibility.md §3。
;;
;; 行の順番が実測の手がかりである(load は上から順に 1 つずつ評価する)。
;; **数えてはいけない 3 つ(B / D / F)を対照群として同じ台本に入れてある。**

(load "test/lisp/test_framework.lisp")

;; 合否の一行を本物の集計で出すために、通る検査を 1 件置く
;; (0 passed の成果物は判定役が合格にしない。check_pass_line.py)
(assert-equal 2 (+ 1 1))

;; --- A: 最上位への中断(%abort-top-level が作るのと同じ形)。数えられる ---
(return-from %top-level 9)

;; --- B: block が自分で捕まえる。**数えてはいけない**(陰性対照) ---
(block b197 (return-from b197 1))

;; --- C: 宛先の block が生きていない。<control-error> の signal 経由で数えられる ---
(return-from no-such-block-197 1)

;; --- D: catch が自分で捕まえる。**数えてはいけない**(陰性対照) ---
(catch 'tg197 (throw 'tg197 1))

;; --- E: catch の無い throw。**調査 §C-2 の穴**(直す前は数えられなかった) ---
(throw 'tg197 1)

;; --- F: tagbody が自分のタグとして解決する。**数えてはいけない**(陰性対照) ---
(tagbody (go done197) (setq f197-never 1) done197)

;; --- G: tagbody の無い go。**調査が ★ を立てていた形** ---
(go nowhere197)

;; --- H: tagbody はあるが宛先のタグが無い。**同じく ★ だった形** ---
(tagbody (go nowhere197))

;; --- I: 素のエラー(引数不足)。signal 経由で数えられる ---
(car)

(isiki-test-report)
(close *isiki-test-stream*)
