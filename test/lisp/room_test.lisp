;; test/lisp/room_test.lisp
;;
;; roomコマンド(src/lisp/utility.lisp、AOTトランスパイル済み)の動作確認。
;; roomはブート時にos_register_aot_init_functions経由でglobal_environmentへ
;; 登録済みなので、本ファイルからloadは不要でそのまま呼び出せる。
;;
;; 本ファイルはtest/lisp/test_framework.lispが定義するassert-equal等をそのまま
;; 使う(boot-entryスクリプトが本ファイルより先にそれをloadしている前提)。

;;; --- 1. %room-group-digits: 3桁区切りカンマ挿入の単体テスト ---

(assert-equal "0" (%room-group-digits "0"))
(assert-equal "9" (%room-group-digits "9"))
(assert-equal "567" (%room-group-digits "567"))
(assert-equal "1,234" (%room-group-digits "1234"))
(assert-equal "12,345" (%room-group-digits "12345"))
(assert-equal "123,456" (%room-group-digits "123456"))
(assert-equal "1,234,567" (%room-group-digits "1234567"))
(assert-equal "123,456,789" (%room-group-digits "123456789"))

;;; --- 2. %room-format-bytes: "1,234,567 byte"形式への変換 ---

(assert-equal "0 byte" (%room-format-bytes 0))
(assert-equal "1,234,567 byte" (%room-format-bytes 1234567))

;;; --- 3. room: *standard-output*捕捉で実際の出力を確認する ---
;;
;; heap全体量・immobilized space全体量/使用量は起動後は変化しない固定値なので
;; 厳密比較できるが、heap使用量はroom呼び出し中も評価の副作用(cons/文字列の
;; 確保)で増え続ける移動対象であり、room内部が参照した瞬間の値とテスト側が
;; 別途取得する値は原理的に一致しない。そのためheap使用量の行だけは前後の
;; ラベル文字列の存在で構造的に確認し、数値そのものは比較しない。

(defun %room-test-expected-prefix ()
  (string-append
    "Heap total: " (%room-format-bytes (%%heap-total-bytes)) (create-string 1 #\Newline)
    "Heap used: "))

(defun %room-test-expected-suffix ()
  (string-append
    (create-string 1 #\Newline)
    "Immobilized space total: " (%room-format-bytes (%%imm-space-total-bytes)) (create-string 1 #\Newline)
    "Immobilized space used: " (%room-format-bytes (%%imm-space-used-bytes)) (create-string 1 #\Newline)))

(assert-output (result actual) (room)
  (assert-equal nil result)
  (assert-equal 0 (string-index (%room-test-expected-prefix) actual))
  (assert-equal t (numberp (string-index (%room-test-expected-suffix) actual))))
