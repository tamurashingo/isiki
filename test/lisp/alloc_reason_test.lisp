;; test/lisp/alloc_reason_test.lisp
;;
;; [計器] **確保の理由別の数え上げ(M8-T1)専用のテスト。**
;; documents/heap-allocation-reasons.md。
;;
;; 走らせ方(通常ビルドでは %%ALLOC-REASON-BYTES が未定義で落ちる。それがゲート):
;;
;;   make test-qemu-alloc-audit
;;
;; **なぜ組み込みを監査ビルドだけに置いたのか**:
;;   組み込みを 1 つ足すと test/c/runtime_test.c の SMALL_HEAP_SIZE の再調整が要る
;;   (余裕は 5KB 未満。症状はテストの NG ではなく**ハング**)。
;;   test/lisp/declare_audit_test.lisp と同じ理由で、専用の make ターゲットで
;;   別ビルドを強制する形を採った。
;;
;; ## 「ちょうど 16 byte 増える」と書けない理由(最初にそう書いて間違えた)
;;
;; **インタプリタは関数を呼ぶたびに引数リストを対で作る。**
;; だから `(cons 1 2)` を 1 回評価したときに対の桶が増える量は 16 ではなく
;; 「16 + 引数リストのぶん」で、しかも測る側の `(%%alloc-reason-bytes ...)` の
;; 呼び出し自身も確保する。**絶対値は事前に導けない。**
;;
;; したがってここでは**導ける主張だけ**を試験にする:
;;
;;   1. 未分類の桶が 0(数え漏れが無い)          … 構造から導ける
;;   2. 合計 = %%HEAP-USED-BYTES の進んだ量       … 構造から導ける
;;   3. 反復数に対して線形(2 階差分が 0)          … 傾きの性質から導ける
;;   4. 物を作ると**その理由の桶が**増える        … 向きだけを主張する
;;
;; **絶対値(1 反復あたり何 byte か)は測って文書に記録する**(M8-T2 がその仕事)。
;; 規則 11 の「断言してよいのは導ける量だけ」に従う。

;; C 側 runtime.h の #define と対応する。**数字が合っていることは下の 4 で縛る**
(defglobal ar-unattributed 0)
(defglobal ar-cons 1)
(defglobal ar-symbol-unint 3)
(defglobal ar-vector-block 6)
(defglobal ar-string 7)
(defglobal ar-inst-vector 19)
(defglobal ar-inst-float 20)
;; 対を n 回作るだけのループ
(defun ar-cons-loop (n)
  (let ((i 0))
    (while (< i n)
      (cons 1 2)
      (setq i (+ i 1)))
    i))

;; 1 反復に対を 1 つ**足した**ループ(陽性対照用)
(defun ar-cons-loop2 (n)
  (let ((i 0))
    (while (< i n)
      (cons 1 2)
      (cons 3 4)
      (setq i (+ i 1)))
    i))

;; 理由 r の桶を、ループを n 回回したあとで読む
(defun ar-run (n r)
  (%%alloc-reason-reset)
  (ar-cons-loop n)
  (%%alloc-reason-bytes r))

