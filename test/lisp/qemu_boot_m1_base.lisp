(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")
;; テストフレームワーク自身の検証(assert-error-class の陽性/陰性対照を含む)。
;; **道具が壊れていると全部の期待値が黙って無効になる**ので、何より先に走らせる
(load "test/lisp/test_framework_test.lisp")
(load "test/lisp/init_test.lisp")
;; [P2] トップレベルの打ち切り(environment の巻き戻し / with-environment の
;; 握り潰し解消 / report-condition の文字列)。init.lisp と条件システムが要る
(load "test/lisp/toplevel_abort_test.lisp")
;; [P3] with-handler の脱出先(ハンドラが正常 return したらその with-handler まで戻る)。
;; インタプリタ版と JIT 版の両方を見る
(load "test/lisp/handler_exit_test.lisp")
(load "test/lisp/isiki_test.lisp")
;; リーダー構文依存の例(#nA 等)は別ファイル。load の戻り値(成功なら t)で構文エラーによる
;; 中断を検出する(qemu_boot_test.lisp と同じ。assert-equal の中に load を直接書かない)
(defglobal *isiki-test-syntax-load-result* (load "test/lisp/isiki_test_syntax.lisp"))
(assert-equal t *isiki-test-syntax-load-result*)
;; 同じ仕様例を defun の本体にして JIT コンパイラに通す版(tools/gen_isiki_test_jit.py で生成)
(load "test/lisp/isiki_test_jit.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
