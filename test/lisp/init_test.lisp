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

;;; --- mapcar / mapc / mapcan ---

(defun %add1 (x) (+ x 1))

(assert-equal 2 (car (mapcar #'%add1 (cons 1 (cons 2 (cons 3 nil))))))
(assert-equal 3 (car (cdr (mapcar #'%add1 (cons 1 (cons 2 (cons 3 nil)))))))
(assert-equal 4 (car (cdr (cdr (mapcar #'%add1 (cons 1 (cons 2 (cons 3 nil))))))))
(assert-equal nil (mapcar #'%add1 nil))

;; mapcはfnを副作用目的で呼ぶ。lambdaがletで束縛したaccを閉じ込め(クロージャ)、
;; set-arefでaccの内容を書き換えることで、呼び出しごとの副作用を検証する
(assert-equal 6
  (let ((acc (make-array 1)))
    (setf (aref acc 0) 0)
    (mapc (lambda (x) (setf (aref acc 0) (+ (aref acc 0) x)))
          (cons 1 (cons 2 (cons 3 nil))))
    (aref acc 0)))

;; mapcは(副作用の後)元のlist自身を返す
(assert-equal 1 (car (mapc #'%add1 (cons 1 (cons 2 nil)))))

(defun %dup2 (x) (cons x (cons x nil)))

(assert-equal 1 (car (mapcan #'%dup2 (cons 1 (cons 2 nil)))))
(assert-equal 1 (car (cdr (mapcan #'%dup2 (cons 1 (cons 2 nil))))))
(assert-equal 2 (car (cdr (cdr (mapcan #'%dup2 (cons 1 (cons 2 nil)))))))
(assert-equal 2 (car (cdr (cdr (cdr (mapcan #'%dup2 (cons 1 (cons 2 nil))))))))
(assert-equal nil (mapcan #'%dup2 nil))

;;; --- member / assoc ---

(assert-equal 2 (car (member 2 (cons 1 (cons 2 (cons 3 nil))))))
(assert-equal nil (member 4 (cons 1 (cons 2 (cons 3 nil)))))

(assert-equal 2 (cdr (assoc 'b (cons (cons 'a 1) (cons (cons 'b 2) nil)))))
(assert-equal nil (assoc 'z (cons (cons 'a 1) nil)))

;;; --- append / reverse ---

(assert-equal 1 (car (append (cons 1 nil) (cons 2 nil))))
(assert-equal 2 (car (cdr (append (cons 1 nil) (cons 2 nil)))))
(assert-equal 1 (car (append (cons 1 nil))))
(assert-equal nil (append))

(assert-equal 3 (car (reverse (cons 1 (cons 2 (cons 3 nil))))))
(assert-equal 2 (car (cdr (reverse (cons 1 (cons 2 (cons 3 nil)))))))
(assert-equal 1 (car (cdr (cdr (reverse (cons 1 (cons 2 (cons 3 nil))))))))
(assert-equal nil (reverse nil))

;;; --- ILOS: defclass / make-instance / slot-value / typep / subclassp ---

(defclass point () ((x :initarg :x :initform 0) (y :initarg :y :initform 0)))

;; initargで指定した値がslot-valueで読める
(assert-equal 1 (slot-value (make-instance 'point ':x 1 ':y 2) 'x))
(assert-equal 2 (slot-value (make-instance 'point ':x 1 ':y 2) 'y))

;; initargを省略した場合はinitformの値(0)になる
(assert-equal 0 (slot-value (make-instance 'point) 'x))

;; setf経由でスロットを書き換えられる
(assert-equal 99
  (let ((p (make-instance 'point)))
    (setf (slot-value p 'x) 99)
    (slot-value p 'x)))

;; 単純継承: 親クラスのスロットも引き継ぐ
(defclass point3d (point) ((z :initarg :z :initform 0)))
(assert-equal 1 (slot-value (make-instance 'point3d ':x 1 ':y 2 ':z 3) 'x))
(assert-equal 3 (slot-value (make-instance 'point3d ':x 1 ':y 2 ':z 3) 'z))

;; typep / subclassp
(assert-equal t (typep (make-instance 'point3d) 'point3d))
(assert-equal t (typep (make-instance 'point3d) 'point))
(assert-equal nil (typep (make-instance 'point) 'point3d))
(assert-equal t (subclassp (%find-class 'point3d) (%find-class 'point)))
(assert-equal nil (subclassp (%find-class 'point) (%find-class 'point3d)))

;;; --- Condition System: signal-condition / with-handler / error ---

;; with-handler + block/return-fromでcatchできる
(assert-equal 'caught
  (block b
    (with-handler (lambda (c) (return-from b 'caught))
      (error "boom"))))

;; ハンドラはconditionのインスタンスをそのまま受け取り、typepで型判定できる
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<simple-error>)))
      (error "boom"))))

;; format-string/format-argumentsスロットにerrorの引数が保持される。
;; assert-equalは値の同一性(==)で比較しstringはsymbolのようにinternされないため、
;; string-to-symbolを介して内容を比較する
(assert-equal (string-to-symbol "bad value")
  (block b
    (with-handler (lambda (c) (return-from b (string-to-symbol (slot-value c 'format-string))))
      (error "bad value" 1 2))))

;; 型判定して処理しない場合は自分でsignal-conditionを呼び、外側のハンドラに渡せる
(assert-equal 'outer
  (block b
    (with-handler (lambda (c) (return-from b 'outer))
      (with-handler (lambda (c) (if (typep c '<simple-error>) (signal-condition c nil) (return-from b 'inner)))
        (error "boom")))))

;; continuableなsignal-conditionは、ハンドラが脱出せず返した値がそのまま結果になり、
;; 呼び出し元の計算が中断されず続く
(assert-equal 101
  (+ 1
     (with-handler (lambda (c) 100)
       (signal-condition (make-instance '<condition>) t))))

;; %abort-top-levelはos_eval_top_levelが張るblock %TOP-LEVELまで届く(単体での動作確認)
(assert-equal 42
  (block %top-level
    (%abort-top-level 42)))

;;; --- dynamic-let / set-dynamic ---

(defdynamic *dl-test* 1)

;; dynamic-letの間だけ新しい値が見える
(assert-equal 2 (dynamic-let ((*dl-test* 2)) (dynamic *dl-test*)))

;; dynamic-letを抜けたら元の値に戻る
(assert-equal 1 (dynamic *dl-test*))

;; 非局所脱出(return-from)でdynamic-letを抜けても元の値に戻る
(assert-equal 1
  (progn
    (block b
      (dynamic-let ((*dl-test* 99))
        (return-from b nil)))
    (dynamic *dl-test*)))

;; 複数変数を同時に動的束縛できる
(defdynamic *dl-a* 'unbound-a)
(defdynamic *dl-b* 'unbound-b)
(assert-equal 10
  (car (dynamic-let ((*dl-a* 10) (*dl-b* 20)) (cons (dynamic *dl-a*) (dynamic *dl-b*)))))
(assert-equal 20
  (cdr (dynamic-let ((*dl-a* 10) (*dl-b* 20)) (cons (dynamic *dl-a*) (dynamic *dl-b*)))))

;; set-dynamicはvarを評価せずformの評価値をvarの動的値に設定する
(defdynamic *sd-test* 1)
(assert-equal 5 (progn (set-dynamic 5 *sd-test*) (dynamic *sd-test*)))

;;; --- case / case-using ---

(assert-equal 'composite (case (* 2 3) ((2 3 5 7) 'prime) ((4 6 8 9) 'composite)))
(assert-equal 'prime (case 5 ((2 3 5 7) 'prime) ((4 6 8 9) 'composite)))

;; どのkeylistにも該当しない場合はtのdefault節を評価する
(assert-equal 'other (case 99 ((1) 'one) (t 'other)))

;; どのkeylistにも該当せず、default節も無い場合はnil
(assert-equal nil (case 99 ((1) 'one)))

;; keyformは1度だけ評価される(2回評価されると2度目に'compositeを返してしまうため
;; 期待通りの結果は評価回数が1であることの間接的な確認になる)
(assert-equal 'prime
  (let ((n 0))
    (case (progn (setq n (+ n 1)) 5)
      ((2 3 5 7) 'prime)
      ((4 6 8 9) 'composite))))

;; case-usingは(funcall predform keyform-value key)の順で呼ぶ。<は非対称なので、
;; 引数順が逆になっていると(< 5 3)=nilとなり検出できる
(assert-equal 'yes (case-using #'< 3 ((5) 'yes) (t 'no)))
(assert-equal 'no (case-using #'< 5 ((3) 'yes) (t 'no)))

;;; --- ignore-errors ---

;; エラーが起きたらnilを返す
(assert-equal nil (ignore-errors (error "boom")))

;; エラーが無ければ最後のformの値を返す
(assert-equal 3 (ignore-errors 1 2 3))

;; <error>でないconditionは捕まえず、外側のwith-handlerまで伝播する
(assert-equal 'outer
  (block b
    (with-handler (lambda (c) (return-from b 'outer))
      (ignore-errors (signal-condition (make-instance '<condition>) nil)))))

;;; --- class / the / assure ---

;; classはILOSでdefclassされたクラス名をクラスオブジェクトに変換する(%find-classと同じ)
(assert-equal t (eq (class point) (%find-class 'point)))

;; theは型チェックをせず、formの値をそのまま返すno-op
(assert-equal 5 (the <integer> 5))

;; assureは組み込み型名(<integer>等)なら対応する述語で判定し、一致すれば値を返す
(assert-equal 10 (assure <integer> 10))

;; 型が一致しなければerror(ignore-errorsで捕まえて確認)
(assert-equal nil (ignore-errors (assure <integer> "x")))

;; ILOSのユーザークラス名を指定した場合はtypepにフォールバックする
(assert-equal t (typep (assure point (make-instance 'point)) 'point))
(assert-equal nil (ignore-errors (assure point 5)))
