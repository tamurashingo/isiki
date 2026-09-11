;;;; 構文別ベンチマークスイートのLisp側(AOTトランスパイル対象)実装。
;;;;
;;;; [性能測定] documents/performance-measurement.md「構文別ベンチマークスイート」
;;;; 節を参照。src/c/bench_subprimitive.c の %%BENCH-C-* 各関数と1対1で対応する
;;;; 「同じ計算をする典型的なLispコード」を置く。両者を同じ命令数計測基盤
;;;; (TCGプラグイン+boot-only差分)で測り、比(AOT版 ÷ C版)を構文ごとに比較する。
;;;;
;;;; 記述方針:
;;;;   - ISLisp標準の構文を素直に使い、特殊なマクロや最適化を意図した書き方は
;;;;     避ける(「典型的なLispコードを書いたら何が起きるか」を測るのが目的)。
;;;;   - init_aot.lisp/utility.lispと同じAOTの制約(defunのパラメータはシンボル
;;;;     のみ、bodyは単一式のみ)に従う。
;;;;   - 再帰系はC側と同様、深さ100で区切って外側のwhileで繰り返す(仕事の
;;;;     単位数は常にN個になる)。深さ1000だとAOTの非末尾再帰が256KBのスタックを
;;;;     溢れさせる(bench_subprimitive.cのBENCH_REC_DEPTHのコメント参照)。
;;;;
;;;; 注意: dotimes/dolistはISLispには存在せず(CommonLisp由来)、トランスパイラも
;;;; 未対応のため、higher-levelなイテレーション構文としてはISLisp標準のforを測る。

;;; --- 1. 単純ループ(制御構造のベースライン) ---
(defun %%bench-aot-loop (n)
  (let ((i n))
    (progn
      (while (> i 0)
        (setq i (- i 1)))
      i)))

;;; --- 2. fixnum算術(加算+比較) ---
(defun %%bench-aot-arith (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc i))
          (setq i (+ i 1))))
      acc)))

;;; --- 3. 末尾再帰 ---
(defun %%bench-aot-tailrec-step (n acc)
  (if (= n 0)
      acc
      (%%bench-aot-tailrec-step (- n 1) (+ acc n))))

(defun %%bench-aot-tailrec (n)
  (let ((reps (div n 100)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-aot-tailrec-step 100 0)))
          (setq r (+ r 1))))
      total)))

;;; --- 4. 非末尾再帰 ---
(defun %%bench-aot-nontailrec-step (n)
  (if (= n 0)
      0
      (+ n (%%bench-aot-nontailrec-step (- n 1)))))

(defun %%bench-aot-nontailrec (n)
  (let ((reps (div n 100)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-aot-nontailrec-step 100)))
          (setq r (+ r 1))))
      total)))

;;; --- 5. 条件分岐(5分岐のcond) ---
(defun %%bench-aot-branch (n)
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
(defun %%bench-aot-let (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a i) (b (+ a 1)) (c (+ b 1)) (d (+ c 1)) (e (+ d 1)))
                             e)))
          (setq i (+ i 1))))
      acc)))

;;; --- 6b. ローカル変数束縛(let*で1変数) ---
;; [性能測定] Phase2の2-0-1: %%bench-aot-letと同じ計算(acc += i+4)を、
;; 内側のlet*の束縛数だけ1個に変えたもの。5束縛版との差を4で割ると、
;; AOTの実コード上での「1束縛あたりのコスト」(インライン展開で消せる分)が
;; 分離できる。
(defun %%bench-aot-let1 (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (+ acc (let* ((a (+ i 4)))
                             a)))
          (setq i (+ i 1))))
      acc)))

;;; --- 6c. 束縛数10のlet*(第1部: 傾きの線形性確認用) ---
(defun %%bench-aot-let10 (n)
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
(defun %%bench-aot-let5const (n)
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
(defun %%bench-aot-let5par (n)
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
(defun %%bench-aot-cons (n)
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
(defun %%bench-aot-for (n)
  (for ((i 0 (+ i 1)) (acc 0 (+ acc i)))
       ((>= i n) acc)))

;;; --- 9. ベクタ操作(書き込み+読み出し) ---
(defun %%bench-aot-vector (n)
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
(defun %%bench-aot-callee (x)
  (+ x 1))

(defun %%bench-aot-funcall (n)
  (let ((acc 0) (i 0))
    (progn
      (while (< i n)
        (progn
          (setq acc (%%bench-aot-callee acc))
          (setq i (+ i 1))))
      acc)))
