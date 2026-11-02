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
;; 外からは見えない([P4-3] 未定義の関数呼び出しは <undefined-function> を signal する)
(assert-error-class '<undefined-function> (fd-must-not-leak))

(labels ((fd-must-not-leak2 () 98))
  (assert-equal 98 (fd-must-not-leak2)))
(assert-error-class '<undefined-function> (fd-must-not-leak2))

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
;; グローバルからは見えない([P4-3] <undefined-function>)
(assert-error-class '<undefined-function> (fd-in-env))

;; 同名関数を環境ごとに別定義できる
(defun fd-per-env () 'global)
(defglobal *fd-env2* (make-environment 'fd-env2))
(defglobal *fd-r2* (with-environment *fd-env2* (defun fd-per-env () 'local)))
(assert-equal 'global (fd-per-env))
(assert-equal 'local (%%eval-in-environment '(fd-per-env) *fd-env2*))

;;; --- Immobilized Page の所有者が frame ではなく environment であること ---
;;
;; za_try_compile_defun は「捕捉環境(capture_env)」と「登録先(owner_env)」を
;; 別々に受け取る。ページとリテラルスロットを owner_env へ登録することで、
;; destroy-environment の回収対象に入る。frame へ登録していた頃は
;; frame が捨てられるだけでページは永久に残っていた。
;;
;; %%imm-space-used-bytes は bump 基準(高水位)なので回収では減らない。
;; 代わりに「回収後はフリーリストからページが取れるので bump が進まない」ことを、
;; Function Cell の大量確保(インタプリタの flet 1回 = 16 byte)で観測する。

(defun fd-cellburn (x) (flet ((h () 1)) (setq x 0) (h)))   ; 固定引数setqでインタプリタ実行
(defun fd-drive (n) (let ((i 0)) (while (< i n) (progn (fd-cellburn 1) (setq i (+ i 1))))))
(assert-equal nil (%%za-compiled-p (function fd-cellburn)))

;; 回収前: 512回 = 8192 byte = ちょうど2ページ分を bump から取る
(defglobal *fd-a0* (%%imm-space-used-bytes))
(defglobal *fd-w0* (fd-drive 512))
(defglobal *fd-a1* (%%imm-space-used-bytes))
(defglobal *fd-c0* (- *fd-a1* *fd-a0*))   ; 対照: 回収前に消費した量

;; 環境を作り、let 越しに defun してから破棄する
(defglobal *fd-e* (make-environment 'fd-reclaim-env))
(defglobal *fd-r1* (with-environment *fd-e* (let ((q 1)) (defun fd-re1 () q))))
(defglobal *fd-r2* (with-environment *fd-e* (let ((q 2)) (defun fd-re2 () q))))
(defglobal *fd-r3* (with-environment *fd-e* (let ((q 3)) (defun fd-re3 () q))))
(defglobal *fd-r4* (with-environment *fd-e* (let ((q 4)) (defun fd-re4 () q))))

;; frame ではなく環境 E に登録されていること。自由変数も見えること
(assert-equal 1 (%%eval-in-environment '(fd-re1) *fd-e*))
(assert-equal 4 (%%eval-in-environment '(fd-re4) *fd-e*))

(defglobal *fd-a2* (%%imm-space-used-bytes))
(assert-equal t (destroy-environment *fd-e*))
;; 破棄しても bump 基準の使用量は減らない(回収はフリーリストへの返却)
(assert-equal *fd-a2* (%%imm-space-used-bytes))

;; 破棄で解放されたページがフリーリストへ入るので、続く同じ処理は
;; bump の消費が減る。
;;
;; [期待値の変更 2026-09-15] 以前はここが「0 byte」だった。JITコードのパッキング
;; (documents/measurement-jit-code-length-distribution.md、os_imm_code_alloc)を
;; 入れる前は fd-re1〜fd-re4 の4関数が**4ページを専有**していたため、破棄すると
;; 4ページ返り、続く2ページ分の確保が全てフリーリストから賄えていた。
;; パッキング後は同一環境の4関数が**1ページに同居する**ので、返るのは1ページだけ。
;; 2ページ必要な処理のうち1ページはフリーリストから、1ページは bump から取る。
;;
;; つまり「0 になる」という性質はパッキングによって失われたが、
;; **検証したい性質(回収したページがフリーリストを経由して再利用される)は
;; 「同じ処理の消費が回収前より減る」で変わらず張れる。**
;; ページ数に依存しない形にすることで、関数サイズが変わっても壊れなくなる。
(defglobal *fd-w1* (fd-drive 512))
(defglobal *fd-c1* (- (%%imm-space-used-bytes) *fd-a2*))
(assert-equal t (> *fd-c0* 0))          ; 対照: 回収前は bump を消費していた
(assert-equal t (< *fd-c1* *fd-c0*))    ; 回収後は減る(= フリーリストから取れている)
