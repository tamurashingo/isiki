;; test/lisp/declare_audit_test.lisp
;;
;; [D6] **監査ビルド(ISIKIOS_DECLARE_AUDIT)専用のテスト。**
;; documents/declaim-design.md「D6 / D7 の決定」。
;;
;; なぜ専用のテストが要るか:
;;   既存の監査(DECLARE_AUDIT_FIXNUM 等)は**型特化した C 関数の入口**で値を見る。
;;   JIT のインライン展開は呼び出しを消すので、**safety 0 で壊れるまさにその場合だけ
;;   監査が何も見なくなる**。だから監査ビルドでは za_inline_enabled が 0 を返して
;;   **展開を抑止する**(そうすれば必ず関数が呼ばれ、既存の監査が効く)。
;;
;;   **抑止されていることを誰も見ていなければ、D6 は守られているか分からない。**
;;   ここで見る。
;;
;; 走らせ方(通常ビルドで走らせると落ちる。それがゲートである):
;;   make test-qemu-declare-audit
;;
;; **なぜ組み込み関数で「監査ビルドか」を判定しないのか**:
;;   組み込みを 1 つ足すと test/c/runtime_test.c の SMALL_HEAP_SIZE の再調整が要る
;;   (余裕は 5KB 未満。症状はテストの NG ではなくハング)。
;;   test/lisp/gc_debug_guard.lisp が同じ理由でカーネルに手を入れていない。
;;   **GC_DEBUG と同じく「専用の make ターゲットで別ビルドを強制する」形を採った。**

(declaim (inline car cdr))
(defun dau-y-car (x) (car x))
(defun dau-y-cdr (x) (cdr x))
(declaim (notinline car cdr))
(defun dau-n-car (x) (car x))
(defun dau-n-cdr (x) (cdr x))

(assert-equal t (%%za-compiled-p (function dau-y-car)))
(assert-equal t (%%za-compiled-p (function dau-n-car)))

;; 宣言そのものは監査ビルドでも効いている(読み取りの経路は変わらない)
(assert-equal (quote (car cdr)) (%%inline-of (quote dau-y-car)))
(assert-equal nil (%%inline-of (quote dau-n-car)))

;; **D6 の本体: 宣言が inline でも、監査ビルドでは展開されない。**
;; 通常ビルドなら inline 側が長くなるので、ここが落ちる = ゲートとして働く。
;; 落ちたときにやること:
;;   1. EXTRA_CFLAGS=-DISIKIOS_DECLARE_AUDIT を付けてビルドしたか確認する
;;   2. 付けているのに落ちるなら、za_inline_enabled の #ifdef が効いていない
(assert-equal (%%disasm-code-len (quote dau-n-car)) (%%disasm-code-len (quote dau-y-car)))
(assert-equal (%%disasm-code-len (quote dau-n-cdr)) (%%disasm-code-len (quote dau-y-cdr)))

;; **safety 0 でも展開されない**(= 無検査の列が出ない)。
;; safety 0 の届け方はインライン展開なので、抑止されていれば
;; safety の値によらず同じ長さになる
(declaim (inline car))
(declaim (optimize (safety 0)))
(defun dau-s0-car (x) (car x))
(declaim (optimize (safety 1)))
(defun dau-s1-car (x) (car x))
(declaim (notinline car))
(assert-equal (%%disasm-code-len (quote dau-s1-car)) (%%disasm-code-len (quote dau-s0-car)))

;; 結果は当然同じ
(assert-equal 1 (dau-y-car (quote (1 2))))
(assert-equal 1 (dau-n-car (quote (1 2))))
(assert-equal 1 (dau-s0-car (quote (1 2))))
