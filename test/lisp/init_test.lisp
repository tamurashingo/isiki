;;; src/lisp/init.lisp で定義したマクロ(let/let*/and/or/cond/for/while/with-open-input-stream)のテスト

;;; --- let ---

(assert-equal 3 (let ((a 1) (b 2)) (+ a b)))

;; letのbodyでのsetqは、そのletの変数だけに閉じて働く(新しいenvironmentに束縛されるため)
(assert-equal 10 (let ((x 5)) (setq x 10) x))

;;; --- let* ---

;; let*は逐次束縛なので、bの初期化式からaを参照できる(letでは不可)
(assert-equal 3 (let* ((a 1) (b (+ a 1)) (c (+ a b))) c))

;;; --- and ---

(defun should-not-be-called-and ()
  (assert-equal 0 1))

(assert-equal t (and))
(assert-equal 3 (and 1 2 3))
(assert-equal nil (and 1 nil (should-not-be-called-and)))

;;; --- or ---

(defun should-not-be-called-or ()
  (assert-equal 0 1))

(assert-equal nil (or))
(assert-equal 1 (or 1 (should-not-be-called-or)))
(assert-equal 2 (or nil 2 3))
(assert-equal nil (or nil nil))

;;; --- cond ---

(defun should-not-be-called-cond ()
  (assert-equal 0 1))

(assert-equal 2 (cond (nil (should-not-be-called-cond)) (t 2) (t 3)))
(assert-equal nil (cond (nil 1)))

;;; --- for ---

;; 1から5までの合計
(assert-equal 15 (for ((i 1 (+ i 1)) (sum 0 (+ sum i))) ((> i 5) sum)))

;; step式を省略した変数(found)は、body中のsetqが次のイテレーションにも
;; 正しく持ち越される(for自身の仮引数だから)ことを確認する
(assert-equal t
  (for ((i 0 (+ i 1)) (found nil)) ((= i 5) found)
    (if (= i 3) (setq found t) nil)))

;; body中のreturn-fromによる早期脱出
(assert-equal 99
  (for ((i 0 (+ i 1))) (nil i)
    (if (= i 3) (return-from nil 99) nil)))

;;; --- while ---

(defun should-not-be-called-while ()
  (assert-equal 0 1))

;; testがnilならbodyは一度も評価されない
(assert-equal nil (while nil (should-not-be-called-while)))

;; body中のreturn-fromによる早期脱出
(assert-equal 42 (while t (return-from nil 42)))

;;; --- with-open-input-stream ---

;; bodyの評価結果が返り、変数もbody内で参照できる
(assert-equal 42 (with-open-input-stream (s (open-output-stream)) 42))
(assert-equal t (with-open-input-stream (s (open-output-stream)) (if s t nil)))

;;; --- setf ---

;; placeがsymbolならsetqに展開される
(assert-equal 10 (let ((x 5)) (setf x 10) x))

;; (car x)/(cdr x)ならset-car/set-cdrに展開される
(assert-equal 99 (let ((c (cons 1 2))) (setf (car c) 99) (car c)))
(assert-equal 99 (let ((c (cons 1 2))) (setf (cdr c) 99) (cdr c)))

;; (aref a i1 i2 ...)ならset-arefに展開される
(assert-equal 42 (let ((a (make-array 3))) (setf (aref a 1) 42) (aref a 1)))
(assert-equal 7 (let ((a (make-array '(2 3)))) (setf (aref a 1 2) 7) (aref a 1 2)))
