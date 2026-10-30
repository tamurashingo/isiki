;; test/lisp/jit_mixed_frame_test.lisp
;;
;; [回帰] **フレーム寸法が関数ごとに違う状態(混在)で壊れたら落ちる。**
;; documents/jit-frame-survey.md §4(案 d = 寸法を関数ごとの実寸にする)。
;;
;; なぜ要るか:
;;   2026-10-30 までは **全関数が同じ寸法**(`ZA_FRAME_TOTAL` = 7480 byte)だった。
;;   既存の試験(`jit_frame_test.lisp` / `jit_rsp_balance_test.lisp`)は
;;   どちらも**1 つの関数の中**を見ており、**関数と関数のあいだ**は 1 つも見ていない。
;;   全関数が同じ寸法である限り、それで穴は無かった。
;;
;;   実寸化すると前提が消える:
;;     quasiquote   2496 byte … 使わない関数は確保しない
;;     flet/labels   832 byte … 同上
;;   つまり**大きいフレームの関数と小さいフレームの関数が混在する。**
;;
;;   **いちばん危ないのは末尾呼び出しである。** 末尾呼び出しの高速 path は
;;       add rsp, <自分の寸法> ; pop r13 ; pop rbx ; jmp <呼び先の fixed_entry>
;;   という形で、**自分のフレームを解体してから呼び先の入口へ飛ぶ**(za.c:5096 付近)。
;;   飛んだ先の入口は `sub rsp, <呼び先の寸法>` をする。
;;   全関数が同じ寸法のあいだは「自分の戻す量 = 呼び先の取る量」が偶然成り立っていたが、
;;   **実寸化するとこれは成り立たない。** どちらか一方を他方の寸法で出したら、
;;   rsp がずれたまま呼び先が走る — **コンパイルは通り、落ちずに暴走する。**
;;
;;   **整列もそれぞれが独立に保たなければならない。** 入口の rsp は戻り番地で
;;   8 mod 16、`push rbx`/`push r13` で元へ戻るので、`sub rsp` の総量が 8 mod 16 の
;;   ときだけ呼び出し規約の整列が保たれる(za.c の `_Static_assert` と同じ条件)。
;;   寸法が関数ごとに違うなら、**この条件は関数ごとに成り立たなければならない。**
;;
;; 測り方:
;;   2 本立てにする。
;;     (1) **静的に読む** — 混在した題材それぞれについて、既存の走査器で
;;         寸法・解体の対・出口を見る。壊れたコードを呼ばずに済む
;;     (2) **実際に呼ぶ** — 値が正しいこと。rsp のずれは「値が違う」ではなく
;;         「ハングする/暴走する」として現れるので、**通れば正しい**側の試験になる
;;
;; [前提] **このファイルは jit_frame_test.lisp と jit_rsp_balance_test.lisp より
;; 後に読むこと。** 両方の読み手(`%jit-frame-size` / `%jrb-fsz`)と、
;; `jit_frame_test.lisp` が置く上限(`jit-frame-size-cap`)を使う。
;; 読ませているのは test/lisp/qemu_boot_jit_frame.lisp(3 番目)。

;;; --- 0. 走査は 1 関数 1 回にする ---
;;
;; `%jrb-audit` は生成コード全体を命令単位で歩くので、**1 回が安くない**
;; (ここは JIT に乗らない解釈実行のループである)。`%jrb-bad` / `%jrb-orphan` …
;; はそのたびに走査をやり直すため、題材 1 つにつき 6 回呼ぶと 6 回歩く。
;; 題材が増えるこのファイルでは、**結果を 1 度だけ取ってから添字で読む。**
;;
;; 戻りの並びは %jrb-audit の定義どおり:
;;   (nsub nadd bad fsz npop orphan nexit exorphan nteardown npreframe)
(defun %jmf-nsub     (a) (elt a 0))
(defun %jmf-nadd     (a) (elt a 1))
(defun %jmf-bad      (a) (elt a 2))
(defun %jmf-fsz      (a) (elt a 3))
(defun %jmf-npop     (a) (elt a 4))
(defun %jmf-orphan   (a) (elt a 5))
(defun %jmf-exorphan (a) (elt a 7))

