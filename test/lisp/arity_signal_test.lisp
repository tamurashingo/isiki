;; test/lisp/arity_signal_test.lisp
;;
;; [P6] arity 不一致(仮引数の個数と実引数の個数が合わない)の検出。
;;
;; 仕様:
;;   spec:896-898(§9.2 (2))「an error shall be signaled if a function is activated
;;     with a number of arguments which is different than the number of parameters
;;     as required in the function definition (error-id. arity-error)」
;;   spec:1549-1550(lambda)も同じ
;;   §29.4 の対応表(spec:7191-7194)が **<program-error>** と定める
;;
;; **<program-error> はスロットを持たないので、期待した個数と実際の個数は
;; condition に載らない。** 専用クラスは仕様のクラス階層(spec:994-1010)に無く、
;; 仕様に無いクラスを増やさない方針(P4-2 の index-out-of-range と同じ)。
;;
;; 検査は cons リストエントリにしか置いていない。**固定引数エントリは
;; 呼び出し側が arity を照合してから入るので無検査でよい**
;; (documents/unbound-arity-survey.md §3-3)。したがって
;;   - 正しい arity の呼び出し = 固定引数エントリ = 検査を通らない
;;   - 誤った arity の呼び出し = cons エントリへ落ちる = 検査に当たる
;; という形になっている。下の JIT 版はその両方を踏んでいる。

