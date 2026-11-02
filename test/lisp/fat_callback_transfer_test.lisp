;; test/lisp/fat_callback_transfer_test.lisp
;;
;; [P1] C から Lisp へ呼び戻す境界で、非局所脱出が値に化けないことの検証。
;;
;; stream.c の FAT 系ストリームは、書き込みバッファが満杯になると write-from! を、
;; 読み込みバッファが空になると read-into! を os_apply_function で Lisp へ呼び戻す。
;; **その戻り値は誰も検査していなかった。**
;; refill_read_buf_fat は戻り値を os_fixnum_magnitude へ通すので、脱出シグナルの
;; ヒープアドレスを「読めたバイト数」として解釈していた
;; (documents/error-unwind-survey.md §A-3 / §F-6)。
;;
;; ここで見るのは 3 点。
;;   1. 呼び戻した Lisp がエラーを signal したら、その場で評価が打ち切られること
;;      (脱出が write-char / read-char の呼び出し元まで届く)
;;   2. ストリームが壊れないこと。close しても暴走せず、以後の読み書きが
;;      黙って嘘の値を返さないこと
;;   3. 脱出のあと、別のストリームは普通に使えること(C 側の状態が
;;      1 つのストリームに閉じていること)
;;
;; **write-from! / read-into! を差し替えるので、このファイルは専用のブートで
;; 最後に走らせる。** 末尾で元の総称関数へ委譲する形に戻すが、他のテストと
;; 同じブートに混ぜると差し替えの影響が読みにくくなる。

(defglobal *p1-device* (%device-handle 'blk0))
(mount "/p1" 'blk0 ':fat16)

;; JIT に通すためループは defun に包む(fat16_test.lisp と同じ理由: トップレベルの
;; while はツリーウォーク型インタプリタの C 再帰になり、数万回でスタックを踏み越える)
(defun %%p1-write-n (stream ch n)
  (let ((i 0))
    (while (< i n)
      (write-char ch stream)
      (setq i (+ i 1)))))

;;; ---------------------------------------------------------------------------
;;; 0. 下敷き: 差し替える前に、素の経路が通ることを確かめる
;;; ---------------------------------------------------------------------------

;; 512byte(write_buf の容量)を明確に超えて書くと flush が必ず起きる
(defglobal *p1-base-stream* (open-output-file "/p1/P1BASE.TXT"))
(assert-equal t (if *p1-base-stream* t nil))
(%%p1-write-n *p1-base-stream* #\A 2000)
(close *p1-base-stream*)
(assert-equal 2000 (length (fat16-read-file *p1-device* "/P1BASE.TXT")))

;;; ---------------------------------------------------------------------------
;;; 1. write-from! が signal したら、書き込みがその場で打ち切られる
;;; ---------------------------------------------------------------------------

(defglobal *p1-orig-write-from* (function write-from!))
(defglobal *p1-orig-read-into* (function read-into!))

;; 呼ばれた回数も数える。**「エラーが出た」だけでは、呼び戻しに到達したのか
;; 手前で落ちたのかが区別できない。**
(defglobal *p1-write-calls* 0)
(defun write-from! (node buffer buffer-offset file-offset count)
  (setq *p1-write-calls* (+ *p1-write-calls* 1))
  (error "p1 write-from! escape"))

(defglobal *p1-wstream* (open-output-file "/p1/P1WR.TXT"))
(assert-equal t (if *p1-wstream* t nil))

;; flush が起きるまで書く。**脱出は write-char の呼び出し元まで届かなければならない**
(assert-error-class '<simple-error> (%%p1-write-n *p1-wstream* #\B 2000))
;; 呼び戻しに実際に到達していたことの証人
(assert-equal t (> *p1-write-calls* 0))

;;; --- ストリームが壊れていないこと ---

;; **もう一度書いても、黙って成功したことにならない。** 再び flush まで進んで
;; 再び脱出する(P1 以前は write-from! の戻り値が非 nil というだけで「書けた」と
;; 判定し、next_offset を len ぶん進めていた)
(defglobal *p1-write-calls-1* *p1-write-calls*)
(assert-error-class '<simple-error> (%%p1-write-n *p1-wstream* #\B 2000))
(assert-equal t (> *p1-write-calls* *p1-write-calls-1*))

;; close は静かに終わる。**失敗した flush が書きかけのバッファを捨てている**ので、
;; close の時点で送るものが残っていないため(write_buf_len = 0、next_offset は
;; 進めない)。エラーは「書き込みが失敗したその場」で 1 度だけ返す契約であり、
;; 同じ脱出値を二度は返さない(os_stream_take_transfer がクリアする)
(assert-equal nil (close *p1-wstream*))

;; **別のストリームは無事。** C 側の状態が 1 つのストリームに閉じていること
(defun write-from! (node buffer buffer-offset file-offset count)
  (funcall *p1-orig-write-from* node buffer buffer-offset file-offset count))
(defglobal *p1-ok-stream* (open-output-file "/p1/P1OK.TXT"))
(%%p1-write-n *p1-ok-stream* #\C 1500)
(close *p1-ok-stream*)
(assert-equal 1500 (length (fat16-read-file *p1-device* "/P1OK.TXT")))

;;; ---------------------------------------------------------------------------
;;; 2. read-into! が signal したら、読み込みがその場で打ち切られる
;;; ---------------------------------------------------------------------------

(defglobal *p1-read-calls* 0)
(defun read-into! (node buffer buffer-offset file-offset count)
  (setq *p1-read-calls* (+ *p1-read-calls* 1))
  (error "p1 read-into! escape"))

;; open-input-stream は FAT だとファイル全体を一括で読んで文字列ストリームにするので
;; refill を通らない。refill を通るのは open-io-file(STREAM_FAT_FILE_IO)だけ
(defglobal *p1-iostream* (open-io-file "/p1/P1BASE.TXT"))
(assert-equal t (if *p1-iostream* t nil))

;; **ここが本丸。** P1 以前は read-into! の戻り値(脱出シグナル)を
;; os_fixnum_magnitude へ通し、そのヒープアドレスを「読めたバイト数」として
;; buf_data へコピーしていた
(assert-error-class '<simple-error> (read-char *p1-iostream*))
(assert-equal t (> *p1-read-calls* 0))

;; 読めなかったのだから、以後の read も「読めた」と嘘をつかない。
;; error が立っているので read-char は EOF 扱い(eos-error-p 既定で <end-of-stream>)
(assert-error-class '<end-of-stream> (read-char *p1-iostream*))
;; eos-error-p を nil にすれば eos-value が返る(黙って偽のバイトを返さない)
(assert-equal 'p1-eof (read-char *p1-iostream* nil 'p1-eof))

(defun read-into! (node buffer buffer-offset file-offset count)
  (funcall *p1-orig-read-into* node buffer buffer-offset file-offset count))
(close *p1-iostream*)

;;; ---------------------------------------------------------------------------
;;; 3. 復元できていること(以後のテストに影響を残さない)
;;; ---------------------------------------------------------------------------

(defglobal *p1-restored* (open-io-file "/p1/P1BASE.TXT"))
(assert-equal #\A (read-char *p1-restored*))
(close *p1-restored*)
