;; test/lisp/bench_jit_test.lisp
;;
;; [性能測定] JIT 経路のベンチ(src/lisp/bench_jit.lisp)の正当性テスト。
;; documents/performance-measurement.md「JIT 経路の基準値」節。
;;
;; 3 つのことを固定する。
;;
;; 1. **測定対象が JIT に乗っていること。**
;;    乗らなくなっても測定は動いて数字を出す。その数字は JIT 経路の値ではない。
;;    tools/bench/run_jit_bench.sh は測定の直前に同じ検査をするが、こちらは
;;    make test-qemu-all で常時回る側の歯止めである。
;;
;; 2. **AOT 版と同じ計算をしていること。**
;;    bench_jit.lisp は bench_aot.lisp から生成した写しで、ソース上の同一性は
;;    tools/bench/check_bench_jit_sync.sh が diff で固定している。ここでは
;;    実行結果で突き合わせる(生成は正しいが JIT の生成コードが間違っている、
;;    という場合はソースの diff では捕まらない)。
;;
;; 3. **JIT に乗らない / 呼べない関数がどれかを記録すること。**
;;    「全部 T のはず」で書くと、測れないカテゴリが静かに混ざる。

;; Nは1000の倍数(cons系がN/1000回、再帰系がN/100回の繰り返しになるため)
(defglobal bench-jit-n 2000)

;;; --- 1. 測定する 6 カテゴリが JIT に乗っていること ---
(assert-equal t (%%za-compiled-p (function %%bench-jit-loop)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-arith)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-tailrec)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-tailrec-step)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-let)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-for)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-funcall)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-callee)))

;;; --- 1b. 測定対象外の関数の JIT 化状況(記録) ---
;; bench_jit.lisp は bench_aot.lisp の機械的な写しなので、測定しない関数も
;; 定義される。**その JIT 化状況もここに書いておく。**書いておかないと、
;; 将来 JIT の対応範囲が動いたときに気づく場所が無い
(assert-equal t (%%za-compiled-p (function %%bench-jit-nontailrec)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-nontailrec-step)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-branch)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-cons)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-vector)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-let1)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-let10)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-let5const)))
(assert-equal t (%%za-compiled-p (function %%bench-jit-deep-recursion)))

;; **let5par だけは JIT に乗らない(実測 2026-10-02、feature/bench-jit)。**
;; 並列束縛の let で 5 つの init がすべて同じローカル変数 i を参照する形
;; (bench_aot.lisp の %%bench-aot-let5par)。原因は調べていない。
;; 本作業は計器を作るだけなので直さない(作業指示書 §9)。
;; **この行が T で落ちたら、それは退行ではなく改善である。** 期待値を T に直すこと
(assert-equal nil (%%za-compiled-p (function %%bench-jit-let5par)))

;;; --- 2. AOT 版と同じ計算をしていること ---
;; 期待値は test/lisp/bench_construct_test.lisp(C 版/AOT 版)と同じもの
(assert-equal 0       (%%bench-jit-loop bench-jit-n))
(assert-equal 1999000 (%%bench-jit-arith bench-jit-n))
(assert-equal 101000  (%%bench-jit-tailrec bench-jit-n))
(assert-equal 6000    (%%bench-jit-branch bench-jit-n))
(assert-equal 2007000 (%%bench-jit-let bench-jit-n))
(assert-equal 999000  (%%bench-jit-cons bench-jit-n))
(assert-equal 1999000 (%%bench-jit-for bench-jit-n))
(assert-equal 1999000 (%%bench-jit-vector bench-jit-n))
(assert-equal 2000    (%%bench-jit-funcall bench-jit-n))

;; AOT 版の戻り値と直接突き合わせる(期待値リテラルの写し間違いを除くため)
(assert-equal (%%bench-aot-loop bench-jit-n)    (%%bench-jit-loop bench-jit-n))
(assert-equal (%%bench-aot-arith bench-jit-n)   (%%bench-jit-arith bench-jit-n))
(assert-equal (%%bench-aot-tailrec bench-jit-n) (%%bench-jit-tailrec bench-jit-n))
(assert-equal (%%bench-aot-let bench-jit-n)     (%%bench-jit-let bench-jit-n))
(assert-equal (%%bench-aot-for bench-jit-n)     (%%bench-jit-for bench-jit-n))
(assert-equal (%%bench-aot-funcall bench-jit-n) (%%bench-jit-funcall bench-jit-n))

