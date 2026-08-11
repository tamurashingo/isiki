#ifndef _P9_H_
#define _P9_H_

#include "types.h"

/** 9Pメッセージタイプ(9P2000, プレーンダイアレクト) */
#define P9_TVERSION 100
#define P9_RVERSION 101
#define P9_TATTACH  104
#define P9_RATTACH  105
#define P9_RERROR   107
#define P9_TWALK    110
#define P9_RWALK    111
#define P9_TOPEN    112
#define P9_ROPEN    113
#define P9_TCREATE  114
#define P9_RCREATE  115
#define P9_TREAD    116
#define P9_RREAD    117
#define P9_TWRITE   118
#define P9_RWRITE   119
#define P9_TCLUNK   120
#define P9_RCLUNK   121

/** Topen/Tcreateのmode: 読み込み専用 */
#define P9_OREAD 0x00
/** Topen/Tcreateのmode: 書き込み専用 */
#define P9_OWRITE 0x01
/** Topen/Tcreateのmode: 読み書き両方 */
#define P9_ORDWR 0x02
/** Topenのmodeに重ねるビット: openと同時にファイルを空にする(truncate) */
#define P9_OTRUNC 0x10

/** 9P2000.uのn_uname: 数値uidを使わないことを示す特別値 */
#define P9_NONUNAME 0xFFFFFFFFu

/** qid構造体のバイトサイズ(type[1]+version[4]+path[8]) */
#define P9_QID_SIZE 13

/**
 * Tversionメッセージを組み立てる
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param msize 提示する最大メッセージサイズ
 * @param version バージョン文字列("9P2000")
 * @return 書き込んだバイト数(size フィールドと一致)
 */
UINT32 os_p9_build_tversion(UINT8 *buf, UINT16 tag, UINT32 msize, const char *version);

/**
 * Tattachメッセージを組み立てる
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param fid attach後にrootを指すfid
 * @param afid 認証用fid(未使用時はP9_NOFID相当を渡す)
 * @param uname ユーザ名
 * @param aname アクセスするファイルツリー名(空文字列可)
 * @param n_uname 9P2000.uの数値uid。使わない場合はP9_NONUNAMEを渡す
 * @return 書き込んだバイト数
 */
UINT32 os_p9_build_tattach(UINT8 *buf, UINT16 tag, UINT32 fid, UINT32 afid, const char *uname, const char *aname, UINT32 n_uname);

/**
 * Twalkメッセージを組み立てる
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param fid walk開始元のfid
 * @param newfid walk結果を束縛する新しいfid
 * @param wnames パス要素の配列
 * @param nwname wnamesの要素数
 * @return 書き込んだバイト数
 */
UINT32 os_p9_build_twalk(UINT8 *buf, UINT16 tag, UINT32 fid, UINT32 newfid, const char **wnames, UINT16 nwname);

/**
 * Topenメッセージを組み立てる
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param fid openするfid
 * @param mode P9_OREAD等
 * @return 書き込んだバイト数
 */
UINT32 os_p9_build_topen(UINT8 *buf, UINT16 tag, UINT32 fid, UINT8 mode);

/**
 * Treadメッセージを組み立てる
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param fid readするfid
 * @param offset 読み込み開始オフセット
 * @param count 読み込み要求バイト数
 * @return 書き込んだバイト数
 */
UINT32 os_p9_build_tread(UINT8 *buf, UINT16 tag, UINT32 fid, UINT64 offset, UINT32 count);

/**
 * Twriteメッセージを組み立てる
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param fid writeするfid
 * @param offset 書き込み開始オフセット
 * @param data 書き込むバイト列
 * @param count dataのバイト数
 * @return 書き込んだバイト数(size フィールドと一致)
 */
UINT32 os_p9_build_twrite(UINT8 *buf, UINT16 tag, UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count);

/**
 * Tcreateメッセージを組み立てる。成功時、fidはwalk済みの親ディレクトリから
 * 新規作成・open済みのファイルを指すようになる(9P標準仕様の挙動)。
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param fid 作成先ディレクトリを指すfid(呼び出し後は新規ファイルを指す)
 * @param name 作成するファイル名(パス区切りを含まない単一要素)
 * @param perm 作成する際のUnixパーミッション(例: 0644)
 * @param mode P9_OWRITE/P9_ORDWR等
 * @return 書き込んだバイト数
 */
UINT32 os_p9_build_tcreate(UINT8 *buf, UINT16 tag, UINT32 fid, const char *name, UINT32 perm, UINT8 mode);

/**
 * Tclunkメッセージを組み立てる
 * @param buf 書き込み先バッファ
 * @param tag メッセージタグ
 * @param fid 解放するfid
 * @return 書き込んだバイト数
 */
UINT32 os_p9_build_tclunk(UINT8 *buf, UINT16 tag, UINT32 fid);

/**
 * Rversionメッセージを解析する
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @param out_msize デバイスが確定したmsizeを格納する先
 * @param out_version バージョン文字列を格納する先(NUL終端する)
 * @param version_cap out_versionの容量
 * @return 正しくRversionとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_rversion(const UINT8 *buf, UINT32 len, UINT32 *out_msize, char *out_version, UINT32 version_cap);

/**
 * Rattachメッセージを解析する(qidの内容は今回使わないため妥当性のみ確認する)
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @return 正しくRattachとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_rattach(const UINT8 *buf, UINT32 len);

/**
 * Rwalkメッセージを解析する
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @param out_nwqid 実際にwalkできた要素数を格納する先
 * @return 正しくRwalkとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_rwalk(const UINT8 *buf, UINT32 len, UINT16 *out_nwqid);

/**
 * Ropenメッセージを解析する(qid/iounitの内容は今回使わないため妥当性のみ確認する)
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @return 正しくRopenとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_ropen(const UINT8 *buf, UINT32 len);

/**
 * Rreadメッセージを解析する
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @param out_data 実データ部分の先頭を指すポインタを格納する先(bufを指す、コピーしない)
 * @param out_count 実データのバイト数を格納する先
 * @return 正しくRreadとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_rread(const UINT8 *buf, UINT32 len, const UINT8 **out_data, UINT32 *out_count);

/**
 * Rclunkメッセージを解析する(payloadは無いため妥当性のみ確認する)
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @return 正しくRclunkとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_rclunk(const UINT8 *buf, UINT32 len);

/**
 * Rwriteメッセージを解析する
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @param out_count 実際に書き込めたバイト数を格納する先
 * @return 正しくRwriteとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_rwrite(const UINT8 *buf, UINT32 len, UINT32 *out_count);

/**
 * Rcreateメッセージを解析する(qid/iounitの内容は今回使わないため妥当性のみ確認する)
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @return 正しくRcreateとして解析できた場合1、そうでなければ0
 */
int os_p9_parse_rcreate(const UINT8 *buf, UINT32 len);

/**
 * 受信メッセージがRerrorかどうかを確認する
 * @param buf 受信バッファ
 * @param len 受信バイト数
 * @param errbuf Rerrorだった場合、エラー文字列(NUL終端)を格納する先
 * @param errbuf_cap errbufの容量
 * @return Rerrorだった場合1、そうでなければ0
 */
int os_p9_check_error(const UINT8 *buf, UINT32 len, char *errbuf, UINT32 errbuf_cap);

#endif /* _P9_H_ */