;;; --- 1. 題材 ---
;;
;; **大きい側は quasiquote を使う関数、小さい側は let を使う葉**にしてある
;; (`jit_rsp_balance_test.lisp` の `%jrb-qq` / `%jrb-plain` と同じ選び方)。
;; flet を大きい側に使わないのは、**flet が固定引数入口を潰す**ためである
;; (`za_form_disables_param_slots`。lambda/flet/labels を含む関数は fixed_entry を
;; 持たないので、そこへの末尾呼び出しは高速 path に入らずトランポリンへ落ちる)。
;; quasiquote は入口を潰さないので、**末尾呼び出しの高速 path の呼び先になれる。**
;; ここが本題なので、大きい側は quasiquote でなければならない。

;; 小さい側: let の葉。まれな領域(quasiquote / flet)を 1 byte も使わない
(defun %jmf-small (n) (let ((a n)) (+ a 1)))
;; 大きい側: quasiquote を使う。**返す値は数にしておく**(呼び出し側の検査が
;; リスト構造に依存しないようにするため)
(defun %jmf-big (n) (let ((q `(a ,n b))) (car (cdr q))))

(assert-equal t (%%za-compiled-p (function %jmf-small)))
(assert-equal t (%%za-compiled-p (function %jmf-big)))
(assert-equal 6 (%jmf-small 5))
(assert-equal 5 (%jmf-big 5))

;; 通常呼び出し(戻り番地を積む側)。**両方向を見る。**
;; 小 -> 大: 小さいフレームの上に大きいフレームが乗る
(defun %jmf-s2b (n) (let ((r (%jmf-big n))) (+ r 1)))
;; 大 -> 小: 大きいフレームの上に小さいフレームが乗る
(defun %jmf-b2s (n) (let ((q `(a ,n b))) (+ (%jmf-small n) (car (cdr q)))))

(assert-equal t (%%za-compiled-p (function %jmf-s2b)))
(assert-equal t (%%za-compiled-p (function %jmf-b2s)))
(assert-equal 6 (%jmf-s2b 5))
(assert-equal 11 (%jmf-b2s 5))

;; 末尾呼び出し。**ここが本質。** 自分のフレームを解体してから呼び先の入口へ飛ぶ
;; 小 -> 大
(defun %jmf-tail-s2b (n) (%jmf-big n))
;; 大 -> 小(progn の最終フォーム。quasiquote は値を捨てても領域は確保される)
(defun %jmf-tail-b2s (n) (progn `(a ,n b) (%jmf-small n)))
;; 大 -> 小(let の本体から抜ける形。**let スコープの gc_roots の解除も
;; 末尾呼び出し経路が一括で行う**ので、こちらも経路として別に見る価値がある)
(defun %jmf-tail-b2s-let (n) (let ((q `(a ,n b))) (%jmf-small (car (cdr q)))))

(assert-equal t (%%za-compiled-p (function %jmf-tail-s2b)))
(assert-equal t (%%za-compiled-p (function %jmf-tail-b2s)))
(assert-equal t (%%za-compiled-p (function %jmf-tail-b2s-let)))
(assert-equal 5 (%jmf-tail-s2b 5))
(assert-equal 6 (%jmf-tail-b2s 5))
(assert-equal 6 (%jmf-tail-b2s-let 5))

;; 固定引数入口のいちばん広いところ(`ZA_MAX_FIXED_ENTRY_PARAMS` = 3)でも混在が
;; 成り立つこと。**引数はフレームを解体する前にレジスタへ読み出しておく**手順
;; (za.c:5085 付近)なので、引数の本数は解体と入口の境目に直接効く
(defun %jmf-small3 (a b c) (let ((s a)) (+ s b c)))
(defun %jmf-big3 (a b c) (let ((q `(x ,a ,b ,c))) (+ (car (cdr q)) b c)))
(defun %jmf-tail3-s2b (a b c) (%jmf-big3 a b c))
(defun %jmf-tail3-b2s (a b c) (let ((q `(x ,a ,b ,c))) (%jmf-small3 (car (cdr q)) b c)))

