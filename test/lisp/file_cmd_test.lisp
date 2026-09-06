;; test/lisp/file_cmd_test.lisp
;;
;; src/lisp/file-cmd.lisp(pwd/cd/ls)の動作確認。
;;
;; 本ファイルはtest/lisp/test_framework.lisp(assert-equal等)とsrc/lisp/device.lisp+
;; src/lisp/ide.lisp+src/lisp/partition.lisp+src/lisp/fat32.lisp+src/lisp/mount.lisp+
;; test/lisp/partition_test.lisp+src/lisp/file-cmd.lispを、本ファイルより先に
;; boot-entryスクリプト(qemu_boot_partition.lisp)がloadしている前提で書かれている。
;;
;; partition_test.lispが既に(mount "/" 'blk0s1 ':fat32)しており、blk0s1(FAT32)の
;; ルートには"TEST.LSP"・"Partition_Slice_LFN_File.txt"・"Partition_Slice_LFN_Dir"
;; (中に"INSIDE.TXT")が存在する状態になっている。そのfixtureをそのまま流用する。
;; 追加でblk0s0(FAT16、"TEST.LSP"あり)を"/mnt"へmountし、複数マウント下での境界
;; 一致とfs-type振り分けも検証する。

;;; --- 1. 初期状態のpwd ---

(assert-equal "/" (pwd))
(assert-equal "/" (dynamic *cwd*))

;;; --- 2. ルートのls(引数あり/なし)がfat32-read-dirと同じ結果になること ---

(defglobal *file-cmd-test-root-listing*
  (list (list "TEST.LSP" ':file 36)
        (list "Partition_Slice_LFN_File.txt" ':file 9)
        (list "Partition_Slice_LFN_Dir" ':dir 0)))

(assert-equal *file-cmd-test-root-listing* (ls "/"))
(assert-equal *file-cmd-test-root-listing* (ls))

;;; --- 3. 相対パスでのcd、pwd、lsの一貫性 ---

(assert-equal "/Partition_Slice_LFN_Dir" (cd "Partition_Slice_LFN_Dir"))
(assert-equal "/Partition_Slice_LFN_Dir" (pwd))
(assert-equal (list (list "." ':dir 0) (list ".." ':dir 0) (list "INSIDE.TXT" ':file 9))
              (ls))

;;; --- 4. ".."で親ディレクトリへ戻れること ---

(assert-equal "/" (cd ".."))
(assert-equal "/" (pwd))

;;; --- 5. 別マウント("/mnt"、FAT16)への移動・一覧表示 ---

(mount "/mnt" 'blk0s0 ':fat16)

(assert-equal "/mnt" (cd "/mnt"))
(assert-equal (list (list "TEST.LSP" ':file 36)) (ls))

;; マウントパスの境界一致(src/c/mount.cのmount_path_match_lenと同じ規則)の
;; 直接確認。"/mnt"は"/mntx"のような接頭辞違いには一致してはいけない。
(assert-equal nil (%filecmd-mount-match-len "/mnt" "/mntx"))
(assert-equal 4 (%filecmd-mount-match-len "/mnt" "/mnt"))
(assert-equal 4 (%filecmd-mount-match-len "/mnt" "/mnt/sub"))
(assert-equal 1 (%filecmd-mount-match-len "/" "/anything"))

;;; --- 6. マウントを跨いだ絶対パス移動、存在しないパスの扱い ---

(assert-equal "/" (cd "/"))
(assert-equal nil (cd "/no-such-dir"))
;; cdに失敗しても*cwd*は変化しない。
(assert-equal "/" (pwd))
(assert-equal nil (ls "/no-such-dir"))
