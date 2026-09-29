;; test/lisp/qemu_boot_error_escape_gc.lisp
;;
;; [P0〜P6 の回帰] **エラー脱出を GC_DEBUG ビルドで流し、shadow stack の
;; LIFO 規律が破れていないことを常設で確認する。**
;;
;; エラー脱出は「戻り値に埋めた制御転送値のバケツリレー」で実装されていて
;; (setjmp/longjmp は使わない)、gc_roots の巻き戻しは GC_PROTECT の
;; __attribute__((cleanup)) に任せている。**途中でフレームを飛ばす経路が増えると
;; ここが静かに破れる**(pitfalls 原則6の形: エラーにならずカウンタだけが増える)。
;;
;; g_gc_lifo_violations は GC_DEBUG ビルドにしか存在しない
;; (gc_unprotect_node の #ifdef ISIKIOS_GC_DEBUG、src/c/runtime.h)。
;; したがってこのファイルは **GC_DEBUG=1 でしか意味を持たない**。
;; make test-qemu-error-escape-gc から実行する。
;;
;; 併せて os_signal_condition の深さ打ち切り(SIGNAL_MAX_DEPTH)が
;; 発動していないことも見る。**発動していたら条件の構築自体が失敗している**ので、
;; 0 でなくなった時点で設計が崩れている。

(load "src/lisp/init.lisp")
(load "test/lisp/test_framework.lisp")

(defglobal *ee-lifo-before* (%%diag-gc-lifo-violations))
(defglobal *ee-overflow-before* (%%diag-signal-overflows))

;; P0〜P6 で入れたエラー脱出のテストを全部通す
(isiki-test-load "test/lisp/toplevel_abort_test.lisp")      ; P2
(isiki-test-load "test/lisp/handler_exit_test.lisp")        ; P3
(isiki-test-load "test/lisp/arith_signal_test.lisp")        ; P4-1
(isiki-test-load "test/lisp/index_signal_test.lisp")        ; P4-2
(isiki-test-load "test/lisp/undefined_signal_test.lisp")    ; P4-3
(isiki-test-load "test/lisp/io_signal_test.lisp")           ; P4-4
(isiki-test-load "test/lisp/type_unbound_test.lisp")        ; P5
(isiki-test-load "test/lisp/arity_signal_test.lisp")        ; P6

;; **ここが本題。** 上の全テストを通しても LIFO 違反が 0 のままであること
(assert-equal *ee-lifo-before* (%%diag-gc-lifo-violations))
(assert-equal 0 (%%diag-gc-lifo-violations))
;; signal の深さ打ち切りは正常系では発動しない
(assert-equal *ee-overflow-before* (%%diag-signal-overflows))
(assert-equal 0 (%%diag-signal-overflows))

(isiki-test-report)
(close *isiki-test-stream*)
