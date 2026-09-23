;; test/lisp/test_framework_test.lisp
;;
;; test_framework.lisp 自身の検証。
;;
;; **新しく置いた検出器は、それ自体が検証対象である。** assert-error-class は
;; 「EVAL-ERROR を signal へ寄せる」作業(documents/error-unwind-survey.md §6-3 B)で
;; 期待値を書く道具になるので、道具が壊れていると全部の期待値が黙って無効になる。
;; 実際、過去に assert-equal をマクロ定義より前で使ったために「検出器が一度も
;; 走っていなかった」事故が起きている(test_framework.lisp 冒頭のコメント参照)。
;;
;; 陰性対照(わざと外れる使い方)は fail を増やしてしまうので、tf-capture で
;; カウンタと出力先を退避してから走らせ、必ず元へ戻す。

;;; ---------------------------------------------------------------------------
;;; 陰性対照を安全に走らせるための捕捉ヘルパ
;;; ---------------------------------------------------------------------------

;; thunk を評価する間だけ *isiki-test-stream* を文字列出力ストリームへ差し替え、
;; pass / fail / attempt の増分と、書き出された文字列を
;; (増pass 増fail 増attempt 出力) で返す。カウンタと出力先は必ず元へ戻すので、
;; 呼び出しの前後で集計は変わらない。
;;
;; *isiki-test-stream* などは defglobal なので、関数の中からの setq が呼び出し元にも
;; 見える(defvar+setq との違い。init.lisp 冒頭の既知の制約を参照)。
(defun tf-capture (thunk)
  (let ((saved-stream *isiki-test-stream*)
        (saved-pass *isiki-test-pass*)
        (saved-fail *isiki-test-fail*)
        (saved-attempt *isiki-test-attempt*)
        (sink (create-string-output-stream)))
    (setq *isiki-test-stream* sink)
    (funcall thunk)
    (let ((out (get-output-stream-string sink))
          (dpass (- *isiki-test-pass* saved-pass))
          (dfail (- *isiki-test-fail* saved-fail))
          (dattempt (- *isiki-test-attempt* saved-attempt)))
      (setq *isiki-test-stream* saved-stream)
      (setq *isiki-test-pass* saved-pass)
      (setq *isiki-test-fail* saved-fail)
      (setq *isiki-test-attempt* saved-attempt)
      (list dpass dfail dattempt out))))

(defun tf-dpass (r) (car r))
(defun tf-dfail (r) (car (cdr r)))
(defun tf-dattempt (r) (car (cdr (cdr r))))
(defun tf-out (r) (car (cdr (cdr (cdr r)))))

;; 期待する [NG] 行を組み立てる。
;; **書式は test_framework.lisp から写した独立のコピーである。** 文言を変えたら
;; ここも落ちる。それが狙い(ソース中に改行入りの文字列リテラルを置かずに
;; 行末の ~% まで含めて比べるための形でもある)。
(defun tf-ng-signaled-text (form actual-class expected-class)
  (let ((s (create-string-output-stream)))
    (format s "[NG] ~S => signaled ~S (expected ~S)~%" form actual-class expected-class)
    (get-output-stream-string s)))

(defun tf-ng-not-signaled-text (form actual expected-class)
  (let ((s (create-string-output-stream)))
    (format s "[NG] ~S => ~S (expected ~S to be signaled)~%" form actual expected-class)
    (get-output-stream-string s)))

(defun tf-ng-equal-text (form actual expected)
  (let ((s (create-string-output-stream)))
    (format s "[NG] ~S => ~S (expected ~S)~%" form actual expected)
    (get-output-stream-string s)))

;;; ---------------------------------------------------------------------------
;;; 捕捉ヘルパ自身の確認(これが効いていないと以下の対照が全部無意味になる)
;;; ---------------------------------------------------------------------------

