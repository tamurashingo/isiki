;;;; src/lisp/disassemble.lisp
;;;;
;;;; JIT(src/c/za.c)が生成した機械語を逆アセンブルして表示する。
;;;;
;;;; 対象は za.c がコンパイルした関数だけで、組み込みprimitiveやAOT
;;;; (src/c/lisp_compiled.c)の関数は対象外(コード範囲を持たないためエラーになる)。
;;;; デコーダの実体は src/c/disasm.c、対応する命令の一覧と選定の経緯は
;;;; documents/disasm-backend-decision.md にある。
;;;;
;;;;   (defun fib (n) (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))
;;;;   (disassemble 'fib)
;;;;
;;;; init.lisp の末尾から load される。

;;; ---------------------------------------------------------------------------
;;; %%disasm-item が返す1項目
;;; (長さ 種別 ニモニック オペランド バイト列 整形済み行 注釈) へのアクセサ。
;;; 種別は 0=命令 / 1=コードに埋め込まれた文字列 / 2=デコード失敗。
;;; 注釈はその命令が指す絶対アドレスの所属領域("<kernel>"等)で、絶対アドレスを
;;; 持たない命令や領域が引けなかった場合は空文字列。

(defun disasm-item-length (item) (elt item 0))
(defun disasm-item-kind (item) (elt item 1))
(defun disasm-item-mnemonic (item) (elt item 2))
(defun disasm-item-operands (item) (elt item 3))
(defun disasm-item-bytes (item) (elt item 4))
(defun disasm-item-line (item) (elt item 5))
(defun disasm-item-comment (item) (elt item 6))

;;; アドレスの所属領域(%%disasm-classify-addr / %%disasm-region-bounds の region 番号)
(defglobal *disasm-region-unknown* 0)
(defglobal *disasm-region-kernel* 1)
(defglobal *disasm-region-immobilized* 2)
(defglobal *disasm-region-gc-heap* 3)

;;; ---------------------------------------------------------------------------

;; fn-or-name(関数オブジェクトまたはその名前のsymbol)のコード範囲を取り、
;; 取れなければエラーにする。%%disasm-code-base は「JITコンパイルされた機械語を
;; 持たない」場合に nil を返すので、判定はこれ1つで足りる
;; (組み込みprimitive・AOT関数・インタプリタ実行の関数がすべてここに入る)。
(defun disasm-code-base (fn-or-name)
  (let ((base (%%disasm-code-base fn-or-name)))
    (if (null base)
        (error "disassemble: JITコンパイルされた機械語を持ちません ~S" fn-or-name)
      base)))

;; base から len バイトを先頭から順にデコードし、項目のリストを返す。
;; 埋め込み文字列(.asciz)はそれ自体が1項目になるので、単純に長さぶん進めればよい。
(defun disasm-items (base len)
  (let ((offset 0) (acc nil) (item nil))
    (while (< offset len)
      (setq item (%%disasm-item base len offset))
      (if (null item)
          ;; 範囲外。ここで打ち切る(itemがnilを返すのは offset >= len のときだけ)
          (setq offset len)
        (progn
          (setq acc (cons (cons offset item) acc))
          (setq offset (+ offset (disasm-item-length item))))))
    (reverse acc)))

;; エントリポイントの位置を1行で出す。ABI-M5の固定引数エントリを持つ関数では
;; 先頭(fixed)と途中(cons)の2箇所がエントリになるため、どちらも示す。
(defun disasm-print-entries (stream fn-or-name)
  (let ((fixed (%%disasm-entry-offset fn-or-name 1))
        (cons-entry (%%disasm-entry-offset fn-or-name 0)))
    (format stream "Entry:  cons=0x~X" (if (null cons-entry) 0 cons-entry))
    (if (null fixed)
        nil
      (format stream "  fixed=0x~X" fixed))
    (format stream "~%")))

(defun disassemble-to-stream (stream fn-or-name)
  (let* ((base (disasm-code-base fn-or-name))
         (len (%%disasm-code-len fn-or-name))
         (items (disasm-items base len))
         (rest items))
    (format stream "Function: ~S~%" fn-or-name)
    (format stream "Code:   ~D bytes at 0x~X~%" len base)
    (disasm-print-entries stream fn-or-name)
    (format stream "offset  bytes                            instruction~%")
    (while (not (null rest))
      ;; car は (offset . item)。整形済みの行は C 側(os_disasm_format_line)が作る
      (format stream "~A~%" (disasm-item-line (cdr (car rest))))
      (setq rest (cdr rest)))
    nil))

;; 逆アセンブル結果を標準出力へ表示する。戻り値は nil。
(defun disassemble (fn-or-name)
  (disassemble-to-stream (standard-output) fn-or-name))

(defglobal *disasm-end-marker* ";;disasm-end")

;; 切り詰めが起きたときに末尾へ付ける1行(改行を含めないので ends-with で判定できる)。
;; 呼び出し側・テストがこの値を参照できるよう文字列リテラルを一箇所に持つ。
(defglobal *disasm-truncated-marker*
  ";; ... truncated: string output stream capacity exceeded; use (disassemble ...) for the full listing")

;; text の末尾が suffix と一致するか
(defun disasm-ends-with (text suffix)
  (let ((n (length text)) (m (length suffix)))
    (if (< n m)
        nil
      (string= suffix (subseq text (- n m) n)))))

;; 逆アセンブル結果を文字列として返す。
;;
;; 文字列出力ストリーム(create-string-output-stream)は容量
;; (STREAM_STRING_OUTPUT_CAP、src/c/stream.h)に達すると以降の書き込みを**黙って
;; 捨てる**。逆アセンブル結果は1関数でも容易にこれを超えるため、切り詰めが起きたことを
;; 必ず戻り値に出す(documents/pitfalls.md 原則6: 失敗は観測可能にすること)。
;;
;; 容量の値をLisp側に持つと定数が二重管理になるので、本文の後ろに番兵を書いて
;; それが残っているかどうかで判定する。番兵が消えていれば途中で溢れている。
(defun disassemble-to-string (fn-or-name)
  (let ((stream (create-string-output-stream)))
    (disassemble-to-stream stream fn-or-name)
    (format stream "~A" *disasm-end-marker*)
    (let ((text (get-output-stream-string stream)))
      (if (disasm-ends-with text *disasm-end-marker*)
          (subseq text 0 (- (length text) (length *disasm-end-marker*)))
        (string-append text *disasm-truncated-marker*)))))

;; 逆アセンブル結果を (アドレス オフセット ニモニック オペランド バイト列 注釈) の
;; リストとして返す。表示用の整形を通さず、そのまま検査したい場合に使う。
;; 注釈は独立した要素にしてある(オペランドへ連結すると利用側がパースする羽目になる)。
(defun disassemble-to-list (fn-or-name)
  (let* ((base (disasm-code-base fn-or-name))
         (len (%%disasm-code-len fn-or-name))
         (rest (disasm-items base len))
         (acc nil)
         (entry nil))
    (while (not (null rest))
      (setq entry (car rest))
      (setq acc (cons (list (+ base (car entry))
                            (car entry)
                            (disasm-item-mnemonic (cdr entry))
                            (disasm-item-operands (cdr entry))
                            (disasm-item-bytes (cdr entry))
                            (disasm-item-comment (cdr entry)))
                      acc))
      (setq rest (cdr rest)))
    (reverse acc)))
