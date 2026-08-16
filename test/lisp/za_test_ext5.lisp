;; test/lisp/za_test_ext5.lisp
;;
;; za.c(defunをx86-64機械語へJITコンパイルするコンパイラ)の拡張5(非局所脱出:
;; block/return-from, catch/throw, tagbody/go, unwind-protect)を検証するテスト。
;; 元は test/lisp/za_test.lisp の一部だったが、QEMUテストのmilestone分割
;; (実行時間が伸びてGitHub Actions上でハングと正常進行の区別がつかなくなった
;; ことへの対策)のため別ファイルへ切り出した。za.cのグラマー全体・za_test.lisp
;; (拡張0〜4)については test/lisp/za_test.lisp のコメントを参照。
;;
;; 本ファイルは test/lisp/test_framework.lisp が定義する assert-equal 等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。za_test.lisp
;; (拡張0〜4)の関数への依存は無い(補助defglobal/defunは本ファイル内で自己完結)。

;;; --- 拡張5: 非局所脱出(block/return-from, catch/throw, tagbody/go, unwind-protect) ---
;;
;; isiki-osにはsetjmp/longjmpが無いため、いずれもeval.cのeval_block/eval_return_from/
;; eval_catch/eval_throw/eval_tagbody/eval_go/eval_unwind_protectと同じ「制御転送値
;; (MAGIC_BLOCK_EXIT/MAGIC_CATCH_EXIT/MAGIC_GO_EXITのTAG_INSTANCE)を戻り値として
;; 上位の呼び出し元へバケツリレーする」方式を機械語化する。ただしtagbody/goのみ、
;; 宛先ラベルが同一JITコンパイル対象関数内に静的に存在するためコンパイル時に位置を
;; 解決して直接jmpする(実行時に制御転送値は作らない)。
;;
;; zaはsetqに対応していないため、tagbody/goによる本物の反復・副作用の観測には
;; setqを使う(za非対応でインタプリタにフォールバックする)別のヘルパー関数を用意し、
;; JITコンパイル対象の関数からは普通の関数呼び出しとして呼ぶ。

(defglobal *isiki-za-test-log* nil)
(defun isiki-za-test-log-push (x) (setq *isiki-za-test-log* (cons x *isiki-za-test-log*)))

(defglobal *isiki-za-test-cleanup-count* 0)
(defun isiki-za-test-bump-cleanup () (setq *isiki-za-test-cleanup-count* (+ *isiki-za-test-cleanup-count* 1)))

(defglobal *isiki-za-test-loop-i* 0)
(defglobal *isiki-za-test-loop-sum* 0)
(defun isiki-za-test-loop-step ()
  (setq *isiki-za-test-loop-sum* (+ *isiki-za-test-loop-sum* *isiki-za-test-loop-i*))
  (setq *isiki-za-test-loop-i* (+ *isiki-za-test-loop-i* 1)))

;; za_classify_operandはleaf(fixnumリテラルまたはparams参照)しか受け付けないため、
;; グローバル変数への参照は`<`や一般呼び出しの引数として直接は書けず、ifのtest位置に
;; 書いても(< *isiki-za-test-loop-i* n)自体がコンパイル失敗しifへ、さらにtagbody/defun
;; 全体へフォールバックが伝播してしまう。またifのtest位置は常にallow_call=0で
;; コンパイルされるため一般呼び出しの結果も直接は使えない。そこで後方ジャンプの
;; ループを試すテストでは、継続判定自体をza非対応のヘルパー(setq/グローバル変数
;; 比較を使う)へ切り出し、判定がfalseになった時点でヘルパー自身がthrowして
;; ループを脱出する設計にする(継続時はnilを返すだけでtagbody側は無条件にgoで
;; 戻る)。これによりJITコンパイル対象側にはleaf限定の比較を一切書かずに済む
(defun isiki-za-test-loop-step-or-done (tag n)
  (isiki-za-test-loop-step)
  (if (< *isiki-za-test-loop-i* n) nil (throw tag t)))

;; --- block/return-from ---

;; return-fromに遭遇しなければbody最後の値をそのまま返す
(defun isiki-za-test-block-normal (x) (block done (+ x 1)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-block-normal)))
(assert-equal 6 (isiki-za-test-block-normal 5))

