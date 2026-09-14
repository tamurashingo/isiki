;; test/lisp/disassemble_test.lisp
;;
;; JIT生成コードの逆アセンブラ(src/c/disasm.c + src/lisp/disassemble.lisp)を
;; 実機上で検証する。デコーダ単体の検証は test/c/disasm_test.c(make test)側にあり、
;; ここで見るのは「za.cが実際に出力した機械語を、端から端まで読み切れるか」である。
;;
;; これはデコーダとJITの整合を守る唯一の常時テストで、za.cに新しい命令の出力を
;; 足したときは disasm-no-undecodable-items が落ちて気付ける。

(defun dis-add (a b) (+ a b))
(defun dis-caller (x) (dis-add x 1))

;; 前提: 対象がJITコンパイルされていること(インタプリタ実行なら以降は無意味)
(assert-equal t (%%za-compiled-p (function dis-add)))
(assert-equal t (%%za-compiled-p (function dis-caller)))

;;; --- メタデータ(Phase 1.1) ---

;; コード範囲が取れる。symbolでも関数オブジェクトでも同じ結果になること
(assert-equal t (> (%%disasm-code-len 'dis-add) 0))
(assert-equal (%%disasm-code-base 'dis-add) (%%disasm-code-base (function dis-add)))
(assert-equal (%%disasm-code-len 'dis-add) (%%disasm-code-len (function dis-add)))

;; consリストABIのエントリポイントはコードブロックの内側にある
(assert-equal t (>= (%%disasm-entry-offset 'dis-add 0) 0))
(assert-equal t (< (%%disasm-entry-offset 'dis-add 0) (%%disasm-code-len 'dis-add)))

;; 組み込みprimitiveは機械語を持たない(AOT・インタプリタ実行も同じ扱い)
(assert-equal nil (%%disasm-code-base 'car))
(assert-equal nil (%%disasm-code-len 'car))
;; 未定義の名前、関数でない値も静かにnilを返す(呼び出し側がエラーメッセージを決める)
(assert-equal nil (%%disasm-code-base 'no-such-function-at-all))
(assert-equal nil (%%disasm-code-base 42))
;; Lisp側のラッパーはエラーにする
(assert-error (disassemble 'car))
(assert-error (disassemble 'no-such-function-at-all))

;;; --- 走査(Phase 1.2) ---

;; 項目の長さの合計がコード長とちょうど一致すること。
;; 命令長を1バイトでも取り違えると、以降のデコードがずれて合計が合わなくなる
(defun disasm-total-length (name)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (total 0))
    (while (not (null items))
      (setq total (+ total (disasm-item-length (cdr (car items)))))
      (setq items (cdr items)))
    total))

(assert-equal (%%disasm-code-len 'dis-add) (disasm-total-length 'dis-add))
(assert-equal (%%disasm-code-len 'dis-caller) (disasm-total-length 'dis-caller))

;; 種別が kind の項目を数える(0=命令 1=埋め込み文字列 2=デコード失敗)
(defun disasm-count-kind (name kind)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (count 0))
    (while (not (null items))
      (if (= (disasm-item-kind (cdr (car items))) kind)
          (setq count (+ count 1))
        nil)
      (setq items (cdr items)))
    count))

;; デコードできない項目が1つも無いこと。za.cが出力する命令はすべて表に入っている、
;; という不変条件そのもの
(assert-equal 0 (disasm-count-kind 'dis-add 2))
(assert-equal 0 (disasm-count-kind 'dis-caller 2))

;; 他の関数を名前で呼ぶ関数には、za_emit_symbol_nameが埋め込んだ
;; NUL終端文字列がコード中に現れる(素朴なlinear sweepが壊れる場所)
(assert-equal t (> (disasm-count-kind 'dis-caller 1) 0))
;; 呼び出しを持たない関数には埋め込み文字列は現れない
(assert-equal 0 (disasm-count-kind 'dis-add 1))

;; 埋め込み文字列の中身が実際の呼び出し先の名前になっていること。
;; ヒューリスティックが「たまたま何かをDATAと判定した」のではないことの確認
(defun disasm-operands-of-mnemonic (name mnemonic)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (found nil))
    (while (not (null items))
      (if (and (null found) (string= mnemonic (disasm-item-mnemonic (cdr (car items)))))
          (setq found (disasm-item-operands (cdr (car items))))
        nil)
      (setq items (cdr items)))
    found))

(assert-equal "\"DIS-ADD\"" (disasm-operands-of-mnemonic 'dis-caller ".asciz"))

;;; --- プロローグ・エピローグ ---

;; n番目の項目のニモニックを返す
(defun disasm-mnemonic-at (name n)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (i 0)
        (found nil))
    (while (not (null items))
      (if (= i n)
          (setq found (disasm-item-mnemonic (cdr (car items))))
        nil)
      (setq i (+ i 1))
      (setq items (cdr items)))
    found))

;; プロローグの先頭は push rbx(jit_push_rbx)
(assert-equal "push" (disasm-mnemonic-at 'dis-add 0))
(assert-equal "rbx" (disasm-item-operands
                     (cdr (car (disasm-items (%%disasm-code-base 'dis-add)
                                             (%%disasm-code-len 'dis-add))))))

;; エピローグの最後は ret(jit_ret)。末尾呼び出しでトランポリンへ抜ける関数でも、
;; フォールスルー経路のエピローグがコードブロックの末尾に残る
(defun disasm-last-mnemonic (name)
  (let ((items (disasm-items (%%disasm-code-base name) (%%disasm-code-len name)))
        (last nil))
    (while (not (null items))
      (setq last (disasm-item-mnemonic (cdr (car items))))
      (setq items (cdr items)))
    last))

(assert-equal "ret" (disasm-last-mnemonic 'dis-add))
(assert-equal "ret" (disasm-last-mnemonic 'dis-caller))

;;; --- 出力(Phase 1.3 / 1.4) ---

;; disassemble-to-string は見出しと命令行を含む1つの文字列を返す
(defglobal *disasm-text* (disassemble-to-string 'dis-add))
(assert-equal t (stringp *disasm-text*))
(assert-equal t (> (length *disasm-text*) 0))
(assert-equal t (> (length *disasm-text*) 100))

;; 切り詰めの検出(documents/pitfalls.md 原則6)。文字列出力ストリームの容量は
;; 1024バイト(STREAM_STRING_OUTPUT_CAP)で、JIT関数1つぶんの逆アセンブル結果は
;; 必ずこれを超える。黙って途中で切れるのではなく、切れたことが戻り値に出ること
(assert-equal t (disasm-ends-with *disasm-text* *disasm-truncated-marker*))

;; 番兵の判定そのものの確認(disasm-ends-withが常にtを返しているだけ、ではないこと)
(assert-equal t (disasm-ends-with "abcdef" "def"))
(assert-equal t (disasm-ends-with "abc" "abc"))
(assert-equal nil (disasm-ends-with "abcdef" "abc"))
(assert-equal nil (disasm-ends-with "ab" "abc"))

;; disassemble-to-list は (アドレス オフセット ニモニック オペランド バイト列)
(defglobal *disasm-list* (disassemble-to-list 'dis-add))
(assert-equal t (> (length *disasm-list*) 0))
(assert-equal 5 (length (car *disasm-list*)))
;; 先頭項目のオフセットは0、アドレスはコード先頭
(assert-equal 0 (elt (car *disasm-list*) 1))
(assert-equal (%%disasm-code-base 'dis-add) (elt (car *disasm-list*) 0))
(assert-equal "push" (elt (car *disasm-list*) 2))
(assert-equal "53" (elt (car *disasm-list*) 4))

;; disassemble自体は標準出力へ出してnilを返す
(assert-output (disasm-result disasm-output) (disassemble 'dis-add)
  (assert-equal nil disasm-result)
  (assert-equal t (> (length disasm-output) 0)))

;; 実際の逆アセンブル結果を記録に残す(壊れたときに何が出ていたか分かるように)。
;; disassemble-to-stringは文字列ストリームの容量で切れるので、ここは
;; disassemble-to-stream で直接テスト結果ファイルへ流す
(format *isiki-test-stream* "~%--- (disassemble 'dis-add) ---~%")
(disassemble-to-stream *isiki-test-stream* 'dis-add)
(format *isiki-test-stream* "~%--- (disassemble 'dis-caller) ---~%")
(disassemble-to-stream *isiki-test-stream* 'dis-caller)
(format *isiki-test-stream* "~%")
(finish-output *isiki-test-stream*)
