;; test/lisp/type_unbound_test.lisp
;;
;; [P5] 正常系のコストがゼロの検出(調査文書 §6-3 C1)。
;;
;;   1. 算術に数値でない引数を渡したとき -> <domain-error>
;;   2. 未束縛の変数を参照したとき       -> <unbound-variable>
;;
;; 仕様の根拠:
;;   + / *        spec:4283-4284「An error shall be signaled if any x is not
;;                a number (error-id. domain-error)」
;;   -            spec:4306 / spec:4324
;;   quotient(/)  spec:4363-4365
;;   = 　         spec:4204-4205
;;   < > <= >=    spec:4250-4251
;;   max / min    spec:4390
;;   abs          spec:4414
;;   未束縛変数    spec:899-902(error-id. undefined-entity。代表例が unbound-variable)
;;                クラスは §29.4 spec:7255-7257 の <unbound-variable>
;;
;; **どちらも「正常系に命令を増やさない」形で入れてある。** 検査は
;;   - 算術: fixnum の高速経路を外れたあと
;;   - 比較: 比較が偽になった分岐の内側
;;   - 変数: 親環境まで辿って見つからなかったあと
;; にしか無い。詳細は PR の objdump の表を見ること。

;;; ---------------------------------------------------------------------------
;;; 1. 算術の型違い -> <domain-error>
;;; ---------------------------------------------------------------------------

(assert-error-class '<domain-error> (+ 1 'a))
(assert-error-class '<domain-error> (+ 'a 1))
(assert-error-class '<domain-error> (- 1 'a))
(assert-error-class '<domain-error> (* 2 'a))
(assert-error-class '<domain-error> (/ 6 'a))
(assert-error-class '<domain-error> (+ 1 2 "x"))
;; 単項マイナス
(assert-error-class '<domain-error> (- 'a))
;; 文字・リスト・ベクタ・シンボル
(assert-error-class '<domain-error> (+ 1 #\a))
(assert-error-class '<domain-error> (+ 1 '(1 2)))
(assert-error-class '<domain-error> (+ 1 (vector 1)))
;; nil も数値ではない
(assert-error-class '<domain-error> (+ 1 nil))

;; float が絡む経路
(assert-error-class '<domain-error> (+ 1.0 'a))
(assert-error-class '<domain-error> (- 1.0 'a))
(assert-error-class '<domain-error> (* 2.0 'a))
(assert-error-class '<domain-error> (/ 6.0 'a))
(assert-error-class '<domain-error> (+ 'a 1.0))

;; object / expected-class が載っていること
(defun p5-catch (thunk)
  (block p5 (with-handler (lambda (c) (return-from p5 c)) (funcall thunk))))

(defglobal *p5-c1* (p5-catch (lambda () (+ 1 'zz))))
(assert-equal 'ZZ (domain-error-object *p5-c1*))
(assert-equal '<NUMBER> (%%class-name (domain-error-expected-class *p5-c1*)))

;;; ---------------------------------------------------------------------------
;;; 2. 比較の型違い -> <domain-error>
;;; ---------------------------------------------------------------------------

(assert-error-class '<domain-error> (< 1 'a))
(assert-error-class '<domain-error> (> 1 'a))
(assert-error-class '<domain-error> (= 1 'a))
(assert-error-class '<domain-error> (/= 1 'a))
(assert-error-class '<domain-error> (<= 1 'a))
(assert-error-class '<domain-error> (>= 1 'a))
(assert-error-class '<domain-error> (< 'a 1))
(assert-error-class '<domain-error> (< 1.0 'a))

