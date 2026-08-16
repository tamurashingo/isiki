;;; src/lisp/init.lisp で定義したマクロ(let/let*/and/or/cond/for/while/with-open-input-stream)のテスト

;;; --- let ---

(assert-equal 3 (let ((a 1) (b 2)) (+ a b)))

;; letのbodyでのsetqは、そのletの変数を書き換える
(assert-equal 10 (let ((x 5)) (setq x 10) x))

;; lambdaクロージャ内からのsetqは、レキシカルスコープ上に見えている外側の変数を書き換える
(assert-equal 99 (let ((x 0)) (let ((f (lambda () (setq x 99)))) (funcall f) x)))

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

;; mapcarは複数のリストを受け取り、対応する位置の要素をまとめて関数に渡せる
(assert-equal t
  (equal '((a . 1) (b . 2) (c . 3))
    (mapcar #'cons '(a b c) '(1 2 3))))

;; 最短のリストが尽きた時点で終了し、他のリストの余った要素は無視する
(assert-equal t
  (equal '((a . 1) (b . 2))
    (mapcar #'cons '(a b) '(1 2 3))))

;;; --- apply ---

;; 個々のobjを構文上並べた引数と末尾のlist引数の要素を連結して関数を呼び出す
(assert-equal 10 (apply #'+ 1 2 '(3 4)))
(assert-equal 10 (apply #'+ '(1 2 3 4)))
(assert-equal 4 (apply (if (< 1 2) (function max) (function min)) 1 2 (list 3 4)))

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

;;; --- create-list / nreverse / maplist / mapl / mapcon (§21.3) ---

(assert-equal t (equal (list 17 17 17) (create-list 3 17)))
(assert-equal t (equal (list nil nil) (create-list 2)))
(assert-equal t (equal nil (create-list 0)))

(assert-equal t (equal (list 3 2 1) (nreverse (list 1 2 3))))
(assert-equal nil (nreverse nil))

;; maplistはfnに要素ではなく後続のsublistを渡す
(assert-equal t (equal (list (list 1 2 3) (list 2 3) (list 3))
                        (maplist (lambda (x) x) (list 1 2 3))))
(assert-equal nil (maplist (lambda (x) x) nil))

;; mapl(mapcのsublist版)は副作用目的で呼び、list自身を返す
(assert-equal 6
  (let ((acc (make-array 1)))
    (setf (aref acc 0) 0)
    (mapl (lambda (x) (setf (aref acc 0) (+ (aref acc 0) (car x))))
          (list 1 2 3))
    (aref acc 0)))
(assert-equal 1 (car (mapl (lambda (x) x) (list 1 2))))

;; mapcon(mapcanのsublist版)。仕様書(§21.3)の例(#'list版)通りの結果になることを確認する
(assert-equal t (equal (list (list 1 2 3 4) (list 2 3 4) (list 3 4) (list 4))
                        (mapcon #'list (list 1 2 3 4))))
(assert-equal nil (mapcon #'list nil))

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

;; typep: ILOSインスタンス以外の組み込み型の値もclass-of経由で正しく判定できる
(assert-equal t (typep 5 '<integer>))
(assert-equal nil (typep 5 '<float>))
(assert-equal t (typep "abc" '<string>))
(assert-equal t (typep nil '<null>))

;; subclassp: <null>は<list>と<symbol>の両方を(直接の)スーパークラスに持つ
(assert-equal t (subclassp (%find-class '<null>) (%find-class '<symbol>)))
(assert-equal t (subclassp (%find-class '<null>) (%find-class '<list>)))
(assert-equal t (subclassp (%find-class '<integer>) (%find-class '<number>)))

;;; --- 総称関数: defgeneric / defmethod / call-next-method / next-method-p / class-of ---

;; class-of: インスタンスが直接属するクラスを返す
(assert-equal t (eq (class-of (make-instance 'point)) (%find-class 'point)))
(assert-equal t (eq (class-of (make-instance 'point3d)) (%find-class 'point3d)))

;; class-of: 組み込み型の値からも対応するpredefinedクラスを返す
(assert-equal t (eq (class-of 5) (%find-class '<integer>)))
(assert-equal t (eq (class-of nil) (%find-class '<null>)))

;; defmethodのspecializerを組み込み型にした場合、非適合な引数には適用されない
;; (%method-applicable-pが組み込み型を正しく判定できることの回帰テスト)
(defgeneric %builtin-specializer-test (obj))
(defmethod %builtin-specializer-test ((obj <integer>)) 'is-integer)
(assert-equal 'is-integer (%builtin-specializer-test 5))
(assert-equal nil (ignore-errors (%builtin-specializer-test "not an integer")))

;; 特定性順dispatch・call-next-method・next-method-p:
;; specializer無しのメソッドは常に適用可能(最も非特定的)。point3dに対しては
;; point3d用メソッドが先にdispatchされ、そちらの中ではnext-method-pがtになり
;; call-next-methodで非特定的メソッドの結果を取得できる。非特定的メソッド自身の
;; 中ではnext-method-pはnil(これより後ろのメソッドが無い)
(defgeneric describe-point (obj))
(defmethod describe-point (obj) (list 'point-desc (next-method-p)))
(defmethod describe-point ((obj point3d)) (list '3d-desc (next-method-p) (call-next-method)))

(assert-equal t (equal (list 'point-desc nil) (describe-point (make-instance 'point))))
(assert-equal t (equal (list '3d-desc t (list 'point-desc nil)) (describe-point (make-instance 'point3d))))

;; call-next-methodの次が無い場合はerrorになる
(defgeneric %no-next-method-test (obj))
(defmethod %no-next-method-test (obj) (call-next-method))
(assert-equal nil (ignore-errors (%no-next-method-test (make-instance 'point))))

;; initialize-object: システム標準のinitarg/initform埋め込みをcall-next-methodで
;; 呼びつつ、サブクラス独自の初期化(labelスロットの自動計算)を追加できる
(defclass labeled-point (point) ((label :initarg :label :initform nil)))
(defmethod initialize-object ((obj labeled-point) initargs)
  (call-next-method)
  (set-slot-value obj 'label (list 'auto (slot-value obj 'x))))

(assert-equal 5 (slot-value (make-instance 'labeled-point ':x 5) 'x))
(assert-equal t (equal (list 'auto 5) (slot-value (make-instance 'labeled-point ':x 5) 'label)))

;;; --- 複数ディスパッチ: 第2引数以降のspecializer ---
;;
;; shape/circle/squareの単一継承階層(ダイヤモンドは使わない、CPL未計算という
;; 既知の簡略化に踏み込まないため)で、両方の引数を見て最も特定的なメソッドが
;; 選ばれることを確認する。

(defclass shape () ())
(defclass circle (shape) ())
(defclass square (shape) ())

(defgeneric combine (a b))
(defmethod combine (a b) 'unspecialized)
(defmethod combine ((a shape) b) 'first-only)
(defmethod combine ((a shape) (b shape)) 'both-shapes)
(defmethod combine ((a circle) (b circle)) 'both-circles)

;; 両方circleなら、両方指定のうち最も特定的な both-circles が選ばれる
(assert-equal 'both-circles (combine (make-instance 'circle) (make-instance 'circle)))

;; circleとsquare(どちらもshapeだがcircle-circleには非適合)なら both-shapes
(assert-equal 'both-shapes (combine (make-instance 'circle) (make-instance 'square)))

;; 第2引数がshapeでない場合は、第1引数のみ指定のfirst-onlyまで絞られる
(assert-equal 'first-only (combine (make-instance 'circle) 5))

;; どちらもshapeでない場合はunspecializedまで絞られる
(assert-equal 'unspecialized (combine 1 2))

;; call-next-method: 2引数指定のメソッド同士でも特定的な順に正しく連鎖する
(defgeneric combine-chain (a b))
(defmethod combine-chain (a b) (list 'base))
(defmethod combine-chain ((a shape) (b shape)) (list 'shapes (call-next-method)))
(defmethod combine-chain ((a circle) (b circle)) (list 'circles (call-next-method)))

(assert-equal t (equal (list 'circles (list 'shapes (list 'base)))
                        (combine-chain (make-instance 'circle) (make-instance 'circle))))

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

;;; --- Condition System: クラス階層(§29) ---

;; subclasspは(%find-classと同じ)クラスオブジェクトを引数に取る(クラス名symbolは不可)
(assert-equal t (subclassp (class <serious-condition>) (class <condition>)))
(assert-equal t (subclassp (class <error>) (class <serious-condition>)))
(assert-equal t (subclassp (class <storage-exhausted>) (class <serious-condition>)))
(assert-equal t (subclassp (class <arithmetic-error>) (class <error>)))
(assert-equal t (subclassp (class <division-by-zero>) (class <arithmetic-error>)))
(assert-equal t (subclassp (class <floating-point-overflow>) (class <arithmetic-error>)))
(assert-equal t (subclassp (class <floating-point-underflow>) (class <arithmetic-error>)))
(assert-equal t (subclassp (class <control-error>) (class <error>)))
(assert-equal t (subclassp (class <parse-error>) (class <error>)))
(assert-equal t (subclassp (class <program-error>) (class <error>)))
(assert-equal t (subclassp (class <domain-error>) (class <program-error>)))
(assert-equal t (subclassp (class <undefined-entity>) (class <program-error>)))
(assert-equal t (subclassp (class <unbound-variable>) (class <undefined-entity>)))
(assert-equal t (subclassp (class <undefined-function>) (class <undefined-entity>)))
(assert-equal t (subclassp (class <simple-error>) (class <error>)))
(assert-equal t (subclassp (class <stream-error>) (class <error>)))
(assert-equal t (subclassp (class <end-of-stream>) (class <stream-error>)))

;; <condition>はsupersを指定しないdefclassなので、暗黙に<standard-object>経由で
;; <object>に接続される(仕様のdisjointネス規定と整合)
(assert-equal t (subclassp (class <condition>) (class <object>)))

;;; --- Condition System: アクセサ(§29.3) ---

;; 正常系: 対応するクラスのインスタンスならinitargで渡した値が返る
(assert-equal '/
  (arithmetic-error-operation (make-instance '<arithmetic-error> ':operation '/ ':operands (list 1 0))))
(assert-equal t
  (equal (list 1 0) (arithmetic-error-operands (make-instance '<arithmetic-error> ':operation '/ ':operands (list 1 0)))))
(assert-equal 5
  (domain-error-object (make-instance '<domain-error> ':object 5 ':expected-class '<integer>)))
(assert-equal '<integer>
  (domain-error-expected-class (make-instance '<domain-error> ':object 5 ':expected-class '<integer>)))
(assert-equal (string-to-symbol "abc")
  (string-to-symbol (parse-error-string (make-instance '<parse-error> ':string "abc" ':expected-class '<integer>))))
(assert-equal (string-to-symbol "msg")
  (string-to-symbol (simple-error-format-string (make-instance '<simple-error> ':format-string "msg" ':format-arguments nil))))
(assert-equal 'strm
  (stream-error-stream (make-instance '<stream-error> ':stream 'strm)))
(assert-equal 'foo
  (undefined-entity-name (make-instance '<undefined-entity> ':name 'foo ':namespace 'variable)))
(assert-equal 'variable
  (undefined-entity-namespace (make-instance '<undefined-entity> ':name 'foo ':namespace 'variable)))

;; 異常系: 型が合わないアクセサ呼び出しは<domain-error>をsignalする(ignore-errorsで捕まえられる)
(assert-equal nil (ignore-errors (arithmetic-error-operation 5)))
(assert-equal nil (ignore-errors (domain-error-object "not a condition")))
(assert-equal nil (ignore-errors (simple-error-format-string (make-instance '<condition>))))

;; %check-condition-classがsignalする<domain-error>の内容自体も確認できる
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (arithmetic-error-operation 5))))
(assert-equal 5
  (block b
    (with-handler (lambda (c) (return-from b (domain-error-object c)))
      (arithmetic-error-operation 5))))

;;; --- Condition System: cerror / condition-continuable / continue-condition ---

;; ハンドラがcontinue-conditionを呼ばず普通に返ると、その結果がcerrorの戻り値になる
;; (continuableに渡された「continue-stringをformatした文字列」がそのまま返る)
(assert-equal (string-to-symbol "retry? y")
  (block b
    (with-handler (lambda (c) (return-from b (string-to-symbol (condition-continuable c))))
      (cerror "retry? ~A" "bad value: ~A" "y"))))

;; ハンドラがcontinue-conditionでvalueを渡すと、それがcerrorの呼び出し元での
;; signal-conditionの戻り値になる(= cerror自体の戻り値になる)
(assert-equal 'resumed
  (with-handler (lambda (c) (continue-condition c 'resumed))
    (cerror "retry" "bad value")))

;; valueを省略した場合はnilが返る
(assert-equal nil
  (with-handler (lambda (c) (continue-condition c))
    (cerror "retry" "bad value")))

;;; --- Condition System: report-condition ---

(assert-equal (string-to-symbol "msg 1 2")
  (let ((s (create-string-output-stream)))
    (report-condition (make-instance '<simple-error> ':format-string "msg ~A ~A" ':format-arguments (list 1 2)) s)
    (string-to-symbol (get-output-stream-string s))))

(assert-equal (string-to-symbol "undefined variable: x")
  (let ((s (create-string-output-stream)))
    (report-condition (make-instance '<undefined-entity> ':name 'x ':namespace 'variable) s)
    (string-to-symbol (get-output-stream-string s))))

;; デフォルトメソッド(<simple-error>等の特化メソッドが無いクラス)はクラス名を出力する
(assert-equal (string-to-symbol "<CONTROL-ERROR>")
  (let ((s (create-string-output-stream)))
    (report-condition (make-instance '<control-error>) s)
    (string-to-symbol (get-output-stream-string s))))

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

;;; --- convert ---

;; symbol -> string(symbol-nameと同じ結果になる。stringはinternされないので
;; string-to-symbol経由で比較する)
(assert-equal (string-to-symbol "ABC") (string-to-symbol (convert 'abc <string>)))

;; string -> symbol(大文字化されてinternされる。os_make_symbolは大文字小文字を
;; 区別しないので'abcと同じsymbolになる)
(assert-equal 'abc (convert "abc" <symbol>))

;; string -> list(文字のリストに変換される)
(assert-equal #\a (car (convert "ab" <list>)))
(assert-equal #\b (car (cdr (convert "ab" <list>))))
(assert-equal nil (cdr (cdr (convert "ab" <list>))))
(assert-equal nil (convert "" <list>))

;; list -> stringは仕様上エラーを発生させる変換(表の該当欄が"–")なので未対応
(assert-equal nil (ignore-errors (convert (cons #\a (cons #\b nil)) <string>)))

;; その他の未対応の組み合わせもerrorになる
(assert-equal nil (ignore-errors (convert 5 <float>)))

;;; --- symbol property list: property / set-property / remove-property ---

;; propertyが無ければ既定値(省略時nil)を返す
(assert-equal nil (property 'zeus 'daughter))
(assert-equal 'none (property 'zeus 'daughter 'none))

;; set-propertyで新規作成、propertyで読める。set-propertyはobjを返す
(assert-equal 'athena (set-property 'athena 'zeus 'daughter))
(assert-equal 'athena (property 'zeus 'daughter))

;; 既存のpropertyをset-propertyで上書きできる
(assert-equal 'ares (set-property 'ares 'zeus 'son))
(set-property 'apollo 'zeus 'son)
(assert-equal 'apollo (property 'zeus 'son))

;; 他のsymbol/property-nameのpropertyには影響しない
(assert-equal 'athena (property 'zeus 'daughter))
(assert-equal nil (property 'hera 'daughter))

;; setf経由でも(property symbol property-name)に書き込める
(setf (property 'zeus 'wife) 'hera)
(assert-equal 'hera (property 'zeus 'wife))

;; remove-propertyは取り除いたpropertyの値を返し、以後propertyは既定値に戻る
(assert-equal 'athena (remove-property 'zeus 'daughter))
(assert-equal nil (property 'zeus 'daughter))

;; 存在しないpropertyのremove-propertyはnilを返す
(assert-equal nil (remove-property 'zeus 'daughter))

;; symbol/property-nameがsymbolでなければerror
(assert-equal nil (ignore-errors (property "zeus" 'daughter)))
(assert-equal nil (ignore-errors (property 'zeus "daughter")))
(assert-equal nil (ignore-errors (set-property 'athena "zeus" 'daughter)))
(assert-equal nil (ignore-errors (remove-property 'zeus "daughter")))

;;; --- with-standard-input / with-standard-output / with-error-output ---

;; 束縛前はnil
(assert-equal nil (standard-output))

;; with-standard-outputの間だけ束縛した値が見える
(assert-equal t
  (with-standard-output (open-output-stream) (if (standard-output) t nil)))

;; 抜けたら元の値(nil)に戻る
(assert-equal nil (standard-output))

(assert-equal nil (standard-input))
(assert-equal t (with-standard-input (open-output-stream) (if (standard-input) t nil)))
(assert-equal nil (standard-input))

(assert-equal nil (error-output))
(assert-equal t (with-error-output (open-output-stream) (if (error-output) t nil)))
(assert-equal nil (error-output))

;;; --- with-open-input-file ---

;; このテスト環境では9Pの実ファイルアクセスをモックしていない(os_virtio9p_openが
;; 常に失敗するダミー実装)ため、実際にファイルを開いて読む動作は確認できない。
;; macroexpand-1でwith-open-input-streamへの展開結果の形だけを確認する
;; (with-open-input-streamのcloseは既存実装のため、ここでは再テストしない)。
(let ((expanded (macroexpand-1 '(with-open-input-file (s "foo.lsp") (read s)))))
  (assert-equal 'with-open-input-stream (car expanded))
  (let ((binding (car (cdr expanded))))
    (assert-equal 's (car binding))
    (let ((stream-form (car (cdr binding))))
      (assert-equal 'open-input-stream (car stream-form))
      (assert-equal (string-to-symbol "foo.lsp") (string-to-symbol (car (cdr stream-form))))))
  (assert-equal 'read (car (car (cdr (cdr expanded))))))

;;; --- open-input-file ---

;; open-input-streamの別名であることをmacroexpand-1相当(defunなのでここでは
;; funcallの展開結果ではなく、実際に9Pが失敗する環境でも同じ結果(g_sym_eval_error
;; 相当のシンボル)を返すことで確認する。
(assert-equal (open-input-stream "foo.lsp") (open-input-file "foo.lsp"))

;;; --- with-open-output-stream / with-open-output-file ---

(assert-equal 42 (with-open-output-stream (s (open-output-stream)) 42))
(assert-equal t (with-open-output-stream (s (open-output-stream)) (if s t nil)))

;; このテスト環境では9Pの実ファイルアクセスをモックしていないため、
;; with-open-input-fileと同様にmacroexpand-1で展開結果の形だけを確認する。
(let ((expanded (macroexpand-1 '(with-open-output-file (s "foo.lsp") (write-char #\a s)))))
  (assert-equal 'with-open-output-stream (car expanded))
  (let ((binding (car (cdr expanded))))
    (assert-equal 's (car binding))
    (let ((stream-form (car (cdr binding))))
      (assert-equal 'open-output-file (car stream-form))
      (assert-equal (string-to-symbol "foo.lsp") (string-to-symbol (car (cdr stream-form))))))
  (assert-equal 'write-char (car (car (cdr (cdr expanded))))))

;;; --- with-open-io-stream / with-open-io-file ---

(assert-equal 42 (with-open-io-stream (s (open-output-stream)) 42))
(assert-equal t (with-open-io-stream (s (open-output-stream)) (if s t nil)))

(let ((expanded (macroexpand-1 '(with-open-io-file (s "foo.lsp") (read s)))))
  (assert-equal 'with-open-io-stream (car expanded))
  (let ((binding (car (cdr expanded))))
    (assert-equal 's (car binding))
    (let ((stream-form (car (cdr binding))))
      (assert-equal 'open-io-file (car stream-form))
      (assert-equal (string-to-symbol "foo.lsp") (string-to-symbol (car (cdr stream-form))))))
  (assert-equal 'read (car (car (cdr (cdr expanded))))))

;;; --- 述語 (not / eql / equal / listp / characterp / stringp / functionp /
;;;     generic-function-p / basic-array-p系 / streamp / instancep) ---

;; notはnullと同じ実体を共用しているので、単に登録名経由で呼べることを確認する
(assert-equal t (not nil))
(assert-equal nil (not 1))

;; eqlは現状eqと同じ判定(fixnum/symbolは即値/internされているため)
(assert-equal t (eql 1 1))
(assert-equal t (eql 'a 'a))
(assert-equal nil (eql 1 2))

;; equalは別オブジェクトでも構造的に内容が同じならT
(assert-equal t (equal (cons 1 (cons 2 nil)) (cons 1 (cons 2 nil))))
(assert-equal nil (equal (cons 1 (cons 2 nil)) (cons 1 (cons 3 nil))))
(assert-equal t (equal "aaa" (create-string 3 #\a)))
(assert-equal nil (equal 1 'a))

(assert-equal t (listp (cons 1 2)))
(assert-equal t (listp nil))
(assert-equal nil (listp 1))

(assert-equal t (characterp #\a))
(assert-equal nil (characterp 1))

(assert-equal t (stringp "abc"))
(assert-equal nil (stringp 1))

(assert-equal t (functionp #'car))
(assert-equal nil (functionp 1))

;; defgeneric/defmethodが未実装のため常にnil
(assert-equal nil (generic-function-p 1))

(assert-equal t (basic-array-p (make-array 3)))
(assert-equal t (basic-array-p "abc"))
(assert-equal nil (basic-array-p 1))

(assert-equal nil (basic-array*-p (make-array 3)))
(assert-equal t (basic-array*-p (make-array '(2 3))))
(assert-equal nil (general-array*-p (make-array 3)))
(assert-equal t (general-array*-p (make-array '(2 3))))

(assert-equal t (basic-vector-p (make-array 3)))
(assert-equal t (basic-vector-p "abc"))
(assert-equal nil (basic-vector-p (make-array '(2 3))))

(assert-equal t (general-vector-p (make-array 3)))
(assert-equal nil (general-vector-p "abc"))
(assert-equal nil (general-vector-p (make-array '(2 3))))

(assert-equal t (streamp (open-output-stream)))
(assert-equal nil (streamp 1))

;; instancep(既存のtypepに委譲、known limitation: domain-errorチェックは行わない)
(assert-equal t (instancep (make-instance 'point3d) (class point3d)))
(assert-equal t (instancep (make-instance 'point3d) (class point)))

;;; --- number class (§19): 符号付き整数とbignum ---

;; 負のfixnumの四則演算(60bit以内なので即値のまま、assert-equalの生比較でも安全)
(assert-equal -2 (+ -5 3))
(assert-equal -7 (+ -3 -4))
(assert-equal -5 (- 5))
(assert-equal 5 (- -5))
(assert-equal -2 (- 3 5))
(assert-equal 0 (- 5 3 2))
(assert-equal -6 (* -2 3))
(assert-equal 6 (* -2 -3))
(assert-equal -4 (/ -12 3))
(assert-equal -4 (/ 12 -3))
(assert-equal 4 (/ -12 -3))

;; bignumは同じ値でも独立に計算すると別オブジェクトになりうるため、raw eqである
;; assert-equalではなく数値比較の=をtと比較する形でテストする
;; 1152921504606846976 = 2^60(FIXNUM_MAGNITUDE_MASK=2^60-1を超えるのでbignumになる)
(assert-equal t (bignump 1152921504606846976))
(assert-equal nil (fixnump 1152921504606846976))
(assert-equal t (integerp 1152921504606846976))
(assert-equal t (numberp 1152921504606846976))
(assert-equal t (fixnump 1152921504606846975)) ; FIXNUM_MAGNITUDE_MASKはまだFIXNUM
(assert-equal nil (bignump 1152921504606846975))
(assert-equal t (integerp 5)) ; FIXNUMもintegerp=T

;; 60bit超えの加算でbignumに昇格し、引き戻すと再度fixnumに降格する
(assert-equal t (= 1152921504606846976 (+ 1152921504606846975 1)))
(assert-equal t (bignump (+ 1152921504606846975 1)))
(assert-equal 1152921504606846975 (- (+ 1152921504606846975 1) 1))
(assert-equal t (fixnump (- (+ 1152921504606846975 1) 1)))

;; bignumの負数・比較・大小関係
(assert-equal t (= -1152921504606846976 (- 0 1152921504606846976)))
(assert-equal t (< 1152921504606846975 1152921504606846976))
(assert-equal t (> 1152921504606846976 1152921504606846975))
(assert-equal t (< -1152921504606846976 1152921504606846975))

;; eql/equalはbignumも異なるオブジェクトでも内容が同じならT
(assert-equal t (eql (+ 1152921504606846976 0) (+ 0 1152921504606846976)))
(assert-equal nil (eql 1152921504606846976 1152921504606846977))
(assert-equal t (equal (cons 1152921504606846976 nil) (cons (+ 1152921504606846976 0) nil)))
(assert-equal nil (instancep (make-instance 'point) (class point3d)))

;;; --- number class (§19): 整数リテラルの符号(+)とradix表記(#b/#o/#x) ---

(assert-equal 42 +42) ; 明示的な'+'付きの整数リテラルも読める
(assert-equal 10 #b1010) ; 2進数リテラル
(assert-equal 10 #B1010) ; 大文字の#Bも同様
(assert-equal 15 #o17) ; 8進数リテラル
(assert-equal 15 #O17) ; 大文字の#Oも同様
(assert-equal 255 #xff) ; 16進数リテラル(小文字の桁)
(assert-equal 255 #XFF) ; 大文字の#X・大文字の桁も同様
(assert-equal -5 #b-101) ; radixリテラルも符号付き
(assert-equal 15 #o+17) ; radixリテラルの明示的な'+'

;; 16進数なら15桁でも60bitを超えるのでbignumになる(#x1000000000000000 = 16^15 = 2^60)
(assert-equal t (bignump #x1000000000000000))
(assert-equal t (= 1152921504606846976 #x1000000000000000))

;;; --- number class (§19): /= ・>= ・<= ・max/min/abs・div/mod・gcd/lcm・isqrt ---

;; /= ・>= ・<= (既存の</>/=と同様、隣接ペアの連鎖判定に一般化している)
(assert-equal t (/= 1 2 3))
(assert-equal nil (/= 1 1 2))
(assert-equal t (>= 3 3 2))
(assert-equal nil (>= 2 3))
(assert-equal t (<= 1 1 2))
(assert-equal nil (<= 3 2))

;; max/min/abs
(assert-equal 5 (max 1 5 3))
(assert-equal 1 (min 5 1 3))
(assert-equal 3 (max -5 3))
(assert-equal -5 (min -5 3))
(assert-equal 7 (abs -7))
(assert-equal 7 (abs 7))

;; div/mod (floor除算。仕様例(§19.4)の8符号パターン)
(assert-equal 4 (div 12 3))
(assert-equal 0 (mod 12 3))
(assert-equal -4 (div 12 -3))
(assert-equal 0 (mod 12 -3))
(assert-equal -4 (div -12 3))
(assert-equal 0 (mod -12 3))
(assert-equal 4 (div -12 -3))
(assert-equal 0 (mod -12 -3))
(assert-equal 4 (div 14 3))
(assert-equal 2 (mod 14 3))
(assert-equal -5 (div 14 -3))
(assert-equal -1 (mod 14 -3))
(assert-equal -5 (div -14 3))
(assert-equal 1 (mod -14 3))
(assert-equal 4 (div -14 -3))
(assert-equal -2 (mod -14 -3))
(assert-equal 'eval-error (div 5 0))
(assert-equal 'eval-error (mod 5 0))

;; gcd/lcm
(assert-equal 4 (gcd 0 -4))
(assert-equal 4 (gcd 12 8))
(assert-equal 0 (gcd 0 0))
(assert-equal 12 (lcm 4 6))
(assert-equal 0 (lcm 0 5))

;; isqrt
(assert-equal 7 (isqrt 49))
(assert-equal 7 (isqrt 63))
(assert-equal 0 (isqrt 0))
(assert-equal 1 (isqrt 1))
(assert-equal 1 (isqrt 2))
(assert-equal 'eval-error (isqrt -1))
;; bignum境界: 1152921504606846975(FIXNUM_MAGNITUDE_MASK)の2乗の平方根が元に戻る
(assert-equal t (= 1152921504606846975 (isqrt (* 1152921504606846975 1152921504606846975))))
(assert-equal t (= 1152921504606846975 (isqrt (+ 1 (* 1152921504606846975 1152921504606846975)))))

;;; --- number class (§19): sqrt/log/exp/sin/cos/tan/atan/atan2・双曲線関数・floor系 ---
;;; ・parse-number・quotient/reciprocal/expt・*pi*・domain-error(sqrt/log/asin/acos) ---

;; 許容誤差付きの数値比較(floatは合成の丸め誤差があるため、bit一致ではなく比較する)
(defun %approx= (a b)
  (< (abs (- a b)) 0.000000001))

;; sqrt: 完全平方数は整数、それ以外はfloat
(assert-equal 2 (sqrt 4))
(assert-equal t (%approx= 1.4142135623730951 (sqrt 2)))

;; log(自然対数)/exp
(assert-equal t (%approx= 1.0 (log 2.718281828459045)))
(assert-equal t (%approx= 2.302585092994046 (log 10)))
(assert-equal t (%approx= 2.718281828459045 (exp 1)))
(assert-equal t (%approx= 1.0 (exp 0)))

;; sin/cos/tan/atan/atan2
(assert-equal t (%approx= 0.0 (sin 0)))
(assert-equal t (%approx= 1.0 (cos 0)))
(assert-equal t (%approx= 0.0 (tan 0)))
(assert-equal t (%approx= 0.7853981633974483 (atan 1)))
(assert-equal t (%approx= 0.0 (atan2 0 1)))
(assert-equal t (%approx= 0.7853981633974483 (atan2 1 1)))

;; sinh/cosh/tanh/atanh(spec例: (sinh 1) => 1.1752011936438014)
(assert-equal t (%approx= 1.1752011936438014 (sinh 1)))
(assert-equal t (%approx= 1.5430806348152437 (cosh 1)))
(assert-equal t (%approx= 0.7615941559557649 (tanh 1)))
(assert-equal t (%approx= 0.5493061443340548 (atanh 0.5)))

;; *pi*
(assert-equal t (%approx= 3.141592653589793 *pi*))

;; floor/ceiling/truncate/round(仕様例の符号パターン)
(assert-equal 3 (floor 3.4))
(assert-equal -4 (floor -3.4))
(assert-equal 4 (ceiling 3.4))
(assert-equal -3 (ceiling -3.4))
(assert-equal 3 (truncate 3.4))
(assert-equal -3 (truncate -3.4))
(assert-equal 4 (round 3.5))
(assert-equal 2 (round 2.5))
(assert-equal 5 (floor 5))

;; quotient/reciprocal(spec 4342-4362行の例)
(assert-equal 4 (quotient 12 3))
(assert-equal t (%approx= 0.16666666666666666 (quotient 2 3 4)))
(assert-equal 1 (reciprocal 1))
(assert-equal t (%approx= 0.25 (reciprocal 4)))

;; expt(spec 4474-4484行の例を全て確認)
(assert-equal 8 (expt 2 3))
(assert-equal 10000 (expt -100 2))
(assert-equal t (%approx= 0.0625 (expt 4 -2)))
(assert-equal t (%approx= 0.25 (expt 0.5 2)))
(assert-equal 1 (expt 5 0))
(assert-equal t (%approx= 1.0 (expt 5.0 0)))
(assert-equal t (%approx= -4.0 (expt -0.25 -1)))
(assert-equal t (%approx= 10.0 (expt 100 0.5)))
(assert-equal t (%approx= 0.001 (expt 100 -1.5)))
(assert-equal t (%approx= 1.0 (expt 3.0 0.0)))
(assert-equal nil (ignore-errors (expt 0 -1)))
(assert-equal nil (ignore-errors (expt 0.0 0.0)))

;; parse-number(spec 4178-4193/7213-7214行)
(assert-equal t (%approx= 123.34 (parse-number "123.34")))
(assert-equal 64206 (parse-number "#XFACE"))
(assert-equal 42 (parse-number "42"))

;; parse-numberが数値として読めない文字列に対して<parse-error>をsignalする
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<parse-error>)))
      (parse-number "abc"))))
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (string= "abc" (parse-error-string c))))
      (parse-number "abc"))))

;; domain-error: sqrtに負の数を渡す(型は合っているが値が違う、ユーザー明示指示の1ケース目)
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (sqrt -1))))
(assert-equal -1
  (block b
    (with-handler (lambda (c) (return-from b (domain-error-object c)))
      (sqrt -1))))

;; domain-error: logに0以下の数値を渡す(2ケース目)
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (log 0))))
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (log -5))))

;; domain-error: asin/acosの定義域-1.0〜1.0の範囲外(3ケース目)
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (asin 2))))
(assert-equal 2
  (block b
    (with-handler (lambda (c) (return-from b (domain-error-object c)))
      (asin 2))))
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (acos -2))))
;; 範囲内の値ではdomain-errorが起きないこと(境界値-1.0/1.0も含む)
(assert-equal t (%approx= 0.0 (asin 0)))
(assert-equal t (%approx= 1.5707963267948966 (asin 1.0)))
(assert-equal t (%approx= -1.5707963267948966 (asin -1.0)))

;; atanhのdomain-errorはlogとの合成の副産物(|x| >= 1でlogの引数が0以下になり伝播する)
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (atanh 1))))
(assert-equal t
  (block b
    (with-handler (lambda (c) (return-from b (typep c '<domain-error>)))
      (atanh -2))))

;;; --- character class (§20): char= ・char/= ・char< ・char> ・char<= ・char>= ---

(assert-equal t (char= #\a #\a))
(assert-equal nil (char= #\a #\b))
(assert-equal nil (char= #\a #\A))
(assert-equal nil (char/= #\a #\a))
(assert-equal t (char/= #\a #\b))
(assert-equal nil (char< #\a #\a))
(assert-equal t (char< #\a #\b))
(assert-equal nil (char< #\b #\a))
(assert-equal t (char> #\b #\a))
(assert-equal t (char<= #\a #\a))
(assert-equal t (char>= #\b #\a))
(assert-equal t (char>= #\a #\a))
;; 3引数以上の隣接ペア連鎖(既存の数値比較と同じ流儀)
(assert-equal t (char< #\a #\b #\c))
(assert-equal nil (char< #\a #\c #\b))

;;; --- arrays / vectors (§22, §23): vector / create-vector / garef / set-garef / #(...) ---

(assert-equal t (equal (vector 'a 'b 'c) '#(a b c)))
(assert-equal t (equal (vector) '#()))
(assert-equal 3 (length (vector 'a 'b 'c)))

(assert-equal 3 (length (create-vector 3)))
(assert-equal nil (garef (create-vector 3) 0))
(assert-equal t (equal (create-vector 3 0) '#(0 0 0)))

;; garef/set-garefはaref/set-arefと同じ実体を共用する(既存のbasic-array*-p等と同様)
(assert-equal 42 (let ((v (create-vector 3))) (set-garef v 1 42) (garef v 1)))
(assert-equal 42 (let ((v (create-vector 3))) (set-garef v 1 42) (aref v 1)))

;; #(...)リテラル
(assert-equal 1 (aref #(1 2 3) 0))
(assert-equal 3 (aref #(1 2 3) 2))
(assert-equal 3 (length #(1 2 3)))
(assert-equal t (equal #(1 2 3) (vector 1 2 3)))

;;; --- string class (§24): string=・string/=・string<・string>・string<=・string>=・char-index・string-index・string-append ---

(assert-equal t (string= "abcd" "abcd"))
(assert-equal nil (string= "abcd" "wxyz"))
(assert-equal nil (string= "abcd" "abcde"))
(assert-equal nil (string= "abcde" "abcd"))
(assert-equal t (string/= "abcd" "wxyz"))
(assert-equal nil (string< "abcd" "abcd"))
(assert-equal t (string< "abcd" "wxyz"))
(assert-equal t (string< "abcd" "abcde"))
(assert-equal nil (string< "abcde" "abcd"))
(assert-equal t (string<= "abcd" "abcd"))
(assert-equal t (string<= "abcd" "wxyz"))
(assert-equal t (string<= "abcd" "abcde"))
(assert-equal nil (string<= "abcde" "abcd"))
(assert-equal nil (string> "abcd" "wxyz"))
(assert-equal t (string>= "abcd" "abcd"))
;; 3引数以上の隣接ペア連鎖(既存の数値比較・char比較と同じ流儀)
(assert-equal t (string< "a" "b" "c"))
(assert-equal nil (string< "a" "c" "b"))

(assert-equal 1 (char-index #\b "abcab"))
(assert-equal nil (char-index #\B "abcab"))
(assert-equal 4 (char-index #\b "abcab" 2))
(assert-equal nil (char-index #\d "abcab"))
(assert-equal nil (char-index #\a "abcab" 4))

(assert-equal 0 (string-index "foo" "foobar"))
(assert-equal 3 (string-index "bar" "foobar"))
(assert-equal nil (string-index "FOO" "foobar"))
(assert-equal nil (string-index "foo" "foobar" 1))
(assert-equal 3 (string-index "bar" "foobar" 1))
(assert-equal nil (string-index "foo" ""))
(assert-equal 0 (string-index "" "foo"))

(assert-equal t (string= "abcdef" (string-append "abc" "def")))
(assert-equal t (string= "abcabc" (string-append "abc" "abc")))
(assert-equal t (string= "abc" (string-append "abc" "")))
(assert-equal t (string= "abc" (string-append "" "abc")))
(assert-equal t (string= "abcdef" (string-append "abc" "" "def")))
(assert-equal t (string= "" (string-append)))

;;; --- sequence functions (§25): elt / set-elt / subseq / map-into ---

(assert-equal 'c (elt '(a b c) 2))
(assert-equal 'b (elt (vector 'a 'b 'c) 1))
(assert-equal #\a (elt "abc" 0))

(assert-equal t (string= "xxOxx" (let ((string (create-string 5 #\x))) (setf (elt string 2) #\O) string)))
(assert-equal t (equal '(1 99 3) (let ((list (list 1 2 3))) (setf (elt list 1) 99) list)))
(assert-equal 42 (let ((v (create-vector 3))) (setf (elt v 1) 42) (elt v 1)))

(assert-equal t (string= "bcd" (subseq "abcdef" 1 4)))
(assert-equal t (equal '(b c d) (subseq '(a b c d e f) 1 4)))
(assert-equal t (equal (subseq (vector 'a 'b 'c 'd 'e 'f) 1 4) #(b c d)))

(assert-equal t
  (equal '(11 12 13 14)
    (let ((a (list 1 2 3 4)) (b (list 10 10 10 10)))
      (map-into a #'+ a b))))
(assert-equal t
  (equal '(10 10 10 10)
    (let ((a (list 1 2 3 4)) (b (list 10 10 10 10)))
      (map-into a #'+ a b)
      b)))
;; kが最短(3要素)なので、aの4番目の要素は書き換えられない
;; (readerがドット対記法をサポートしないため、期待値は`cons`で組み立てる)
(assert-equal t
  (equal (list (cons 'one 11) (cons 'two 12) (cons 'three 13) 14)
    (let ((a (list 11 12 13 14)) (k '(one two three)))
      (map-into a #'cons k a))))
;; sequence*を指定しない場合は、functionを引数無しでdestinationの長さの回数呼ぶ
(assert-equal t
  (equal '(2 4 6 8)
    (let ((a (list 1 2 3 4)) (counter (create-vector 1)))
      (setf (elt counter 0) 0)
      (map-into a (lambda () (setf (elt counter 0) (+ (elt counter 0) 2)))))))
