;;;; PWD/CD/LSのLisp側API。*mounts*(src/lisp/mount.lisp)を使って絶対パスを
;;;; デバイス+相対パスへ解決し、fat16-read-dir/fat32-read-dirへ橋渡しする。
;;;;
;;;; device.lisp/mount.lisp/fat16.lisp/fat32.lispと同様、AOTトランスパイル対象外の、
;;;; 通常のload形式(インタプリタ実行専用)ファイル。REPLから
;;;; (load "src/lisp/device.lisp")→(load "src/lisp/mount.lisp")→
;;;; (load "src/lisp/fat16.lisp")→(load "src/lisp/fat32.lisp")(fat16/fat32は
;;;; 順不同)の後に(load "src/lisp/file-cmd.lisp")で明示的に読み込む。
;;;;
;;;; ディレクトリ一覧をマウント経由で取るためのC↔Lisp橋渡し関数(src/c/mount.cの
;;;; os_mount_fat_read_file相当のread-dir版)は存在しないため、os_mount_resolve
;;;; (src/c/mount.c)と同じ最長一致ルールをここでLisp側に再実装し、
;;;; fat32-read-dir/fat16-read-dirを直接呼ぶ(C側の変更を避ける)。9P
;;;; ("/9p"配下)はos_mount_resolveと異なりここでは解決対象にしない
;;;; (fat32-read-dir/fat16-read-dir相当の9P用read-dirが無いため)。

