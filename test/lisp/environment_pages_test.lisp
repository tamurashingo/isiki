;; test/lisp/environment_pages_test.lisp
;;
;; Phase3(documents/environment.md、環境の7番目のslot「pages」)の動作確認。
;; za_try_compile_defunの成功パスが、コンパイル中にg_jit_codeへ積んだ機械語を
;; Immobilized Spaceの環境所有ページへコピーし、自己参照movabs(jit_movabs_self_ref、
;; シンボル名文字列やLAMBDA-ENV名の埋め込み)を新しい配置先アドレスへパッチすることを、
;; 実機/QEMU上で生成された実際のJITコードを実行して確認する。
;;
;; ネイティブ`make test`はza_try_compile_defunが常にインタプリタへフォールバックする
;; ため(za.c内のISIKIOS_UNIT_TESTガード)この検証はできず、`make test-qemu`でのみ
;; 実行される。C層のみの機構(os_imm_pages_alloc_contiguous、環境のpagesスロット登録、
;; os_environment_reclaim_pages)はtest/c/runtime_test.cのネイティブテストで別途検証済み。
;;
;; 検証範囲は単一環境(このREPLプロセスの環境自身)での再配置の正しさに限る。
;; 親環境/子環境をまたぐシャドウイングの実機確認はPhase4のmake-environment/
;; switch-environment導入後に行う(documents/environment.md参照)。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。

;;; --- 1. 定数のみの単純なdefunも再配置後に正しく実行できる ---

(defun iep-const (x)
  42)
(assert-equal t (%%za-compiled-p (function iep-const)))
(assert-equal 42 (iep-const 1))

;;; --- 2. 他のdefunをシンボル名で呼び出すdefun(呼び出し先シンボル名文字列の
;;;        自己参照movabsを埋め込む、za.c:2381-2382)が、再配置後も正しいシンボル名で
;;;        呼び出し先を解決できる ---

(defun iep-add3 (x y z) (+ x y z))
(assert-equal t (%%za-compiled-p (function iep-add3)))

(defun iep-call-other (x y)
  (iep-add3 x y 1))
(assert-equal t (%%za-compiled-p (function iep-call-other)))
(assert-equal 6 (iep-call-other 2 3))

;;; --- 3. let-localをキャプチャするlambda(LAMBDA-ENV名・パラメータ名の自己参照
;;;        movabsを埋め込む、za.c:1635-1693)が、再配置後も正しくクロージャとして動く ---

(defun iep-make-adder (n)
  (let ((base n))
    (lambda (x) (+ x base))))
(assert-equal t (%%za-compiled-p (function iep-make-adder)))
(let ((add10 (iep-make-adder 10))
      (add100 (iep-make-adder 100)))
  (assert-equal 15 (funcall add10 5))
  (assert-equal 105 (funcall add100 5))
  (assert-equal 15 (funcall add10 5)))

;;; --- 4. 複数のdefunを連続してコンパイルしても、それぞれ別のImmobilized Pageへ
;;;        再配置され、g_jit_code(ステージング用スクラッチバッファ)の再利用によって
;;;        互いのコードが破壊されないこと ---

(defun iep-seq1 (x) (+ x 1))
(defun iep-seq2 (x) (+ x 2))
(defun iep-seq3 (x) (+ x 3))
(assert-equal t (%%za-compiled-p (function iep-seq1)))
(assert-equal t (%%za-compiled-p (function iep-seq2)))
(assert-equal t (%%za-compiled-p (function iep-seq3)))
(assert-equal 11 (iep-seq1 10))
(assert-equal 12 (iep-seq2 10))
(assert-equal 13 (iep-seq3 10))
;; 先に定義したものを後からもう一度呼んでも(スクラッチバッファ再利用後も)壊れていない
(assert-equal 11 (iep-seq1 10))

;;; --- 5. 同一環境内での再defun(既知の制限: 旧ページは回収されずリークするが、
;;;        Function Cell経由の呼び出し先切り替え自体は再配置後も正しく動く) ---

(defun iep-redefine-me (x) (+ x 1))
(assert-equal t (%%za-compiled-p (function iep-redefine-me)))
(assert-equal 11 (iep-redefine-me 10))

(defun iep-redefine-me (x) (+ x 100))
(assert-equal t (%%za-compiled-p (function iep-redefine-me)))
(assert-equal 110 (iep-redefine-me 10))
