;; test/lisp/io_signal_test.lisp
;;
;; [P4-4] 入出力(load / open-*-file / file-length)の EVAL-ERROR 返しを
;; signal へ移した分の検証。
;;
;; **クラスの選択は仕様未確認。** open-input-file / open-output-file /
;; open-io-file について仕様が定めている誤りは「filename が文字列でないこと」だけで、
;; 開く操作自体は "The corresponding file is opened in an implementation-defined way"
;; (spec:6266-6268)とされ、**開けなかった場合の error-id は挙げられていない**。
;; load はそもそも ISLisp 仕様に無い(実装独自)。
;;
;; <stream-error> を使わないのは、そのスロットが stream ただ1つで、仕様が
;; 「the stream on which the error occurred」(spec:7146-7147)と定めているため。
;; ここで失敗しているのはストリームを開く操作そのもので、載せられるストリームが無い。
;; <simple-error> なら失敗したパスと下位層のメッセージをそのまま運べる。
;;
;; **file-length だけは signal しない。** spec:6858-6859「Returns the length of the
;; file named by filename, or **returns nil if the length cannot be determined**」。
;; signal すべきと仕様が定めているのは filename が文字列でない場合だけ(spec:6860)。

(defun io-catch (thunk)
  (block io
    (with-handler (lambda (c) (return-from io c)) (funcall thunk))))

;;; ---------------------------------------------------------------------------
;;; 1. 開けないファイル -> <simple-error>
;;; ---------------------------------------------------------------------------

