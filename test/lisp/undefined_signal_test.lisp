;; test/lisp/undefined_signal_test.lisp
;;
;; [P4-3] 未定義関数・immutable-binding・「関数でないものの呼び出し」の
;; EVAL-ERROR 返しを signal へ移した分の検証。
;;
;; 仕様の error-id と、それが表すクラス(§29.4 の対応表 spec:7236-7263):
;;
;;   undefined-function -> <undefined-function>(spec:7261-7263)
;;   immutable-binding  -> **<program-error>**(spec:7239-7241)
;;   domain-error       -> <domain-error>
;;
;; 根拠:
;;   関数適用で束縛が無い    spec:1455-1457 / spec:1461
;;   (function name)        spec:1512-1513
;;   defconstant は immutable  spec:1699-1700 / spec:435-437
;;   funcall の非関数        spec:1667

(defun uf-ok-pre (x) (+ x 1))

;;; ---------------------------------------------------------------------------
;;; 1. 未定義関数(インタプリタ)
;;; ---------------------------------------------------------------------------

(assert-error-class '<undefined-function> (uf-no-such-function-1))
(assert-error-class '<undefined-function> (function uf-no-such-function-2))
(assert-error-class '<undefined-function> (%%funcall-by-name 'uf-no-such-function-3))

;; 階層(<undefined-function> < <undefined-entity> < <program-error> < <error>)
(assert-error-class '<undefined-entity> (uf-no-such-function-1))
(assert-error-class '<program-error> (uf-no-such-function-1))
(assert-error-class '<error> (uf-no-such-function-1))

;;; ---------------------------------------------------------------------------
;;; 2. name / namespace スロット(spec:7160-7163、spec:7181-7182)
;;; ---------------------------------------------------------------------------

(defun uf-catch (thunk)
  (block uf2
    (with-handler (lambda (c) (return-from uf2 c)) (funcall thunk))))

(defglobal *uf-c1* (uf-catch (lambda () (uf-no-such-function-1))))
(assert-equal 'UF-NO-SUCH-FUNCTION-1 (undefined-entity-name *uf-c1*))
(assert-equal 'FUNCTION (undefined-entity-namespace *uf-c1*))

(defglobal *uf-c2* (uf-catch (lambda () (function uf-no-such-function-2))))
(assert-equal 'UF-NO-SUCH-FUNCTION-2 (undefined-entity-name *uf-c2*))

(defglobal *uf-c3* (uf-catch (lambda () (%%funcall-by-name 'uf-no-such-function-3))))
(assert-equal 'UF-NO-SUCH-FUNCTION-3 (undefined-entity-name *uf-c3*))

;;; ---------------------------------------------------------------------------
;;; 3. immutable-binding -> <program-error>
;;; ---------------------------------------------------------------------------

(defconstant uf-const 10)
(assert-equal 10 uf-const)
(assert-error-class '<program-error> (setq uf-const 99))
;; 書き換わっていないこと
(assert-equal 10 uf-const)

;;; ---------------------------------------------------------------------------
;;; 4. 関数でないものの呼び出し -> <domain-error>(spec:1667)
;;; ---------------------------------------------------------------------------

(assert-error-class '<domain-error> (funcall 5))
(assert-error-class '<domain-error> (funcall nil))
(assert-error-class '<domain-error> (funcall "abc"))
;; **タグ検査が無く、5 & ~TAG_MASK = 0 でアドレス0を読んでいた経路**
(assert-error-class '<domain-error> ((car '(1 2)) 3))
;; TAG_INSTANCE ではあるが関数ではないもの
(assert-error-class '<domain-error> (funcall (vector 1 2)))

;; expected-class が <FUNCTION> であること
(defglobal *uf-c4* (uf-catch (lambda () (funcall 5))))
(assert-equal 5 (domain-error-object *uf-c4*))
(assert-equal '<FUNCTION> (%%class-name (domain-error-expected-class *uf-c4*)))

;;; ---------------------------------------------------------------------------
;;; 5. 評価がその場で打ち切られること
;;; ---------------------------------------------------------------------------

(defglobal *uf-trace* nil)
(defun uf-note (x) (setq *uf-trace* (cons x *uf-trace*)) x)

(setq *uf-trace* nil)
(assert-error-class '<undefined-function>
  (list (uf-note 'a) (uf-no-such-function-1) (uf-note 'b)))
(assert-equal '(A) (reverse *uf-trace*))

(setq *uf-trace* nil)
(assert-error-class '<domain-error>
  (list (uf-note 'a) (funcall 5) (uf-note 'b)))
(assert-equal '(A) (reverse *uf-trace*))

;;; ---------------------------------------------------------------------------
;;; 6. JIT 版
;;; ---------------------------------------------------------------------------
;;
;; **JIT は Function Cell 経由で呼ぶ。** 未定義の名前に対して
;; os_get_function_cell が nil を返し、os_apply_via_cell がそれを
;; <undefined-function> にする。末尾呼び出しは共有トランポリンを通るので、
;; そちらも cell を落とさずに os_apply_via_cell へ渡すようにした
;; (落とすと fn=nil だけが渡り「nil は関数ではない」= <domain-error> になり、
;;  **同じソースが末尾呼び出しかどうかでクラスが変わってしまう**)。
;;
;; **名前は JIT 側から運ばれてこない**ので name スロットは nil になる。
;; ABI 変更を伴うため P6 の範囲とし、ここでは nil であることを固定する。

;; 末尾呼び出し(共有トランポリン経由)
(defun uf-jit-tail () (uf-no-such-function-jit))
(assert-equal t (%%za-compiled-p (function uf-jit-tail)))
(assert-error-class '<undefined-function> (uf-jit-tail))
(defglobal *uf-jit-c* (uf-catch (lambda () (uf-jit-tail))))
(assert-equal nil (undefined-entity-name *uf-jit-c*))
(assert-equal 'FUNCTION (undefined-entity-namespace *uf-jit-c*))

;; 非末尾呼び出し(os_apply_via_cell 経由)
(defun uf-jit-nontail () (+ 1 (uf-no-such-function-jit2)))
(assert-equal t (%%za-compiled-p (function uf-jit-nontail)))
(assert-error-class '<undefined-function> (uf-jit-nontail))

;; 関数でないものの呼び出し
(defun uf-jit-funcall (f x) (funcall f x))
(assert-equal t (%%za-compiled-p (function uf-jit-funcall)))
(assert-error-class '<domain-error> (uf-jit-funcall 5 1))
(assert-equal 3 (uf-jit-funcall (function uf-ok-pre) 2))

;; immutable-binding
(defun uf-jit-setq () (setq uf-const 77))
(assert-equal t (%%za-compiled-p (function uf-jit-setq)))
(assert-error-class '<program-error> (uf-jit-setq))
(assert-equal 10 uf-const)

;; **後から定義すれば解決すること**(未定義の Function Cell をキャッシュしない、
;; za.c の既存の振る舞いを signal 化で壊していないことの確認)
(defun uf-late-caller () (uf-late-callee))
(assert-error-class '<undefined-function> (uf-late-caller))
(defun uf-late-callee () 'late-ok)
(assert-equal 'late-ok (uf-late-caller))

;;; ---------------------------------------------------------------------------
;;; 7. 正常系(変えていないこと)
;;; ---------------------------------------------------------------------------

(defun uf-ok (x) (+ x 1))
(assert-equal 3 (uf-ok 2))
(assert-equal 3 (funcall (function uf-ok) 2))
(assert-equal 3 (funcall #'uf-ok 2))
(assert-equal 3 (apply (function uf-ok) '(2)))
(assert-equal 3 ((lambda (x) (+ x 1)) 2))
(assert-equal 3 (%%funcall-by-name 'uf-ok 2))
(defglobal *uf-var* 1)
(assert-equal 5 (setq *uf-var* 5))
(assert-equal 5 *uf-var*)
