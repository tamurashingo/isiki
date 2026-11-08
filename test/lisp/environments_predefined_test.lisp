;; test/lisp/environments_predefined_test.lisp
;;
;; *environments*: init.lisp読み込み前の登録がdefdynamicで消されないこと。
;;
;; 実機の対話的なREPLでは、os_repl_step(repl.c)がinit.lisp読み込みより先に
;; proc->envを遅延生成し、os_make_process_environment(runtime.c)経由で
;; *environments*へ登録する(REPLプロンプトはinit.lisp読み込み前から動くため)。
;; defdynamicはeval_defdynamicが無条件にos_set_dynamicで上書きする仕様なので、
;; init.lisp内の(defdynamic *environments* ...)がこの事前登録を消してnilに
;; 戻してしまわないことを確認する必要がある。
;;
;; qemu実機起動ではこの事前登録のタイミングが再現できない(同梱した基盤ライブラリは
;; process 0がREPLループへ入る**前**に読まれるので、proc->envの遅延生成より先になる)。
;; そのため本ファイルはscript_test.c(ネイティブテスト)専用とし、init.lispを
;; loadする直前にmain()からos_make_process_environment("F1")を呼んだ状態を
;; 前提にする。qemu boot script(qemu_boot_test.lisp等)からはloadしないこと。
;;
;; **この試験は #187 の陽性対照でもある。** init.lispの
;; (defdynamic *environments* (dynamic *environments*)) を素の nil へ戻すと、
;; ここが落ちる(実際に確かめた)。安全形の一覧と機械的な検査は
;; tools/check_defdynamic_safe.py。

(assert-equal t (if (member 'F1 (list-environments)) t nil))
