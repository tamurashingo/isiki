;; test/lisp/qemu_boot_abi_bench.lisp
;;
;; 呼び出し規約の刷新(documents/abi-redesign.md)の前後で
;; **「1 呼び出しあたりの確保量」**を比べるための計測基盤の入口。
;; 合否 4 件は「測る対象がちゃんと機械語に落ちていること」を見ている。
;;
;; [#208] **自動の周回に入れた。** それまでは文書に手順だけがあって
;; `Makefile` にも継続的検査にも 1 度も書かれたことがなく、**一度も走っていなかった。**
;; 「文書に手順だけがあって誰も走らせない」は #208 が生まれた状態そのものである。
;; 秒で終わるので手で呼ぶ入口のままにしない。
;;
;; [#203] 本体は**素の load ではなく isiki-test-load で読む。**
;; 素の load だと件数の行(#count)が出ないので、
;; 縮んだときに**どのファイルか言えない**段が 1 つ増える

(load "test/lisp/test_framework.lisp")
(isiki-test-load "test/lisp/abi_bench.lisp")

(isiki-test-report)
(close *isiki-test-stream*)