(defglobal *p5-c2* (p5-catch (lambda () (< 1 'yy))))
(assert-equal 'YY (domain-error-object *p5-c2*))

;;; ---------------------------------------------------------------------------
;;; 3. max / min / abs
;;; ---------------------------------------------------------------------------

(assert-error-class '<domain-error> (max 1 'a))
(assert-error-class '<domain-error> (min 1 'a))
(assert-error-class '<domain-error> (max 'a))
(assert-error-class '<domain-error> (abs 'a))

;;; ---------------------------------------------------------------------------
;;; 4. 未束縛の変数 -> <unbound-variable>
;;; ---------------------------------------------------------------------------

(assert-error-class '<unbound-variable> p5-no-such-variable)
;; 階層(<unbound-variable> < <undefined-entity> < <program-error> < <error>)
(assert-error-class '<undefined-entity> p5-no-such-variable)
(assert-error-class '<program-error> p5-no-such-variable)
(assert-error-class '<error> p5-no-such-variable)

;; name / namespace(spec:7160-7163、spec:7181-7182)
(defglobal *p5-c3* (p5-catch (lambda () p5-no-such-variable)))
(assert-equal 'P5-NO-SUCH-VARIABLE (undefined-entity-name *p5-c3*))
(assert-equal 'VARIABLE (undefined-entity-namespace *p5-c3*))

;; **nil が束縛されているのは未束縛ではない。** これが区別できることが要点で、
;; 「見つからなかったら nil」では両者が同じ値になってしまっていた
(defglobal *p5-bound-nil* nil)
(assert-equal nil *p5-bound-nil*)
(assert-equal t (null *p5-bound-nil*))

;; let で束縛した nil も同じ
(assert-equal nil (let ((p5-local nil)) p5-local))
;; let の外では未束縛
(assert-error-class '<unbound-variable> (progn (let ((p5-local-2 1)) p5-local-2) p5-local-2))

;;; ---------------------------------------------------------------------------
;;; 5. 評価がその場で打ち切られること
;;; ---------------------------------------------------------------------------

(defglobal *p5-trace* nil)
(defun p5-note (x) (setq *p5-trace* (cons x *p5-trace*)) x)

(setq *p5-trace* nil)
(assert-error-class '<domain-error> (list (p5-note 'a) (+ 1 'z) (p5-note 'b)))
(assert-equal '(A) (reverse *p5-trace*))

(setq *p5-trace* nil)
(assert-error-class '<unbound-variable>
  (list (p5-note 'a) p5-no-such-variable (p5-note 'b)))
(assert-equal '(A) (reverse *p5-trace*))

;;; ---------------------------------------------------------------------------
;;; 6. JIT 版
;;; ---------------------------------------------------------------------------

(defun p5-add-jit (x) (+ 1 x))
(assert-equal t (%%za-compiled-p (function p5-add-jit)))
(assert-error-class '<domain-error> (p5-add-jit 'a))
(assert-equal 3 (p5-add-jit 2))

(defun p5-lt-jit (x) (< 1 x))
(assert-equal t (%%za-compiled-p (function p5-lt-jit)))
(assert-error-class '<domain-error> (p5-lt-jit 'a))
(assert-equal t (p5-lt-jit 2))
(assert-equal nil (p5-lt-jit 0))

(defun p5-eq-jit (x) (= 1 x))
(assert-equal t (%%za-compiled-p (function p5-eq-jit)))
(assert-error-class '<domain-error> (p5-eq-jit 'a))
(assert-equal t (p5-eq-jit 1))

(defun p5-unbound-jit () p5-no-such-variable-jit)
(assert-equal t (%%za-compiled-p (function p5-unbound-jit)))
(assert-error-class '<unbound-variable> (p5-unbound-jit))
;; 後から defglobal すれば読める(未束縛をキャッシュしていないこと)
(defglobal p5-no-such-variable-jit 42)
(assert-equal 42 (p5-unbound-jit))

;;; ---------------------------------------------------------------------------
;;; 7. 正常系(変えていないこと)
;;; ---------------------------------------------------------------------------

(assert-equal 6 (+ 1 2 3))
(assert-equal 0 (+))
(assert-equal 1 (*))
(assert-equal -1 (- 1 2))
(assert-equal -5 (- 5))
(assert-equal 24 (* 2 3 4))
(assert-equal 2 (/ 6 3))
(assert-equal 6.0 (+ 1.0 2 3))
(assert-equal 3.0 (- 5.0 2))
(assert-equal t (< 1 2 3))
(assert-equal nil (< 1 3 2))
(assert-equal t (= 2 2))
(assert-equal nil (= 2 3))
(assert-equal t (>= 3 3))
(assert-equal 3 (max 1 3 2))
(assert-equal 1 (min 1 3 2))
(assert-equal 5 (abs -5))
(assert-equal 5.0 (abs -5.0))
;; bignum も従来どおり
(assert-equal t (> (* 1000000000000 1000000000000) 0))
(assert-equal t (= (* 1000000000000 1000000000000) 1000000000000000000000000))
(assert-equal 1000000000000000000000000 (abs -1000000000000000000000000))
