;; test/lisp/za_test_ext17.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張17
;; (flet)を検証するテスト。za_test_ext5.lisp〜za_test_ext16.lispと同様に
;; 独立ファイルとして切り出す。za.cのグラマー全体・za_test.lisp(拡張0〜4)
;; については test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結、iz17-プレフィクスで独自にヘルパーを定義する)。
;;
;; flet/labelsは、束縛関数名をgensymへ差し替え、実行時にgensym経由でglobal_environment
;; 上へ「保存→新しい値を設定→(bodyを評価)→無条件に復元」する(unwind-protectと同じ
;; save/restore規約)。束縛関数の本体自体は常にインタプリタ実行であり、そのクロージャ
;; 環境はeval_flet(eval.c)と同じ「外側env」なので、束縛関数自身・兄弟束縛関数は
;; 見えない(自己/相互再帰不可)ことがflet本来の仕様であり、za.c側の実装もこれを
;; 変えない(za_compile_flet_labels定義直前のコメント参照)。
;;
;; labels(1〜7はflet、8以降がlabels)はeval_labelsの意味論通り束縛関数の本体からも
;; 自身・兄弟束縛が見える(自己/相互再帰可能)。za.cはこれをza_rewrite_fn_refsで
;; 束縛body自身のASTを書き換え、呼び出しhead位置・(function name)のname位置に現れる
;; 束縛名をgensymへ差し替えることで対応する(za_rewrite_fn_refsのコメント参照)。

;;; --- 1. flet基本 ---

(defun iz17-flet-basic (x)
  (flet ((double (y) (* y 2)))
    (double x)))
(assert-equal t (%%za-compiled-p (function iz17-flet-basic)))
(assert-equal 10 (iz17-flet-basic 5))

;;; --- 2. 外側の同名関数をシャドーイングし、fletの動的extentを抜けると復元される ---

(defun iz17-adder (n) (+ n 1000))
(defun iz17-flet-shadow (x)
  (flet ((iz17-adder (y) (+ y 1)))
    (iz17-adder x)))
(assert-equal t (%%za-compiled-p (function iz17-flet-shadow)))
(assert-equal 6 (iz17-flet-shadow 5))
;; flet突入前の外側定義がそのまま復元されていること(save/restoreの後始末確認)。
(assert-equal 1005 (iz17-adder 5))

;;; --- 3. 束縛関数の本体自身からは自身・兄弟束縛が見えず、常に外側解釈になる ---
;;
;; eval_flet(eval.c)の意味論通り、束縛関数の本体のクロージャ環境は外側envのままなので、
;; 本体内で自分と同名の関数を呼ぶと、束縛前の外側定義(=iz17-count-outer)が呼ばれる。
;; 無限再帰にもエラーにもならず、常に外側解釈で終わることを確認する。

