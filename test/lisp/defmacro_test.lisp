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
