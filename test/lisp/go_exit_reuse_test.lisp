;; test/lisp/go_exit_reuse_test.lisp
;;
;; (go tag) の脱出の印を**行き先ごとに 1 つだけ作って使い回す**ようにした(issue #153)。
;; 解釈実行の `for` は 1 反復あたり 96 byte 確保していて、その 1/3(32 byte)が
;; 「跳ぶことを知らせるためだけの物体」だった。
;;
;; **使い回しは共有された可変状態である。** issue #153 が着手を見送った理由がこれで、
;; このリポジトリは同じ族のバグを繰り返し踏んでいる。だから
;; **壊れるとしたらどこか**を名指しして押さえる:
;;
;;   1. 入れ子の tagbody が同じ名前のタグを持つとき、内側が先に拾うこと
;;   2. unwind-protect の片付けの中から go したとき(伝播の途中で別の go が起きる)
;;   3. 行き先が 2 つ以上あるとき、印が混ざらないこと
;;   4. 2 回目以降の呼び出しでも効くこと(= 使い回した印が壊れていないこと)
;;
;; 使い回し自身は Lisp から同一性で観測できない(印は値として表に出ない)。
;; **減ったことを見るのは test/lisp/alloc_breakdown.lisp**(`make test-qemu-alloc-breakdown`)。
;; ここで見るのは**意味論が変わっていないこと**である。

;;; --- 1. 入れ子の tagbody が同じ名前のタグを持つ ---
;; `go` は**内側の tagbody**に捕まる(印は名前しか運ばないが、
;; 照合は内側の tagbody が自分の body に対して行うので、共有しても変わらない)
(defun ger-nested-same-name ()
  (let ((log nil))
    (tagbody
      (tagbody
        (go l)
        (setq log (cons 'inner-not-reached log))
       l
        (setq log (cons 'inner-l log)))
      (setq log (cons 'outer-after log))
     l
      (setq log (cons 'outer-l log)))
    log))
(assert-equal '(outer-l outer-after inner-l) (ger-nested-same-name))

;;; --- 2. unwind-protect の片付けの中から go する ---
;; 伝播の途中で片付けが走る。片付けが終わってから go が完了すること
(defun ger-unwind-cleanup ()
  (let ((log nil))
    (tagbody
      (unwind-protect
          (go done)
        (setq log (cons 'cleanup log)))
      (setq log (cons 'not-reached log))
     done
      (setq log (cons 'done log)))
    log))
(assert-equal '(done cleanup) (ger-unwind-cleanup))

;;; --- 3. 行き先が 2 つ以上あるとき、印が混ざらない ---
;; 印はシンボルごとに 1 つなので、a と b で別の物体になる
(defun ger-two-tags (which)
  (let ((r nil))
    (tagbody
      (if (= which 1) (go a) (go b))
     a
      (setq r 'went-a)
      (go end)
     b
      (setq r 'went-b)
     end)
    r))
(assert-equal 'went-a (ger-two-tags 1))
(assert-equal 'went-b (ger-two-tags 2))

;;; --- 4. 2 回目以降も効く(使い回した印が壊れていない)---
;; 1 回目は作る、2 回目以降はキャッシュを返す。**両方を通ること**を見る
(assert-equal 'went-b (ger-two-tags 2))
(assert-equal 'went-a (ger-two-tags 1))

;;; --- 5. ループで何度も使い回す(これが減らしたかった経路)---
(defun ger-loop-sum (n)
  (let ((s 0))
    (for ((i 0 (+ i 1))) ((>= i n) s)
      (setq s (+ s i)))))
(assert-equal 45 (ger-loop-sum 10))
(assert-equal 4950 (ger-loop-sum 100))

;;; --- 6. 入れ子のループ(内側と外側で同じ %for-loop を使い回す)---
;; `for` の展開形は gensym を使わず `%for-loop` を固定で使うので、
;; **入れ子にすると同じ名前のタグが 2 つ live になる**。ここが一番怖い所である
(defun ger-nested-loops (n m)
  (let ((c 0))
    (for ((i 0 (+ i 1))) ((>= i n) c)
      (for ((j 0 (+ j 1))) ((>= j m) nil)
        (setq c (+ c 1))))))
(assert-equal 6 (ger-nested-loops 2 3))
(assert-equal 20 (ger-nested-loops 4 5))