;;; --- 2b. 非末尾再帰は JIT 経路では測れない(既知の制約) ---
;; **%%bench-jit-nontailrec を呼んではいけない。** 内側の
;; %%bench-jit-nontailrec-step を深さ 100 で回すが、JIT 経路ではその手前で
;; C スタック(STACK_SIZE = 256KB、src/c/process.c:27)を使い切り、ガードページを
;; 踏んでプロセスが停止する。**報告は -serial stdio にしか出ないので、
;; 外から見ると QEMU が無反応になるだけである**(実際にこの作業で 2 回踏んだ)。
;; documents/known-issue-deep-if-nesting-jit.md が「残っているタスク」として
;; 挙げている「C スタック枯渇からの復帰」そのもので、報告は出るが戻ってこない。
;;
;; 実測(2026-10-02、feature/bench-jit、トップレベルから直接呼んだ場合):
;;   深さ 31=496 / 32=528 / 33=561 は返る。**34 で STACK OVERFLOW(下端側)。**
;;   AOT 版(%%bench-aot-nontailrec)は同じ S 式で深さ 100 が通る(1 段 330 byte)。
;;   256KB / 33 段 ≒ 1 段あたり 7.5KB。AOT の 20 倍以上太い
;;   (ベースの消費を無視した概算。1 段ぶんを直接測ってはいない)。
;;
;; **上限は関数だけの性質ではない。** 呼び出し元が既に使っている C スタックの
;; ぶんだけ下がる。トップレベルから直接呼ぶと 33 段通るが、
;; isiki-test-load -> load -> assert-equal の下から呼ぶと 33 段で溢れた
;; (この違いで最初に書いたテストがハングした)。
;; **したがって境界値そのものを回帰テストにしてはいけない。**
;; 余裕のある 20 段で「JIT の非末尾再帰が動くこと」だけを固定する
(assert-equal 210 (%%bench-jit-nontailrec-step 20))


;;; --- 2c. for は AOT と JIT で展開形が違う(記録として固定する) ---
;; [性能測定] documents/performance-measurement.md「for の 2.6 倍の切り分け」。
;;
;; init.lisp の for は毎反復 (list step1 step2) を作る**旧展開形**のままで、
;; transpile.lisp の expand-for は一時変数+素の setq の**新展開形**である。
;; したがって %%bench-jit-for と %%bench-aot-for は**同じ S 式だが別のコード**で、
;; 命令数を比べても意味が無い(bench_baseline.tsv では for の帯を "-" にして
;; 検出器の対象から外してある)。
;;
;; 差は確保量にそのまま出る。確保量はこの環境で**ぶれない計器**である(規則 9)。
;;
;; **この 2 行は「直ったら落ちる」テストである。**
;; init.lisp の for を expand-for と同じ形へ移植したら 32 が 0 になって落ちる。
;; そのときは:
;;   1. ここの期待値を 0 にする
;;   2. tools/bench/bench_baseline.tsv の for 2 行の帯を "-" から戻し、測り直す
;;   3. documents/jit-unsupported-syntax.md の for の行を「乗るもの」へ移す
;;   4. transpile.lisp:721 の「init.lisp の defmacro for/while と同じ展開規則」が
;;      **そこで初めて本当になる**
;;
;; ついでに ide.lisp:40-45 が記録している「for マクロは GC が特定のタイミングで
;; 走ると以後永久に結果が壊れる」既知のバグも、原因(ループ本体に毎回新規生成
;; される let)が同じなので、インタプリタと JIT の経路には**まだ残っている**
(assert-equal 32 (bench-jit-alloc-per-iter (function %%bench-jit-for) 20000 60000))
(assert-equal 0  (bench-jit-alloc-per-iter (function %%bench-aot-for) 20000 60000))
;; 対照: 確保しない構文では両経路とも 0(計器が「0 以外」を出す側に偏っていない)
(assert-equal 0  (bench-jit-alloc-per-iter (function %%bench-jit-loop) 20000 60000))
(assert-equal 0  (bench-jit-alloc-per-iter (function %%bench-aot-loop) 20000 60000))

;;; --- 3. Immobilized Space の消費 ---
;; [性能測定] defun ごとの Immobilized Space 消費は既知の未解決課題
;; (documents/investigation-defun-page-cost.md)。bench_jit.lisp の 20 個の
;; defun を 1 ブートで定義するぶんがどれだけかを記録する。
;;
;; 実測(2026-10-02): load 前 484,496 -> load 後 555,504 byte。
;; **20 defun で 71,008 byte(1 defun あたり約 3.5KB)**、総量 16,777,216 byte の
;; 0.42%。ベンチを回した後に測り直しても 555,504 byte のままで、
;; **実行回数には比例しない**(Phase1 で za_fn_meta_t を fnptr 単位に一意化した
;; 結果)。run_jit_bench.sh は 1 ブートで 1 回だけ定義するので枯れない
(defglobal bench-jit-imm-used (%%imm-space-used-bytes))
(defglobal bench-jit-imm-total (%%imm-space-total-bytes))
(format *isiki-test-stream* "#imm-after-bench used=~D total=~D~%"
        bench-jit-imm-used bench-jit-imm-total)
;; 総量の 5% 未満であること(実測 3.3%)。ここが近づいてきたら、
;; 1 ブートで定義できる defun の数が上限に当たる
(assert-equal t (if (< (* bench-jit-imm-used 20) bench-jit-imm-total) t nil))
