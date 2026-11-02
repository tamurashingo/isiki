;; test/lisp/index_signal_test.lisp
;;
;; [P4-2] 添字・範囲の EVAL-ERROR 返しを signal へ移した分の検証。
;;
;; 仕様の error-id と、それが表すクラス(§29.4 の対応表 spec:7200-7260):
;;
;;   index-out-of-range -> **<program-error>**(spec:7249-7252)
;;   domain-error       -> <domain-error>
;;
;; **<index-out-of-range> というクラスは仕様に無い。** クラス階層
;; (spec:994-1010)にも現れない。したがって「どのシーケンスのどの添字だったか」は
;; condition に載らない(<program-error> はスロットを持たない)。
;;
;; 関数ごとの根拠:
;;   elt / set-elt      範囲外 index-out-of-range(spec:5316/5347)、型違い domain-error
;;   subseq             同上(spec:5374-5377)
;;   length             型違い domain-error(spec:5290)
;;   array-dimensions   型違い domain-error(spec:5077)
;;   aref / set-aref    型違い domain-error(spec:5049)。**範囲外は仕様未確認**
;;   string-elt         **ISLisp 仕様に無い実装独自の関数。仕様未確認**
;;
;; expected-class について: 仕様の「basic-vector でも list でもない」は 1 つの
;; クラスで表せない(ISLisp に <sequence> は無い)ので、受け付けるクラスの一方
;; (<basic-vector>)を入れている。

(defun ix-catch (thunk)
  (block ix-catch
    (with-handler (lambda (c) (return-from ix-catch c))
      (funcall thunk))))

;;; ---------------------------------------------------------------------------
;;; 1. 範囲外 -> <program-error>(index-out-of-range)
;;; ---------------------------------------------------------------------------

(assert-error-class '<program-error> (elt '(1 2) 99))
(assert-error-class '<program-error> (elt "ab" 99))
(assert-error-class '<program-error> (elt (vector 1 2) 99))
(assert-error-class '<program-error> (set-elt 9 (vector 1 2) 99))
(assert-error-class '<program-error> (string-elt "ab" 99))
(assert-error-class '<program-error> (aref (create-vector 2 0) 99))
(assert-error-class '<program-error> (set-aref 1 (create-vector 2 0) 99))

;; 階層(<program-error> < <error>)
(assert-error-class '<error> (elt '(1 2) 99))

;;; ---------------------------------------------------------------------------
;;; 2. 型違い -> <domain-error>
;;; ---------------------------------------------------------------------------

(assert-error-class '<domain-error> (length 5))
(assert-error-class '<domain-error> (length 'a))
(assert-error-class '<domain-error> (elt 5 0))
(assert-error-class '<domain-error> (set-elt 9 5 0))
(assert-error-class '<domain-error> (subseq 5 0 1))
(assert-error-class '<domain-error> (array-dimensions 5))

;; object / expected-class が載っていること
(defglobal *ix-len* (ix-catch (lambda () (length 5))))
(assert-equal 5 (domain-error-object *ix-len*))
(assert-equal '<BASIC-VECTOR> (%%class-name (domain-error-expected-class *ix-len*)))

(defglobal *ix-ad* (ix-catch (lambda () (array-dimensions 5))))
(assert-equal 5 (domain-error-object *ix-ad*))
(assert-equal '<BASIC-ARRAY> (%%class-name (domain-error-expected-class *ix-ad*)))

;;; ---------------------------------------------------------------------------
;;; 3. subseq の範囲検査(**P4-2 で新設**)
;;; ---------------------------------------------------------------------------
;;
;; **これまで範囲検査がそもそも無かった。** 仕様は 0 <= z1 <= z2 <= (length seq) を
;; 要求している(spec:5367-5376)が、検査が無いため (subseq "abc" 1 99) が
;; 確保済み領域の外を読んでいた。list なら nil の cdr が自分自身なので
;; nil を要求された長さぶん並べて返していた。

(assert-error-class '<program-error> (subseq "abc" 1 99))
(assert-error-class '<program-error> (subseq '(1 2 3) 0 99))
(assert-error-class '<program-error> (subseq (vector 1 2 3) 0 99))
;; z1 > z2 も範囲外
(assert-error-class '<program-error> (subseq "abcdef" 4 1))
;; 境界はエラーにならない(z2 == length は有効)
(assert-equal t (string= "abc" (subseq "abc" 0 3)))
(assert-equal t (string= "" (subseq "abc" 3 3)))
(assert-equal nil (subseq '(1 2 3) 3 3))
(assert-equal t (equal '(2 3) (subseq '(1 2 3) 1 3)))
;; 空リストに対する長さ0の subseq
(assert-equal nil (subseq '() 0 0))
(assert-error-class '<program-error> (subseq '() 0 1))

;;; ---------------------------------------------------------------------------
;;; 4. 評価がその場で打ち切られること
;;; ---------------------------------------------------------------------------

(defglobal *ix-trace* nil)
(defun ix-note (x) (setq *ix-trace* (cons x *ix-trace*)) x)

(setq *ix-trace* nil)
(assert-error-class '<program-error>
  (list (ix-note 'a) (elt '(1 2) 99) (ix-note 'b)))
(assert-equal '(A) (reverse *ix-trace*))

(setq *ix-trace* nil)
(assert-error-class '<domain-error>
  (list (ix-note 'a) (length 5) (ix-note 'b)))
(assert-equal '(A) (reverse *ix-trace*))

;;; ---------------------------------------------------------------------------
;;; 5. JIT 版
;;; ---------------------------------------------------------------------------
;;
;; elt / set-elt は JIT から**固定引数版**(primitive_elt2 / primitive_set_elt3)を
;; 直接呼ぶ経路がある。そちらは env を受け取らないので global-environment を
;; 使って signal する。**その経路でも同じクラスになること**をここで固定する。

(defun ix-elt-jit (s i) (elt s i))
(assert-equal t (%%za-compiled-p (function ix-elt-jit)))
(assert-error-class '<program-error> (ix-elt-jit '(1 2) 99))
(assert-error-class '<domain-error> (ix-elt-jit 5 0))
(assert-equal 2 (ix-elt-jit '(1 2) 1))

(defun ix-set-elt-jit (o s i) (set-elt o s i))
(assert-equal t (%%za-compiled-p (function ix-set-elt-jit)))
(assert-error-class '<program-error> (ix-set-elt-jit 9 (vector 1 2) 99))
(assert-equal 9 (ix-set-elt-jit 9 (vector 1 2) 0))

(defun ix-length-jit (s) (length s))
(assert-equal t (%%za-compiled-p (function ix-length-jit)))
(assert-error-class '<domain-error> (ix-length-jit 5))
(assert-equal 2 (ix-length-jit '(1 2)))

;;; ---------------------------------------------------------------------------
;;; 6. 正常系(変えていないこと)
;;; ---------------------------------------------------------------------------

(assert-equal 3 (length '(a b c)))
(assert-equal 0 (length '()))
(assert-equal 3 (length "abc"))
(assert-equal 'c (elt '(a b c) 2))
(assert-equal #\a (elt "abc" 0))
(assert-equal #\b (string-elt "abc" 1))
(assert-equal '(2) (array-dimensions (vector 'a 'b)))
(assert-equal '(3) (array-dimensions "foo"))
(assert-equal 0 (aref (create-vector 2 0) 1))
