;; test/lisp/for_semantics_test.lisp
;;
;; [意味論] ISLisp の `for` の意味論を 3 経路(インタプリタ / JIT / AOT)で固定する。
;; documents/for-expansion.md §4。
;;
;; **なぜ要るか。**
;; `init.lisp` の `for` と `transpile.lisp` の `expand-for` は**別の展開形**である
;; (前者は毎反復 `(list step1 step2 ...)` を作り、後者は一時変数へ全部評価してから
;;  書き戻す)。形が違うので、**同じ意味論になっているかは実測しないと分からない。**
;;
;; `init.lisp` 側を `expand-for` と同じ形へ移植するときに、このファイルが
;; 「移植前と移植後で意味論が変わっていないこと」の基準になる。
;; **移植したあとに 1 件も落ちないことを確認すること。**
;;
;; いちばん危ないのは**並列代入**である。旧展開形が毎反復 `list` を作っていたのは、
;; まさに並列代入のためかもしれない(全部評価してから、まとめて代入する形)。
;; 新展開形が `list` を使わずに並列性を保っているかを、交換で判定する。

;;; --- 1. 並列代入(いちばん危ないところ) ---
;; i と j を互いの**旧値**で更新する。
;;   並列なら 1 反復ごとに入れ替わる  -> n=0:(0 1)=1  n=1:(1 0)=10  n=2:(0 1)=1
;;   逐次(i を先に書き換えて j がその新値を読む)なら両方 1 -> n>=1 で 11
;; 戻り値は (+ (* i 10) j) の fixnum(AOT に list を使わせないため)
(defun for-swap-interp-check (n)
  ;; この関数自身は JIT に乗るが、下のトップレベル呼び出しはインタプリタで評価される
  (for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k n) (+ (* i 10) j))))

;; インタプリタ経路(トップレベルの for をそのまま評価する)
(assert-equal 1  (for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k 0) (+ (* i 10) j))))
(assert-equal 10 (for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k 1) (+ (* i 10) j))))
(assert-equal 1  (for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k 2) (+ (* i 10) j))))
(assert-equal 10 (for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k 3) (+ (* i 10) j))))

;; JIT 経路
(assert-equal t (%%za-compiled-p (function for-swap-interp-check)))
(assert-equal 1  (for-swap-interp-check 0))
(assert-equal 10 (for-swap-interp-check 1))
(assert-equal 1  (for-swap-interp-check 2))
(assert-equal 10 (for-swap-interp-check 3))

;; AOT 経路(src/lisp/bench_aot.lisp の %%bench-aot-for-swap)
(assert-equal 1  (%%bench-aot-for-swap 0))
(assert-equal 10 (%%bench-aot-for-swap 1))
(assert-equal 1  (%%bench-aot-for-swap 2))
(assert-equal 10 (%%bench-aot-for-swap 3))

;; **3 経路が同じ答えを返すことを直接突き合わせる**(期待値リテラルの写し間違いを除く)
(assert-equal (%%bench-aot-for-swap 1) (for-swap-interp-check 1))
(assert-equal (%%bench-aot-for-swap 2) (for-swap-interp-check 2))
(assert-equal (%%bench-aot-for-swap 3)
              (for ((i 0 j) (j 1 i) (k 0 (+ k 1))) ((>= k 3) (+ (* i 10) j))))

;;; --- 2. step 式の評価回数 / end-test の評価順 / 本体の回数 ---
;; n=5 のとき: step は 5 回(1 反復 1 回)、end-test は 6 回(5 反復 + 最後の判定)、
;; 本体は 5 回。**end-test は本体の前**(ループの先頭)で評価される
(defglobal for-sem-step 0)
(defglobal for-sem-test 0)
(defglobal for-sem-body 0)
(defun for-sem-bump () (progn (setq for-sem-step (+ for-sem-step 1)) for-sem-step))
(defun for-sem-tbump (i n) (progn (setq for-sem-test (+ for-sem-test 1)) (>= i n)))
(defun for-sem-bbump () (progn (setq for-sem-body (+ for-sem-body 1)) nil))

(setq for-sem-step 0)
(setq for-sem-test 0)
(setq for-sem-body 0)
(defglobal for-sem-result
  (for ((i 0 (+ i 1)) (s 0 (for-sem-bump))) ((for-sem-tbump i 5) (list i s))
       (for-sem-bbump)))
(assert-equal '(5 5) for-sem-result)
(assert-equal 5 for-sem-step)   ; step は 1 反復 1 回
(assert-equal 6 for-sem-test)   ; end-test は 5 反復 + 最後の 1 回 = 本体の前
(assert-equal 5 for-sem-body)

;;; --- 3. 境界(step 省略 / 変数 1 個 / 変数 0 個) ---
(assert-equal '(3 7) (for ((i 0 (+ i 1)) (fixed 7)) ((>= i 3) (list i fixed))))
(assert-equal 3      (for ((i 0 (+ i 1))) ((>= i 3) i)))
(assert-equal 'done  (for () (t 'done)))

;;; --- 4. 本体から変数が見えること / 本体の setq が効くこと ---
;; 「ループ本体の let 廃止」が変数の見え方を変えていないかを固定する
(defglobal for-sem-seen nil)
(defglobal for-sem-acc
  (for ((i 0 (+ i 1)) (acc nil)) ((>= i 3) acc)
       (setq acc (cons i acc))
       (setq for-sem-seen i)))
(assert-equal '(2 1 0) for-sem-acc)
(assert-equal 2 for-sem-seen)

;;; --- 5. 1 反復あたりの確保量 ---
;; [性能測定] documents/for-expansion.md §1。
;;
;; **経緯:** 以前は jit 側が 32(cons 2 個)だった。init.lisp の for が毎反復
;; (list step1 step2) を作る旧展開形だったためで、PR #116 でその 32 を記録に固定し、
;; **2026-10-04 の移植(一時変数方式)で 0 になった。**
;; AOT 側は「ループ本体の let 廃止」で先に作り直されていたので最初から 0 である。
;;
;; **数字だけ書き換えると、なぜこのテストがあるのか分からなくなる。**
;; ここが 0 以外に戻ったら、init.lisp の for が旧展開形へ戻ったということである。
(assert-equal 0 (bench-jit-alloc-per-iter (function %%bench-jit-for) 20000 60000))
(assert-equal 0 (bench-jit-alloc-per-iter (function %%bench-aot-for) 20000 60000))
