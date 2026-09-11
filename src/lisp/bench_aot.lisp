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
;;;;   - 再帰系はC側と同様、深さ1000で区切って外側のwhileで繰り返す(仕事の
;;;;     単位数は常にN個になる)。
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
  (let ((reps (div n 1000)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-aot-tailrec-step 1000 0)))
          (setq r (+ r 1))))
      total)))

;;; --- 4. 非末尾再帰 ---
(defun %%bench-aot-nontailrec-step (n)
  (if (= n 0)
      0
      (+ n (%%bench-aot-nontailrec-step (- n 1)))))

(defun %%bench-aot-nontailrec (n)
  (let ((reps (div n 1000)) (r 0) (total 0))
    (progn
      (while (< r reps)
        (progn
          (setq total (+ total (%%bench-aot-nontailrec-step 1000)))
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

;;; --- 7. cons/リスト操作(長さ1000のリストを構築して走査、N/1000回) ---
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
