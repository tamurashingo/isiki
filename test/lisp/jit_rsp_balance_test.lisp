;; test/lisp/jit_rsp_balance_test.lisp
;;
;; [回帰] **JIT 生成コードがフレームを戻し忘れたら落ちる。**
;; documents/jit-frame-survey.md §8-5、documents/jit-static-code-check.md。
;;
;; なぜ要るか:
;;   フレームの実寸化(§4。quasiquote/flet をフレーム末尾へ回して関数ごとに
;;   `sub rsp` の量を変える案)では、プロローグの `sub rsp` を後埋めし、
;;   出口の `add rsp` と一致させることになる。出力箇所は
;;   `sub` が 2・`add` が 3(`za.c` の `jit_sub_rsp_imm32` / `jit_add_rsp_imm32`)で、
;;   **`add` の 2 箇所は末尾呼び出しの経路で、本体のコンパイル中に出力される。**
;;
;;   ここが 1 箇所でも狂うと、**コンパイルエラーにはならない。**
;;   `ret` のときに `rsp` がずれ、呼び出し元の戻り番地でないものへ飛ぶので、
;;   症状は「どこで壊れたか分からないハング/暴走」になる。
;;   **最後の歯止めは、生成コードを静的に読む検査しかない。**
;;
;; 怖い故障は 2 種類ある。**両方を見なければならない:**
;;
;;   (A) **値が違う**    — `sub` と `add` の即値が食い違う
;;   (B) **命令が無い**  — 出口の 1 つで `add rsp` を出し忘れる(= 漏らす)
;;
;;   **(B) のほうが本番で怖い。** 実寸化で「使う関数だけ確保する」形にすると、
;;   出口ごとに「この経路では何 byte 戻すか」を判断することになるので、
;;   **判断漏れ = 命令が出ない**が起こりうる。
;;
;; [この検査の初版は (B) を見ていなかった]
;;   初版は「imm32 の即値が全部同じこと」だけを見ていた。**値の一致は、
;;   存在しない命令を見られない。** 1 箇所が丸ごと出なかった場合、残った
;;   `add rsp` は全部正しい値なので**検査は通る。そしてスタックは壊れている。**
;;   実験で確かめた(`add rsp` 7 byte を `nop` で潰すと `bad=0` のまま通った)。
;;   陽性対照も「即値を 1 byte 変える」= (A) 側で取っていたので、
;;   **穴が見えていなかった。**
;;
;; (B) をどう見るか — **`pop r13` を道標にする**
;;   `jit_pop_r13()` の呼び出しは `za.c` に 3 箇所しかなく、**いずれも
;;   `jit_add_rsp_imm32(ZA_FRAME_TOTAL)` の直後**である(エピローグ 1 + 末尾
;;   呼び出し 2)。つまりフレーム解体は必ず
;;       add rsp, imm32 ; pop r13 ; pop rbx ; (ret または jmp)
;;   の形で出る。だから次の 2 つが (B) を捕まえる:
;;
;;   - **`add rsp, imm32` の個数 = `pop r13` の個数**
;;   - **すべての `pop r13` の直前の命令が `add rsp, imm32` であること**(orphan = 0)
;;
;;   **「個数」を使ってよいのはここだけである。** `sub` と `add` の個数は
;;   一致しない(`sub` は入口ごと、`add` は出口経路ごと)。`add` と `pop r13` は
;;   同じ 3 箇所から対で出るので一致する。
;;
;; 測り方:
;;   **実行して確かめてはいけない。** 狂ったコードを呼べば、その場でスタックが
;;   壊れてテスト自体が死ぬ(「落ちる」ではなく「ハングする」)。
;;   命令の長さは `%%DISASM-ITEM`(自前のデコーダ)に数えさせる。
;;   **バイト列を素で走査してはいけない** — `movabs` の 8 byte 即値の中に
;;   `48 81 EC` が現れれば `sub rsp, imm32` と誤認する。
;;   **境界はデコーダ、値はバイト。**
;;
;;   `sub rsp, 0x20` / `add rsp, 0x20`(imm8)は `jit_call_r11` が call の前後で
;;   対にして出す shadow space なので、ここでは数えない。

