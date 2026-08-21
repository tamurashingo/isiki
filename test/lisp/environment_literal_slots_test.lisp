;; test/lisp/environment_literal_slots_test.lisp
;;
;; Phase3.6(documents/environment.md、リテラルスロットプール枯渇の解決)の動作確認。
;; g_za_quote_slots/g_za_number_slots/g_za_lambda_slots(za.c、各上限40。let-local変数の
;; box昇格対応により、以前フォールバックしていた「setq+エスケープするlambda捕捉」を
;; 含む関数がzaでコンパイルされるようになった分の追加消費を吸収するため32から拡張した)
;; はコンパイル試行ごとに確保されるが、コンパイルが最終的にフォールバック(インタプリタ
;; 委譲)する場合、そのコンパイル試行が確保したスロットはza_release_literal_slot_allocs
;; 経由で即座にフリーリストへ返却される(za_try_compile_defunの各失敗exitに仕込んだ処理)。
;;
;; ネイティブ`make test`はza_try_compile_defunが常にインタプリタへフォールバックする
;; ため(za.c内のISIKIOS_UNIT_TESTガード)この検証はできず、`make test-qemu`でのみ
;; 実行される。C層のみの機構(os_gc_unregister_root、環境のliteral-slotsスロット登録、
;; os_environment_reclaim_literal_slots)はtest/c/runtime_test.cのネイティブテストで
;; 別途検証済み。
;;
;; 検証範囲は「同一のコンパイル試行内で確保→即時解放」というfailure-exit経路に限る。
;; environment_pages_test.lispの5番(同一環境内での再defun)と同じ理由で、
;; 「同一環境内で成功したJITコンパイルを再defunした場合」の旧スロット回収は本テストの
;; 対象外(既知の制限、環境破棄時にのみ回収されるため、destroy-environmentが無い
;; Phase4以前は同一環境内での確認ができない)。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。
;; za_test_ext17.lispの7番(iz17-flet-escape、(function bound-name)による束縛関数の
;; 動的extent外への持ち出し)と同じフォールバックパターンを利用する。このパターンは
;; flet本体のコンパイル自体はいったん進み(gensym用quote-slot 1個、クロージャ用
;; lambda-slot 1個を確保した後)、g_za_saw_flet_labels_escapeが立って最終的に
;; フォールバックする。各プールの上限(40)を優に超える33+5=38回、この確保→フォール
;; バックを繰り返しても失敗しないことを確認する(1回ごとに確保したスロットは即座に
;; フリーリストへ返却されるため、繰り返し回数自体がプールの恒久消費には影響しない
;; ことの確認でもある)。

;;; --- 1. escapeするflet定義を32回を超えて繰り返し再定義しても、
;;;        毎回正しくフォールバックし、結果も正しいままである ---

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

(defun iels-escaper (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iels-escaper)))
(assert-equal 9 (funcall (iels-escaper 100) 3))

;;; --- 2. labels版のescapeパターン(iz17-labels-function-escapeと同じ形)でも
;;;        同様に繰り返し確認する ---

(defun iels-labels-escaper (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (funcall (function fact) n)))
(assert-equal nil (%%za-compiled-p (function iels-labels-escaper)))
(assert-equal 120 (iels-labels-escaper 5))

(defun iels-labels-escaper (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (funcall (function fact) n)))
(assert-equal nil (%%za-compiled-p (function iels-labels-escaper)))
(assert-equal 120 (iels-labels-escaper 5))

(defun iels-labels-escaper (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (funcall (function fact) n)))
(assert-equal nil (%%za-compiled-p (function iels-labels-escaper)))
(assert-equal 120 (iels-labels-escaper 5))

(defun iels-labels-escaper (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (funcall (function fact) n)))
(assert-equal nil (%%za-compiled-p (function iels-labels-escaper)))
(assert-equal 120 (iels-labels-escaper 5))

(defun iels-labels-escaper (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (funcall (function fact) n)))
(assert-equal nil (%%za-compiled-p (function iels-labels-escaper)))
(assert-equal 120 (iels-labels-escaper 5))

;;; --- 3. 上記の33回超のescape→フォールバックを経た後でも、新規のflet/labels定義が
;;;        (フリーリスト返却により)quote-slot/lambda-slotとも枯渇せずJIT化できる ---

(defun iels-fresh-flet (x)
  (flet ((double (y) (* y 2)))
    (double x)))
(assert-equal t (%%za-compiled-p (function iels-fresh-flet)))
(assert-equal 20 (iels-fresh-flet 10))

(defun iels-fresh-labels (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (fact n)))
(assert-equal t (%%za-compiled-p (function iels-fresh-labels)))
(assert-equal 120 (iels-fresh-labels 5))