(assert-equal t (%%za-compiled-p (function %jmf-small3)))
(assert-equal t (%%za-compiled-p (function %jmf-big3)))
(assert-equal t (%%za-compiled-p (function %jmf-tail3-s2b)))
(assert-equal t (%%za-compiled-p (function %jmf-tail3-b2s)))
(assert-equal 9 (%jmf-small3 2 3 4))
(assert-equal 9 (%jmf-big3 2 3 4))
(assert-equal 9 (%jmf-tail3-s2b 2 3 4))
(assert-equal 9 (%jmf-tail3-b2s 2 3 4))

;; 相互再帰。**大と小が交互に呼び合って深く潜る。**
;; 1 段ごとに寸法が切り替わるので、寸法を間違えると潜った先で戻り番地が壊れる
(defun %jmf-mut-small (n) (let ((a n)) (if (= n 0) 0 (+ a (%jmf-mut-big (- n 1))))))
(defun %jmf-mut-big (n) (let ((q `(a ,n b))) (if (= n 0) 0 (+ (car (cdr q)) (%jmf-mut-small (- n 1))))))

(assert-equal t (%%za-compiled-p (function %jmf-mut-small)))
(assert-equal t (%%za-compiled-p (function %jmf-mut-big)))

;;; --- 2. 寸法を「どの関数のものか」区別して読む ---
;;
;; **絶対値で固定しない。** 実寸化すると関数ごとに違う値になるので、
;; 実寸化の前でも後でも成り立つ「関係」だけを置く
;; (`jit_frame_test.lisp` の `qq >= probe` と同じ方針)。

(defglobal jmf-a-small     (%jrb-audit (function %jmf-small)))
(defglobal jmf-a-big       (%jrb-audit (function %jmf-big)))
(defglobal jmf-a-s2b       (%jrb-audit (function %jmf-s2b)))
(defglobal jmf-a-b2s       (%jrb-audit (function %jmf-b2s)))
(defglobal jmf-a-tail-s2b  (%jrb-audit (function %jmf-tail-s2b)))
(defglobal jmf-a-tail-b2s  (%jrb-audit (function %jmf-tail-b2s)))
(defglobal jmf-a-tail-b2sl (%jrb-audit (function %jmf-tail-b2s-let)))
(defglobal jmf-a-small3    (%jrb-audit (function %jmf-small3)))
(defglobal jmf-a-big3      (%jrb-audit (function %jmf-big3)))
(defglobal jmf-a-tail3-s2b (%jrb-audit (function %jmf-tail3-s2b)))
(defglobal jmf-a-tail3-b2s (%jrb-audit (function %jmf-tail3-b2s)))
(defglobal jmf-a-mut-small (%jrb-audit (function %jmf-mut-small)))
(defglobal jmf-a-mut-big   (%jrb-audit (function %jmf-mut-big)))

;;; 2-A. **添字が合っていること。**
;; 上の `%jmf-fsz` 等は `%jrb-audit` の戻りの**並びを知っている**。並びが変われば
;; 添字は黙ってずれ、以降の判定は別の数を見たまま通ってしまう。
;; 別の読み手と突き合わせて、ここで固定する
(assert-equal t (if (= (%jmf-fsz jmf-a-small) (%jrb-fsz (function %jmf-small))) t nil))
(assert-equal t (if (= (%jmf-nadd jmf-a-small) (%jrb-nadd (function %jmf-small))) t nil))
(assert-equal t (if (= (%jmf-npop jmf-a-small) (%jrb-npop (function %jmf-small))) t nil))

;;; 2-B. **2 つの読み手が同じ関数について同じ値を返すこと。**
;; `%jit-frame-size`(プロローグの先頭 64 byte だけを見る)と
;; `%jrb-fsz`(全命令を走査する)は**実装が独立している。**
;; 実寸化は「出力時に参照された最も高い位置から寸法を決める」変更なので、
;; **プロローグの `sub rsp` を後から埋め直す**ことになる。
;; 埋め直しが片方の読み手にしか見えない形(例: 先頭の `sub` だけ直して
;; 2 つ目の入口を直し忘れる)で入ったら、ここが落ちる。
;;
;; 落ちたときにやること:
;;   1. `(%jit-frame-size (function f))` と `(%jrb-fsz (function f))` を手で出して
;;      どちらが何を返しているか見る
;;   2. 生成コードを `disassemble` で読み、**入口が 2 つ(consリスト版/固定引数版)とも
;;      同じ量を `sub` しているか**を確かめる。違っていたらそれが故障である
(assert-equal t (if (= (%jit-frame-size (function %jmf-small)) (%jmf-fsz jmf-a-small)) t nil))
(assert-equal t (if (= (%jit-frame-size (function %jmf-big))   (%jmf-fsz jmf-a-big))   t nil))
(assert-equal t (if (= (%jit-frame-size (function %jmf-tail-s2b)) (%jmf-fsz jmf-a-tail-s2b)) t nil))
(assert-equal t (if (= (%jit-frame-size (function %jmf-tail-b2s)) (%jmf-fsz jmf-a-tail-b2s)) t nil))

;;; 2-C. **どの関数の寸法も 0 より大きく、上限以下であること。**
;; 上限は `jit_frame_test.lisp` の `jit-frame-size-cap`(= いまの全関数共通の寸法)。
;; **増えるのは落とす。減るのは通す。** 実寸化は寸法を下げる変更なので、
;; どの関数もこの値を超えてはならない。
;;
;; 「0 より大きい」をここで全員に掛けないのは、**下の 2-D が兼ねている**ためである。
;; `%jrb-audit` は 1 つも見つからなければ `fsz` を 0 のまま返し、
;; `(mod 0 16)` は 8 にならない。つまり**走査の空振りは 2-D で落ちる**
(assert-equal t (if (> (%jmf-fsz jmf-a-small) 0) t nil))
(assert-equal t (if (> (%jmf-fsz jmf-a-big) 0) t nil))
(assert-equal t (if (<= (%jmf-fsz jmf-a-big) jit-frame-size-cap) t nil))
(assert-equal t (if (<= (%jmf-fsz jmf-a-big3) jit-frame-size-cap) t nil))
(assert-equal t (if (<= (%jmf-fsz jmf-a-mut-big) jit-frame-size-cap) t nil))

;;; 2-D. **16 で割った余りが 8 であること(関数ごとに)。**
;; za.c は `_Static_assert((ZA_FRAME_TOTAL % 16) == 8)` でビルド時に見ているが、
;; それは**定数 1 つ**に対する検査である。実寸化すると寸法は関数ごとに決まるので、
;; **ビルド時の検査は「どの関数も 8 mod 16」を保証しない。**
;; ここは出力された機械語の即値そのものを見るので、関数ごとに効く。
;;
;; **末尾呼び出しでは「自分の戻す量 = 呼び先の取る量」が成り立たない。**
;; だから整列は、呼ぶ側と呼ばれる側が**それぞれ独立に**保たなければならない。
;; つまりこの判定は、混在した題材の**全員**について必要である。
;;
;; 落ちたときにやること:
;;   1. 落ちた関数の寸法を出し、16 で割った余りを見る(8 でなければ整列が壊れている)
;;   2. `tools/jit_frame_breakdown.py` で内訳を出し、**16 の倍数でない領域**を探す。
;;      za.c の各領域は 16 の倍数、上端の 0x28 だけが 8 を出している決まりなので、
;;      どれかの領域の大きさか、実寸を切り出す境界が 16 の倍数になっていない
;;   3. 直すのは**寸法の決め方の側**で、この判定を緩めてはいけない
;;      (緩めると呼び出し規約が静かに壊れる)
(assert-equal 8 (mod (%jmf-fsz jmf-a-small) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-big) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-s2b) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-b2s) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-tail-s2b) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-tail-b2s) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-tail-b2sl) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-small3) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-big3) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-tail3-s2b) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-tail3-b2s) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-mut-small) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-mut-big) 16))

;;; 2-E. **大きい側 >= 小さい側。**
;; 実寸化の前は等しく、後は quasiquote を使う側が大きい。
;; **「等しい」ことは要求しない** — 等しさを要求したら、それは実寸化を禁じるのと同じ
(assert-equal t (if (>= (%jmf-fsz jmf-a-big)     (%jmf-fsz jmf-a-small))  t nil))
(assert-equal t (if (>= (%jmf-fsz jmf-a-big3)    (%jmf-fsz jmf-a-small3)) t nil))
(assert-equal t (if (>= (%jmf-fsz jmf-a-mut-big) (%jmf-fsz jmf-a-mut-small)) t nil))
(assert-equal t (if (>= (%jmf-fsz jmf-a-b2s)     (%jmf-fsz jmf-a-s2b))    t nil))
;; 末尾呼び出しの両側。**呼ぶ側と呼び先で寸法が違ってよい**ことを、
;; 向きだけで押さえる(小さい側の呼び出し元 <= 大きい側の呼び先、とその逆)
(assert-equal t (if (<= (%jmf-fsz jmf-a-tail-s2b) (%jmf-fsz jmf-a-big))   t nil))
(assert-equal t (if (>= (%jmf-fsz jmf-a-tail-b2s) (%jmf-fsz jmf-a-small)) t nil))
(assert-equal t (if (>= (%jmf-fsz jmf-a-tail-b2sl) (%jmf-fsz jmf-a-small)) t nil))
(assert-equal t (if (<= (%jmf-fsz jmf-a-tail3-s2b) (%jmf-fsz jmf-a-big3))  t nil))
(assert-equal t (if (>= (%jmf-fsz jmf-a-tail3-b2s) (%jmf-fsz jmf-a-small3)) t nil))

;;; --- 3. 混在した題材でも、1 関数の中の整合が崩れていないこと ---
;;
;; `jit_rsp_balance_test.lisp` が見ている 3 つを、**混在の題材に対して**掛け直す。
;; 題材が変わると出口の形も変わる(末尾呼び出しが 2 経路出る / 引数が 3 本ある)ので、
;; 同じ判定でも**別の生成コードを見ている**ことになる。
;;   bad      = sub と add の即値が食い違う(故障 A)
;;   orphan   = add rsp を伴わない pop r13 がある(故障 B = 命令が無い)
;;   exorphan = 解体もフレーム前の脱出も伴わない出口がある
(assert-equal 0 (%jmf-bad jmf-a-small))
(assert-equal 0 (%jmf-bad jmf-a-big))
(assert-equal 0 (%jmf-bad jmf-a-s2b))
(assert-equal 0 (%jmf-bad jmf-a-b2s))
(assert-equal 0 (%jmf-bad jmf-a-tail-s2b))
(assert-equal 0 (%jmf-bad jmf-a-tail-b2s))
(assert-equal 0 (%jmf-bad jmf-a-tail-b2sl))
(assert-equal 0 (%jmf-bad jmf-a-small3))
(assert-equal 0 (%jmf-bad jmf-a-big3))
(assert-equal 0 (%jmf-bad jmf-a-tail3-s2b))
(assert-equal 0 (%jmf-bad jmf-a-tail3-b2s))
(assert-equal 0 (%jmf-bad jmf-a-mut-small))
(assert-equal 0 (%jmf-bad jmf-a-mut-big))

(assert-equal 0 (%jmf-orphan jmf-a-small))
(assert-equal 0 (%jmf-orphan jmf-a-big))
(assert-equal 0 (%jmf-orphan jmf-a-s2b))
(assert-equal 0 (%jmf-orphan jmf-a-b2s))
(assert-equal 0 (%jmf-orphan jmf-a-tail-s2b))
(assert-equal 0 (%jmf-orphan jmf-a-tail-b2s))
(assert-equal 0 (%jmf-orphan jmf-a-tail-b2sl))
(assert-equal 0 (%jmf-orphan jmf-a-small3))
(assert-equal 0 (%jmf-orphan jmf-a-big3))
(assert-equal 0 (%jmf-orphan jmf-a-tail3-s2b))
(assert-equal 0 (%jmf-orphan jmf-a-tail3-b2s))
(assert-equal 0 (%jmf-orphan jmf-a-mut-small))
(assert-equal 0 (%jmf-orphan jmf-a-mut-big))

(assert-equal 0 (%jmf-exorphan jmf-a-small))
(assert-equal 0 (%jmf-exorphan jmf-a-big))
(assert-equal 0 (%jmf-exorphan jmf-a-s2b))
(assert-equal 0 (%jmf-exorphan jmf-a-b2s))
(assert-equal 0 (%jmf-exorphan jmf-a-tail-s2b))
(assert-equal 0 (%jmf-exorphan jmf-a-tail-b2s))
(assert-equal 0 (%jmf-exorphan jmf-a-tail-b2sl))
(assert-equal 0 (%jmf-exorphan jmf-a-small3))
(assert-equal 0 (%jmf-exorphan jmf-a-big3))
(assert-equal 0 (%jmf-exorphan jmf-a-tail3-s2b))
(assert-equal 0 (%jmf-exorphan jmf-a-tail3-b2s))
(assert-equal 0 (%jmf-exorphan jmf-a-mut-small))
(assert-equal 0 (%jmf-exorphan jmf-a-mut-big))

;; add rsp と pop r13 は za.c の同じ 3 箇所から対で出るので個数が一致する
(assert-equal t (if (= (%jmf-nadd jmf-a-tail-s2b)  (%jmf-npop jmf-a-tail-s2b))  t nil))
(assert-equal t (if (= (%jmf-nadd jmf-a-tail-b2s)  (%jmf-npop jmf-a-tail-b2s))  t nil))
(assert-equal t (if (= (%jmf-nadd jmf-a-tail-b2sl) (%jmf-npop jmf-a-tail-b2sl)) t nil))
(assert-equal t (if (= (%jmf-nadd jmf-a-tail3-s2b) (%jmf-npop jmf-a-tail3-s2b)) t nil))
(assert-equal t (if (= (%jmf-nadd jmf-a-tail3-b2s) (%jmf-npop jmf-a-tail3-b2s)) t nil))

;;; --- 4. 末尾呼び出しの経路が実際に出ていること ---
;;
;; **ここが空振りすると、上の判定は「末尾呼び出しの無いコード」を見て通ってしまう。**
;; `sub` は入口ごと(2 箇所)、`add` は出口経路ごと。末尾呼び出しを含む関数は
;; 本体の途中にも `add rsp` が出るので、add の個数が sub より多くなる
;; (`jit_rsp_balance_test.lisp` §2 と同じ見方を、混在の題材へ掛け直したもの)。
;;
;; 落ちたときにやること: その関数が**末尾呼び出しとしてコンパイルされていない**
;; ということなので、題材の形(progn / let の最終フォームか、引数の本数が
;; `ZA_MAX_FIXED_ENTRY_PARAMS` 以下か)を先に疑うこと
(assert-equal t (if (> (%jmf-nadd jmf-a-tail-s2b)  (%jmf-nsub jmf-a-tail-s2b))  t nil))
(assert-equal t (if (> (%jmf-nadd jmf-a-tail-b2s)  (%jmf-nsub jmf-a-tail-b2s))  t nil))
(assert-equal t (if (> (%jmf-nadd jmf-a-tail-b2sl) (%jmf-nsub jmf-a-tail-b2sl)) t nil))
(assert-equal t (if (> (%jmf-nadd jmf-a-tail3-s2b) (%jmf-nsub jmf-a-tail3-s2b)) t nil))
(assert-equal t (if (> (%jmf-nadd jmf-a-tail3-b2s) (%jmf-nsub jmf-a-tail3-b2s)) t nil))
;; 末尾呼び出しを含まない側では add は出口(エピローグ)の分だけ。
;; **対照として置く** — 上の「多い」が題材によらず成り立つなら判定になっていない
(assert-equal t (if (= (%jmf-nadd jmf-a-small) (%jmf-nsub jmf-a-small)) t nil))
(assert-equal t (if (= (%jmf-nadd jmf-a-big)   (%jmf-nsub jmf-a-big))   t nil))

;;; --- 5. 実際に呼んで値が正しいこと ---
;;
;; **rsp のずれは「値が違う」として現れない。** 戻り番地でないものへ飛ぶので、
;; 症状はハング/暴走である。だからここは「通れば正しい」側の試験になる。
;; 静的な判定(§2〜§4)が捕まえられないもの — たとえば**解体の量そのものが
;; 呼び先の寸法になっている**ような故障 — はここでしか出ない。

;;; 5-A. 通常呼び出しと末尾呼び出しの両方向
(assert-equal 6 (%jmf-s2b 5))
(assert-equal 11 (%jmf-b2s 5))
(assert-equal 5 (%jmf-tail-s2b 5))
(assert-equal 6 (%jmf-tail-b2s 5))
(assert-equal 6 (%jmf-tail-b2s-let 5))
(assert-equal 9 (%jmf-tail3-s2b 2 3 4))
(assert-equal 9 (%jmf-tail3-b2s 2 3 4))

;;; 5-B. 相互再帰で深く潜って、値が正しいこと
;;
;; **深さは上限より十分下にする。** 262144 / 7480 = 35 段で、実測は
;; 「26 前後で保護が発火」(stack_guard_test.lisp)。上限ぎりぎりを書くと
;; 呼び出し元が使う C スタックのぶんで溢れ、**落ちるのではなくハングする**
;; (documents/for-expansion.md で実際に踏んだ)。
;; 12 段あれば大と小が 6 回ずつ交互に現れるので、混在の検査としては足りる。
;;
;; 期待値は 12+11+...+1 = 78。**どちらから始めても同じ値になる**ので、
;; 大から始めた場合と小から始めた場合の両方を見る(片方だけだと、
;; 一方の向きの寸法違いを見逃す)
(assert-equal 78 (%jmf-mut-small 12))
(assert-equal 78 (%jmf-mut-big 12))
;; 浅いところも見る(1 段だけ切り替わる形。深さの効果と切り分けられる)
(assert-equal 1 (%jmf-mut-small 1))
(assert-equal 1 (%jmf-mut-big 1))
(assert-equal 0 (%jmf-mut-small 0))
(assert-equal 0 (%jmf-mut-big 0))

;;; 5-C. **確保を跨いで値が生き残ること**
;;
;; 大きい側の quasiquote は実行時に os_make_cons で確保する。つまり
;; **呼び先の中で GC が走りうる。** 小さいフレームに積んである値
;; (スロットへ link した引数・アキュムレータ)が、呼び先の確保で動く GC を
;; 跨いで正しく再配置されなければならない。
;;
;; フレームの寸法が関数ごとに違うと、スロットの番地も関数ごとに違う位置になる。
;; **gc_rootnode はフレーム内に置かれている**ので、寸法の取り違えは
;; 「別の関数のスロット配置で自分のフレームを読む」= GC ルートの指し先が狂う
;; という形でも出る。値で捕まえられるのはこの類である。
;;
;; **確保量は 1 回あたりで稼ぐ。段数では稼がない。** quasiquote の要素を増やすと
;; 1 回の呼び出しで作る cons が増える(ZA_MAX_QQ_ELEMENTS = 16 が上限)。
;; 段数を増やして稼ぐ形にすると、**末尾呼び出しとしてコンパイルされなかった場合に
;; 深く潜ってハングする**(落ちない故障になる)。段数は深さの上限から十分下に置き、
;; 「万一 JIT に乗らず解釈実行になっても安全な数」にしてある。
;;
;; 末尾再帰なのでスタックは伸びない。期待値は 60+59+...+1 = 1830
(defun %jmf-big-alloc (n) (let ((q `(a ,n ,n ,n ,n ,n ,n ,n ,n ,n ,n ,n ,n ,n b))) (car (cdr q))))
(defun %jmf-gc-loop (n acc) (let ((r (%jmf-big-alloc n))) (if (= n 0) acc (%jmf-gc-loop (- n 1) (+ acc r)))))
(assert-equal t (%%za-compiled-p (function %jmf-big-alloc)))
(assert-equal t (%%za-compiled-p (function %jmf-gc-loop)))
(assert-equal 7 (%jmf-big-alloc 7))
(defglobal jmf-gc-before (%%gc-collect-count))
(assert-equal 1830 (%jmf-gc-loop 60 0))
(defglobal jmf-gc-after (%%gc-collect-count))
;; 確保量を増やした大きい側も、混在の静的な整合を満たしていること
(defglobal jmf-a-big-alloc (%jrb-audit (function %jmf-big-alloc)))
(defglobal jmf-a-gc-loop   (%jrb-audit (function %jmf-gc-loop)))
(assert-equal 8 (mod (%jmf-fsz jmf-a-big-alloc) 16))
(assert-equal 8 (mod (%jmf-fsz jmf-a-gc-loop) 16))
(assert-equal 0 (%jmf-bad jmf-a-big-alloc))
(assert-equal 0 (%jmf-bad jmf-a-gc-loop))
(assert-equal 0 (%jmf-orphan jmf-a-big-alloc))
(assert-equal 0 (%jmf-orphan jmf-a-gc-loop))
(assert-equal 0 (%jmf-exorphan jmf-a-big-alloc))
(assert-equal 0 (%jmf-exorphan jmf-a-gc-loop))
;; 小さい側(gc-loop)が大きい側(big-alloc)を呼び、かつ自分自身へ末尾呼び出しする形
(assert-equal t (if (<= (%jmf-fsz jmf-a-gc-loop) (%jmf-fsz jmf-a-big-alloc)) t nil))
(assert-equal t (if (> (%jmf-nadd jmf-a-gc-loop) (%jmf-nsub jmf-a-gc-loop)) t nil))