;; ネストしたifの中のreturn-fromで早期脱出し、以降のbody要素は評価されない
(defun isiki-za-test-block-return (x)
  (block done
    (if (< x 0) (return-from done 0))
    (+ x 1)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-block-return)))
(assert-equal 0 (isiki-za-test-block-return (- 5)))
(assert-equal 6 (isiki-za-test-block-return 5))

;; (return-from name) : 値を省略するとnilを返す
(defun isiki-za-test-block-return-novalue (x)
  (block done
    (if (< x 0) (return-from done))
    (+ x 1)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-block-return-novalue)))
(assert-equal nil (isiki-za-test-block-return-novalue (- 1)))
(assert-equal 6 (isiki-za-test-block-return-novalue 5))

;; return-fromがifのtest位置に現れる場合も制御転送がifを素通りしてblockまで伝播する
(defun isiki-za-test-block-return-in-test (x)
  (block done
    (if (return-from done x) 2 3)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-block-return-in-test)))
(assert-equal 5 (isiki-za-test-block-return-in-test 5))

;; return-fromが一般呼び出しの引数位置に現れる場合、残りの引数評価・呼び出し自体を
;; 中止してそのまま制御転送がblockまで伝播する(呼び出し先関数は実行されない)
(defun isiki-za-test-echo2 (a b) b)
(defun isiki-za-test-block-return-in-callarg (x)
  (block done
    (isiki-za-test-echo2 (return-from done x) 99)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-block-return-in-callarg)))
(assert-equal 7 (isiki-za-test-block-return-in-callarg 7))
(close (open-output-file "tmp/ckpt-20-block.txt"))

;; --- catch/throw ---

;; 同一関数内でcatch/throwのtagが一致する基本ケース
(defun isiki-za-test-catch-throw-same (tag)
  (catch tag (throw tag 42)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-catch-throw-same)))
(assert-equal 42 (isiki-za-test-catch-throw-same 'isiki-za-test-tag-a))

;; throwに遭遇しなければcatchはbody最後の値をそのまま返す
(defun isiki-za-test-catch-basic (tag)
  (catch tag (+ 1 2)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-catch-basic)))
(assert-equal 3 (isiki-za-test-catch-basic 'isiki-za-test-tag-b))

;; 動的スコープを跨ぐthrow: catchを持つ関数が別のza関数を呼び、その中でthrowする
(defun isiki-za-test-throw-helper (tag val) (throw tag val))
(defun isiki-za-test-catch-calls-helper (tag)
  (catch tag (isiki-za-test-throw-helper tag 77)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-throw-helper)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-catch-calls-helper)))
(assert-equal 77 (isiki-za-test-catch-calls-helper 'isiki-za-test-tag-c))

;; tagが一致しないcatchはthrowを素通しし、外側の一致するcatchまで伝播する
;; (nlx_depthが0/1と2段ネストしても互いに衝突しないことの確認)
(defun isiki-za-test-catch-mismatch (tag-outer tag-inner)
  (catch tag-outer
    (catch tag-inner
      (throw tag-outer 55))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-catch-mismatch)))
(assert-equal 55 (isiki-za-test-catch-mismatch 'isiki-za-test-tag-outer 'isiki-za-test-tag-inner))

;; JITコンパイルされたthrowだけの関数を、インタプリタ側(トップレベル)のcatchで
;; 受け止める: 制御転送値の実行時表現がインタプリタと完全に一致することの確認
(defun isiki-za-test-throw-standalone (tag val) (throw tag val))
(assert-equal t (%%za-compiled-p (function isiki-za-test-throw-standalone)))
(assert-equal 99 (catch 'isiki-za-test-tag-d (isiki-za-test-throw-standalone 'isiki-za-test-tag-d 99)))
(close (open-output-file "tmp/ckpt-21-catch-throw.txt"))

;; --- unwind-protect ---

;; 正常終了時もcleanup-formsが必ず実行される
(defun isiki-za-test-uw-normal (x)
  (unwind-protect
      (+ x 1)
    (isiki-za-test-bump-cleanup)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-uw-normal)))
(setq *isiki-za-test-cleanup-count* 0)
(assert-equal 6 (isiki-za-test-uw-normal 5))
(assert-equal 1 *isiki-za-test-cleanup-count*)

