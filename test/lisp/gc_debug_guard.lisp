;; test/lisp/gc_debug_guard.lisp
;;
;; [GCデバッグ] **GC_DEBUG ビルドを要求するテストが、通常ビルドで走ったら落ちる**ための
;; ゲート。documents/for-expansion.md §1-4。
;;
;; なぜ要るか:
;;   `GC_DEBUG=1 make build` の後に `make test-qemu-milestone` をフラグ無しで叩くと、
;;   **test-qemu-milestone が build に依存しているのでその場で通常ビルドへ戻される。**
;;   すると %%DIAG-GC-STRESS が存在しないので GC は 1 回も強制されず、
;;   **「GC を強制したつもりで強制していない」まま全部通る。**
;;   2026-10-04 に実際に踏んだ(1 ブート無駄にした)。
;;
;;   PR #87 の `git stash` 空振り(「前」を測ったつもりで「後」を測っていた)と
;;   同じ構造である。そして **Makefile のコメントは既に警告していた。**
;;   **コメントによる警告は効かない。効くのは落ちる仕組みだけである。**
;;
;; 判定の仕組み:
;;   %%DIAG-GC-STRESS は GC_DEBUG ビルドにしか登録されない(runtime.c:3307 の
;;   #ifdef ISIKIOS_GC_DEBUG の中)。通常ビルドで呼ぶと未定義関数が signal されるので、
;;   ignore-errors で捕まえて判定する。**カーネルには手を入れていない**
;;   (組み込み関数を 1 つ足すと SMALL_HEAP_SIZE の再調整が要る既知の罠がある)。

;; GC_DEBUG ビルドなら t、通常ビルドなら nil
(defun gc-debug-build-p ()
  (if (ignore-errors (progn (%%diag-gc-stress 0) t)) t nil))

;; GC_DEBUG ビルドでなければ failed を増やして理由を書き、nil を返す。
;; 呼び出し側は戻り値が t のときだけ本題へ進むこと
(defun require-gc-debug-build (label)
  (if (gc-debug-build-p)
      (progn
        (format *isiki-test-stream* "[GC_DEBUG-OK] ~A: GC_DEBUG ビルドです~%" label)
        (finish-output *isiki-test-stream*)
        t)
    (progn
      (setq *isiki-test-fail* (+ *isiki-test-fail* 1))
      (format *isiki-test-stream*
              "[NG] ~A: **通常ビルドです。**%%DIAG-GC-STRESS が無いので GC を強制できません。~%"
              label)
      (format *isiki-test-stream*
              "     env GC_DEBUG=1 を **make test-qemu-milestone 側にも**渡すこと~%")
      (format *isiki-test-stream*
              "     (GC_DEBUG=1 make build だけでは、test が依存する build で戻される)~%")
      (finish-output *isiki-test-stream*)
      nil)))
