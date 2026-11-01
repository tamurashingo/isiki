;; test/lisp/bench_interp_test.lisp
;;
;; [性能測定] 解釈実行の経路のベンチ(src/lisp/bench_interp.lisp)の正当性
;; テストと、ゲートの**陰性対照・陽性対照**。
;; documents/performance-measurement.md「解釈実行の経路の基準値」節。
;;
;; 4 つのことを固定する。
;;
;; 1. **測定対象がインタプリタで走っていること**(%%za-compiled-p が NIL)。
;;    JIT 側のゲートの逆向きである。lambda がコンパイルされるようになったら
;;    この行が落ちる。**落ちたら退行ではなく、計器が別のものを測るように
;;    なったという報せである。**
;;
;; 2. **AOT 版と同じ計算をしていること。**
;;    bench_interp.lisp は bench_aot.lisp から生成した写しで、ソース上の
;;    同一性は tools/bench/check_bench_jit_sync.sh が diff で固定している。
;;    ここでは実行結果で突き合わせる(生成は正しいが評価が間違っている、
;;    という場合はソースの diff では捕まらない)。
;;
;; 3. **ゲートの陰性対照と陽性対照**(規則 8)。
;;
;; 4. **1 反復あたりの確保量が出ること。**
;;    確保量はこの環境でぶれない計器である(規則 9)。値そのものはここで
;;    固定せず、**親が測って documents/performance-measurement.md へ記録する**。
;;    ここで固定するのは「計器が 0 でも -1 でもない値を返すこと」だけである
;;    (-1 は GC が走ったことを意味する。bench_jit_guard.lisp 参照)。
;;    **測っていない数字を期待値として書かない**(規則 9)。

;; N は 1000 の倍数(cons 系が N/1000 回、再帰系が N/100 回の繰り返しになるため)。
;; ここで測る 3 カテゴリは div を使わないが、他経路のテストと揃えておく
(defglobal bench-interp-n 2000)