;; *cwd* : 現在の作業ディレクトリを表す"/"始まりの絶対パス文字列。cdで更新する。
;; *devices*/*mounts*と同じ理由でdefdynamic+%%set-dynamicを使う(defglobalは
;; 呼び出し元の環境にしか書き込まれないため)。
(defdynamic *cwd* "/")

;;; --- パス文字列のユーティリティ(fat16.lisp/fat32.lisp非依存の独立実装) ---
;;;
;;; %fat32-split-path/%fat16-split-pathと同じ分解ロジックだが、file-cmd.lispは
;;; fat16.lisp/fat32.lispのどちらか一方しかloadされていない環境でも動く必要が
;;; あるため独立実装する。要素数は現実的なディレクトリ深さ(せいぜい数十)に
;;; 収まるため、%fat16-reverse-iter/%fat32-reverse-iterのようなwhileベースの
;;; 反転は使わずビルトインのreverse(init_aot.lisp)をそのまま使う。

;; (%filecmd-split-path path) : "/DOCS/SUB"のようなpathを"/"区切りで非空要素の
;; リスト("DOCS" "SUB")に分解する。"/"や""は空パス扱いでnil。
(defun %filecmd-split-path (path)
  (let ((start 0) (len (length path)) (rev-parts nil) (slash-pos nil) (end nil))
    (while (< start len)
      (setq slash-pos (char-index #\/ path start))
      (setq end (if slash-pos slash-pos len))
      (if (> end start)
          (setq rev-parts (cons (subseq path start end) rev-parts)))
      (setq start (+ end 1)))
    (reverse rev-parts)))

;; (%filecmd-join-components-iter components acc) : componentsを先頭から順に
;; accへ"/"+要素の形で連結していく。
(defun %filecmd-join-components-iter (components acc)
  (if (null components)
      acc
      (%filecmd-join-components-iter
        (cdr components)
        (string-append acc (string-append "/" (car components))))))

;; (%filecmd-join-components components) : パス要素のリストを"/"区切りの絶対
;; パス文字列に戻す。componentsがnilなら"/"(ルート)自身。
(defun %filecmd-join-components (components)
  (if (null components)
      "/"
      (%filecmd-join-components-iter components "")))

;; (%filecmd-normalize-components-iter components acc) : "."は無視、".."は
;; accの先頭(直前の要素)を1つ取り除く(accが空なら無視してルートに留まる、
;; 一般的なcdの挙動)。accは逆順に積む。
(defun %filecmd-normalize-components-iter (components acc)
  (if (null components)
      (reverse acc)
      (let ((c (car components)))
        (cond
          ((string= c ".")
           (%filecmd-normalize-components-iter (cdr components) acc))
          ((string= c "..")
           (%filecmd-normalize-components-iter
             (cdr components) (if (null acc) nil (cdr acc))))
          (t (%filecmd-normalize-components-iter
               (cdr components) (cons c acc)))))))

;; (%filecmd-normalize-components components) : "."/".."を解決した正規化済み
;; パス要素リストを返す。
(defun %filecmd-normalize-components (components)
  (%filecmd-normalize-components-iter components nil))

;; (%filecmd-absolute-p path) : pathが"/"始まりの絶対パスかを判定する。
(defun %filecmd-absolute-p (path)
  (eq (char-index #\/ path) 0))

;; (%filecmd-absolute-path base path) : path(絶対パスならそのまま、相対パス
;; ならbase(通常*cwd*)からの相対)を"."/".."込みで正規化した絶対パス文字列に
;; する。
(defun %filecmd-absolute-path (base path)
  (let ((components
          (if (%filecmd-absolute-p path)
              (%filecmd-split-path path)
              (append (%filecmd-split-path base) (%filecmd-split-path path)))))
    (%filecmd-join-components (%filecmd-normalize-components components))))

;;; --- マウント解決(src/c/mount.cのos_mount_resolveのLisp再実装) ---

;; (%filecmd-mount-match-len mount-path path) : mount-pathがpathの接頭辞として
;; 正しく一致する場合その文字数を、しなければnilを返す。一致直後がpathの終端か
;; "/"であることを要求する(例: マウントパス"/mnt"は"/mnt2/x"には一致しない)。
;; ただしmount-pathが"/"自身の場合は常に一致する。mount_path_match_len
;; (src/c/mount.c)と同じ規則。
(defun %filecmd-mount-match-len (mount-path path)
  (let ((mlen (length mount-path)) (plen (length path)))
    (if (and (= mlen 1) (string= mount-path "/"))
        1
        (if (and (<= mlen plen) (string= (subseq path 0 mlen) mount-path))
            (if (or (= mlen plen) (string= (subseq path mlen (+ mlen 1)) "/"))
                mlen
                nil)
            nil))))

;; (%filecmd-resolve-mount-iter path mounts best best-len) : mounts(*mounts*の
;; 残り)を先頭から走査し、pathに最も長く一致するエントリをbest/best-lenへ
;; 積み上げていく。
(defun %filecmd-resolve-mount-iter (path mounts best best-len)
  (if (null mounts)
      best
      (let ((entry (car mounts)))
        (let ((mount-path (car entry))
              (device (car (cdr entry)))
              (fs-type (cdr (cdr entry))))
          (let ((m (%filecmd-mount-match-len mount-path path)))
            (if (and m (> m best-len))
                (%filecmd-resolve-mount-iter
                  path (cdr mounts)
                  (list device fs-type
                        (if (and (= (length mount-path) 1) (string= mount-path "/"))
                            path
                            (subseq path m (length path))))
                  m)
                (%filecmd-resolve-mount-iter path (cdr mounts) best best-len)))))))

;; (%filecmd-resolve-mount path) : *mounts*から絶対パスpathに最長一致する
;; マウントエントリを探し、(list device fs-type relative-path)を返す。一致する
;; マウントが無ければnil。9Pは対象外(os_mount_resolveと異なる点、file-header
;; コメント参照)。
(defun %filecmd-resolve-mount (path)
  (%filecmd-resolve-mount-iter path (dynamic *mounts*) nil -1))

;; (%filecmd-read-dir device fs-type relative-path) : fs-typeに応じて
;; fat32-read-dir/fat16-read-dirへ振り分ける。対応外のfs-type(現状9P等)なら
;; nil。
(defun %filecmd-read-dir (device fs-type relative-path)
  (let ((handle (%device-handle device)))
    (cond
      ((eq fs-type ':fat32) (fat32-read-dir handle relative-path))
      ((eq fs-type ':fat16) (fat16-read-dir handle relative-path))
      (t nil))))

;;; --- コマンド本体 ---

;; (%filecmd-print-entries out entries) : entries(("NAME" :file/:dir size)の
;; リスト)の各NAMEを1行ずつoutへ表示する。
(defun %filecmd-print-entries (out entries)
  (if (null entries)
      nil
      (progn
        (format out "~A~%" (car (car entries)))
        (%filecmd-print-entries out (cdr entries)))))

;; (%filecmd-no-such-path out path) : "PATH: no such file or directory"を
;; outへ表示してnilを返す(describeの"no such device"と同じ流儀)。
(defun %filecmd-no-such-path (out path)
  (progn
    (format out "~A: no such file or directory~%" path)
    nil))

;; (pwd) : *cwd*を%device-output-streamへ表示し、*cwd*を返す。
(defun pwd ()
  (progn
    (format (%device-output-stream) "~A~%" (dynamic *cwd*))
    (dynamic *cwd*)))

;; (ls &rest path-args) : path-args省略時は*cwd*、指定時はそのパス(絶対/
;; *cwd*からの相対)が指すディレクトリのエントリ一覧を、fat32-read-dir/
;; fat16-read-dirと同じ("NAME" :file/:dir size)の形のリストで返す。副作用と
;; して各エントリ名を1行ずつ%device-output-streamへ表示する。マウントが
;; 見つからない、またはディレクトリが存在しない場合はエラーメッセージを表示
;; してnil。対象がマウントルート自身の場合はread-dirを呼ばず常に存在する
;; ものとして扱う(マウント登録=ルートの存在保証。FATのサブディレクトリは
;; 必ず"."/".."エントリを持つためread-dirのnil=非存在と判定できるが、ルート
;; だけは"."/".."を持たないため中身が0件の場合と区別できない、
;; fat32-read-dir/fat16-read-dir自体の既知の制約)。
(defun ls (&rest path-args)
  (let ((target (%filecmd-absolute-path
                  (dynamic *cwd*) (if (null path-args) "" (car path-args)))))
    (let ((resolved (%filecmd-resolve-mount target)))
      (if (null resolved)
          (%filecmd-no-such-path (%device-output-stream) target)
          (let ((device (car resolved))
                (fs-type (car (cdr resolved)))
                (relative (car (cdr (cdr resolved)))))
            (if (null (%filecmd-split-path relative))
                (let ((entries (%filecmd-read-dir device fs-type relative)))
                  (progn
                    (%filecmd-print-entries (%device-output-stream) entries)
                    entries))
                (let ((entries (%filecmd-read-dir device fs-type relative)))
                  (if (null entries)
                      (%filecmd-no-such-path (%device-output-stream) target)
                      (progn
                        (%filecmd-print-entries (%device-output-stream) entries)
                        entries)))))))))

;; (cd path) : path(絶対/*cwd*からの相対)が実在するディレクトリを指す場合、
;; *cwd*をその正規化済み絶対パスへ更新して返す。マウントが見つからない、
;; またはディレクトリが存在しない場合はエラーメッセージを表示してnil
;; (*cwd*は変更しない)。存在判定はlsと同じ規則(マウントルート自身は常に
;; 存在、それ以外はread-dirが非nilを返すことで判定)。
(defun cd (path)
  (let ((target (%filecmd-absolute-path (dynamic *cwd*) path)))
    (let ((resolved (%filecmd-resolve-mount target)))
      (if (null resolved)
          (%filecmd-no-such-path (%device-output-stream) target)
          (let ((device (car resolved))
                (fs-type (car (cdr resolved)))
                (relative (car (cdr (cdr resolved)))))
            (if (or (null (%filecmd-split-path relative))
                    (%filecmd-read-dir device fs-type relative))
                (progn
                  (%%set-dynamic '*cwd* target)
                  target)
                (%filecmd-no-such-path (%device-output-stream) target)))))))
