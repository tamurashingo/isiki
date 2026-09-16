;; test/lisp/declaim_test.lisp
;;
;; declaim (Phase 2: optimize のみ) の回帰テスト。documents/declaim-design.md
;;
;; **CommonLisp の declaim とは意味論が異なる。** environment 単位で作用し、
;; 親からは引き継がない。declaim を保持するのは environment のみで、
;; let/flet/labels/関数呼び出しが作る frame は持たない(os_definition_env が
;; 読み飛ばして囲みの environment へ届く)。
;;
;; 本 Phase では生成コードは変わらない。値が正しく記録・参照されるかだけを見る。

;;; --- 6-1. 基本動作: 指定しなかった項目は変更されない ---
(assert-equal '(1 1 1) (%%current-optimize))
(declaim (optimize (speed 3)))
(assert-equal '(3 1 1) (%%current-optimize))
(declaim (optimize (safety 0)))
(assert-equal '(3 0 1) (%%current-optimize))   ; speed は保たれる
(declaim (optimize (space 2)))
(assert-equal '(3 0 2) (%%current-optimize))
;; 複数を一度に、かつ複数の宣言指定子を一度に
(declaim (optimize (speed 1) (safety 1)) (optimize (space 1)))
(assert-equal '(1 1 1) (%%current-optimize))

;;; --- 6-2. frame の透過: let/flet/labels/関数本体の中でも environment に届く ---
(declaim (optimize (speed 3)))
(let ((x 1))
  (defun dcl-f (a) a))
(assert-equal '(3 1 1) (%%optimize-of 'dcl-f))    ; let の frame は読み飛ばされる
(assert-equal t (%%za-compiled-p (function dcl-f)))

(flet ((dcl-h () 1))
  (defun dcl-f2 (a) a))
(assert-equal '(3 1 1) (%%optimize-of 'dcl-f2))

(labels ((dcl-h2 () 1))
  (defun dcl-f3 (a) a))
(assert-equal '(3 1 1) (%%optimize-of 'dcl-f3))

;; frame の中で declaim しても、囲みの environment へ記録される
(let ((y 1))
  (declaim (optimize (safety 3))))
(assert-equal '(3 3 1) (%%current-optimize))      ; let を抜けても残っている
(declaim (optimize (safety 1)))

;; 関数本体(= 呼び出しが作る frame)の中で declaim しても同じ。
;; declaim は za_is_excluded_special_form に入れてあるので、本体に declaim を含む
;; 関数は JIT されずインタプリタで動く(za.c が declaim の機械語を出せないため。
;; 入れ忘れると一般呼び出しとしてコンパイルされ EVAL-ERROR になる)
(defun dcl-setter () (declaim (optimize (space 3))))
(assert-equal nil (%%za-compiled-p (function dcl-setter)))
(assert-equal nil (dcl-setter))
(assert-equal '(3 1 3) (%%current-optimize))
(declaim (optimize (space 1)))

;;; --- 6-3. environment の分離: 親から引き継がず、互いに波及しない ---
(declaim (optimize (speed 3)))
(assert-equal '(3 1 1) (%%current-optimize))

(defglobal *dcl-e* (make-environment 'dcl-env))
;; 親(global)は speed 3 だが、新しい environment は既定値から始まる
(assert-equal '(1 1 1) (%%eval-in-environment '(%%current-optimize) *dcl-e*))
(%%eval-in-environment '(defun dcl-g (a) a) *dcl-e*)
(assert-equal '(1 1 1) (%%eval-in-environment '(%%optimize-of 'dcl-g) *dcl-e*))

;; E での declaim は global へ波及しない
(%%eval-in-environment '(declaim (optimize (speed 0) (safety 3))) *dcl-e*)
(assert-equal '(0 3 1) (%%eval-in-environment '(%%current-optimize) *dcl-e*))
(assert-equal '(3 1 1) (%%current-optimize))      ; global は無傷

;; 逆方向も。global の変更は E へ波及しない
(declaim (optimize (space 2)))
(assert-equal '(0 3 1) (%%eval-in-environment '(%%current-optimize) *dcl-e*))
(declaim (optimize (space 1)))

;; sibling 同士も独立している(F1〜F4 のプロセス環境と同じ性質。
;; プロセス環境も os_make_environment で作られる environment なので、
;; F1 の declaim は F2 へ影響しない。documents/declaim-design.md 参照)
(defglobal *dcl-e2* (make-environment 'dcl-env2))
(assert-equal '(1 1 1) (%%eval-in-environment '(%%current-optimize) *dcl-e2*))
(%%eval-in-environment '(declaim (optimize (speed 2))) *dcl-e2*)
(assert-equal '(2 1 1) (%%eval-in-environment '(%%current-optimize) *dcl-e2*))
(assert-equal '(0 3 1) (%%eval-in-environment '(%%current-optimize) *dcl-e*))

;;; --- 6-4. 定義済み関数には影響しない ---
(declaim (optimize (speed 1)))
(defun dcl-h3 (a) a)
(assert-equal '(1 1 1) (%%optimize-of 'dcl-h3))
(declaim (optimize (speed 3)))
(assert-equal '(1 1 1) (%%optimize-of 'dcl-h3))   ; 定義済みは変わらない
(defun dcl-h3 (a) a)                              ; 再定義すると新しい値で作り直される
(assert-equal '(3 1 1) (%%optimize-of 'dcl-h3))

;;; --- 6-5. エラーと無視 ---
(declaim (optimize (speed 1) (safety 1) (space 1)))
;; 値域外はエラー
(assert-error (declaim (optimize (speed 4))))
(assert-error (declaim (optimize (speed -1))))
;; エラーになっても、それ以前の値は壊れていない
(assert-equal '(1 1 1) (%%current-optimize))
;; optimize 以外の指定子は無視される(Phase 3 以降で実装予定のものを
;; 先に書いたコードが動かなくなるのを避けるため)
(assert-equal nil (declaim (type fixnum x)))
(assert-equal nil (declaim (inline dcl-f)))
(assert-equal nil (declaim (notinline dcl-f)))
(assert-equal '(1 1 1) (%%current-optimize))
;; 未知の quality も無視される
(declaim (optimize (debug 3)))
(assert-equal '(1 1 1) (%%current-optimize))
;; 戻り値は nil
(assert-equal nil (declaim (optimize (speed 2))))

;;; --- 6-7. GC 後も値が保たれること(Cheney コピーでスロットが複製される) ---
(defun dcl-force-gc ()
  (let ((c (%%gc-collect-count)))
    (while (= (%%gc-collect-count) c) (cons 1 2))
    (%%gc-collect-count)))

(declaim (optimize (speed 3) (safety 0) (space 2)))
(defun dcl-gc-fn (a) a)
(assert-equal '(3 0 2) (%%current-optimize))
(assert-equal '(3 0 2) (%%optimize-of 'dcl-gc-fn))
(dcl-force-gc)
(assert-equal '(3 0 2) (%%current-optimize))       ; environment のスロットが生きている
(assert-equal '(3 0 2) (%%optimize-of 'dcl-gc-fn)) ; meta(Immobilized)も生きている
(dcl-force-gc)
(dcl-force-gc)
(assert-equal '(3 0 2) (%%current-optimize))
(assert-equal '(3 0 2) (%%optimize-of 'dcl-gc-fn))
;; 子環境の値も GC を跨いで保たれる
(assert-equal '(0 3 1) (%%eval-in-environment '(%%current-optimize) *dcl-e*))
;; GC 後も declaim を書き換えられる
(declaim (optimize (speed 1) (safety 1) (space 1)))
(assert-equal '(1 1 1) (%%current-optimize))

;;; --- インタプリタ実行の関数は meta を持たないので nil ---
(defun dcl-nojit (x) (progn (setq x 0) x))
(assert-equal nil (%%za-compiled-p (function dcl-nojit)))
(assert-equal nil (%%optimize-of 'dcl-nojit))
