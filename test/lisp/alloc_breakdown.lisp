;; test/lisp/alloc_breakdown.lisp
;;
;; [計器] **インタプリタ `for` の 1 反復あたりの確保を、理由別に出す(M8-T2)。**
;; documents/heap-allocation-reasons.md §3。
;;
;;   make test-qemu-alloc-breakdown
;;
;; ## なぜ傾きで出すのか
;;
;; **絶対値は事前に導けない。** インタプリタでは「測る行為そのものが確保する」
;; (関数を呼ぶたびに引数リストを対で作る)ので、1 回の測定には
;; 測定自身の確保が一定量混ざる。
;;
;; **反復数を変えた 2 点の差を取れば、その一定量は消える。**
;; ここでは n=100 と n=200 の差を 100 で割って 1 反復あたりを出す。
;;
;; ## 経路
;;
;; **`for` は最上位の形として置く。** `defun` の中に入れると JIT が
;; コンパイルしてしまい、インタプリタの確保ではなくなる。

;; 理由の番号は C 側 runtime.h の #define と対応する
(defglobal ab-count 23)

;; 全部の桶を 1 つのリストに写す。
;; **番号の大きい方から読む**ので、この関数自身が作る対は
;; 対の桶(番号 1)を最後に汚す。量は一定なので差で消える
(defun ab-snapshot ()
  (let ((acc nil) (i (- ab-count 1)))
    (while (>= i 0)
      (setq acc (cons (%%alloc-reason-bytes i) acc))
      (setq i (- i 1)))
    acc))

;; 2 点の差を 1 反復あたりに直して出す
(defun ab-report (s1 s2 iters)
  (let ((i 0) (a s1) (b s2))
    (while (< i ab-count)
      (let ((d (- (car b) (car a))))
        (if (> d 0)
            (format *isiki-test-stream* "#alloc-slope reason=~D delta=~D per-iter=~D~%"
                    i d (div d iters))
            nil))
      (setq a (cdr a))
      (setq b (cdr b))
      (setq i (+ i 1)))
    nil))

;;; --- 測定 ---
;; 1 点目: 100 反復
(%%alloc-reason-reset)
(for ((i 0 (+ i 1))) ((>= i 100) nil))
(defglobal ab-s1 (ab-snapshot))

;; 2 点目: 200 反復
(%%alloc-reason-reset)
(for ((i 0 (+ i 1))) ((>= i 200) nil))
(defglobal ab-s2 (ab-snapshot))

(ab-report ab-s1 ab-s2 100)

;;; --- 内訳の主張(測る前に documents/heap-allocation-reasons.md §3 で宣言した) ---
;; 対 4 個(`(>= i n)` と `(+ i 1)` の引数リスト)+ 脱出の印 1 個(`(go %for-loop)`)
(defglobal ab-cons (div (- (car (cdr ab-s2)) (car (cdr ab-s1))) 100))
(defun ab-nth (lst n)
  (if (= n 0) (car lst) (ab-nth (cdr lst) (- n 1))))
(defglobal ab-go (div (- (ab-nth ab-s2 17) (ab-nth ab-s1 17)) 100))
(format *isiki-test-stream* "#alloc-for-interp cons=~D go=~D total=~D~%"
        ab-cons ab-go (+ ab-cons ab-go))

;; **宣言した内訳**: 対 64 + 脱出の印 32 = 96
(assert-equal 64 ab-cons)
(assert-equal 32 ab-go)
(assert-equal 96 (+ ab-cons ab-go))

;; 数え漏れが無いこと・合計が水位と合うこと(T1 の不変量をここでも見る)
(assert-equal 0 (%%alloc-reason-gc-since-reset))
(assert-equal 0 (%%alloc-reason-diff))