;; 通る assert-equal を捕捉すると pass が 1 増え、fail は増えず、何も出力されない
(defglobal *tf-sane* (tf-capture (lambda () (assert-equal 1 1))))
(assert-equal '(1 0 1) (list (tf-dpass *tf-sane*) (tf-dfail *tf-sane*) (tf-dattempt *tf-sane*)))
(assert-equal "" (tf-out *tf-sane*))

;; 落ちる assert-equal を捕捉すると fail が 1 増え、[NG] が出る
(defglobal *tf-ng* (tf-capture (lambda () (assert-equal 1 2))))
(assert-equal '(0 1 1) (list (tf-dpass *tf-ng*) (tf-dfail *tf-ng*) (tf-dattempt *tf-ng*)))
(assert-equal (tf-ng-equal-text 2 2 1) (tf-out *tf-ng*))

;; 捕捉の前後でカウンタが元に戻っていること(戻っていないと以下の集計が狂う)
(defglobal *tf-fail-mark* *isiki-test-fail*)
(defglobal *tf-restore* (tf-capture (lambda () (assert-equal 1 2))))
(assert-equal t (= *tf-fail-mark* *isiki-test-fail*))

;;; ---------------------------------------------------------------------------
;;; assert-error-class: 陽性対照
;;; ---------------------------------------------------------------------------

;; ちょうどのクラスで通る
(defglobal *tf-ec-exact* (tf-capture (lambda () (assert-error-class '<simple-error> (error "tf boom")))))
(assert-equal '(1 0 1) (list (tf-dpass *tf-ec-exact*) (tf-dfail *tf-ec-exact*) (tf-dattempt *tf-ec-exact*)))
(assert-equal "" (tf-out *tf-ec-exact*))

;; **サブクラスでも通る。** <simple-error> は <error> の、<error> は
;; <serious-condition> のサブクラス(spec:994-1010 のクラス階層)
(defglobal *tf-ec-super* (tf-capture (lambda () (assert-error-class '<error> (error "tf boom")))))
(assert-equal '(1 0) (list (tf-dpass *tf-ec-super*) (tf-dfail *tf-ec-super*)))

(defglobal *tf-ec-super2* (tf-capture (lambda () (assert-error-class '<serious-condition> (error "tf boom")))))
(assert-equal '(1 0) (list (tf-dpass *tf-ec-super2*) (tf-dfail *tf-ec-super2*)))

;; C プリミティブ由来の condition でも効く((car 5) は <domain-error> を signal する)
(defglobal *tf-ec-c* (tf-capture (lambda () (assert-error-class '<domain-error> (car 5)))))
(assert-equal '(1 0) (list (tf-dpass *tf-ec-c*) (tf-dfail *tf-ec-c*)))

;; クラスオブジェクトを直接渡しても効く(typep と同じ designator)
(defglobal *tf-ec-obj* (tf-capture (lambda () (assert-error-class (%find-class '<simple-error>) (error "tf boom")))))
(assert-equal '(1 0) (list (tf-dpass *tf-ec-obj*) (tf-dfail *tf-ec-obj*)))

;;; ---------------------------------------------------------------------------
;;; assert-error-class: 陰性対照
;;; ---------------------------------------------------------------------------

;; **クラスが違えば落ちる。** これが効かないと assert-error と区別が付かない
(defglobal *tf-ec-wrong* (tf-capture (lambda () (assert-error-class '<division-by-zero> (error "tf boom")))))
(assert-equal '(0 1 1) (list (tf-dpass *tf-ec-wrong*) (tf-dfail *tf-ec-wrong*) (tf-dattempt *tf-ec-wrong*)))
(assert-equal (tf-ng-signaled-text '(error "tf boom") '<simple-error> '<division-by-zero>)
              (tf-out *tf-ec-wrong*))

;; **signal されなければ落ちる。** 値をそのまま返す形
(defglobal *tf-ec-none* (tf-capture (lambda () (assert-error-class '<simple-error> 42))))
(assert-equal '(0 1 1) (list (tf-dpass *tf-ec-none*) (tf-dfail *tf-ec-none*) (tf-dattempt *tf-ec-none*)))
(assert-equal (tf-ng-not-signaled-text 42 42 '<simple-error>) (tf-out *tf-ec-none*))

;; **P4-1 で 0 除算が signal に変わった**(<division-by-zero>、spec:4889)。
;; ここは以前「signal されないこと」の陰性対照だった(documents/error-unwind-survey.md
;; §A-4 のパターン1)。**書き換え忘れると必ず落ちる**形にしてあったので、
;; 移行の取りこぼしがそのまま検出できた
(defglobal *tf-ec-div* (tf-capture (lambda () (assert-error-class '<division-by-zero> (div 1 0)))))
(assert-equal '(1 0) (list (tf-dpass *tf-ec-div*) (tf-dfail *tf-ec-div*)))
(assert-equal "" (tf-out *tf-ec-div*))

;; **まだ signal しない経路**での陰性対照。(length 5) は EVAL-ERROR を値として返す
;; (P4-2「添字・範囲」の対象)。上と同じく、そこが signal に変わったらここが落ちる
(defglobal *tf-ec-len* (tf-capture (lambda () (assert-error-class '<domain-error> (length 5)))))
(assert-equal '(0 1) (list (tf-dpass *tf-ec-len*) (tf-dfail *tf-ec-len*)))
(assert-equal (tf-ng-not-signaled-text '(length 5) 'eval-error '<domain-error>)
              (tf-out *tf-ec-len*))

;;; ---------------------------------------------------------------------------
;;; assert-error との住み分け
;;; ---------------------------------------------------------------------------

;; assert-error は「何かが signal された」までしか見ないので、クラスが何であれ通る
(defglobal *tf-ae-any* (tf-capture (lambda () (assert-error (error "tf boom")))))
(assert-equal '(1 0) (list (tf-dpass *tf-ae-any*) (tf-dfail *tf-ae-any*)))

;; assert-error は「何かが signal された」ので 0 除算も通るようになった(P4-1)
(defglobal *tf-ae-div* (tf-capture (lambda () (assert-error (div 1 0)))))
(assert-equal '(1 0) (list (tf-dpass *tf-ae-div*) (tf-dfail *tf-ae-div*)))

;; assert-error も EVAL-ERROR 返しは捕まえられない(signal されていないため)。
;; まだ signal しない (length 5) で確かめる
(defglobal *tf-ae-len* (tf-capture (lambda () (assert-error (length 5)))))
(assert-equal '(0 1) (list (tf-dpass *tf-ae-len*) (tf-dfail *tf-ae-len*)))
