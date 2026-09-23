;; test/lisp/qemu_boot_instcount_gate_check.lisp
;;
;; 命令数ゲート(documents/performance-measurement.md「手法1の改良案」)の
;; **合格判定**。使い始める前に、計器が使い物になることを先に示す。
;;
;;   陰性対照: 同じバイナリを 2 ブートして、区間の命令数がほぼ一致すること
;;   陽性対照: タグ検査 1 組で、約 +10 命令/回 が分離して見えること
;;
;; 陽性対照は primitive_add2_fixnum の中身を差し替えた**別ビルド**が要るので、
;; このファイルは「同じ区間を同じ条件で測る」ところまでを受け持つ。
;;
;; 使い方: make test-qemu-instcount-gate MILESTONE=test/lisp/qemu_boot_instcount_gate_check.lisp

(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

;; **パス1(プラグイン無し)はこの行を取りに来る。**
;; Makefile が sed で拾って gate_start=/gate_end= に渡す
(format *isiki-test-stream* "#gate ~A~%" (%%gate-plugin-args))
(finish-output *isiki-test-stream*)

(defun gc-id (x y) x)                       ; primitive を一切呼ばない
(defun gc-fix (x y)
  (declare (type <fixnum> x)) (declare (type <fixnum> y)) (+ x y))
(defun gc-loop (f a b n)
  (let ((i 0) (r nil))
    (while (< i n) (setq r (funcall f a b)) (setq i (+ i 1)))
    r))

(defglobal *gc-n* 200000)
(defglobal *gc-warm* 2000)

;; **ウォームアップはゲートの外で。** JIT コンパイルとコードページの確保が
;; 区間の中に入ると、それが命令数に乗る
(gc-loop (function gc-id)  12345 6789 *gc-warm*)
(gc-loop (function gc-fix) 12345 6789 *gc-warm*)
(assert-equal t (%%za-compiled-p (function gc-id)))
(assert-equal t (%%za-compiled-p (function gc-fix)))
(assert-equal 19134 (gc-fix 12345 6789))

;; 区間ごとに GC 回数の差も出す。**区間の中で GC が起きたら命令数は信用できない**
(defun gc-seg (label thunk-f a b n)
  ;; tick も一緒に出す。**区間の中でタイマー割り込みが何回入ったか**の代理指標。
  ;; 命令数がブート間で完全一致するのに tick が動くなら、
  ;; 割り込みハンドラの命令は数えられていないことになる(設計4 の確認)
  (let ((g0 (%%gc-collect-count)) (t0 (get-internal-real-time)))
    (%%gate-open)
    (gc-loop thunk-f a b n)
    (%%gate-close)
    (format *isiki-test-stream* "#seg ~A gc=~D ticks=~D~%"
            label (- (%%gc-collect-count) g0) (- (get-internal-real-time) t0))
    (finish-output *isiki-test-stream*)))

;; SEG 0: 空のゲート。**ゲート自身のオフセット**(マーカーの呼び出しぶん)。
;; 他の区間と同じ形式で出す(Makefile が行番号で突き合わせる)
(let ((g0 (%%gc-collect-count)) (t0 (get-internal-real-time)))
  (%%gate-open)
  (%%gate-close)
  (format *isiki-test-stream* "#seg empty-gate gc=~D ticks=~D~%"
          (- (%%gc-collect-count) g0) (- (get-internal-real-time) t0))
  (finish-output *isiki-test-stream*))

;; SEG 1: 呼び出し 1 回の床(primitive を呼ばない)
(gc-seg "f-id" (function gc-id) 12345 6789 *gc-n*)
;; SEG 2: 測りたいもの(fixnum 特化版を call)
(gc-seg "+fix-decl" (function gc-fix) 12345 6789 *gc-n*)
;; SEG 3: SEG 2 の再測(**同一ブート内の再現性**)
(gc-seg "+fix-decl-again" (function gc-fix) 12345 6789 *gc-n*)

;; ゲートを実際に通った回数。プラグインの gate_opens/gate_closes と突き合わせる
(format *isiki-test-stream* "#hits ~S  (open . close) 期待 (4 . 4)~%" (%%gate-hits))
(assert-equal 4 (car (%%gate-hits)))
(assert-equal 4 (cdr (%%gate-hits)))
(finish-output *isiki-test-stream*)

(isiki-test-report)
(close *isiki-test-stream*)
