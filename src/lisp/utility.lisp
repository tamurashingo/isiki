;;;; トランスパイラでAOTコンパイルする追加アプリケーション関数を置くファイル。
;;;;
;;;; init_aot.lispがinit.lispからの移動先であるのに対し、こちらは新規に追加する
;;;; 関数を置く。トランスパイラからの扱いは同じで、ここでdefunされた関数も
;;;; os_register_aot_init_functions経由でglobal_environmentへネイティブ関数として
;;;; 登録され、init.lisp側からインタプリタで定義された関数と同じシンボル名で
;;;; 呼び出せる。init_aot.lispと同じ制約(defunパラメータはシンボルのみ、bodyは
;;;; 単一式のみ、文字リテラルは未対応)に従う必要がある。

;;; --- room ---
;;;
;;; CommonLispのroomコマンド相当。heapの全体量(From空間、コピーGC用に確保している
;;; ヒープ全体の半分)・使用量と、Immobilized Spaceの全体量・使用量を、いずれも
;;; 生バイト数を3桁区切りのカンマ付きで"1,234,567 byte"のように表示する。

;; (%room-digit-string n) : 非負整数nを10進文字列に変換する。文字リテラルが
;; トランスパイラ未対応のため、create-string-output-stream+format "~D"経由で
;; 変換する(init_aot.lispのcerrorと同じパターン)。
(defun %room-digit-string (n)
  (let ((str (create-string-output-stream)))
    (progn
      (format str "~D" n)
      (get-output-stream-string str))))

;; (%room-group-digits-fill digits result src dst count) : digitsの末尾(src)から
;; 先頭へ向かって1文字ずつresultへコピーしつつ、3文字コピーするごとに","を
;; 挿入する再帰ヘルパー。","はcharリテラルが未対応のため(string-elt "," 0)で
;; 取り出す。
(defun %room-group-digits-fill (digits result src dst count)
  (if (< src 0)
      result
      (let ((next-count (+ count 1)))
        (progn
          (set-elt (string-elt digits src) result dst)
          (if (and (= (mod next-count 3) 0) (> src 0))
              (progn
                (set-elt (string-elt "," 0) result (- dst 1))
                (%room-group-digits-fill digits result (- src 1) (- dst 2) next-count))
              (%room-group-digits-fill digits result (- src 1) (- dst 1) next-count))))))

;; (%room-group-digits digits) : 数字だけの文字列digitsを3桁区切りのカンマ付き
;; 文字列にする("1234567" -> "1,234,567")。
(defun %room-group-digits (digits)
  (let* ((len (length digits))
         (ncommas (div (- len 1) 3))
         (result (create-string (+ len ncommas))))
    (progn
      (%room-group-digits-fill digits result (- len 1) (- (+ len ncommas) 1) 0)
      result)))

;; (%room-format-bytes n) : 非負整数nを"1,234,567 byte"の形式の文字列にする。
(defun %room-format-bytes (n)
  (string-append (%room-group-digits (%room-digit-string n)) " byte"))

;; (%room-output-stream) : *standard-output*が動的束縛されていれば(テスト時の
;; with-standard-output含む)それを使い、なければ画面への新規出力ストリームを開く。
;; *standard-output*はinit.lisp側でdefdynamicされているが、動的束縛はシンボル名
;; ベースのグローバルスタックなので、AOT側からdynamicで読んでも正しく束縛が見える。
(defun %room-output-stream ()
  (if (dynamic *standard-output*)
      (dynamic *standard-output*)
      (open-output-stream)))

;; (room) : heapの全体量・使用量、Immobilized Spaceの全体量・使用量を、いずれも
;; 3桁区切りカンマ付きのバイト数で表示する。
(defun room ()
  (let ((out (%room-output-stream)))
    (progn
      (format out "Heap total: ~A~%" (%room-format-bytes (%%heap-total-bytes)))
      (format out "Heap used: ~A~%" (%room-format-bytes (%%heap-used-bytes)))
      (format out "Immobilized space total: ~A~%" (%room-format-bytes (%%imm-space-total-bytes)))
      (format out "Immobilized space used: ~A~%" (%room-format-bytes (%%imm-space-used-bytes)))
      nil)))
