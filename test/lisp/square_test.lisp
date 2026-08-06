;;; defun で定義した関数のテスト


(defun multiply (a b)
  (if (eq b 0)
      0
      (+ a (multiply a (- b 1)))))

(defun square (x)
  (multiply x x))

(assert-equal 0 (square 0))
(assert-equal 1 (square 1))
(assert-equal 4 (square 2))
(assert-equal 9 (square 3))
(assert-equal 100 (square 10))
;; (assert-equal 10 (square 3))
