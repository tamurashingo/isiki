;; test/lisp/arith_signal_test.lisp
;;
;; [P4-1] 算術の EVAL-ERROR 返しを signal へ移した分の検証。
;;
;; 対象と仕様の error-id(documents/error-unwind-survey.md §A-4、
;; 引用は tmp/islisp-spec.txt の行番号):
;;
;;   div / mod のゼロ除算   -> <division-by-zero>  spec:4889
;;   isqrt の負数           -> <domain-error>      spec:4984
;;   float の非数値         -> <domain-error>      spec:4734
;;   / のゼロ除算           -> <division-by-zero>  **仕様未確認**
;;
;; **`/` は ISLisp 仕様に無い実装独自の名前**である(仕様にあるのは quotient)。
;; したがって error-id の指定は無い。同じ演算である quotient(spec:4365)と
;; div(spec:4889)に揃えた、という判断である。
;;
;; クラスとスロットまで固定するのはこのファイル(QEMU 専用。test_framework.lisp の
;; assert-error-class を使う)。init_test.lisp 側は test/c/script_test.c からも
;; 読まれるため ignore-errors で「signal されること」だけを見る。

;;; ---------------------------------------------------------------------------
;;; 1. クラス(インタプリタ版)
;;; ---------------------------------------------------------------------------

(assert-error-class '<division-by-zero> (div 5 0))
(assert-error-class '<division-by-zero> (mod 5 0))
(assert-error-class '<division-by-zero> (/ 1 0))
(assert-error-class '<domain-error> (isqrt -1))
(assert-error-class '<domain-error> (float 'a))

;; 階層も効くこと(<division-by-zero> < <arithmetic-error> < <error>、spec:994-1010)
(assert-error-class '<arithmetic-error> (div 5 0))
(assert-error-class '<error> (div 5 0))
(assert-error-class '<serious-condition> (div 5 0))

;;; ---------------------------------------------------------------------------
;;; 2. スロット(spec §29.3.1)
;;; ---------------------------------------------------------------------------
;;
;; **operation にはシンボルを入れている。** 仕様の型は <function> だが、
;; init.lisp の %quotient2 が以前からシンボルを入れており、report-condition の ~A も
;; 関数オブジェクトでは #<FUNCTION-BUILTIN> としか出ない。既存の形に合わせた。

(defun ar-catch (thunk)
  (block ar-catch
    (with-handler (lambda (c) (return-from ar-catch c))
      (funcall thunk))))

(defglobal *ar-div* (ar-catch (lambda () (div 7 0))))
(assert-equal 'DIV (arithmetic-error-operation *ar-div*))
(assert-equal '(7 0) (arithmetic-error-operands *ar-div*))

(defglobal *ar-mod* (ar-catch (lambda () (mod 7 0))))
(assert-equal 'MOD (arithmetic-error-operation *ar-mod*))
(assert-equal '(7 0) (arithmetic-error-operands *ar-mod*))

;; / は n 項なので operands も n 個そのまま入る
(defglobal *ar-slash* (ar-catch (lambda () (/ 100 0 2))))
(assert-equal '/ (arithmetic-error-operation *ar-slash*))
(assert-equal '(100 0 2) (arithmetic-error-operands *ar-slash*))

;; domain-error 側は object / expected-class
(defglobal *ar-isqrt* (ar-catch (lambda () (isqrt -1))))
(assert-equal -1 (domain-error-object *ar-isqrt*))
(assert-equal '<INTEGER> (%%class-name (domain-error-expected-class *ar-isqrt*)))

(defglobal *ar-float* (ar-catch (lambda () (float 'a))))
(assert-equal 'a (domain-error-object *ar-float*))
(assert-equal '<NUMBER> (%%class-name (domain-error-expected-class *ar-float*)))

;;; ---------------------------------------------------------------------------
;;; 3. 評価がその場で打ち切られること
;;; ---------------------------------------------------------------------------
;;
;; **これが仕様 §9.1(a)「Evaluation of the current form shall stop」の本体。**
;; 以前は (list 1 (div 1 0) 2) が (1 EVAL-ERROR 2) になっていた。

(defglobal *ar-trace* nil)
(defun ar-note (x) (setq *ar-trace* (cons x *ar-trace*)) x)

(setq *ar-trace* nil)
(assert-error-class '<division-by-zero>
  (list (ar-note 'a) (div 1 0) (ar-note 'b)))
(assert-equal '(A) (reverse *ar-trace*))

;;; ---------------------------------------------------------------------------
;;; 4. JIT 版(型宣言つきの特化路を含む)
;;; ---------------------------------------------------------------------------
;;
;; 特化版(primitive_divide2_fixnum など)は 0 のとき n 項版 primitive_divide へ
;; 委譲しているので経路は 1 つに合流する。**合流していることをここで固定する。**

(defun ar-div-jit (a b) (div a b))
(assert-equal t (%%za-compiled-p (function ar-div-jit)))
(assert-error-class '<division-by-zero> (ar-div-jit 5 0))
(assert-equal 2 (ar-div-jit 5 2))

(defun ar-slash-jit (a b) (/ a b))
(assert-equal t (%%za-compiled-p (function ar-slash-jit)))
(assert-error-class '<division-by-zero> (ar-slash-jit 5 0))

;; 型宣言つき(fixnum 特化路)
(defun ar-slash-fix (a b) (declare (type <integer> a) (type <integer> b)) (/ a b))
(assert-equal t (%%za-compiled-p (function ar-slash-fix)))
(assert-error-class '<division-by-zero> (ar-slash-fix 5 0))
(assert-equal 2 (ar-slash-fix 5 2))

(defun ar-isqrt-jit (a) (isqrt a))
(assert-equal t (%%za-compiled-p (function ar-isqrt-jit)))
(assert-error-class '<domain-error> (ar-isqrt-jit -1))
(assert-equal 7 (ar-isqrt-jit 49))

;;; ---------------------------------------------------------------------------
;;; 5. 変えていないもの
;;; ---------------------------------------------------------------------------

;; float のゼロ除算は IEEE 754 どおり(inf/nan)。signal しない
(assert-equal t (floatp (/ 1.0 0.0)))
(assert-equal t (floatp (/ -1.0 0.0)))

;; 正常系
(assert-equal 2 (div 5 2))
(assert-equal 1 (mod 5 2))
(assert-equal 7 (isqrt 49))
(assert-equal 2.0 (float 2))

;; quotient / reciprocal は以前から <division-by-zero> を signal している
;; (%quotient2 が / へ渡す前に自分で 0 を弾く。経路が変わっていないことの確認)
(assert-error-class '<division-by-zero> (quotient 1 0))
(assert-error-class '<division-by-zero> (reciprocal 0))

;; ハンドラが無ければトップレベルへ abort する(打ち切られるので足跡で確かめる)
(setq *ar-trace* nil)
(progn (ar-note 'before) (div 1 0) (ar-note 'after))
(assert-equal '(BEFORE) (reverse *ar-trace*))
