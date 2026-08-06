;;; &rest 仮引数のテスト


(defun sum-list (xs)
  (if (null xs)
      0
      (+ (car xs) (sum-list (cdr xs)))))

(defun sum-all (&rest xs)
  (sum-list xs))

(assert-equal 6 (sum-all 1 2 3))
(assert-equal 0 (sum-all))

;; 固定の仮引数と&restを併用できることの確認
(defun first-and-rest-sum (first &rest rest)
  (+ first (sum-list rest)))

(assert-equal 6 (first-and-rest-sum 1 2 3))
(assert-equal 1 (first-and-rest-sum 1))