;;; --- 1. 測定する 3 カテゴリがインタプリタで走っていること ---
;; **これがこの経路の定義である。** T になったら interp の行は interp の
;; 値ではない(issue #114 = 名前にしか書かれていない事実は記録されていない)
(assert-equal nil (%%za-compiled-p %%bench-interp-loop))
(assert-equal nil (%%za-compiled-p %%bench-interp-arith))
(assert-equal nil (%%za-compiled-p %%bench-interp-let))
;; 陽性対照も同じく解釈実行でなければ、対照にならない
(assert-equal nil (%%za-compiled-p %%bench-interp-loopcost))

;;; --- 2. AOT 版と同じ計算をしていること ---
;; 期待値は test/lisp/bench_construct_test.lisp(C 版/AOT 版)と同じもの
(assert-equal 0       (funcall %%bench-interp-loop bench-interp-n))
(assert-equal 1999000 (funcall %%bench-interp-arith bench-interp-n))
(assert-equal 2007000 (funcall %%bench-interp-let bench-interp-n))

;; AOT 版の戻り値と直接突き合わせる(期待値リテラルの写し間違いを除くため)
(assert-equal (%%bench-aot-loop bench-interp-n)
              (funcall %%bench-interp-loop bench-interp-n))
(assert-equal (%%bench-aot-arith bench-interp-n)
              (funcall %%bench-interp-arith bench-interp-n))
(assert-equal (%%bench-aot-let bench-interp-n)
              (funcall %%bench-interp-let bench-interp-n))

;;; --- 2b. 陽性対照が嘘をついていないこと(規則 5) ---
;; %%bench-interp-loopcost は loop に捨てる let* を 1 つ足しただけで、
;; **戻り値は同じ**でなければならない。違っていたら「計算を変えたから
;; 値が動いた」になり、陽性対照の資格を失う
(assert-equal 0 (funcall %%bench-interp-loopcost bench-interp-n))
(assert-equal (funcall %%bench-interp-loop bench-interp-n)
              (funcall %%bench-interp-loopcost bench-interp-n))

;;; --- 3. ゲート(bench-interp-guard)の陰性対照 ---
(assert-equal t (bench-interp-guard "%%bench-interp-loop" %%bench-interp-loop))
(assert-equal 0 *bench-interp-guard-ng*)
(assert-equal t (bench-interp-guard-gate))

;;; --- 3b. ゲートの陽性対照: わざと JIT に乗る形を渡す ---
;; %%bench-interp-loop と**同じ計算**を defun で書く。defun は
;; za_try_compile_defun を通るので JIT に乗る(= この経路の数字ではない)。
;;
;; 対照群は嘘をついていてはならない(規則 5)。この関数は JIT に乗るだけで、
;; 計算は同じである。「壊れた関数だから落ちた」ではないことを戻り値で確かめる
(defun bench-interp-compiled-loop (n)
  (let ((i n))
    (progn
      (while (> i 0)
        (setq i (- i 1)))
      i)))

;; 前提: この形は JIT に乗る
(assert-equal t (%%za-compiled-p (function bench-interp-compiled-loop)))
;; 前提: それでも計算は正しい(対照群が嘘をついていない)
(assert-equal 0 (bench-interp-compiled-loop bench-interp-n))
(assert-equal (funcall %%bench-interp-loop bench-interp-n)
              (bench-interp-compiled-loop bench-interp-n))

;; 本題: guard がこれを nil と判定し、件数を数えること
(assert-equal nil (bench-interp-guard "bench-interp-compiled-loop"
                                      (function bench-interp-compiled-loop)))
(assert-equal 1 *bench-interp-guard-ng*)

;; 本題: gate が nil を返し、failed を 1 増やして make を落とすこと
(defglobal bench-interp-fail-before *isiki-test-fail*)
(assert-equal nil (bench-interp-guard-gate))
(defglobal bench-interp-fail-delta (- *isiki-test-fail* bench-interp-fail-before))

;; 後始末。陽性対照で**意図的に**増やした failed を戻す。
;; 戻さないとこのテストファイル自身が " 0 failed" を壊す
(setq *isiki-test-fail* bench-interp-fail-before)
(setq *bench-interp-guard-ng* 0)

;; 増えたのがちょうど 1 件だったことを、戻した後に確認する
(assert-equal 1 bench-interp-fail-delta)
;; 後始末が効いていること
(assert-equal t (bench-interp-guard-gate))

;;; --- 4. 1 反復あたりの確保量(ぶれない計器。規則 9) ---
;; [性能測定] 解釈実行の while は init.lisp の defmacro により
;; block/tagbody/go へ展開される。**go は 1 反復ごとに確保する**
;; (docs/plans/compiler-optimization.md M8 の「96 byte/反復」はこの話である)。
;; したがってこの値は 0 にはならない。
;;
;; 計器は test/lisp/bench_jit_guard.lisp の bench-jit-alloc-per-iter を
;; 使い回す(**同じものを 2 箇所に書けば片方だけ直される**)。あちらは
;; funcall で呼ぶだけなので、defglobal に入った lambda でもそのまま使える。
;;
;; n1/n2 を 2000/6000 と小さく取るのは **GC を走らせないため**である。
;; 96 byte/反復なら (6000 + 2000) x 96 ≒ 768KB で、
;; 実測で GC 0 回だった範囲(60,000 反復 x 32 byte ≒ 1.9MB)の中に収まる。
;; GC が走ると計器は -1 を返し、下のアサーションが落ちる(**黙って嘘の 0 を
;; 返すことはない**)
(defglobal bench-interp-alloc-loop
  (bench-jit-alloc-per-iter %%bench-interp-loop 2000 6000))
(defglobal bench-interp-alloc-arith
  (bench-jit-alloc-per-iter %%bench-interp-arith 2000 6000))
(defglobal bench-interp-alloc-let
  (bench-jit-alloc-per-iter %%bench-interp-let 2000 6000))
(defglobal bench-interp-alloc-loopcost
  (bench-jit-alloc-per-iter %%bench-interp-loopcost 2000 6000))

;; **値は親が測って記録する。**ここでは出力に残すだけ
;; (規則 9: 測っていない数字を期待値として書かない)
(format *isiki-test-stream*
        "#interp-alloc-per-iter loop=~D arith=~D let=~D loopcost=~D~%"
        bench-interp-alloc-loop bench-interp-alloc-arith
        bench-interp-alloc-let bench-interp-alloc-loopcost)

;; 固定するのは「計器が働いていること」だけ。
;; 0 なら解釈実行の go が確保をやめたということ、-1 なら GC が走ったということで、
;; どちらも確保量の行を根拠に何かを言ってはいけない状態である
(assert-equal t (if (> bench-interp-alloc-loop 0) t nil))
(assert-equal t (if (> bench-interp-alloc-arith 0) t nil))
(assert-equal t (if (> bench-interp-alloc-let 0) t nil))
(assert-equal t (if (> bench-interp-alloc-loopcost 0) t nil))

;; **loopcost と loop の確保量の大小はここでは固定しない。**
;; 「捨てる let* を 1 つ足したから確保量も増えるはず」は**まだ測っていない
;; 予測**である(規則 9・11)。予測として
;; documents/performance-measurement.md「解釈実行の経路の基準値」節に書き、
;; 上の #interp-alloc-per-iter の行で親が突き合わせる。
;; **ここで期待値にしてしまうと、予測が外れたときに「退行」として現れる。**
