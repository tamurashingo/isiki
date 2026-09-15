;; test/lisp/frame_definition_test.lisp
;;
;; frame 分離(os_make_frame / os_definition_env)の回帰テスト。
;;
;; インタプリタは関数適用・マクロ展開・flet/labels・クロージャ捕捉のたびに
;; 新しい環境を作るが、それらは呼び出しが終われば捨てられる。定義をそこへ書くと
;; 到達不能になるため、defun 等は「最も近い捨てられない環境」へ登録する。
;;
;; 同時に、次の2つを壊していないことを確認する:
;;   - 自由変数の捕捉(関数オブジェクトの word3 は**呼び出し時の env** のまま)
;;   - 環境ごとに関数を定義/上書きできる仕様

;;; --- let の中の defun ---

(let ((fd-x 1))
  (defun fd-f () fd-x))
(assert-equal 1 (fd-f))                 ; let を抜けたあとでも呼べる

;; 捕捉が壊れていないこと: let-local を書き換えたら追従する(遅延束縛)
(let ((fd-y 1))
  (defun fd-g () fd-y)
  (assert-equal 1 (fd-g))
  (setq fd-y 2)
  (assert-equal 2 (fd-g)))
(assert-equal 2 (fd-g))                 ; 抜けたあとも最後の値が見える

;; 自由変数を持たない場合
(let ((fd-z 1))
  (defun fd-h () 42))
(assert-equal 42 (fd-h))

;; 多重の let を抜けても届く
(let ((a 1))
  (let ((b 2))
    (let ((c 3))
      (defun fd-nested () (+ a (+ b c))))))
(assert-equal 6 (fd-nested))

;;; --- defun の中の defun ---

(defun fd-outer () (defun fd-inner () 42))
(assert-equal 'fd-inner (fd-outer))
(assert-equal 42 (fd-inner))

;; 引数を捕捉する場合
(defun fd-outer2 (n) (defun fd-inner2 () n))
(assert-equal 'fd-inner2 (fd-outer2 7))
(assert-equal 7 (fd-inner2))

;;; --- flet / labels の中の defun ---

(flet ((fd-local () 1))
  (defun fd-from-flet () 2))
(assert-equal 2 (fd-from-flet))

(labels ((fd-local2 () 1))
  (defun fd-from-labels () 3))
(assert-equal 3 (fd-from-labels))

;;; --- flet / labels の束縛が外へ漏れていないこと(仕様の維持) ---

(flet ((fd-must-not-leak () 99))
  (assert-equal 99 (fd-must-not-leak)))
;; 外からは見えない(未定義の関数呼び出しはシンボル EVAL-ERROR を値として返す)
(assert-equal 'eval-error (fd-must-not-leak))

(labels ((fd-must-not-leak2 () 98))
  (assert-equal 98 (fd-must-not-leak2)))
(assert-equal 'eval-error (fd-must-not-leak2))

;; flet の束縛が外側の同名定義を一時的に隠し、抜けたら戻ること
(defun fd-shadowed () 'outer)
(assert-equal 'outer (fd-shadowed))
(flet ((fd-shadowed () 'inner))
  (assert-equal 'inner (fd-shadowed)))
(assert-equal 'outer (fd-shadowed))

;;; --- or / case も let へ展開されるので同じ経路を通る ---

(defglobal *fd-or* (or nil (progn (defun fd-from-or () 11) 'done)))
(assert-equal 11 (fd-from-or))

;;; --- defmacro も同じ経路 ---

(let ((fd-w 1))
  (defmacro fd-mac (x) `(+ ,x 100)))
(assert-equal 105 (fd-mac 5))

;;; --- 環境ごとの関数定義(維持したい仕様) ---

(defglobal *fd-env* (make-environment 'fd-env))
(assert-equal 'fd-in-env (with-environment *fd-env* (defun fd-in-env () 7)))
;; 対象環境では呼べる
(assert-equal 7 (%%eval-in-environment '(fd-in-env) *fd-env*))
;; グローバルからは見えない
(assert-equal 'eval-error (fd-in-env))

;; 同名関数を環境ごとに別定義できる
(defun fd-per-env () 'global)
(defglobal *fd-env2* (make-environment 'fd-env2))
(defglobal *fd-r2* (with-environment *fd-env2* (defun fd-per-env () 'local)))
(assert-equal 'global (fd-per-env))
(assert-equal 'local (%%eval-in-environment '(fd-per-env) *fd-env2*))