;; return-from(blockへの脱出)でprotected-formを抜けてもcleanupは必ず実行され、
;; protected-formの結果(制御転送値そのもの)はcleanup後もそのままblockまで伝播する
(defun isiki-za-test-uw-return-from (x)
  (block done
    (unwind-protect
        (if (< x 0) (return-from done (- 1)) (+ x 100))
      (isiki-za-test-bump-cleanup))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-uw-return-from)))
(setq *isiki-za-test-cleanup-count* 0)
(assert-equal (- 1) (isiki-za-test-uw-return-from (- 1)))
(assert-equal 1 *isiki-za-test-cleanup-count*)
(assert-equal 105 (isiki-za-test-uw-return-from 5))
(assert-equal 2 *isiki-za-test-cleanup-count*)

;; throwでprotected-formを抜けてもcleanupは必ず実行される
(defun isiki-za-test-uw-throw (tag x)
  (unwind-protect
      (if (< x 0) (throw tag (- 2)) (+ x 1))
    (isiki-za-test-bump-cleanup)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-uw-throw)))
(setq *isiki-za-test-cleanup-count* 0)
(assert-equal (- 2) (catch 'isiki-za-test-tag-e (isiki-za-test-uw-throw 'isiki-za-test-tag-e (- 1))))
(assert-equal 1 *isiki-za-test-cleanup-count*)
(assert-equal 11 (isiki-za-test-uw-throw 'isiki-za-test-tag-e 10))
(assert-equal 2 *isiki-za-test-cleanup-count*)
(close (open-output-file "tmp/ckpt-22-unwind-protect.txt"))

;; --- tagbody/go ---

;; 前方ジャンプ: (go skip)でtagbody内の後続の1要素を読み飛ばす。tagbodyは
;; 常にnilを返す(結果はisiki-za-test-log-pushの副作用で確認する)
(defun isiki-za-test-tagbody-forward (x)
  (tagbody
    (if (< x 0) (go skip))
    (isiki-za-test-log-push 1)
   skip
    (isiki-za-test-log-push 2)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-tagbody-forward)))
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-tagbody-forward (- 1)))
(assert-equal '(2) *isiki-za-test-log*)
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-tagbody-forward 5))
(assert-equal '(2 1) *isiki-za-test-log*)

;; 後方ジャンプ(カウントループ): tagbody自身のnlx_depthは0(内部にcatchを持たない)。
;; ループの継続判定はisiki-za-test-loop-step-or-done(za非対応のヘルパー)に切り出し、
;; nがループ回数に達した時点でtagへthrowして抜ける。呼び出し側がcatchで受け止める
(defun isiki-za-test-tagbody-count-loop (tag n)
  (tagbody
   top
    (isiki-za-test-loop-step-or-done tag n)
    (go top)))
(assert-equal t (%%za-compiled-p (function isiki-za-test-tagbody-count-loop)))
(setq *isiki-za-test-loop-i* 0)
(setq *isiki-za-test-loop-sum* 0)
(assert-equal t (catch 'isiki-za-test-tag-loop1 (isiki-za-test-tagbody-count-loop 'isiki-za-test-tag-loop1 5)))
(assert-equal 5 *isiki-za-test-loop-i*)
(assert-equal 10 *isiki-za-test-loop-sum*)

;; goがblockを飛び越えるのはOK(blockはnlx_depthを増やさないため): ループ本体を
;; blockで包んでもコンパイル・実行結果は変わらない。こちらもtagbody自身のnlx_depthは0
(defun isiki-za-test-go-through-block (tag n)
  (tagbody
   top
    (block inner
      (isiki-za-test-loop-step-or-done tag n)
      (go top))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-go-through-block)))
(setq *isiki-za-test-loop-i* 0)
(setq *isiki-za-test-loop-sum* 0)
(assert-equal t (catch 'isiki-za-test-tag-loop2 (isiki-za-test-go-through-block 'isiki-za-test-tag-loop2 3)))
(assert-equal 3 *isiki-za-test-loop-i*)
(assert-equal 3 *isiki-za-test-loop-sum*)