;;; --- 1. 未分類が 0(数え漏れが無い) ---
;; **ここが落ちたら、理由を置かずに確保する経路が増えたということ。**
;; 直し方: その確保の直前に ALLOC_AUDIT_REASON(...) を置く。
;; 桶が足りなければ runtime.h の #define を足し、この試験にも足すこと。
;; 対・文字列・配列・浮動小数・gensym を一度に通して、広めに踏む
(%%alloc-reason-reset)
(defglobal ar-mixed (list (cons 1 2) (create-string 3 #\a)
                          (create-array (list 4) 0) 1.5 (gensym)))
(assert-equal 0 (%%alloc-reason-bytes ar-unattributed))
(assert-equal 0 (%%alloc-reason-count ar-unattributed))

;;; --- 2. 合計 = ヒープが進んだ量(二重計上も数え漏れも無い) ---
;; **Lisp から 2 つの計器を読んで引き算してはいけない。**
;; インタプリタでは「測る行為そのものが確保する」(関数呼び出しが引数リストを
;; 対で作る)ので、
;;
;;     (assert-equal (- (%%heap-used-bytes) h0) (全部の桶を足した値))
;;
;; と書くと左辺を読んだ後・右辺を数える間にも確保が起きて**必ずずれる**。
;; 最初の版は実際にこれで落ちた(桶の合計 4304 / ヒープの差 3584。差 720 は
;; reset と「ヒープを読む」の間に挟まった評価の確保だった)。
;;
;; だから差は **C の中で、間に何も挟まらない形で**取る(%%ALLOC-REASON-DIFF)。
;; こうすると**いつ呼んでも 0** になる(呼ぶまでの確保は合計と水位の両方に等しく入る)
(%%alloc-reason-reset)
(defglobal ar-work (ar-cons-loop 200))
;; GC が走ると水位は戻るが桶は累積なので、差の判定は成立しない。先に見る
(assert-equal 0 (%%alloc-reason-gc-since-reset))
;; **1 回の確保がちょうど 1 つの桶に入る**ので、構造的に 0 になる。
;; 0 でなければ数え方が壊れている(理由を戻し忘れた、桶に入れずに確保した等)
(assert-equal 0 (%%alloc-reason-diff))

;;; --- 3. 反復数に対して線形(2 階差分が 0) ---
;; 1 反復あたりの量は導けないが、**反復数に比例すること**は導ける。
;; 比例していなければ、反復の中に反復数に依らない確保が紛れている
(defglobal ar-n1 (ar-run 100 ar-cons))
(defglobal ar-n2 (ar-run 200 ar-cons))
(defglobal ar-n3 (ar-run 300 ar-cons))
(assert-equal (- ar-n2 ar-n1) (- ar-n3 ar-n2))
;; 傾きは正(対を作っているのだから増える)
(assert-equal t (> (- ar-n2 ar-n1) 0))
;; 1 反復あたりは 16 の倍数(対は 16 byte。端数が出たら桁がおかしい)
(assert-equal 0 (mod (- ar-n2 ar-n1) 16))

;;; --- 4. 陽性対照: 対を 1 つ足すと**対の桶だけ**が増える ---
;; 番号 1 が本当に「対」であることの裏。絶対値ではなく**向き**を主張する
(%%alloc-reason-reset)
(ar-cons-loop 100)
(defglobal ar-c1 (%%alloc-reason-bytes ar-cons))
(defglobal ar-s1 (%%alloc-reason-bytes ar-string))
(defglobal ar-v1 (%%alloc-reason-bytes ar-vector-block))
(%%alloc-reason-reset)
(ar-cons-loop2 100)
(defglobal ar-c2 (%%alloc-reason-bytes ar-cons))
(defglobal ar-s2 (%%alloc-reason-bytes ar-string))
(defglobal ar-v2 (%%alloc-reason-bytes ar-vector-block))
;; 対の桶は増える
(assert-equal t (> ar-c2 ar-c1))
;; **増えた量は 100 反復ぶんなので 100 で割り切れる**
(assert-equal 0 (mod (- ar-c2 ar-c1) 100))
;; 他の理由の桶は動かない(動くなら理由を戻し忘れている)
(assert-equal ar-s1 ar-s2)
(assert-equal ar-v1 ar-v2)

;;; --- 5. 理由の番号がその理由を指していること(向きだけ) ---
;; 配列は「本体ブロック」と「タグ付きの物体」の 2 つを確保する。
;; **両方が別の桶に入る**ことがこの計器の値である
(%%alloc-reason-reset)
(defglobal ar-vec (create-array (list 4) 0))
(assert-equal t (> (%%alloc-reason-bytes ar-vector-block) 0))
(assert-equal t (> (%%alloc-reason-bytes ar-inst-vector) 0))
;; 浮動小数はタグ付きの物体 1 つ
(%%alloc-reason-reset)
(defglobal ar-f (+ 1.5 2.5))
(assert-equal t (> (%%alloc-reason-bytes ar-inst-float) 0))
;; gensym は interned でない記号
(%%alloc-reason-reset)
(defglobal ar-g (gensym))
(assert-equal t (> (%%alloc-reason-bytes ar-symbol-unint) 0))

;;; --- 6. reset が効く ---
;; **「reset 直後に合計が 0」は Lisp から観測できない。** 合計を数える
;; 全部の桶を足す側が 23 回の呼び出しで確保するので、数え終えた時点では
;; もう 0 ではない(最初の版はこれで落ちた。1872 byte になっていた)。
;;
;; 観測できるのは「**その読み取りが触らない理由の桶**が 0 に戻っていること」である。
;; 桶を 1 つ読むだけでは配列も文字列も浮動小数も作らない
(%%alloc-reason-reset)
(assert-equal 0 (%%alloc-reason-bytes ar-vector-block))
(%%alloc-reason-reset)
(assert-equal 0 (%%alloc-reason-bytes ar-string))
(%%alloc-reason-reset)
(assert-equal 0 (%%alloc-reason-bytes ar-inst-float))
;; 差の不変量は reset の直後にも成り立つ
(%%alloc-reason-reset)
(assert-equal 0 (%%alloc-reason-diff))
