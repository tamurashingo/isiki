;; test/lisp/inline_builtin_test.lisp
;;
;; builtin のインライン展開(Phase 3)の回帰テスト。documents/inline-builtin.md
;;
;; (declaim (inline car cdr null eq)) で指定された builtin を、call ではなく
;; 命令列として直接展開する。**既定は無効**で、宣言が無ければ従来どおり call を出す。
;;
;; car/cdr の展開は os_car_checked / os_cdr_checked と**等価**でなければならない。
;; どちらも nil や非 cons に対して domain-error を signal する。nil は自己参照 cons
;; なので、単なるタグ剥がし + オフセット読みに置き換えると (car nil) が nil を
;; 返してしまう。展開側は判定を置き、外れたら従来のヘルパー呼び出しへ落ちる。

(defun ib-len (fn) (%%disasm-code-len fn))

;;; --- 既定は無効 ---
(assert-equal nil (%%current-inline))

(defun ib-n-car (x) (car x))
(defun ib-n-cdr (x) (cdr x))
(defun ib-n-null (x) (null x))
(defun ib-n-eq (x y) (eq x y))
(defun ib-n-deep (x) (car (cdr (car (cdr x)))))
(assert-equal t (%%za-compiled-p (function ib-n-car)))
(assert-equal nil (%%inline-of 'ib-n-car))

;;; --- inline 宣言で展開される ---
(declaim (inline car cdr null eq))
(assert-equal '(car cdr null eq) (%%current-inline))

(defun ib-y-car (x) (car x))
(defun ib-y-cdr (x) (cdr x))
(defun ib-y-null (x) (null x))
(defun ib-y-eq (x y) (eq x y))
(defun ib-y-deep (x) (car (cdr (car (cdr x)))))
(assert-equal t (%%za-compiled-p (function ib-y-car)))
(assert-equal '(car cdr null eq) (%%inline-of 'ib-y-car))

;; 展開されるとコードが増える(call 1つが判定+速いpath+遅いpathに置き換わるため)
(assert-equal t (> (ib-len (function ib-y-car)) (ib-len (function ib-n-car))))
(assert-equal t (> (ib-len (function ib-y-cdr)) (ib-len (function ib-n-cdr))))
(assert-equal t (> (ib-len (function ib-y-null)) (ib-len (function ib-n-null))))
(assert-equal t (> (ib-len (function ib-y-eq)) (ib-len (function ib-n-eq))))
;; 4箇所ある式では増分も4倍になる
(assert-equal (* 4 (- (ib-len (function ib-y-car)) (ib-len (function ib-n-car))))
              (- (ib-len (function ib-y-deep)) (ib-len (function ib-n-deep))))

;;; --- 動作の同一性(6-2) ---
(defglobal *ib-c* (cons 1 2))
(defglobal *ib-nested* '((1 2) (3 4)))
(defglobal *ib-other* (cons 1 2))

;; 通常の cons
(assert-equal (ib-n-car *ib-c*) (ib-y-car *ib-c*))
(assert-equal 1 (ib-y-car *ib-c*))
(assert-equal (ib-n-cdr *ib-c*) (ib-y-cdr *ib-c*))
(assert-equal 2 (ib-y-cdr *ib-c*))
;; ネストしたリスト
(assert-equal (ib-n-deep *ib-nested*) (ib-y-deep *ib-nested*))
(assert-equal 4 (ib-y-deep *ib-nested*))
;; null
(assert-equal (ib-n-null nil) (ib-y-null nil))
(assert-equal t (ib-y-null nil))
(assert-equal (ib-n-null 1) (ib-y-null 1))
(assert-equal nil (ib-y-null 1))
(assert-equal nil (ib-y-null *ib-c*))
;; eq: 同一オブジェクトと、内容は同じだが別オブジェクト
(assert-equal (ib-n-eq *ib-c* *ib-c*) (ib-y-eq *ib-c* *ib-c*))
(assert-equal t (ib-y-eq *ib-c* *ib-c*))
(assert-equal (ib-n-eq *ib-c* *ib-other*) (ib-y-eq *ib-c* *ib-other*))
(assert-equal nil (ib-y-eq *ib-c* *ib-other*))
(assert-equal t (ib-y-eq nil nil))
(assert-equal nil (ib-y-eq nil 1))

;; nil に対する car/cdr は展開しても domain-error のまま
;; (nil は自己参照 cons なので、判定を省くと nil を返してしまう)
(assert-error (ib-y-car nil))
(assert-error (ib-y-cdr nil))
(assert-error (ib-n-car nil))
;; cons でない値も同じ
(assert-error (ib-y-car 1))
(assert-error (ib-y-cdr 'sym))

;;; --- notinline で戻る ---
(declaim (notinline car cdr))
(assert-equal '(null eq) (%%current-inline))
(defun ib-back-car (x) (car x))
(assert-equal (ib-len (function ib-n-car)) (ib-len (function ib-back-car)))
(assert-equal 1 (ib-back-car *ib-c*))
(declaim (notinline null eq))
(assert-equal nil (%%current-inline))

;;; --- 未知の名前はエラーにならない(6-4) ---
(assert-equal nil (declaim (inline nonexistent-fn)))
;; [変更 (inline-arith)] ここは以前 `+` を「未実装の builtin」の例として使っていたが、
;; **`+` は対象になった**(documents/inline-arith.md §7)。対象外の例は、
;; 展開の対象でない builtin へ差し替える。`cons` は確保を伴うので対象にならない
(assert-equal nil (declaim (inline cons)))
(assert-equal nil (declaim (notinline nonexistent-fn)))
(assert-equal nil (%%current-inline))
;; 算術が対象になったことの確認(打ち消しまでがここの責務)
(declaim (inline +))
(assert-equal '(+) (%%current-inline))
(declaim (notinline +))
(assert-equal nil (%%current-inline))
;; 有効な名前と混ざっていても、有効な分だけ効く
(declaim (inline nonexistent-fn car))
(assert-equal '(car) (%%current-inline))
(declaim (notinline car))
(assert-equal nil (%%current-inline))

;;; --- スコープ: frame は透過し、environment は引き継がない(6-3) ---
(declaim (inline car))
(let ((x 1))
  (defun ib-scoped (y) (car y)))
(assert-equal '(car) (%%inline-of 'ib-scoped))       ; let の frame は読み飛ばされる
(assert-equal 1 (ib-scoped *ib-c*))

(flet ((ib-h () 1))
  (defun ib-scoped2 (y) (car y)))
(assert-equal '(car) (%%inline-of 'ib-scoped2))

;; 環境を作って移動すると引き継がれない
(defglobal *ib-e* (make-environment 'ib-env))
(assert-equal nil (%%eval-in-environment '(%%current-inline) *ib-e*))
(%%eval-in-environment '(defun ib-in-env (y) (car y)) *ib-e*)
(assert-equal nil (%%eval-in-environment '(%%inline-of 'ib-in-env) *ib-e*))
;; E での宣言は global へ波及しない
(%%eval-in-environment '(declaim (inline cdr)) *ib-e*)
(assert-equal '(cdr) (%%eval-in-environment '(%%current-inline) *ib-e*))
(assert-equal '(car) (%%current-inline))

;;; --- optimize と同居しても互いを壊さない(同じ fixnum に相乗りしている) ---
(declaim (optimize (speed 3) (safety 0)))
(assert-equal '(3 0 1) (%%current-optimize))
(assert-equal '(car) (%%current-inline))
(declaim (inline null))
(assert-equal '(3 0 1) (%%current-optimize))          ; optimize は壊れない
(assert-equal '(car null) (%%current-inline))
(declaim (optimize (speed 1) (safety 1)))
(assert-equal '(car null) (%%current-inline))         ; inline も壊れない

;;; --- GC を跨いでも展開されたコードが正しく動くこと ---
(defun ib-force-gc ()
  (let ((c (%%gc-collect-count)))
    (while (= (%%gc-collect-count) c) (cons 1 2))
    (%%gc-collect-count)))
(declaim (inline car cdr null eq))
(defun ib-gc-fn (x) (car (cdr x)))
;; *ib-nested* = ((1 2) (3 4)) なので (cdr x) = ((3 4))、その car は (3 4)
(assert-equal '(3 4) (ib-gc-fn *ib-nested*))
(ib-force-gc)
(assert-equal '(3 4) (ib-gc-fn *ib-nested*))
(ib-force-gc)
(ib-force-gc)
(assert-equal '(3 4) (ib-gc-fn *ib-nested*))
(assert-equal t (ib-y-null nil))
(assert-equal t (ib-y-eq *ib-c* *ib-c*))

;; [原則8] 生成コードに GC 対象のアドレスが焼き込まれていないこと。
;; null/eq の展開は g_sym_t を即値として materialize するため、この監査に引っかからない
;; ことを確認しておく(documents/inline-builtin.md 4-4)
(assert-equal 0 (%%za-heap-imm-count))

;; 後片付け: 以降のテストへ影響させない
(declaim (notinline car cdr null eq))
(assert-equal nil (%%current-inline))
