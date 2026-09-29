;; test/lisp/frame_variable_test.lisp
;;
;; frame 分離の変数側(defglobal / defconstant / defvar)の回帰テスト。
;; これらも defun と同じく os_definition_env 経由で「最も近い捨てられない環境」へ
;; 登録される。defconstant は値(os_set_variable)と定数フラグ(os_mark_constant)の
;; **両方**を同じ環境へ書かないと、setq での上書き禁止が効かなくなる。

;;; --- defglobal ---

(let ((fv-dummy 1))
  (defglobal *fv-g* 11))
(assert-equal 11 *fv-g*)                ; let を抜けたあとでも参照できる

;; 関数呼び出しの中(CALL-ENV)でも同じ
(defun fv-def-global () (defglobal *fv-g2* 12))
(assert-equal '*fv-g2* (fv-def-global))
(assert-equal 12 *fv-g2*)

;; flet の中でも同じ
(flet ((h () 1))
  (defglobal *fv-g3* 13))
(assert-equal 13 *fv-g3*)

;; 値は setq で書き換えられる(定数ではない)
(defglobal *fv-g4* 1)
(assert-equal 2 (setq *fv-g4* 2))
(assert-equal 2 *fv-g4*)

;;; --- defconstant ---

(let ((fv-dummy 1))
  (defconstant +fv-k+ 21))
(assert-equal 21 +fv-k+)                ; 値が外から見える

;; **定数として扱われること**(値とフラグの登録先が一致している証拠)。
;; 定数への setq は g_sym_eval_error を返す(eval.c の os_is_constant 保護)
;; [P4-3] immutable-binding(spec:435-437)-> <program-error>(spec:7239-7241)
(assert-error-class '<program-error> (setq +fv-k+ 99))
(assert-equal 21 +fv-k+)                ; 値は変わっていない

;; let の外で定義した定数も従来どおり
(defconstant +fv-k2+ 22)
(assert-equal 22 +fv-k2+)
(assert-error-class '<program-error> (setq +fv-k2+ 99))

;; let の中から、外で定義された定数への setq も拒否される
;; (os_is_constant が frame を素通りして親チェーンを辿れていることの確認)
(assert-error-class '<program-error> (let ((fv-dummy 1)) (setq +fv-k+ 98)))
(assert-equal 21 +fv-k+)

;;; --- defvar ---

(let ((fv-dummy 1))
  (defvar *fv-v* 31))
(assert-equal 31 *fv-v*)

;; defvar は既に束縛があれば value-form を評価しない。
;; 検査と書き込みが同じ環境(owner)に対して行われていないと、
;; frame 側に無いからと再評価してしまう
(defvar *fv-v* 32)
(assert-equal 31 *fv-v*)                ; 上書きされない
(let ((fv-dummy 1))
  (defvar *fv-v* 33))
(assert-equal 31 *fv-v*)                ; frame 越しでも上書きされない

;;; --- 環境ごとの変数(維持したい仕様) ---

(defglobal *fv-per-env* 'global)
(defglobal *fv-env* (make-environment 'fv-env))
(defglobal *fv-r* (with-environment *fv-env* (defglobal *fv-per-env* 'local)))
(assert-equal 'global *fv-per-env*)
(assert-equal 'local (%%eval-in-environment '*fv-per-env* *fv-env*))
