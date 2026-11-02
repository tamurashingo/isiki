;; test/lisp/bench_interp_control.lisp
;;
;; [性能測定] **解釈実行の計器の陽性対照。**
;; documents/performance-measurement.md「解釈実行の経路の基準値」節、規則 8。
;;
;; 規則 8 が要求するのは「計器が対象を見られることを先に示す」ことである。
;; この計器の対象は**解釈実行の費用**なので、
;; **解釈実行に意図的に費用を足したら値が動く**ことを見せなければならない。
;; 見せられなければ、M8-T4 で評価の入口に置く検査の費用も、同じように
;; 見えないまま「変わらなかった」と書かれる。
;;
;; **足し方(大げさに足す。規則 8)。**
;; %%bench-interp-loop の本体に、%%bench-aot-let の内側の let* を**1 つだけ**
;; 足す。結果は捨てるので計算は変わらない(どちらも 0 を返す)。
;;
;;   %%bench-interp-loop     (while (> i 0)                    (setq i (- i 1)))
;;   %%bench-interp-loopcost (while (> i 0) <let* を 1 つ>      (setq i (- i 1)))
;;
;; while は &rest body なので、展開形の差は progn の要素が 1 つ増えるだけで、
;; **余分な包みは入らない**(init.lisp の while は
;; `(block nil (tagbody %while-loop (if test (progn ,@body (go %while-loop)) nil)))`)。
;;
;; **なぜ「大げさ」と言えるのか(測る前の見積り。規則 10・11)。**
;; 足した let* は 1 反復あたり os_eval の入口を約 15 回増やす
;;   let* 自身 1 + init 5 式 + その引数の原子 8 + 本体の e 1 = 15
;; さらに `+` の呼び出しを 4 回増やす。既知の尺度は
;; 「インタプリタの関数呼び出し 1 回 = os_eval 区間で +533 命令/反復」
;; (同書「インタプリタ落ちの検出器」節の陽性対照の実測)なので、
;; 差は 10^3 命令/単位の桁になる見込みである。
;; N=100,000 での計器の分解能は 1 ブートのぶれ 11〜31M を 2N で割った
;; **55〜155 命令/単位**なので、**分解能の 1 桁上**を足したことになる。
;;
;; **別の形の証拠(§5-5)。**
;; 足したのは %%bench-aot-let の内側の let* そのものなので、
;;
;;     interp/loopcost - interp/loop  ≒  interp/let - interp/arith
;;
;; が成り立たなければならない(右辺は、同じ let* が `i` という 1 つの原子と
;; 入れ替わった差なので、**シンボル参照 1 回ぶんだけ**左辺より小さい)。
;; 2 つの独立な対が同じ増分を測ることになる。
;; **1 行の値ではなく、この並びのほうが根拠である**(規則 3)。
;;
;; let* が真実源(src/lisp/bench_aot.lisp の %%bench-aot-let)からの逐語の
;; 写しであることは tools/bench/check_bench_jit_sync.sh が grep で固定する。
;; **手書きなので、固定しないと真実源が動いた日に静かにずれる。**
;;
;; 使い方:
;;   BENCH_CASES="loop loopcost" BENCH_REPEAT=1 BENCH_NO_CHECK=1 \
;;     make test-qemu-interp-bench
;;
;; 対照群は嘘をついていてはならない(規則 5)。この 2 つは
;; 1 反復あたりに評価するフォームの数が違うだけで、**戻り値は同じ 0** である。

;; 陽性対照。%%bench-interp-loop と同じ計算(n から 0 まで減らして 0 を返す)に、
;; 捨てる let* を 1 つ足しただけのもの。**defglobal + lambda なので
;; bench_interp.lisp と同じく必ずインタプリタで走る**
(defglobal %%bench-interp-loopcost
  (lambda (n)
    (let ((i n))
      (progn
        (while (> i 0)
          (let* ((a i) (b (+ a 1)) (c (+ b 1)) (d (+ c 1)) (e (+ d 1)))
            e)
          (setq i (- i 1)))
        i))))
