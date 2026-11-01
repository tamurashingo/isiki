;;;; 構文別ベンチマークスイートの**解釈実行(インタプリタ)経路**の実装。
;;;;
;;;; **このファイルは生成物である。直接編集しないこと。**
;;;;   真実源: src/lisp/bench_aot.lisp
;;;;   生成:   python3 tools/bench/gen_bench_jit.py
;;;;   同期の固定: make check-bench-jit-sync (tools/bench/check_bench_jit_sync.sh)
;;;;
;;;; 本体の S 式は bench_aot.lisp と1文字も違わない。差分は
;;;;   (defun %%bench-aot-X (args) BODY)
;;;;     -> (defglobal %%bench-interp-X (lambda (args) BODY))
;;;; の**包み方だけ**である。
;;;;
;;;; [性能測定] documents/performance-measurement.md「解釈実行の経路の基準値」節。
;;;;
;;;; **なぜ lambda なのか。** defun は src/c/eval.c の eval_defun が必ず
;;;; za_try_compile_defun を通すので、書いた時点で JIT 経路になる。
;;;; eval_lambda は make_interpreted_function を直接呼ぶだけで、コンパイルを
;;;; 試しもしない。**「lambda は必ずインタプリタ」は既存の実装の性質であり、
;;;; この計器のために足したつまみではない**(カーネルに JIT を切るつまみは無い)。
;;;;
;;;; **測定の前に %%za-compiled-p が NIL であることを必ず確認すること**
;;;; (test/lisp/bench_interp_guard.lisp)。JIT 側のゲートの**逆向き**である。
;;;; lambda が将来コンパイルされるようになったら、この行は黙って
;;;; 「インタプリタを測ったつもりで JIT を測った数字」になる。
;;;; 名前は根拠にならない(issue #114)。
;;;;
;;;; **他の経路の数字と直接比べてはいけない。** N が 1 桁違う
;;;; (解釈実行は AOT/JIT より 1〜2 桁遅いので、同じ N では測定時間が爆発する)。
;;;; この行は「解釈実行の経路の中での前後比較」にだけ使う。
;;;;
;;;; **呼べない関数がある**(下記)。本体が別のベンチ関数を名前で呼ぶものは
;;;; defglobal では解決できない。呼び先も lambda にすると funcall を挟むことに
;;;; なり本体の S 式が真実源と変わるので、そうはしない。
;;;; ドライバはこの印を grep して拒否する(件数は導出。規則 13)。
;;;;   [呼べない] %%bench-interp-tailrec-step
;;;;   [呼べない] %%bench-interp-tailrec
;;;;   [呼べない] %%bench-interp-nontailrec-step
;;;;   [呼べない] %%bench-interp-nontailrec
;;;;   [呼べない] %%bench-interp-funcall
;;;;   [呼べない] %%bench-interp-deep-recursion

;;; --- 1. 単純ループ(制御構造のベースライン) ---
(defglobal %%bench-interp-loop (lambda (n)
  (let ((i n))
    (progn
      (while (> i 0)
        (setq i (- i 1)))
      i))))

;;; --- 2. fixnum算術(加算+比較) ---
(defglobal %%bench-interp-arith (lambda (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc i))
          (setq i (+ i 1))))
      acc))))

;;; --- 3. 末尾再帰 ---
;;; [呼べない] %%bench-interp-tailrec-step -- 本体が別のベンチ関数を名前で呼ぶ(defglobal は変数なので関数位置では解決されない)
(defglobal %%bench-interp-tailrec-step (lambda (n acc)
  (if (= n 0)
      acc
      (%%bench-interp-tailrec-step (- n 1) (+ acc n)))))

;;; [呼べない] %%bench-interp-tailrec -- 本体が別のベンチ関数を名前で呼ぶ(defglobal は変数なので関数位置では解決されない)
(defglobal %%bench-interp-tailrec (lambda (n)
  (let ((reps (div n 100)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-interp-tailrec-step 100 0)))
          (setq r (+ r 1))))
      total))))

;;; --- 4. 非末尾再帰 ---
;;; [呼べない] %%bench-interp-nontailrec-step -- 本体が別のベンチ関数を名前で呼ぶ(defglobal は変数なので関数位置では解決されない)
(defglobal %%bench-interp-nontailrec-step (lambda (n)
  (if (= n 0)
      0
      (+ n (%%bench-interp-nontailrec-step (- n 1))))))

;;; [呼べない] %%bench-interp-nontailrec -- 本体が別のベンチ関数を名前で呼ぶ(defglobal は変数なので関数位置では解決されない)
(defglobal %%bench-interp-nontailrec (lambda (n)
  (let ((reps (div n 100)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-interp-nontailrec-step 100)))
          (setq r (+ r 1))))
      total))))

;;; --- 5. 条件分岐(5分岐のcond) ---
(defglobal %%bench-interp-branch (lambda (n)
  (let ((acc 0) (i 0) (k 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (cond ((= k 0) 1)
                                 ((= k 1) 2)
                                 ((= k 2) 3)
                                 ((= k 3) 4)
                                 (t 5))))
          (setq k (if (>= k 4) 0 (+ k 1)))
          (setq i (+ i 1))))
      acc))))

;;; --- 6. ローカル変数束縛(let*で5変数) ---
(defglobal %%bench-interp-let (lambda (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a i) (b (+ a 1)) (c (+ b 1)) (d (+ c 1)) (e (+ d 1)))
                             e)))
          (setq i (+ i 1))))
      acc))))

