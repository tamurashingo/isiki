;; test/lisp/inline_decl_test.lisp
;;
;; 算術の inline 宣言の**解釈**(Phase 2)。documents/inline-arith.md §7
;;
;; **このフェーズでは展開しない。** 宣言が読まれて生成側(za.c)まで届いていることだけを見る。
;; 展開が入るのは次のフェーズで、そのとき code-len が変わる。
;;
;; 既存の car/cdr/null/eq の展開(documents/inline-builtin.md)とは、
;;   - ビットの置き場所(declaim 値の bit8〜)
;;   - za_inline_enabled で門番する形
;; が共通で、**算術は 4 ビット増やしただけ**である。

;; ビット集合から 1 ビットを取り出す。logand が無いので div/mod で見る
(defun idt-has (bits name)
  (let ((b (%%inline-bit-of name)))
    (if (= b 0) nil (= 1 (mod (div bits b) 2)))))

;;; --- 1. 算術の名前がビットを持つようになった ---
(assert-equal t (> (%%inline-bit-of '+) 0))
(assert-equal t (> (%%inline-bit-of '-) 0))
(assert-equal t (> (%%inline-bit-of '*) 0))
(assert-equal t (> (%%inline-bit-of '/) 0))
;; 4 つとも別のビットであること
(assert-equal nil (= (%%inline-bit-of '+) (%%inline-bit-of '-)))
(assert-equal nil (= (%%inline-bit-of '*) (%%inline-bit-of '/)))
;; 既存の 4 つとも重ならない
(assert-equal nil (= (%%inline-bit-of '+) (%%inline-bit-of 'car)))
;; 対象外の名前は 0(declaim が黙って無視するための判定)
(assert-equal 0 (%%inline-bit-of 'cons))
(assert-equal 0 (%%inline-bit-of 'idt-has))

;;; --- 2. declaim が算術にも効く(以前は未知の名前として無視されていた)---
(assert-equal nil (%%current-inline))
(declaim (inline +))
(assert-equal '(+) (%%current-inline))
(declaim (inline - * /))
(assert-equal '(+ - * /) (%%current-inline))
(declaim (notinline * /))
(assert-equal '(+ -) (%%current-inline))
(declaim (notinline + -))
(assert-equal nil (%%current-inline))

;;; --- 3. declare (inline ...) が関数へ届く(以前は読み捨てられていた)---
(defun idt-plain (x y) (+ x y))
(defun idt-decl (x y) (declare (inline +)) (+ x y))
(defun idt-decl-type (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline +))
  (+ x y))
;; 型宣言と同じ declare にまとめても同じ意味
(defun idt-decl-merged (x y)
  (declare (type <fixnum> x) (type <fixnum> y) (inline +))
  (+ x y))
(assert-equal nil (%%inline-of 'idt-plain))
(assert-equal '(+) (%%inline-of 'idt-decl))
(assert-equal '(+) (%%inline-of 'idt-decl-type))
(assert-equal '(+) (%%inline-of 'idt-decl-merged))
;; 型宣言のほうも壊れていないこと
(assert-equal '(<FIXNUM> <FIXNUM>) (%%declared-types-of 'idt-decl-type))
(assert-equal '(<FIXNUM> <FIXNUM>) (%%declared-types-of 'idt-decl-merged))
;; **結果は宣言の有無で変わらない**
(assert-equal 7 (idt-plain 3 4))
(assert-equal 7 (idt-decl 3 4))
(assert-equal 7 (idt-decl-type 3 4))
(assert-equal 7 (idt-decl-merged 3 4))

;;; --- 4. declare は declaim の上に重なる。内側が優先 ---
;; **「落とす方向」がここで実際に走る。** 合成規則の半分(内側が外側を打ち消す /
;; 後の宣言が前を取り消す)が一度も実行されないと、壊れていても分からない。
;;   idt-override  … 内側の notinline が外側の declaim を打ち消す
;;   idt-last-wins … 同じ深さで後の notinline が前の inline を取り消す
;;   idt-here-nest … let の内側の notinline を %%inline-here で読む(§5)
(declaim (inline + -))
(defun idt-inherit (x y) (+ x y))
(assert-equal '(+ -) (%%inline-of 'idt-inherit))
(defun idt-override (x y) (declare (notinline +)) (+ x y))
(assert-equal '(-) (%%inline-of 'idt-override))          ; + だけ落ちる
(defun idt-add-on (x y) (declare (inline *)) (+ x y))
(assert-equal '(+ - *) (%%inline-of 'idt-add-on))        ; 足される
;; **同じ深さなら後から来たものが優先**
(defun idt-last-wins (x y) (declare (inline *) (notinline *)) (+ x y))
(assert-equal '(+ -) (%%inline-of 'idt-last-wins))
(defun idt-last-wins2 (x y) (declare (notinline +) (inline +)) (+ x y))
(assert-equal '(+ -) (%%inline-of 'idt-last-wins2))
(declaim (notinline + -))
(assert-equal nil (%%current-inline))

;;; --- 5. %%inline-here: let の内側まで見える ---
;; **JIT ではコンパイル時に畳まれる。** ここが Phase 3 以降で展開の判定に使われる値
(defun idt-here-top (n)
  (declare (inline +))
  (%%inline-here))
(assert-equal t (%%za-compiled-p (function idt-here-top)))
(assert-equal t (idt-has (idt-here-top 0) '+))
(assert-equal nil (idt-has (idt-here-top 0) '-))

;; let の本体の declare が届く
(defun idt-here-let (n)
  (let ((a 1))
    (declare (inline *))
    (%%inline-here)))
(assert-equal t (%%za-compiled-p (function idt-here-let)))
(assert-equal t (idt-has (idt-here-let 0) '*))
(assert-equal nil (idt-has (idt-here-let 0) '+))

;; 内側が優先。外側で立てて内側で落とす
(defun idt-here-nest (n)
  (declare (inline + *))
  (let ((a 1))
    (declare (notinline *))
    (%%inline-here)))
(assert-equal t (idt-has (idt-here-nest 0) '+))
(assert-equal nil (idt-has (idt-here-nest 0) '*))

;; **let を出たら戻る**(スコープが閉じること)
(defun idt-here-after (n)
  (declare (inline +))
  (let ((a (let ((b 1)) (declare (inline *)) b)))
    (%%inline-here)))
(assert-equal t (idt-has (idt-here-after 0) '+))
(assert-equal nil (idt-has (idt-here-after 0) '*))

;;; --- 6. **let の初期化式には効かない**(CommonLisp と同じ)---
;; 初期化式は `((lambda (v) . body) init)` の init 側で、body より先にコンパイルされる。
;; 宣言を適用するのは body の直前だけなので、初期化式は外側のスコープのまま。
;;
;; **この節はコンパイル順序の番人である。**
;; 正しさが「init 式が body より先にコンパイルされる」という**別の場所の性質**に
;; 依存している。将来 let の展開や引数の評価順を触ると、**明示的な処理が無いぶん
;; 静かに壊れる。** ここが落ちたら、まず za_compile_let の中での
;; 「init のコンパイル」と「g_za_inline_scope の適用」の前後関係を見ること。
(defun idt-init-form (n)
  (let ((outer (%%inline-here)))          ; ← 宣言の外(この let の body ではない)
    outer))
(declaim (inline -))
(defun idt-init-vs-body (n)
  (let ((in-init (%%inline-here)))        ; 外側(declaim の - だけ)
    (let ((a 1))
      (declare (inline +))
      (list in-init (%%inline-here)))))   ; 本体(- と +)
(assert-equal t (%%za-compiled-p (function idt-init-vs-body)))
(defglobal *idt-pair* (idt-init-vs-body 0))
(assert-equal nil (idt-has (car *idt-pair*) '+))          ; 初期化式には効いていない
(assert-equal t   (idt-has (car *idt-pair*) '-))          ; 外側の declaim は効いている
(assert-equal t   (idt-has (car (cdr *idt-pair*)) '+))    ; 本体には効いている
(assert-equal t   (idt-has (car (cdr *idt-pair*)) '-))
(declaim (notinline -))

;;; --- 7. **まだ展開しない演算**(`*` `/`)---
;;
;; Phase 2 ではここに `+` も並んでいて「宣言を付けても code-len が変わらない」
;; ことを固定していた。**Phase 3 で `+` `-` が展開されるようになったので、
;; この assert がその変更を正しく検出して落ちた。** 期待値を更新した。
;; `+` `-` の展開は §9 で見る。
;;
;; `*` `/` は展開命令列をまだ書いていないので、宣言しても call のままである。
(defun idt-len-mul (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (* x y))
(defun idt-len-mul-i (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline *))
  (* x y))
(assert-equal (%%disasm-code-len 'idt-len-mul) (%%disasm-code-len 'idt-len-mul-i))
(assert-equal 12 (idt-len-mul 3 4))
(assert-equal 12 (idt-len-mul-i 3 4))

;;; --- 8. 既存の car/cdr/null/eq が壊れていないこと ---
;; **declare でも効くようになった**(以前は declaim だけだった)のが変化点
(defun idt-car-declaim (x) (car x))
(declaim (inline car))
(defun idt-car-on (x) (car x))
(declaim (notinline car))
(defun idt-car-off (x) (car x))
(defun idt-car-declare (x) (declare (inline car)) (car x))
(assert-equal nil (%%inline-of 'idt-car-declaim))
(assert-equal '(car) (%%inline-of 'idt-car-on))
(assert-equal nil (%%inline-of 'idt-car-off))
(assert-equal '(car) (%%inline-of 'idt-car-declare))
;; 展開されている側は長い
(assert-equal t (> (%%disasm-code-len 'idt-car-on) (%%disasm-code-len 'idt-car-off)))
(assert-equal t (> (%%disasm-code-len 'idt-car-declare) (%%disasm-code-len 'idt-car-off)))
;; **結果は同じ**
(assert-equal 1 (idt-car-on '(1 2)))
(assert-equal 1 (idt-car-off '(1 2)))
(assert-equal 1 (idt-car-declare '(1 2)))

;;; --- 9. **展開が効く(Phase 3)。予測をテストで固定する** ---
;;
;; 【規則 11】実装前に PR へこう書いた:
;;   「展開される命令列は型宣言なしの経路と**同一**(違うのは通らない
;;    フォールバックの呼び先だけ)。だから完全一致が正常である」
;; **予測が確定しているなら検出器にできる。** 観測して満足するのではなく、
;; 一致をここで固定する。将来どちらかの経路だけが変わった日に落ちる。

(defun idt-mnemonics (name)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (acc nil))
    (while (not (null items))
      (setq acc (cons (disasm-item-mnemonic (cdr (car items))) acc))
      (setq items (cdr items)))
    acc))
(defun idt-callee-p (name callee)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (found nil))
    (while (not (null items))
      (if (string= callee (disasm-item-comment (cdr (car items)))) (setq found t) nil)
      (setq items (cdr items)))
    found))

(defun idt-add-nodecl (x y) (+ x y))
(defun idt-add-decl (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (+ x y))
(defun idt-add-decl-inline (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline +))
  (+ x y))
(defun idt-sub-nodecl (x y) (- x y))
(defun idt-sub-decl (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (- x y))
(defun idt-sub-decl-inline (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (inline -))
  (- x y))

;; **加算的であること**: inline 宣言が無い経路は一切変わらない
(assert-equal 690 (%%disasm-code-len 'idt-add-decl))       ; call だけ(従来どおり)
(assert-equal 735 (%%disasm-code-len 'idt-add-nodecl))     ; 展開あり(従来どおり)
(assert-equal 690 (%%disasm-code-len 'idt-sub-decl))
(assert-equal 738 (%%disasm-code-len 'idt-sub-nodecl))
;; **宣言すると展開される**
(assert-equal 735 (%%disasm-code-len 'idt-add-decl-inline))
(assert-equal 738 (%%disasm-code-len 'idt-sub-decl-inline))

;; **命令列が同一であること**(予測の固定)
(assert-equal (idt-mnemonics 'idt-add-nodecl) (idt-mnemonics 'idt-add-decl-inline))
(assert-equal (idt-mnemonics 'idt-sub-nodecl) (idt-mnemonics 'idt-sub-decl-inline))

;; **違うのはフォールバックの呼び先だけ**。GENERIC に戻っていたら別の事故
(assert-equal t   (idt-callee-p 'idt-add-nodecl "primitive_add2"))
(assert-equal nil (idt-callee-p 'idt-add-nodecl "primitive_add2_fixnum"))
(assert-equal t   (idt-callee-p 'idt-add-decl-inline "primitive_add2_fixnum"))
(assert-equal nil (idt-callee-p 'idt-add-decl-inline "primitive_add2"))
(assert-equal t   (idt-callee-p 'idt-sub-decl-inline "primitive_subtract2_fixnum"))
(assert-equal nil (idt-callee-p 'idt-sub-decl-inline "primitive_subtract2"))

;; **結果は展開の有無で変わらない**(桁溢れと負数まで見る)
(defglobal *idt-max* *most-positive-fixnum*)
(assert-equal 7 (idt-add-decl-inline 3 4))
(assert-equal (idt-add-nodecl 3 4) (idt-add-decl-inline 3 4))
(assert-equal (idt-add-nodecl -3 4) (idt-add-decl-inline -3 4))
(assert-equal (idt-add-nodecl 3 -4) (idt-add-decl-inline 3 -4))
(assert-equal (idt-add-nodecl -3 -4) (idt-add-decl-inline -3 -4))
(assert-equal (idt-add-nodecl *idt-max* 1) (idt-add-decl-inline *idt-max* 1))
(assert-equal t (bignump (idt-add-decl-inline *idt-max* 1)))
(assert-equal (idt-sub-nodecl 3 4) (idt-sub-decl-inline 3 4))
(assert-equal (idt-sub-nodecl 4 3) (idt-sub-decl-inline 4 3))
(assert-equal (idt-sub-nodecl -3 4) (idt-sub-decl-inline -3 4))
(assert-equal (idt-sub-nodecl -3 -4) (idt-sub-decl-inline -3 -4))
(assert-equal 0 (idt-sub-decl-inline 5 5))
(assert-equal (idt-sub-nodecl *most-negative-fixnum* 1)
              (idt-sub-decl-inline *most-negative-fixnum* 1))

;; **notinline で戻る**
(declaim (inline + -))
(defun idt-add-declaim (x y) (declare (type <fixnum> x)) (declare (type <fixnum> y)) (+ x y))
(defun idt-add-off (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (declare (notinline +))
  (+ x y))
(assert-equal 735 (%%disasm-code-len 'idt-add-declaim))
(assert-equal 690 (%%disasm-code-len 'idt-add-off))
(assert-equal 7 (idt-add-declaim 3 4))
(assert-equal 7 (idt-add-off 3 4))
(declaim (notinline + -))

;; single / double は展開の対象外(この PR では fixnum だけ)
(defun idt-add-sgl-inline (x y)
  (declare (type <single-float> x)) (declare (type <single-float> y)) (declare (inline +))
  (+ x y))
(assert-equal 690 (%%disasm-code-len 'idt-add-sgl-inline))
(assert-equal t (idt-callee-p 'idt-add-sgl-inline "primitive_add2_single"))
