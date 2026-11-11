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
;;; 2-b. [#199] **失敗の理由が区別できること**
;;; ---------------------------------------------------------------------------
;;
;; 直す前、FAT 経路の open-output-file は **どの段で落ちても
;; "cannot resolve file" と名乗っていた**。#199 では解決は成功していて
;; 切り詰め(= 中身を空にする)だけが失敗していたので、
;; **この文面が原因を別の場所へ誘導した**(調査した本人も最初そう読んだ)。
;;
;; いまは段ごとに別の文面を返す(src/c/mount.c の os_mount_fat_resolve_file_node
;; の out_detail、src/c/stream_lisp.c がそれをそのまま載せる):
;;
;;   解決できない        … "cannot resolve file"
;;   切り詰めができない  … "cannot truncate existing file"
;;   作成できない        … "cannot create file"
;;   下回りが居ない      … "fat driver not available"
;;   マウントが無い      … "no such mount"(FAT 経路に入る前で落ちる)
;;
;; **縮小を実装したあとも、ディスクフル等で切り詰めは失敗しうる**ので区別は残す。
;; この試験ファイルは FAT の円盤を持たない段でも走る(qemu_boot_m1_base.lisp 等)
;; ため、ここで固定できるのは最後の 1 つ。**要点は、解決の失敗を名乗って
;; いないこと。** FAT 経路側(切り詰めが通るようになったこと)は
;; test/lisp/fat16_test.lisp / fat32_test.lisp の「[#199] 既存の中身がある
;; ファイルの上書き保存」の節で見ている。
(assert-equal "open-output-file: /no-such-mount/x.txt: no such mount"
              (car (simple-error-format-arguments *io-c2*)))

(defglobal *io-c2b* (io-catch (lambda () (open-io-file "/no-such-mount/x.txt"))))
(assert-equal "open-io-file: /no-such-mount/x.txt: no such mount"
              (car (simple-error-format-arguments *io-c2b*)))

(defglobal *io-c2c* (io-catch (lambda () (open-input-file "/no-such-mount/x.txt"))))
(assert-equal "open-input-file: /no-such-mount/x.txt: no such mount"
              (car (simple-error-format-arguments *io-c2c*)))

;;; ---------------------------------------------------------------------------
;;; 3. load
;;; ---------------------------------------------------------------------------

(assert-error-class '<simple-error> (load "/9p/io-no-such-file.lisp"))
(defglobal *io-c3* (io-catch (lambda () (load "/9p/io-no-such-file.lisp"))))
(defglobal *io-msg3* (car (simple-error-format-arguments *io-c3*)))
(assert-equal "load: " (subseq *io-msg3* 0 6))

;; [M2-T2 / #193] **どちらの理由で落ちたのかを固定する。**
;; この件は M2 で**失敗の理由が入れ替わったのに結果が同じ**になった:
;;   以前: cc_load は *mounts* を引かず、"/9p/io-no-such-file.lisp" を
;;         そのまま 9p のパスとして開いていた(9p のルート直下に "9p" という
;;         入口が無いので落ちていた)
;;   いま: マウント表で "/9p" が剥がされ、「ホスト共有配下の存在しないファイル」
;;         として 9p の下位層で落ちる
;; **どちらも <simple-error> なので、クラスだけ見ていると意図が変わったことに
;; 誰も気づかない。** そこで「9p の下位層まで到達して、そこで落ちた」ことを
;; 文面で固定する(= 下の「解決できない」件とは別の理由であることを残す)。
;; 下位層の文面は src/c/virtio9p.c が組む。walk が通らないので "Twalk" で始まる
;; (Rerror でも "Twalk: Rerror: ..." になる)。**ここが落ちたら、まず
;; 9p の失敗段が変わったのか、マウント解決へ戻ってしまったのかを見分けること。**
(assert-equal "load: /9p/io-no-such-file.lisp: Twalk" (subseq *io-msg3* 0 37))

;; [M2-T1 / #193] **解決できない絶対パスはホスト共有へ落とさない。**
;; 落とすと、割り当て名の綴り間違いがホスト共有の文面で失敗して原因が
;; 分からなくなる。文面を丸ごと固定して、**上の件と理由が違うことを残す**
(assert-error-class '<simple-error> (load "/no-such-mount/x.lisp"))
(defglobal *io-c3b* (io-catch (lambda () (load "/no-such-mount/x.lisp"))))
(assert-equal "load: /no-such-mount/x.lisp: no such mount"
              (car (simple-error-format-arguments *io-c3b*)))

;; 構文エラーのあるファイルをその場で作って load する
;; [M2-T1 / #193] **load と open-*-file でパスの基準が違うのは、
;; 先頭に区切りが無い場合だけになった。**
;;   先頭が '/' でない -> ホスト共有(9p)のルートからの相対指定。**マウント表は引かない**
;;   先頭が '/'        -> open-*-file と同じくマウント表を引く
;; 2 つのグローバルを持つのは、**この両方を同じファイルで試すため**である
;; (以前は「load はマウント表を通らない」ので持っていた。理由が変わった)。
;; **心覚え: 相対 = 母艦(開発時)、絶対 = 機械の中。**
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
;; [陰性対照] 先頭に区切りが無い名前は従来どおりホスト共有から読まれる
(assert-equal t (load *io-ok-load*))
(assert-equal 7 *io-ld*)
;; [M2-T1 / #193] **同じファイルを "/9p/..." の絶対パスでも load できる。**
;; こちらはマウント表(組み込みの "/9p")を引く経路である
(setq *io-ld* 0)
(assert-equal t (load *io-ok-path*))
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