(defun ar2-two (a b) (list a b))
(defun ar2-zero () 'zero)
(defun ar2-rest (a &rest r) (list a r))

;;; ---------------------------------------------------------------------------
;;; 1. インタプリタ
;;; ---------------------------------------------------------------------------

;; 足りない
(assert-error-class '<program-error> (ar2-two 1))
(assert-error-class '<program-error> (ar2-two))
;; 多すぎ
(assert-error-class '<program-error> (ar2-two 1 2 3))
;; 引数を取らない関数
(assert-error-class '<program-error> (ar2-zero 1))
;; &rest は「足りない」だけが起きる
(assert-error-class '<program-error> (ar2-rest))

;; 階層(<program-error> < <error>)
(assert-error-class '<error> (ar2-two 1))
(assert-error-class '<serious-condition> (ar2-two 1))

;;; ---------------------------------------------------------------------------
;;; 2. lambda / funcall / apply
;;; ---------------------------------------------------------------------------

(assert-error-class '<program-error> ((lambda (a b) (list a b)) 1))
(assert-error-class '<program-error> (funcall (function ar2-two) 1))
(assert-error-class '<program-error> (funcall (lambda (a b) a) 1 2 3))
(assert-error-class '<program-error> (apply (function ar2-two) '(1)))

;;; ---------------------------------------------------------------------------
;;; 3. 評価がその場で打ち切られること
;;; ---------------------------------------------------------------------------

(defglobal *ar2-trace* nil)
(defun ar2-note (x) (setq *ar2-trace* (cons x *ar2-trace*)) x)

(setq *ar2-trace* nil)
(assert-error-class '<program-error>
  (list (ar2-note 'a) (ar2-two 1) (ar2-note 'b)))
(assert-equal '(A) (reverse *ar2-trace*))

;;; ---------------------------------------------------------------------------
;;; 4. JIT 版
;;; ---------------------------------------------------------------------------
;;
;; **正しい arity の呼び出しは固定引数エントリを通る**(検査を通らない)。
;; **誤った arity の呼び出しは cons エントリへ落ちる**(検査に当たる)。
;; 同じ関数で両方を踏んで、どちらも期待どおりになることを固定する。

(defun ar2-jit-two (a b) (+ a b))
(assert-equal t (%%za-compiled-p (function ar2-jit-two)))
(assert-equal 3 (ar2-jit-two 1 2))            ; 固定引数エントリ
(assert-error-class '<program-error> (ar2-jit-two 1))     ; cons エントリ
(assert-error-class '<program-error> (ar2-jit-two 1 2 3))
;; 正常な呼び出しがそのあとも通ること(検査が状態を壊していない)
(assert-equal 7 (ar2-jit-two 3 4))

(defun ar2-jit-zero () 42)
(assert-equal t (%%za-compiled-p (function ar2-jit-zero)))
(assert-equal 42 (ar2-jit-zero))
(assert-error-class '<program-error> (ar2-jit-zero 1))

;; &rest 付き(パラメータスロットを使わない経路)
(defun ar2-jit-rest (a &rest r) (list a r))
(assert-equal t (%%za-compiled-p (function ar2-jit-rest)))
(assert-equal '(1 NIL) (ar2-jit-rest 1))
(assert-equal '(1 (2 3)) (ar2-jit-rest 1 2 3))
(assert-error-class '<program-error> (ar2-jit-rest))

;; 引数が4個(ZA_MAX_FIXED_ENTRY_PARAMS を超える経路)
(defun ar2-jit-four (a b c d) (list a b c d))
(assert-equal t (%%za-compiled-p (function ar2-jit-four)))
(assert-equal '(1 2 3 4) (ar2-jit-four 1 2 3 4))
(assert-error-class '<program-error> (ar2-jit-four 1 2 3))
(assert-error-class '<program-error> (ar2-jit-four 1 2 3 4 5))

;; JIT 版でも funcall 経由で当たること
(assert-error-class '<program-error> (funcall (function ar2-jit-two) 1))

;;; ---------------------------------------------------------------------------
;;; 5. AOT 済み関数(事前コンパイル済み)
;;; ---------------------------------------------------------------------------
;;
;; **AOT 済み関数も cons リストエントリに検査を持つ。** member / assoc / reverse は
;; src/lisp/init_aot.lisp 由来で、トランスパイラが生成した C 関数になっている。
;; ここも固定引数エントリは無検査で、個数が合わない呼び出しだけが
;; cons エントリ(lisp_ll_xxx__step)へ落ちて検査に当たる。
;;
;; 入れる前は黙って通っていた(実機で確認した値):
;;   (member 1)        => NIL
;;   (member 1 '(1 2) 'extra) => (1 2)
;;   (assoc 1)         => NIL
;;   (reverse)         => NIL

(assert-error-class '<program-error> (member 1))
(assert-error-class '<program-error> (member 1 '(1 2) 'extra))
(assert-error-class '<program-error> (assoc 1))
(assert-error-class '<program-error> (reverse))
(assert-error-class '<program-error> (reverse '(1 2) 'extra))

;; 正しい個数なら従来どおり
(assert-equal '(1 2) (member 1 '(1 2)))
(assert-equal nil (member 9 '(1 2)))
(assert-equal '(1 . 2) (assoc 1 '((1 . 2))))
(assert-equal '(3 2 1) (reverse '(1 2 3)))

;; &rest を持つ AOT 関数(apply)は「足りない」だけが起きる
(assert-error-class '<program-error> (apply))

;;; ---------------------------------------------------------------------------
;;; 6. 正常系(変えていないこと)
;;; ---------------------------------------------------------------------------

(assert-equal '(1 2) (ar2-two 1 2))
(assert-equal 'zero (ar2-zero))
(assert-equal '(1 NIL) (ar2-rest 1))
(assert-equal '(1 (2)) (ar2-rest 1 2))
(assert-equal '(1 (2 3 4)) (ar2-rest 1 2 3 4))
(assert-equal '(1 2) ((lambda (a b) (list a b)) 1 2))
(assert-equal 3 (funcall (function ar2-jit-two) 1 2))
(assert-equal 3 (apply (function ar2-jit-two) '(1 2)))
;; &rest だけの関数
(defun ar2-only-rest (&rest r) r)
(assert-equal nil (ar2-only-rest))
(assert-equal '(1 2) (ar2-only-rest 1 2))
;; 高階関数(mapcar が内部で呼ぶ arity)
(assert-equal '(2 3 4) (mapcar (lambda (x) (+ x 1)) '(1 2 3)))
