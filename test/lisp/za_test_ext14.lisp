;; test/lisp/za_test_ext14.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張14
;; (tagbodyのネスト対応)を検証するテスト。za_test_ext5.lisp〜za_test_ext13.lispと
;; 同様に独立ファイルとして切り出す。za.cのグラマー全体・za_test.lisp(拡張0〜4)
;; については test/lisp/za_test.lisp のコメントを参照。tagbody/go自体の基本(単一
;; レベル)の挙動は test/lisp/za_test_ext5.lisp を参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。他のext系
;; ファイルへの依存は無い(自己完結、za_test_ext5.lispと同じ趣旨のヘルパーを
;; iz14-プレフィクスで独自に定義する)。
;;
;; za_compile_tagbodyは呼ばれるたびに新規のza_tagbody_ctx_tをスタックローカルに
;; 構築して再帰するだけなので、tagbodyが何重にネストしても各レベルは独立している。
;; goは常に自分に渡されたtb_ctx(=最内側のtagbody)だけを見るため、同名タグの
;; 再利用(for/whileマクロが常に%for-loop/%while-loopを使う等)は変数の
;; シャドーイングと同じ意味論で正しく解決される。内側⇔外側の一方にしか無いタグへの
;; goは、いずれも自分自身のタグ表に無い名前として未解決の前方参照になり、本体走査
;; 終了時に0を返して安全にfallbackする(za.c内のコメント参照)。
;;
;; za.cの+/*のオペランドへのネスト対応は拡張16(za_test_ext16.lisp参照)、引数位置での
;; 一般呼び出しのネストは拡張15(za_test_ext15.lisp参照)でそれぞれ対応済みだが、
;; いずれもtagbodyのネストとは無関係の別対応なので、本ファイルのテストでは
;; ヘルパー呼び出しの引数を常にleaf(paramそのもの・fixnumリテラル)に限定し、
;; 複合値の受け渡しは避ける。

(defglobal *iz14-log* nil)
(defun iz14-log-push (x) (setq *iz14-log* (cons x *iz14-log*)))

(defglobal *iz14-loop-i* 0)
(defglobal *iz14-loop-sum* 0)
(defun iz14-loop-step ()
  (setq *iz14-loop-sum* (+ *iz14-loop-sum* *iz14-loop-i*))
  (setq *iz14-loop-i* (+ *iz14-loop-i* 1)))
(defun iz14-loop-step-or-done (tag n)
  (iz14-loop-step)
  (if (< *iz14-loop-i* n) nil (throw tag t)))

;;; --- 1a. 手書き2重ネストtagbody(前方ジャンプ、内側goが内側自身のタグへ) ---

(defun iz14-nested-tagbody-forward (x)
  (tagbody
   outer-top
    (tagbody
     inner-top
      (if (< x 0) (go skip))
      (iz14-log-push 1)
     skip
      (iz14-log-push 2))
    (iz14-log-push 3)))
(assert-equal t (%%za-compiled-p (function iz14-nested-tagbody-forward)))
(setq *iz14-log* nil)
(assert-equal nil (iz14-nested-tagbody-forward (- 1)))
(assert-equal '(3 2) *iz14-log*)
(setq *iz14-log* nil)
(assert-equal nil (iz14-nested-tagbody-forward 5))
(assert-equal '(3 2 1) *iz14-log*)

;;; --- 1b. 手書き2重ネストtagbody(後方ジャンプ+同名タグのシャドーイング確認) ---
;;
;; 外側・内側の両方が同じタグ名`loop`を持つ。内側の(go loop)が正しく内側自身の
;; `loop`に解決されるなら、外側の`loop`直後にある(iz14-bump-outer-visits)は
;; 1回しか呼ばれない。誤って外側の`loop`にジャンプしてしまうバグがあれば、
;; ループの繰り返し回数と同じ回数呼ばれてしまうはずで、このテストはその違いを
;; 検出できる。

(defglobal *iz14-outer-visits* 0)
(defun iz14-bump-outer-visits () (setq *iz14-outer-visits* (+ *iz14-outer-visits* 1)))

(defun iz14-nested-tagbody-shadow (tag n)
  (tagbody
   loop
    (iz14-bump-outer-visits)
    (tagbody
     loop
      (iz14-loop-step-or-done tag n)
      (go loop))))
(assert-equal t (%%za-compiled-p (function iz14-nested-tagbody-shadow)))
(setq *iz14-loop-i* 0)
(setq *iz14-loop-sum* 0)
(setq *iz14-outer-visits* 0)
(assert-equal t (catch 'iz14-tag-shadow (iz14-nested-tagbody-shadow 'iz14-tag-shadow 4)))
(assert-equal 4 *iz14-loop-i*)
(assert-equal 6 *iz14-loop-sum*)
(assert-equal 1 *iz14-outer-visits*)

;;; --- 2. for-in-for(forマクロはtagbody+block nilへ展開されるため、
;;; forを2重に書くだけでtagbodyが2重ネストする。全レベルで同じタグ
;;; %for-loopが再利用されることを実際に確認する) ---

(defglobal *iz14-nested-for-count* 0)
(defun iz14-bump-nested-for-count () (setq *iz14-nested-for-count* (+ *iz14-nested-for-count* 1)))

(defun iz14-nested-for (n m)
  (for ((i 0 (+ i 1)))
       ((>= i n) nil)
    (for ((j 0 (+ j 1)))
         ((>= j m) nil)
      (iz14-bump-nested-for-count))))
(assert-equal t (%%za-compiled-p (function iz14-nested-for)))
(setq *iz14-nested-for-count* 0)
(assert-equal nil (iz14-nested-for 2 3))
(assert-equal 6 *iz14-nested-for-count*)

;;; --- 3. while-in-while(whileマクロも同じ理屈でtagbodyが2重ネストする。
;;; タグ名は%while-loopで、forとは異なる名前だが両レベルで再利用される点は同じ) ---

(defun iz14-nested-while (n m)
  (let ((i 0))
    (while (< i n)
      (let ((j 0))
        (while (< j m)
          (iz14-bump-nested-for-count)
          (setq j (+ j 1))))
      (setq i (+ i 1)))))
(assert-equal t (%%za-compiled-p (function iz14-nested-while)))
(setq *iz14-nested-for-count* 0)
(iz14-nested-while 2 3)
(assert-equal 6 *iz14-nested-for-count*)

;;; --- 4. 3重ネスト(mandelbrot.lispのy-loop→x-loop→収束loopと同じ深さ) ---

(defun iz14-triple-nested-for (n m k)
  (for ((i 0 (+ i 1)))
       ((>= i n) nil)
    (for ((j 0 (+ j 1)))
         ((>= j m) nil)
      (for ((l 0 (+ l 1)))
           ((>= l k) nil)
        (iz14-bump-nested-for-count)))))
(assert-equal t (%%za-compiled-p (function iz14-triple-nested-for)))
(setq *iz14-nested-for-count* 0)
(iz14-triple-nested-for 2 3 4)
(assert-equal 24 *iz14-nested-for-count*)

;;; --- 5. 内側tagbodyからのgoが外側限定のタグを指す場合は安全にfallback ---
;;
;; 内側tagbody自身は`outer-only`というタグを持たないため、(go outer-only)は
;; 内側自身の未解決タグ表に前方参照として積まれるだけで、内側tagbodyの本体走査が
;; 終わった時点で解決できず0を返す(=defun全体がインタプリタへfallback)。
;; 実際にgoが実行されない引数(x=nil)で呼び、フォールバック後の実行時エラーを避ける
;; (za_test_ext5.lispのisiki-za-test-go-unknown-fallbackと同じ避け方)。

(defun iz14-go-outer-only-fallback (x)
  (tagbody
   outer-only
    (tagbody
      (if x (go outer-only))
      (iz14-log-push 42))))
(assert-equal nil (%%za-compiled-p (function iz14-go-outer-only-fallback)))
(setq *iz14-log* nil)
(assert-equal nil (iz14-go-outer-only-fallback nil))
(assert-equal '(42) *iz14-log*)

;;; --- 6. catchを挟んだネストtagbody(base_nlx_depthの整合性の回帰確認) ---
;;
;; catchでnlx_depthが1つ増えた状態で外側tagbodyが開かれ(base_nlx_depth=1)、その
;; 本体の中に(タグを持たない)内側tagbodyがさらに挟まっていても、外側の(go
;; outer-loop)はnlx_depthを変えずに(内側tagbody自身もcatch/unwind-protectを
;; 持たないため)正しく外側自身のタグへ直接ジャンプできる。

(defun iz14-nested-tagbody-with-catch (tag n)
  (catch tag
    (tagbody
     outer-loop
      (tagbody
        (iz14-loop-step-or-done tag n))
      (go outer-loop))))
(assert-equal t (%%za-compiled-p (function iz14-nested-tagbody-with-catch)))
(setq *iz14-loop-i* 0)
(setq *iz14-loop-sum* 0)
(assert-equal t (iz14-nested-tagbody-with-catch 'iz14-tag-catch 5))
(assert-equal 5 *iz14-loop-i*)
(assert-equal 10 *iz14-loop-sum*)
