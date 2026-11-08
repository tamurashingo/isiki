;;; defmacro で定義したマクロ(my-if)のテスト
;;; my-if が if と同じ動きをすることを確認する


(defmacro my-if (test then else)
  `(if ,test ,then ,else))

(assert-equal 10 (my-if 1 10 20))
(assert-equal 20 (my-if nil 10 20))

;; テストが呼び出されるまでelse/thenが評価されないことも合わせて確認する
(defun should-not-be-called ()
  (assert-equal 0 1))

(assert-equal 10 (my-if 1 10 (should-not-be-called)))
(assert-equal 20 (my-if nil (should-not-be-called) 20))

;;; &restとquasiquoteのunquote-splicingを使うマクロ(my-when)のテスト
;;; my-when が (if condition (progn body...) nil) と同じ動きをすることを確認する

(defmacro my-when (condition &rest body)
  `(if ,condition
       (progn ,@body)
       nil))

(assert-equal 3 (my-when 1 1 2 3))
(assert-equal nil (my-when nil (should-not-be-called)))

;;; [#142] quasiquote の中で非局所脱出したら、脱出の印を構造に組み込まないこと
;;;
;;; `(unquote x)` の x が return-from / throw / go / signal で脱出すると、
;;; 評価器は値ではなく**脱出の印**(非局所脱出シグナル)を返す。
;;; これを検査せずに cons すると、**脱出の印がリストの要素として埋め込まれる**。
;;; 埋め込まれた形は見た目がただのリストなので誰も気づかず、
;;; あとで遠くの場所で「形が壊れている」として現れる。
;;;
;;; 2026-11-08 に実際に踏んだ(#142): 深い入れ子のコンパイル中に
;;; JIT のスタック残量検査が <storage-exhausted> を signal し、
;;; ハンドラが %TOP-LEVEL へ脱出した印が qq_expand によって
;;; `((lambda <脱出の印>) ...)` に組み込まれ、JIT の仮引数の検査が当たっていた。
;;;
;;; **ここが通らないと、同じ壊れ方が静かに戻ってくる。**

;; (unquote x) の位置での脱出
(defun qq-nlx-unquote ()
  (block b `(1 ,(return-from b 42) 3)))
(assert-equal 42 (qq-nlx-unquote))

;; (unquote-splicing x) の位置での脱出
(defun qq-nlx-splicing ()
  (block b `(1 ,@(return-from b 43) 3)))
(assert-equal 43 (qq-nlx-splicing))

;; 入れ子のリストの中での脱出(再帰の戻り値の検査が効いていること)
(defun qq-nlx-nested ()
  (block b `((a ,(return-from b 44)))))
(assert-equal 44 (qq-nlx-nested))

;; 脱出の種類を変えても同じ(catch/throw)
(defun qq-nlx-throw ()
  (catch 'qq-nlx-tag `(1 ,(throw 'qq-nlx-tag 45) 3)))
(assert-equal 45 (qq-nlx-throw))

;; 脱出しない通常の経路が壊れていないこと(陰性対照)
(defun qq-nlx-normal ()
  (block b `(1 ,(+ 1 1) 3)))
(assert-equal 3 (length (qq-nlx-normal)))
(assert-equal 2 (elt (qq-nlx-normal) 1))