;;; --- 6b. ローカル変数束縛(let*で1変数) ---
;; [性能測定] Phase2の2-0-1: %%bench-interp-letと同じ計算(acc += i+4)を、
;; 内側のlet*の束縛数だけ1個に変えたもの。5束縛版との差を4で割ると、
;; AOTの実コード上での「1束縛あたりのコスト」(インライン展開で消せる分)が
;; 分離できる。
(defglobal %%bench-interp-let1 (lambda (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a (+ i 4)))
                             a)))
          (setq i (+ i 1))))
      acc))))

;;; --- 6c. 束縛数10のlet*(第1部: 傾きの線形性確認用) ---
(defglobal %%bench-interp-let10 (lambda (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a i) (b (+ a 1)) (c (+ b 1)) (d (+ c 1)) (e (+ d 1))
                                  (f (+ e 1)) (g (+ f 1)) (h (+ g 1)) (j (+ h 1)) (k (+ j 1)))
                             k)))
          (setq i (+ i 1))))
      acc))))

;;; --- 6d. 束縛数5だがinitが全て定数(第1部: init評価分の分離用) ---
;; initが全て即値リテラル。即値はGC_PROTECTもcontrol transferチェックも省略
;; されるため、これが「束縛そのもの」の純コストになる。bodyは最後の束縛だけを
;; 参照する(全変数を参照すると内側lambdaに捕捉されフォールバック経路になる)
(defglobal %%bench-interp-let5const (lambda (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a 1) (b 2) (c 3) (d 4) (e 5))
                             e)))
          (setq i (+ i 1))))
      acc))))

;;; --- 6e. 束縛数5のlet(並列束縛、第1部: let*との傾き比較 + GC_PROTECT分の分離用) ---
;; initは非box化ローカル参照なのでGC_PROTECTは発行されるがcontrol transfer
;; チェックは省略される。let5constとの差がGC_PROTECT1回分のコストになる
(defglobal %%bench-interp-let5par (lambda (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let ((a i) (b i) (c i) (d i) (e i))
                             e)))
          (setq i (+ i 1))))
      acc))))

;;; --- 7. cons/リスト操作(長さ1000のリストを構築して走査、N/1000回) ---
;; ここの1000は再帰の深さ(BENCH_REC_DEPTH)ではなくリスト長なので、
;; 仕事の単位数をNに保つため除数は1000のままにする
(defglobal %%bench-interp-cons (lambda (n)
  (let ((reps (div n 1000)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (let ((lst nil) (i 0))
            (progn
              (while (< i 1000)
                (progn
                  (setq lst (cons i lst))
                  (setq i (+ i 1))))
              (while lst
                (progn
                  (setq total (+ total (car lst)))
                  (setq lst (cdr lst))))))
          (setq r (+ r 1))))
      total))))

;;; --- 8. イテレーション構文(ISLisp標準のfor) ---
(defglobal %%bench-interp-for (lambda (n)
  (for ((i 0 (+ i 1)) (acc 0 (+ acc i)))
       ((>= i n) acc))))

;;; --- 9. ベクタ操作(書き込み+読み出し) ---
(defglobal %%bench-interp-vector (lambda (n)
  (let ((vec (create-vector 1000 0)) (acc 0) (idx 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (set-elt i vec idx)
          (setq acc (+ acc (elt vec idx)))
          (setq idx (if (>= (+ idx 1) 1000) 0 (+ idx 1)))
          (setq i (+ i 1))))
      acc))))

;;; --- 10. 関数呼び出し(非末尾・非自己再帰) ---
(defglobal %%bench-interp-callee (lambda (x)
  (+ x 1)))

;;; [呼べない] %%bench-interp-funcall -- 本体が別のベンチ関数を名前で呼ぶ(defglobal は変数なので関数位置では解決されない)
(defglobal %%bench-interp-funcall (lambda (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (%%bench-interp-callee acc))
          (setq i (+ i 1))))
      acc))))

;;; --- [性能測定] Phase5 第0部: スタックガードの動作確認用(深度1000で溢れる) ---
;; AOTの非末尾再帰は1レベルあたり約330byte消費するため、深度1000で約330KBとなり
;; 256KBのスタックを溢れさせる。ガードが無ければゲストが無反応のまま停止する
;;; [呼べない] %%bench-interp-deep-recursion -- 本体が別のベンチ関数を名前で呼ぶ(defglobal は変数なので関数位置では解決されない)
(defglobal %%bench-interp-deep-recursion (lambda (n)
  (if (= n 0)
      0
      (+ n (%%bench-interp-deep-recursion (- n 1))))))

;;; --- 11. for の並列代入の確認(ベンチではなく意味論の probe) ---
;; [性能測定/意味論] ISLisp の for は step 式を**並列に評価して並列に代入する**。
;; init.lisp の for は毎反復 (list step1 step2 ...) を作ってからまとめて代入し、
;; transpile.lisp の expand-for は一時変数へ全部評価してからまとめて書き戻す。
;; **形が違うので、同じ意味論になっているかを実測で確かめる必要がある**
;; (documents/for-expansion.md)。
;;
;; i と j を互いの旧値で更新する。並列なら 1 反復ごとに入れ替わり、
;; 逐次(i を先に書き換えてから j がその新値を読む)なら両方 1 になる。
;;
;;   n=1 -> 並列 (1 0) / 逐次 (1 1)
;;   n=2 -> 並列 (0 1) / 逐次 (1 1)
;;
;; AOT・JIT・インタプリタの 3 経路で同じ答えになることを
;; test/lisp/for_semantics_test.lisp が固定する。
;; 戻り値はリストではなく (+ (* i 10) j) の fixnum にする
;; (AOT のトランスパイラに list を使わせないため。値で区別できれば十分)
(defglobal %%bench-interp-for-swap (lambda (n)
  (for ((i 0 j) (j 1 i) (k 0 (+ k 1)))
       ((>= k n) (+ (* i 10) j)))))