(defun iz17-count (n) 'outer-count-called)
(defun iz17-flet-self-shadow (x)
  (flet ((iz17-count (n) (if (< n 1) 'base (iz17-count (- n 1)))))
    (iz17-count x)))
(assert-equal t (%%za-compiled-p (function iz17-flet-self-shadow)))
(assert-equal 'outer-count-called (iz17-flet-self-shadow 5))
;; flet突入前の外側定義がそのまま復元されていること。
(assert-equal 'outer-count-called (iz17-count 999))

;;; --- 4. 複数バインディング ---

(defun iz17-flet-two-bindings (a b)
  (flet ((f1 (x) (+ x a))
         (f2 (x) (* x b)))
    (+ (f1 1) (f2 2))))
(assert-equal t (%%za-compiled-p (function iz17-flet-two-bindings)))
(assert-equal 15 (iz17-flet-two-bindings 10 2))

;;; --- 5. return-fromがflet本体を飛び越えても、保存した旧バインディングは
;;;        必ず復元される(unwind-protectと同じcleanup規約) ---

(defun iz17-flet-return-from (x)
  (block done
    (flet ((f (y) (+ y 1)))
      (if (< x 0) (return-from done 'neg) (f x)))))
(assert-equal t (%%za-compiled-p (function iz17-flet-return-from)))
(assert-equal 'neg (iz17-flet-return-from -1))
(assert-equal 6 (iz17-flet-return-from 5))

;;; --- 6. JIT化されたouter defunが再帰する中でflet形式へ何度も再入する ---
;;
;; 各再帰呼び出しは自分自身のスタックフレーム上でsave/restoreを行うため、内側の
;; 再帰呼び出しが復元した時点で外側呼び出しのバインディングへ正しく戻ることを確認する
;; (LIFOの再入安全性。stepはnを自分のフレームのキャプチャenv経由で捉える)。

(defun iz17-flet-reentrant (n)
  (if (< n 1)
      0
      (flet ((step (y) (+ y n)))
        (+ (step 0) (iz17-flet-reentrant (- n 1))))))
(assert-equal t (%%za-compiled-p (function iz17-flet-reentrant)))
(assert-equal 15 (iz17-flet-reentrant 5))

;;; --- 7. (function bound-name)で束縛関数を動的extentの外へ持ち出すケースは
;;;        無条件fallbackし、インタプリタ経由の結果は仕様通り正しいままである ---

(defun iz17-flet-escape (x)
  (flet ((sq (y) (* y y)))
    (function sq)))
(assert-equal nil (%%za-compiled-p (function iz17-flet-escape)))
(assert-equal 9 (funcall (iz17-flet-escape 100) 3))

;;; --- 8. labels自己再帰 ---
;;
;; eval_labelsの意味論により束縛関数の本体からは自分自身が見える(fletとの違い)。
;; za_rewrite_fn_refsが本体内の自己再帰呼び出しをgensym経由で解決できるよう書き換える
;; ため、fletと同じくJIT化される。

(defun iz17-labels-fact (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (fact n)))
(assert-equal t (%%za-compiled-p (function iz17-labels-fact)))
(assert-equal 120 (iz17-labels-fact 5))

;;; --- 9. labels相互再帰 ---
;;
;; 同じlabelsで同時に束縛された兄弟関数も、gensym割り当て完了後にリライトするため
;; 互いに参照可能。

(defun iz17-labels-mutual (n)
  (labels ((is-even (k) (if (= k 0) t (is-odd (- k 1))))
           (is-odd (k) (if (= k 0) nil (is-even (- k 1)))))
    (is-even n)))
(assert-equal t (%%za-compiled-p (function iz17-labels-mutual)))
(assert-equal t (iz17-labels-mutual 10))
(assert-equal nil (iz17-labels-mutual 7))

;;; --- 10. labels束縛関数の本体内部に、同名を再束縛する入れ子labelsがあるケース。
;;;         za_rewrite_fn_refsが内側束縛名用にgensym_slot_addr=0(未確定センチネル)の
;;;         一時スコープフレームを積んで再帰するため、内側のfへの参照はセンチネルとして
;;;         見つかり書き換え対象から外れる(=前処理時点では素通りし、実行時のeval_labels
;;;         による通常のレキシカル評価で内側のfが正しくシャドーイングする)。これにより
;;;         フォールバックせずJIT化される(結果は従来通り内側のfが呼ばれ105)。 ---

(defun iz17-labels-nested-shadow (n)
  (labels ((f (k) (labels ((f (j) (+ j 100))) (f k))))
    (f n)))
(assert-equal t (%%za-compiled-p (function iz17-labels-nested-shadow)))
(assert-equal 105 (iz17-labels-nested-shadow 5))

;;; --- 11. labels本体内でユーザーマクロ(let)を使ってもza_rewrite_fn_refsが
;;;         za_macroexpandで展開してから判定するため、fallbackせずJIT化される ---

(defun iz17-labels-with-let (n)
  (labels ((helper (k) (let ((doubled (* k 2))) (if (< k 1) doubled (helper (- k 1))))))
    (helper n)))
(assert-equal t (%%za-compiled-p (function iz17-labels-with-let)))
(assert-equal 0 (iz17-labels-with-let 3))

;;; --- 12. labels本体内で(function bound-name)により束縛関数自身を取り出すケースは
;;;         (fletの7と同様)無条件fallbackし、インタプリタ経由の結果は正しいままである ---

(defun iz17-labels-function-escape (n)
  (labels ((fact (k) (if (< k 2) 1 (* k (fact (- k 1))))))
    (funcall (function fact) n)))
(assert-equal nil (%%za-compiled-p (function iz17-labels-function-escape)))
(assert-equal 120 (iz17-labels-function-escape 5))