;;; --- 6. 記録(落ちたときに手で突き合わせるための数字) ---
;;
;; **関数ごとの寸法はここにしか出ない。** 判定は関係で書いてあるので、
;; 「いま実際に何 byte なのか」は出力で残す。実寸化の前は全部同じ値が並び、
;; 後は小さい側だけが下がる — **その差が実寸化が効いた証拠になる。**
(format *isiki-test-stream*
        "#jit-mixed small=~D big=~D s2b=~D b2s=~D tail-s2b=~D tail-b2s=~D tail-b2s-let=~D~%"
        (%jmf-fsz jmf-a-small) (%jmf-fsz jmf-a-big)
        (%jmf-fsz jmf-a-s2b) (%jmf-fsz jmf-a-b2s)
        (%jmf-fsz jmf-a-tail-s2b) (%jmf-fsz jmf-a-tail-b2s) (%jmf-fsz jmf-a-tail-b2sl))
(format *isiki-test-stream*
        "#jit-mixed small3=~D big3=~D tail3-s2b=~D tail3-b2s=~D mut-small=~D mut-big=~D cap=~D~%"
        (%jmf-fsz jmf-a-small3) (%jmf-fsz jmf-a-big3)
        (%jmf-fsz jmf-a-tail3-s2b) (%jmf-fsz jmf-a-tail3-b2s)
        (%jmf-fsz jmf-a-mut-small) (%jmf-fsz jmf-a-mut-big) jit-frame-size-cap)
;; GC が実際に走ったかは**判定にしない**(ヒープの寸法とこの試験までの確保量で
;; 変わるため、固定すると関係の無い変更で落ちる)。走った回数は記録として残す
(format *isiki-test-stream* "#jit-mixed gc-loop collect-delta=~D~%"
        (- jmf-gc-after jmf-gc-before))
(finish-output *isiki-test-stream*)
