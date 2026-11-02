;;;; 構文別ベンチマークスイートのJIT経路(defun->za_try_compile_defun)実装。
;;;;
;;;; **このファイルは生成物である。直接編集しないこと。**
;;;;   真実源: src/lisp/bench_aot.lisp
;;;;   生成:   python3 tools/bench/gen_bench_jit.py
;;;;   同期の固定: make test-bench-jit-sync (tools/bench/check_bench_jit_sync.sh)
;;;;
;;;; 本体の S 式は bench_aot.lisp と1文字も違わない(関数名の
;;;; %%bench-aot-* -> %%bench-jit-* の置換だけが差分)。そうしないと
;;;; AOT 経路と JIT 経路の命令数を比較しても意味を持たない。
;;;;
;;;; [性能測定] documents/performance-measurement.md「JIT 経路の基準値」節を参照。
;;;; AOT 版(%%bench-aot-*、カーネルへ埋め込み済みのネイティブコード)と違い、
;;;; こちらは実行時に load して defun させるため za_try_compile_defun が走る。
;;;; **測定の前に %%za-compiled-p が T であることを必ず確認すること**
;;;; (test/lisp/bench_jit_guard.lisp)。インタプリタへ落ちた関数を測ると
;;;; 「JIT を測ったつもりでインタプリタを測った数字」が基準値として残る。

;;; --- 1. 単純ループ(制御構造のベースライン) ---
(defun %%bench-jit-loop (n)
  (let ((i n))
    (progn
      (while (> i 0)
        (setq i (- i 1)))
      i)))

;;; --- 2. fixnum算術(加算+比較) ---
(defun %%bench-jit-arith (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc i))
          (setq i (+ i 1))))
      acc)))

;;; --- 3. 末尾再帰 ---
(defun %%bench-jit-tailrec-step (n acc)
  (if (= n 0)
      acc
      (%%bench-jit-tailrec-step (- n 1) (+ acc n))))

(defun %%bench-jit-tailrec (n)
  (let ((reps (div n 100)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-jit-tailrec-step 100 0)))
          (setq r (+ r 1))))
      total)))

;;; --- 4. 非末尾再帰 ---
(defun %%bench-jit-nontailrec-step (n)
  (if (= n 0)
      0
      (+ n (%%bench-jit-nontailrec-step (- n 1)))))

(defun %%bench-jit-nontailrec (n)
  (let ((reps (div n 100)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-jit-nontailrec-step 100)))
          (setq r (+ r 1))))
      total)))

;;; --- 5. 条件分岐(5分岐のcond) ---
(defun %%bench-jit-branch (n)
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
      acc)))

;;; --- 6. ローカル変数束縛(let*で5変数) ---
(defun %%bench-jit-let (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a i) (b (+ a 1)) (c (+ b 1)) (d (+ c 1)) (e (+ d 1)))
                             e)))
          (setq i (+ i 1))))
      acc)))

;;; --- 6b. ローカル変数束縛(let*で1変数) ---
;; [性能測定] Phase2の2-0-1: %%bench-jit-letと同じ計算(acc += i+4)を、
;; 内側のlet*の束縛数だけ1個に変えたもの。5束縛版との差を4で割ると、
;; AOTの実コード上での「1束縛あたりのコスト」(インライン展開で消せる分)が
;; 分離できる。
(defun %%bench-jit-let1 (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a (+ i 4)))
                             a)))
          (setq i (+ i 1))))
      acc)))

;;; --- 6c. 束縛数10のlet*(第1部: 傾きの線形性確認用) ---
(defun %%bench-jit-let10 (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a i) (b (+ a 1)) (c (+ b 1)) (d (+ c 1)) (e (+ d 1))
                                  (f (+ e 1)) (g (+ f 1)) (h (+ g 1)) (j (+ h 1)) (k (+ j 1)))
                             k)))
          (setq i (+ i 1))))
      acc)))

;;; --- 6d. 束縛数5だがinitが全て定数(第1部: init評価分の分離用) ---
;; initが全て即値リテラル。即値はGC_PROTECTもcontrol transferチェックも省略
;; されるため、これが「束縛そのもの」の純コストになる。bodyは最後の束縛だけを
;; 参照する(全変数を参照すると内側lambdaに捕捉されフォールバック経路になる)
(defun %%bench-jit-let5const (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a 1) (b 2) (c 3) (d 4) (e 5))
                             e)))
          (setq i (+ i 1))))
      acc)))

;;; --- 6e. 束縛数5のlet(並列束縛、第1部: let*との傾き比較 + GC_PROTECT分の分離用) ---
;; initは非box化ローカル参照なのでGC_PROTECTは発行されるがcontrol transfer
;; チェックは省略される。let5constとの差がGC_PROTECT1回分のコストになる
(defun %%bench-jit-let5par (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let ((a i) (b i) (c i) (d i) (e i))
                             e)))
          (setq i (+ i 1))))
      acc)))

;;; --- 7. cons/リスト操作(長さ1000のリストを構築して走査、N/1000回) ---
;; ここの1000は再帰の深さ(BENCH_REC_DEPTH)ではなくリスト長なので、
;; 仕事の単位数をNに保つため除数は1000のままにする
(defun %%bench-jit-cons (n)
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
      total)))

;;; --- 8. イテレーション構文(ISLisp標準のfor) ---
(defun %%bench-jit-for (n)
  (for ((i 0 (+ i 1)) (acc 0 (+ acc i)))
       ((>= i n) acc)))

;;; --- 9. ベクタ操作(書き込み+読み出し) ---
(defun %%bench-jit-vector (n)
  (let ((vec (create-vector 1000 0)) (acc 0) (idx 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (set-elt i vec idx)
          (setq acc (+ acc (elt vec idx)))
          (setq idx (if (>= (+ idx 1) 1000) 0 (+ idx 1)))
          (setq i (+ i 1))))
      acc)))

;;; --- 10. 関数呼び出し(非末尾・非自己再帰) ---
(defun %%bench-jit-callee (x)
  (+ x 1))

(defun %%bench-jit-funcall (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (%%bench-jit-callee acc))
          (setq i (+ i 1))))
      acc)))

;;; --- [性能測定] Phase5 第0部: スタックガードの動作確認用(深度1000で溢れる) ---
;; AOTの非末尾再帰は1レベルあたり約330byte消費するため、深度1000で約330KBとなり
;; 256KBのスタックを溢れさせる。ガードが無ければゲストが無反応のまま停止する
(defun %%bench-jit-deep-recursion (n)
  (if (= n 0)
      0
      (+ n (%%bench-jit-deep-recursion (- n 1)))))

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
(defun %%bench-jit-for-swap (n)
  (for ((i 0 j) (j 1 i) (k 0 (+ k 1)))
       ((>= k n) (+ (* i 10) j))))