;; tagbody自身がcatchのbody(nlx_depth=1)の中に開かれても、goは同じtagbody内の
;; タグを問題無く直接ジャンプできる(base_nlx_depthがtagbodyごとに自分の開始深さを
;; 記録しているため)。ここではthrowの受け皿となるcatchが関数自身に内蔵されている
(defun isiki-za-test-tagbody-inside-catch (tag n)
  (catch tag
    (tagbody
     top
      (isiki-za-test-loop-step-or-done tag n)
      (go top))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-tagbody-inside-catch)))
(setq *isiki-za-test-loop-i* 0)
(setq *isiki-za-test-loop-sum* 0)
(assert-equal t (isiki-za-test-tagbody-inside-catch 'isiki-za-test-tag-f 4))
(assert-equal 4 *isiki-za-test-loop-i*)
(assert-equal 6 *isiki-za-test-loop-sum*)
(close (open-output-file "tmp/ckpt-23-tagbody.txt"))

;; --- フォールバック確認 ---

;; ネストしたtagbody(1つのJITコンパイル対象関数内にtagbodyが複数レベル)は拡張14
;; (za_test_ext14.lisp参照)で対応済み。内側tagbodyのgoが自分自身のタグ(inner)
;; だけを使う単純なケースなので、ここではコンパイルされることを確認する
;; (ネストの網羅的な検証はza_test_ext14.lispを参照)。
(defun isiki-za-test-nested-tagbody-fallback (x)
  (tagbody
   outer
    (tagbody
     inner
      (isiki-za-test-log-push x))))
(assert-equal t (%%za-compiled-p (function isiki-za-test-nested-tagbody-fallback)))
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-nested-tagbody-fallback 42))
(assert-equal '(42) *isiki-za-test-log*)

;; 自身の持たないタグへのgo(body中に無いラベル)も非対応でフォールバックする。
;; 実際にgoが実行されない引数で呼ぶことで、フォールバック後の実行時エラーを避ける
(defun isiki-za-test-go-unknown-fallback (x)
  (tagbody
    (if x (go nowhere))
    (isiki-za-test-log-push 1)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-go-unknown-fallback)))
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-go-unknown-fallback nil))
(assert-equal '(1) *isiki-za-test-log*)

;; goがcatchのスパンを飛び越える(=対応するcatchのbodyより外側のタグへ飛ぶ)位置に
;; 書かれている場合も非対応でフォールバックする。フォールバック後はインタプリタが
;; catch/tagbodyの制御転送を動的に正しく伝播させるため、実際にgoが実行される
;; 経路(x=t)でも正しくラベルまでジャンプできることも確認する
(defun isiki-za-test-go-cross-catch-fallback (tag x)
  (tagbody
    (if x (catch tag (go escape)))
    (isiki-za-test-log-push 1)
   escape
    (isiki-za-test-log-push 2)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-go-cross-catch-fallback)))
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-go-cross-catch-fallback 'isiki-za-test-tag-g t))
(assert-equal '(2) *isiki-za-test-log*)
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-go-cross-catch-fallback 'isiki-za-test-tag-g nil))
(assert-equal '(2 1) *isiki-za-test-log*)

;; goがunwind-protectのスパンを飛び越える場合も非対応でフォールバックする。
;; フォールバック後はインタプリタがeval_unwind_protectの「cleanupは常に実行する」
;; 規約通りに動作し、goで実際に脱出してもcleanupが実行されることを確認する
;; (za側でこの規約を壊す近道のjmpにコンパイルしてしまわないことの回帰テスト)
(defun isiki-za-test-go-cross-uw-fallback (x)
  (tagbody
    (if x (unwind-protect (go escape) (isiki-za-test-log-push 99)))
    (isiki-za-test-log-push 1)
   escape
    (isiki-za-test-log-push 2)))
(assert-equal nil (%%za-compiled-p (function isiki-za-test-go-cross-uw-fallback)))
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-go-cross-uw-fallback t))
(assert-equal '(2 99) *isiki-za-test-log*)
(setq *isiki-za-test-log* nil)
(assert-equal nil (isiki-za-test-go-cross-uw-fallback nil))
(assert-equal '(2 1) *isiki-za-test-log*)
(close (open-output-file "tmp/ckpt-24-final.txt"))