(assert-error-class '<simple-error> (open-input-file "/9p/io-no-such-file.txt"))
(assert-error-class '<simple-error> (open-input-stream "/9p/io-no-such-file.txt"))
;; 階層
(assert-error-class '<error> (open-input-file "/9p/io-no-such-file.txt"))
(assert-error-class '<serious-condition> (open-input-file "/9p/io-no-such-file.txt"))

;; どのマウントにも解決できないパス
(assert-error-class '<simple-error> (open-input-file "/no-such-mount/x.txt"))
(assert-error-class '<simple-error> (open-output-file "/no-such-mount/x.txt"))
(assert-error-class '<simple-error> (open-io-file "/no-such-mount/x.txt"))

;;; ---------------------------------------------------------------------------
;;; 2. メッセージ(失敗したパスが載っていること)
;;; ---------------------------------------------------------------------------
;;
;; format-string は "~A" 固定で、本文は format-arguments 側に入れてある。
;; **パスに ~ が含まれていても format の指示子として解釈されないようにするため。**

(defglobal *io-c1* (io-catch (lambda () (open-input-file "/9p/io-no-such-file.txt"))))
(assert-equal "~A" (simple-error-format-string *io-c1*))
(defglobal *io-msg1* (car (simple-error-format-arguments *io-c1*)))
(assert-equal t (stringp *io-msg1*))
;; "open-input-file: " で始まり、パスが続く
(assert-equal "open-input-file: " (subseq *io-msg1* 0 17))
(assert-equal "/9p/io-no-such-file.txt" (subseq *io-msg1* 17 40))

;; report-condition 経由でも同じ本文が出る
(assert-equal *io-msg1* (%report-condition-string *io-c1*))

(defglobal *io-c2* (io-catch (lambda () (open-output-file "/no-such-mount/x.txt"))))
(assert-equal "open-output-file: " (subseq (car (simple-error-format-arguments *io-c2*)) 0 18))

;;; ---------------------------------------------------------------------------
;;; 3. load
;;; ---------------------------------------------------------------------------

(assert-error-class '<simple-error> (load "/9p/io-no-such-file.lisp"))
(defglobal *io-c3* (io-catch (lambda () (load "/9p/io-no-such-file.lisp"))))
(assert-equal "load: " (subseq (car (simple-error-format-arguments *io-c3*)) 0 6))

;; 構文エラーのあるファイルをその場で作って load する
;; **load と open-*-file ではパスの基準が違う。** open-*-file は *mounts* を
;; 引くので "/9p/..." だが、load(cc_load)は os_mount_resolve を通さず
;; 9p のルートからの相対パスを直接開く。同じファイルを両方から指すので2つ持つ
(defglobal *io-broken-path* "/9p/tmp/io_broken_p44.lisp")
(defglobal *io-broken-load* "tmp/io_broken_p44.lisp")
(defglobal *io-broken-out* (open-output-file *io-broken-path*))
(write-char #\( *io-broken-out*)
(write-char #\a *io-broken-out*)
(close *io-broken-out*)
(assert-error-class '<simple-error> (load *io-broken-load*))
(defglobal *io-c4* (io-catch (lambda () (load *io-broken-load*))))
(assert-equal t (stringp (car (simple-error-format-arguments *io-c4*))))

;; 正常な内容なら t を返す(戻り値の契約は変えていない)
(defglobal *io-ok-path* "/9p/tmp/io_ok_p44.lisp")
(defglobal *io-ok-load* "tmp/io_ok_p44.lisp")
(defglobal *io-ok-out* (open-output-file *io-ok-path*))
(write-char #\( *io-ok-out*)
(write-char #\d *io-ok-out*) (write-char #\e *io-ok-out*) (write-char #\f *io-ok-out*)
(write-char #\g *io-ok-out*) (write-char #\l *io-ok-out*) (write-char #\o *io-ok-out*)
(write-char #\b *io-ok-out*) (write-char #\a *io-ok-out*) (write-char #\l *io-ok-out*)
(write-char #\Space *io-ok-out*)
(write-char #\* *io-ok-out*) (write-char #\i *io-ok-out*) (write-char #\o *io-ok-out*)
(write-char #\- *io-ok-out*) (write-char #\l *io-ok-out*) (write-char #\d *io-ok-out*)
(write-char #\* *io-ok-out*)
(write-char #\Space *io-ok-out*)
(write-char #\7 *io-ok-out*) (write-char #\) *io-ok-out*)
(close *io-ok-out*)
(assert-equal t (load *io-ok-load*))
(assert-equal 7 *io-ld*)

;;; ---------------------------------------------------------------------------
;;; 4. file-length は nil を返す(signal しない)
;;; ---------------------------------------------------------------------------

(assert-equal nil (file-length "/9p/io-no-such-file.txt"))
(assert-equal nil (file-length "/no-such-mount/x.txt"))
;; 実在するファイルなら長さが返る(上で書いた2文字のファイル)
(assert-equal 2 (file-length *io-broken-path*))

;;; ---------------------------------------------------------------------------
;;; 5. 評価がその場で打ち切られること
;;; ---------------------------------------------------------------------------

(defglobal *io-trace* nil)
(defun io-note (x) (setq *io-trace* (cons x *io-trace*)) x)

(setq *io-trace* nil)
(assert-error-class '<simple-error>
  (list (io-note 'a) (open-input-file "/9p/io-no-such-file.txt") (io-note 'b)))
(assert-equal '(A) (reverse *io-trace*))

;;; ---------------------------------------------------------------------------
;;; 6. 正常系(変えていないこと)
;;; ---------------------------------------------------------------------------

(defglobal *io-s* (open-input-file *io-ok-path*))
(assert-equal #\( (read-char *io-s*))
(close *io-s*)

;; 書いて読み戻せること
(defglobal *io-rt-path* "/9p/tmp/io_rt_p44.txt")
(defglobal *io-rt-out* (open-output-file *io-rt-path*))
(write-char #\X *io-rt-out*)
(close *io-rt-out*)
(assert-equal 1 (file-length *io-rt-path*))
(defglobal *io-rt-in* (open-input-file *io-rt-path*))
(assert-equal #\X (read-char *io-rt-in*))
(close *io-rt-in*)

;; open-io-file も実在パスなら開ける
(defglobal *io-io* (open-io-file *io-rt-path*))
(assert-equal t (if *io-io* t nil))
(close *io-io*)
