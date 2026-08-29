;; test/lisp/bitwise_char_test.lisp
;;
;; FAT16-M0(0)(FAT16-M0(1)(documents/fs.md)で追加したビット演算/文字コード変換の
;; subprimitive(logand/logior/logxor/ash/char-code/code-char)が、QEMU実機上の
;; ブート済みカーネルでAOTコンパイル経路(init_aot.lisp -> lisp_compiled.c)を通して
;; 正しく動くことを確認する。ネイティブ`make test`はcc_logand等のC関数を直接呼ぶ
;; だけでglobal_environmentへの登録やAOTディスパッチ経路を経由しないため、この
;; QEMU側の確認が別途必要。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal)を、本ファイルより先に
;; boot-entryスクリプトがloadしている前提で書かれている。

;;; --- logand/logior/logxor ---

(assert-equal 61440 (logand #xF0F0 #xFF00))
(assert-equal 65535 (logior #xF0F0 #x0F0F))
(assert-equal 4080 (logxor #xFF00 #xF0F0))

;;; --- ash (正のcountで左シフト、負のcountで右シフト) ---

(assert-equal 256 (ash 1 8))
(assert-equal 0 (ash 3 -8))
(assert-equal 240 (ash #xF0F0 -8))

;;; --- char-code/code-char ---

(assert-equal 65 (char-code #\A))
(assert-equal #\A (code-char 65))