;;; --- 走査器 ---

;; off の命令が rsp を imm32 で動かすものなら即値を返す。違えば -1。
;; kind: 236 = 0xEC(sub rsp, imm32) / 196 = 0xC4(add rsp, imm32)
(defun %jrb-imm32-at (base off kind)
  (if (and (= (%%peek (+ base off)) 72)        ; 0x48 REX.W
           (= (%%peek (+ base off 1)) 129)     ; 0x81 (imm32 形式)
           (= (%%peek (+ base off 2)) kind))
      (+ (%%peek (+ base off 3))
         (* 256 (%%peek (+ base off 4)))
         (* 65536 (%%peek (+ base off 5)))
         (* 16777216 (%%peek (+ base off 6))))
    -1))

;; off の命令が「解体の道標」かどうか
(defun %jrb-pop-r13-p (base off)       ; 41 5D
  (and (= (%%peek (+ base off)) 65) (= (%%peek (+ base off 1)) 93)))
(defun %jrb-pop-rbx-p (base off)       ; 5B
  (= (%%peek (+ base off)) 91))
;; off が `add rsp, 40`(48 83 C4 28)かどうか。
;; **スタックガード(issue #110)の断念経路**が出す形で、
;; シャドウスペース 32 + 16byte 境界合わせ 8 を戻すもの。
;; **フレームを作る前に抜ける出口**なので、解体(add rsp, imm32)を伴わない
(defun %jrb-add-rsp-40-p (base off)
  (and (= (%%peek (+ base off)) 72) (= (%%peek (+ base off 1)) 131)
       (= (%%peek (+ base off 2)) 196) (= (%%peek (+ base off 3)) 40)))
;; off の命令が「出口」かどうか。**decidable なものだけ数える。**
;;   ret        C3
;;   jmp r11    41 FF E3   (**call r11 は 41 FF D3**。ModRM で区別する)
;; `jmp rel32`(トランポリンへ抜ける経路)は**本体内の分岐と区別が付かない**ので
;; 数えない。そのぶん残る穴は下の §注意 に書いてある
(defun %jrb-exit-p (base off)
  (if (= (%%peek (+ base off)) 195)
      t
    (and (= (%%peek (+ base off)) 65)
         (= (%%peek (+ base off 1)) 255)
         (= (%%peek (+ base off 2)) 227))))

;; 生成コード全体を命令単位で歩き、
;;   (sub の個数  add の個数  即値の食い違い  即値  pop r13 の個数  orphan の個数
;;    出口の個数  解体を伴わない出口の個数)
;; を返す。
;;   orphan      = 直前の命令が add rsp, imm32 でない pop r13 の数
;;   exit-orphan = **解体でもフレーム前の脱出でもない出口の数**
;;   nteardown   = 解体を伴う出口の数
;;   npreframe   = フレームを作る前に抜ける出口の数(スタックガードの断念経路)
(defun %jrb-audit (f)
  (let ((base (%%disasm-code-base f)) (len (%%disasm-code-len f)))
    (let ((off 0) (nsub 0) (nadd 0) (bad 0) (fsz 0) (npop 0) (orphan 0)
          (nexit 0) (exorphan 0) (nteardown 0) (npreframe 0)
          (p1 -1) (p2 -1) (p3 -1))
      (progn
        (while (< off len)
          (let ((it (%%disasm-item base len off)))
            (if (null it)
                (setq off len)
              (let ((s (%jrb-imm32-at base off 236))
                    (a (%jrb-imm32-at base off 196)))
                (progn
                  (if (> s 0)
                      (progn
                        (setq nsub (+ nsub 1))
                        (if (= fsz 0) (setq fsz s) nil)
                        (if (= s fsz) nil (setq bad (+ bad 1))))
                    nil)
                  (if (> a 0)
                      (progn
                        (setq nadd (+ nadd 1))
                        (if (= fsz 0) (setq fsz a) nil)
                        (if (= a fsz) nil (setq bad (+ bad 1))))
                    nil)
                  (if (%jrb-pop-r13-p base off)
                      (progn
                        (setq npop (+ npop 1))
                        (if (< p1 0)
                            (setq orphan (+ orphan 1))
                          (if (> (%jrb-imm32-at base p1 196) 0)
                              nil
                            (setq orphan (+ orphan 1)))))
                    nil)
                  ;; **出口の数え上げ。** 出口は 2 通りのどちらかでなければならない:
                  ;;   (a) 解体を伴う     add rsp,imm32 -> pop r13 -> pop rbx -> 出口
                  ;;   (b) フレーム前に抜ける  add rsp,40 -> ret(スタックガードの断念経路)
                  ;; どちらでもない出口は orphan = **4 つ目の出口が増えた**ということ
                  (if (%jrb-exit-p base off)
                      (progn
                        (setq nexit (+ nexit 1))
                        (if (and (>= p3 0)
                                 (%jrb-pop-rbx-p base p1)
                                 (%jrb-pop-r13-p base p2)
                                 (> (%jrb-imm32-at base p3 196) 0))
                            (setq nteardown (+ nteardown 1))
                          (if (and (>= p1 0) (%jrb-add-rsp-40-p base p1))
                              (setq npreframe (+ npreframe 1))
                            (setq exorphan (+ exorphan 1)))))
                    nil)
                  (setq p3 p2) (setq p2 p1) (setq p1 off)
                  (setq off (+ off (car it))))))))
        (list nsub nadd bad fsz npop orphan nexit exorphan nteardown npreframe)))))

(defun %jrb-nsub   (f) (car (%jrb-audit f)))
(defun %jrb-nadd   (f) (car (cdr (%jrb-audit f))))
(defun %jrb-bad    (f) (car (cdr (cdr (%jrb-audit f)))))
(defun %jrb-fsz    (f) (car (cdr (cdr (cdr (%jrb-audit f))))))
(defun %jrb-npop   (f) (car (cdr (cdr (cdr (cdr (%jrb-audit f)))))))
(defun %jrb-orphan (f) (car (cdr (cdr (cdr (cdr (cdr (%jrb-audit f))))))))
(defun %jrb-nexit     (f) (elt (%jrb-audit f) 6))
(defun %jrb-exorphan  (f) (elt (%jrb-audit f) 7))
(defun %jrb-nteardown (f) (elt (%jrb-audit f) 8))
(defun %jrb-npreframe (f) (elt (%jrb-audit f) 9))

;; 最初の pop r13 のオフセット。見つからなければ -1
(defun %jrb-first-pop-r13-off (f)
  (let ((base (%%disasm-code-base f)) (len (%%disasm-code-len f)))
    (let ((off 0) (found -1))
      (progn
        (while (and (< found 0) (< off len))
          (let ((it (%%disasm-item base len off)))
            (if (null it)
                (setq off len)
              (progn
                (if (%jrb-pop-r13-p base off) (setq found off) nil)
                (setq off (+ off (car it)))))))
        found))))

;; 最初の add rsp, imm32 のオフセット。見つからなければ -1
(defun %jrb-first-add-off (f)
  (let ((base (%%disasm-code-base f)) (len (%%disasm-code-len f)))
    (let ((off 0) (found -1))
      (progn
        (while (and (< found 0) (< off len))
          (let ((it (%%disasm-item base len off)))
            (if (null it)
                (setq off len)
              (progn
                (if (> (%jrb-imm32-at base off 196) 0) (setq found off) nil)
                (setq off (+ off (car it)))))))
        found))))

;;; --- 1. 経路ごとに整合していること ---
;;
;; 機能ごとに出口の形が違う(エピローグ / 末尾呼び出し / NLX / tagbody の go)。
;; どれも「即値が 1 つの値で揃っている」かつ「解体が対になっている」こと。

(defun %jrb-leaf (n) (+ n 1))
(defun %jrb-plain (n) (let ((a n)) (+ a 1)))
(defun %jrb-tail (n) (%jrb-leaf n))
(defun %jrb-rec (n) (if (= n 0) 0 (+ n (%jrb-rec (- n 1)))))
(defun %jrb-nlx (n) (block done (if (< n 0) (return-from done 'neg) n)))
(defun %jrb-catch (n) (catch 'tag (if (< n 0) (throw 'tag 'neg) n)))
;; 仮引数への setq は JIT が別の理由で断念するので、局所変数へ受け直す
;; (jit_limits_test.lisp の TAGBODY で踏んだのと同じ罠)
(defun %jrb-tagbody (n) (let ((r n)) (progn (tagbody (if (> r 0) (go jrb-done) nil) (setq r 0) jrb-done) r)))
(defun %jrb-qq (n) `(a ,n b))
(defun %jrb-flet (n) (flet ((f (y) (+ y 1))) (f n)))

(assert-equal t (%%za-compiled-p (function %jrb-leaf)))
(assert-equal t (%%za-compiled-p (function %jrb-plain)))
(assert-equal t (%%za-compiled-p (function %jrb-tail)))
(assert-equal t (%%za-compiled-p (function %jrb-rec)))
(assert-equal t (%%za-compiled-p (function %jrb-nlx)))
(assert-equal t (%%za-compiled-p (function %jrb-catch)))
(assert-equal t (%%za-compiled-p (function %jrb-tagbody)))
(assert-equal t (%%za-compiled-p (function %jrb-qq)))
(assert-equal t (%%za-compiled-p (function %jrb-flet)))

;;; 1-A. 値が揃っていること(故障 A)
(assert-equal 0 (%jrb-bad (function %jrb-leaf)))
(assert-equal 0 (%jrb-bad (function %jrb-plain)))
(assert-equal 0 (%jrb-bad (function %jrb-tail)))
(assert-equal 0 (%jrb-bad (function %jrb-rec)))
(assert-equal 0 (%jrb-bad (function %jrb-nlx)))
(assert-equal 0 (%jrb-bad (function %jrb-catch)))
(assert-equal 0 (%jrb-bad (function %jrb-tagbody)))
(assert-equal 0 (%jrb-bad (function %jrb-qq)))
(assert-equal 0 (%jrb-bad (function %jrb-flet)))

;;; 1-B. 解体が対になっていること(故障 B = 命令が無い)
;; すべての pop r13 の直前が add rsp, imm32 であること
(assert-equal 0 (%jrb-orphan (function %jrb-leaf)))
(assert-equal 0 (%jrb-orphan (function %jrb-plain)))
(assert-equal 0 (%jrb-orphan (function %jrb-tail)))
(assert-equal 0 (%jrb-orphan (function %jrb-rec)))
(assert-equal 0 (%jrb-orphan (function %jrb-nlx)))
(assert-equal 0 (%jrb-orphan (function %jrb-catch)))
(assert-equal 0 (%jrb-orphan (function %jrb-tagbody)))
(assert-equal 0 (%jrb-orphan (function %jrb-qq)))
(assert-equal 0 (%jrb-orphan (function %jrb-flet)))

;; add rsp, imm32 の個数と pop r13 の個数が一致すること
;; (この 2 つは za.c の同じ 3 箇所から対で出る。**sub と add の個数は一致しない**)
(assert-equal t (if (= (%jrb-nadd (function %jrb-leaf))    (%jrb-npop (function %jrb-leaf)))    t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-plain))   (%jrb-npop (function %jrb-plain)))   t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-tail))    (%jrb-npop (function %jrb-tail)))    t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-rec))     (%jrb-npop (function %jrb-rec)))     t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-nlx))     (%jrb-npop (function %jrb-nlx)))     t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-catch))   (%jrb-npop (function %jrb-catch)))   t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-tagbody)) (%jrb-npop (function %jrb-tagbody))) t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-qq))      (%jrb-npop (function %jrb-qq)))      t nil))
(assert-equal t (if (= (%jrb-nadd (function %jrb-flet))    (%jrb-npop (function %jrb-flet)))    t nil))

;; 即値を**関係**で押さえる(2026-10-30)。
;;
;; **ここには 7480 が 2 つ直書きされていた。** 全関数が同じ寸法だったので
;; 「即値 = ZA_FRAME_TOTAL = 7480」と書けたが、**実寸化すると関数ごとに
;; 違う値になる**ので、絶対値は段を越えて生き残らない。
;; 残すのは、実寸化の前でも後でも成り立つ関係だけである:
;;
;;   (a) **0 より大きい** … 走査が空振りしていない(0 なら「食い違い 0」も無意味)
;;   (b) **16 で割った余りが 8** … 呼び出し規約の整列。za.c の
;;       `_Static_assert((ZA_FRAME_TOTAL % 16) == 8)` と同じ条件を、
;;       **出力された機械語の即値の側から**見る。ビルド時の検査は定数 1 つに
;;       対するものなので、**実寸化したあと「どの関数も 8 mod 16」は保証されない**
;;   (c) **quasiquote を使う関数 >= 使わない関数** … 実寸化の前は等しく、
;;       後は qq 側が大きい(jit_frame_test.lisp の qq >= probe と同じ方針)
;;
;; **関数をまたいで「等しい」ことは要求しない。** 等しさを要求すると、
;; それは実寸化を禁じるのと同じになる。
;; 上限(いまの全関数共通の寸法)は jit_frame_test.lisp の jit-frame-size-cap が持ち、
;; 混在そのもの(大と小が呼び合う形)は jit_mixed_frame_test.lisp が見る。
;;
;; 落ちたときにやること:
;;   (a) なら `%%disasm-code-base` / `%%disasm-code-len` が nil を返していないか、
;;       プロローグの形(48 81 EC + imm32)が変わっていないかを見る
;;   (b) なら `tools/jit_frame_breakdown.py` の内訳から**16 の倍数でない領域**を探す。
;;       **判定を緩めてはいけない**(緩めると呼び出し規約が静かに壊れる)
;;   (c) なら「使わない領域を切り落とす」方向が逆になっている
(assert-equal t (if (> (%jrb-fsz (function %jrb-plain)) 0) t nil))
(assert-equal t (if (> (%jrb-fsz (function %jrb-tail)) 0) t nil))
(assert-equal t (if (> (%jrb-fsz (function %jrb-qq)) 0) t nil))
(assert-equal 8 (mod (%jrb-fsz (function %jrb-plain)) 16))
(assert-equal 8 (mod (%jrb-fsz (function %jrb-tail)) 16))
(assert-equal 8 (mod (%jrb-fsz (function %jrb-qq)) 16))
(assert-equal t (if (>= (%jrb-fsz (function %jrb-qq)) (%jrb-fsz (function %jrb-plain))) t nil))
(assert-equal t (if (>= (%jrb-fsz (function %jrb-qq)) (%jrb-fsz (function %jrb-tail))) t nil))

;; 走査が空振りしていないこと(0 件なら「食い違い 0」も「orphan 0」も無意味)
(assert-equal t (if (> (%jrb-nsub (function %jrb-plain)) 0) t nil))
(assert-equal t (if (> (%jrb-nadd (function %jrb-plain)) 0) t nil))
(assert-equal t (if (> (%jrb-npop (function %jrb-plain)) 0) t nil))

;;; 1-C. **出口そのものを数える**(案 d の前に入れた。第 1 段)
;;
;; `pop r13` は**出口の道標であって出口ではない。** PR #120 で残した穴:
;;   **`add rsp` も `pop r13` も出さない 4 つ目の出口が追加されたら、
;;     どちらの検査も鳴らない。**
;; 出口そのもの(`ret` / `jmp r11`)を数えて、**どれも解体を伴っている**ことを見る。
;;
;;   解体 = `add rsp, imm32` -> `pop r13` -> `pop rbx`
;;
;; **`jmp rel32` は数えない。** トランポリンへ抜ける経路に使われるが、
;; 本体内の分岐(if / tagbody の go)と**エンコーディングで区別が付かない**。
;; だから `ret` と `jmp r11` だけが decidable な出口である。
;;
;; §注意(残る穴): **`jmp rel32` で抜ける新しい出口**は、解体を伴わなくても
;; ここでは鳴らない。穴は小さくなったが閉じてはいない
;; (documents/jit-static-code-check.md §5)。
(assert-equal 0 (%jrb-exorphan (function %jrb-leaf)))
(assert-equal 0 (%jrb-exorphan (function %jrb-plain)))
(assert-equal 0 (%jrb-exorphan (function %jrb-tail)))
(assert-equal 0 (%jrb-exorphan (function %jrb-rec)))
(assert-equal 0 (%jrb-exorphan (function %jrb-nlx)))
(assert-equal 0 (%jrb-exorphan (function %jrb-catch)))
(assert-equal 0 (%jrb-exorphan (function %jrb-tagbody)))
(assert-equal 0 (%jrb-exorphan (function %jrb-qq)))
(assert-equal 0 (%jrb-exorphan (function %jrb-flet)))

;; **解体を伴う出口の数は、解体(pop r13)の数を超えない。**
;; (`jmp rel32` で抜ける経路は数えていないので「以下」になる)
(assert-equal t (if (<= (%jrb-nteardown (function %jrb-leaf)) (%jrb-npop (function %jrb-leaf))) t nil))
(assert-equal t (if (<= (%jrb-nteardown (function %jrb-tail)) (%jrb-npop (function %jrb-tail))) t nil))
;; **フレーム前に抜ける出口は、入口の数だけある**(スタックガードが入口ごとに出す)。
;; ここが 0 になったら**スタックガードが消えている**(issue #110 の回帰)
(assert-equal t (if (> (%jrb-npreframe (function %jrb-leaf)) 0) t nil))
(assert-equal t (if (= (%jrb-npreframe (function %jrb-leaf))
                       (%jrb-nsub (function %jrb-leaf))) t nil))

;; **走査が空振りしていないこと。** 出口が 1 つも見つからなければ上は無意味
(assert-equal t (if (> (%jrb-nexit (function %jrb-leaf)) 0) t nil))
(assert-equal t (if (> (%jrb-nexit (function %jrb-tail)) 0) t nil))

;;; --- 2. 末尾呼び出しの経路が走査に入っていること(§4-3) ---
;;
;; `sub` は入口ごと(2 箇所)、`add` は出口経路ごと。末尾呼び出しを含む関数は
;; **本体の途中にも `add rsp` が出る**ので add の個数が sub より多くなる。
;; **ここが等しくなったら末尾呼び出しの経路を走査していない**ので落とす。
(assert-equal t (if (> (%jrb-nadd (function %jrb-tail))
                       (%jrb-nsub (function %jrb-tail))) t nil))
;; 末尾呼び出しを含まない関数では add は出口(エピローグ)の分だけ
(assert-equal t (if (= (%jrb-nadd (function %jrb-leaf))
                       (%jrb-nsub (function %jrb-leaf))) t nil))

;;; --- 3. 陽性対照(規則 8)---
;;
;; 壊したコードは**呼ばない。** 呼べばその場でスタックが壊れてテストごと死ぬ。
;; 静的に読むだけなので安全に壊して戻せる。
;;
;; 壊す場所は **%jrb-tail の最初の `add rsp, imm32`**。これは
;; za.c の `jit_add_rsp_imm32` のうちエピローグより先に出力される 2 箇所の 1 つ、
;; すなわち**末尾呼び出しの経路そのもの**である(§4-3)。

(defglobal jrb-base (%%disasm-code-base (function %jrb-tail)))
(defglobal jrb-base2 (%%disasm-code-base (function %jrb-leaf)))
(defglobal jrb-off (%jrb-first-add-off (function %jrb-tail)))
;; 見つかっていること(-1 だと以下の対照が空振りする)
(assert-equal t (if (> jrb-off 0) t nil))

;;; 3-A. **命令を丸ごと消す**(故障 B の陽性対照。これが初版に無かった)
;;
;; `add rsp, imm32` は 7 byte(48 81 C4 + imm32)。全部 0x90(nop)にすると
;; 「出し忘れた」のと同じ形になる。**残った add rsp の即値は全部正しいので、
;; 値の一致だけを見る検査はこれを通してしまう。**
(defglobal jrb-b0 (%%peek (+ jrb-base jrb-off)))
(defglobal jrb-b1 (%%peek (+ jrb-base (+ jrb-off 1))))
(defglobal jrb-b2 (%%peek (+ jrb-base (+ jrb-off 2))))
(defglobal jrb-b3 (%%peek (+ jrb-base (+ jrb-off 3))))
(defglobal jrb-b4 (%%peek (+ jrb-base (+ jrb-off 4))))
(defglobal jrb-b5 (%%peek (+ jrb-base (+ jrb-off 5))))
(defglobal jrb-b6 (%%peek (+ jrb-base (+ jrb-off 6))))

(defglobal jrb-nop-0 (%%poke (+ jrb-base jrb-off) 144))
(defglobal jrb-nop-1 (%%poke (+ jrb-base (+ jrb-off 1)) 144))
(defglobal jrb-nop-2 (%%poke (+ jrb-base (+ jrb-off 2)) 144))
(defglobal jrb-nop-3 (%%poke (+ jrb-base (+ jrb-off 3)) 144))
(defglobal jrb-nop-4 (%%poke (+ jrb-base (+ jrb-off 4)) 144))
(defglobal jrb-nop-5 (%%poke (+ jrb-base (+ jrb-off 5)) 144))
(defglobal jrb-nop-6 (%%poke (+ jrb-base (+ jrb-off 6)) 144))

;; **これが本題。** 1 箇所消したら orphan が立つこと
(assert-equal t (if (> (%jrb-orphan (function %jrb-tail)) 0) t nil))
;; 個数の一致も崩れること
(assert-equal t (if (= (%jrb-nadd (function %jrb-tail))
                       (%jrb-npop (function %jrb-tail))) nil t))
;; **そして「値の一致」は通ってしまうこと**(初版の穴そのもの。記録として残す)
(assert-equal 0 (%jrb-bad (function %jrb-tail)))

;; 戻す
(defglobal jrb-res-0 (%%poke (+ jrb-base jrb-off) jrb-b0))
(defglobal jrb-res-1 (%%poke (+ jrb-base (+ jrb-off 1)) jrb-b1))
(defglobal jrb-res-2 (%%poke (+ jrb-base (+ jrb-off 2)) jrb-b2))
(defglobal jrb-res-3 (%%poke (+ jrb-base (+ jrb-off 3)) jrb-b3))
(defglobal jrb-res-4 (%%poke (+ jrb-base (+ jrb-off 4)) jrb-b4))
(defglobal jrb-res-5 (%%poke (+ jrb-base (+ jrb-off 5)) jrb-b5))
(defglobal jrb-res-6 (%%poke (+ jrb-base (+ jrb-off 6)) jrb-b6))
(assert-equal 0 (%jrb-orphan (function %jrb-tail)))
(assert-equal t (if (= (%jrb-nadd (function %jrb-tail))
                       (%jrb-npop (function %jrb-tail))) t nil))

;;; 3-B. **即値を変える**(故障 A の陽性対照)
(defglobal jrb-poke-at (+ jrb-base (+ jrb-off 3)))
(defglobal jrb-saved-byte (%%peek jrb-poke-at))
(defglobal jrb-bump (%%poke jrb-poke-at (+ jrb-saved-byte 1)))
(assert-equal t (if (> (%jrb-bad (function %jrb-tail)) 0) t nil))
;; 値を変えただけなので解体の対は崩れない — **A と B は別の故障である**
(assert-equal 0 (%jrb-orphan (function %jrb-tail)))
(defglobal jrb-unbump (%%poke jrb-poke-at jrb-saved-byte))
(assert-equal 0 (%jrb-bad (function %jrb-tail)))

;;; 3-D. **`pop r13` を潰して出口を残す**(出口の数え上げの陽性対照)
;;
;; **道標だけを消して出口を残す。** これは「代理が破れた」形そのもので、
;; `add rsp` の個数と `pop r13` の個数がずれ、**出口が解体を伴わなくなる。**
;; `pop r13` は 2 byte(41 5D)なので nop 2 つで潰す。
(defglobal jrb-pop-off (%jrb-first-pop-r13-off (function %jrb-leaf)))
(assert-equal t (if (> jrb-pop-off 0) t nil))
(defglobal jrb-pb0 (%%peek (+ jrb-base2 jrb-pop-off)))
(defglobal jrb-pb1 (%%peek (+ jrb-base2 (+ jrb-pop-off 1))))
(defglobal jrb-pnop-0 (%%poke (+ jrb-base2 jrb-pop-off) 144))
(defglobal jrb-pnop-1 (%%poke (+ jrb-base2 (+ jrb-pop-off 1)) 144))
;; **出口が解体を伴わなくなること**(これが本題)
(assert-equal t (if (> (%jrb-exorphan (function %jrb-leaf)) 0) t nil))
;; 個数の一致も崩れること
(assert-equal t (if (= (%jrb-nadd (function %jrb-leaf))
                       (%jrb-npop (function %jrb-leaf))) nil t))
;; **そして「即値の一致」は通ってしまうこと**(道標を消しただけなので)
(assert-equal 0 (%jrb-bad (function %jrb-leaf)))
;; 戻す
(defglobal jrb-pres-0 (%%poke (+ jrb-base2 jrb-pop-off) jrb-pb0))
(defglobal jrb-pres-1 (%%poke (+ jrb-base2 (+ jrb-pop-off 1)) jrb-pb1))
(assert-equal 0 (%jrb-exorphan (function %jrb-leaf)))
(assert-equal t (if (= (%jrb-nadd (function %jrb-leaf))
                       (%jrb-npop (function %jrb-leaf))) t nil))

;;; 3-C. 戻したコードがちゃんと動くこと(ここで初めて呼ぶ。戻し忘れも落とす)
(assert-equal 6 (%jrb-tail 5))
(assert-equal 210 (%jrb-rec 20))
(assert-equal 'neg (%jrb-nlx -1))
(assert-equal 'neg (%jrb-catch -1))
(assert-equal '(a 7 b) (%jrb-qq 7))
(assert-equal 8 (%jrb-flet 7))
(assert-equal 2 (%jrb-plain 1))

(format *isiki-test-stream*
        "#jit-rsp fsz=~D  leaf(sub ~D/add ~D/pop ~D)  tail(sub ~D/add ~D/pop ~D)~%"
        (%jrb-fsz (function %jrb-plain))
        (%jrb-nsub (function %jrb-leaf)) (%jrb-nadd (function %jrb-leaf)) (%jrb-npop (function %jrb-leaf))
        (%jrb-nsub (function %jrb-tail)) (%jrb-nadd (function %jrb-tail)) (%jrb-npop (function %jrb-tail)))
(finish-output *isiki-test-stream*)
