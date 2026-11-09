#ifndef _LOAD_H_
#define _LOAD_H_

#include "types.h"
#include "stream.h"

/**
 * ストリームからS式を読めなくなるまで順に評価する「読んで評価する輪」。
 *
 * 9P経由のLOAD・マウント経由のLOAD(M2)・カーネルへ同梱したソースの
 * 読み込み(M1)で**共有する**。呼び出し元は開いたストリームを渡すだけでよい。
 * ストリームはこの関数が閉じる(正常終了・構文エラー・I/Oエラーのいずれでも)。
 *
 * **包まれたストリーム(MAGIC_STREAM)も渡せる。** その場合os_stream_t本体は
 * GCのたびに別アドレスへ複製されるので、この関数は**反復ごとに
 * self_handleから実体を取り直す**(M2-T0 / #196)。呼び出し元が渡した生ポインタは
 * 1回目のGCで古くなるため、**この関数から戻った後は使わないこと。**
 * 包まれていない(self_handleがnil)ストリーム、すなわち9p経由のcc_loadと
 * カーネル同梱の読み込みはCのローカル/静的領域にあってGCで動かないので、
 * 渡した生ポインタをそのまま使う。
 *
 * **打ち切りは失敗にならない。** エラーで打ち切られたフォームは理由を1行表示して
 * 飛ばし、次のフォームへ進む。最後まで読めれば打ち切りがあってもg_sym_tを返す。
 * したがって**戻り値だけでは「半分失敗した」が分からない**ので、
 * 止める判断をしたい呼び出し元はout_aborted_countを渡す。
 *
 * @param stream 読み込み元(開いた状態で渡す。この関数が閉じる)
 * @param name エラー表示に使う読み込み元の名前。**開く用途には使わない**
 *             (9P経由ならパスそのもの)
 * @param env 評価に使う環境(読んだ各S式もこのenvで評価される)
 * @param out_aborted_count 打ち切られたフォームの件数を**加算**する先。
 *        不要なら0を渡す(呼び出し元が数えない場合は何も起きない)
 * @return 最後まで読めればg_sym_t。構文エラー・I/Oエラーのときは
 *         os_signal_io_errorの戻り値(signalが上がるので通常は戻らない)
 */
lisp_val_t os_load_from_stream(os_stream_t *stream, const char *name, lisp_val_t env,
                               int *out_aborted_count);

/**
 * 組み込み関数LOAD。第一引数のパスの9Pファイルをstreamで開き、
 * os_load_from_streamへ渡す(= 開き方だけを決める層)。
 * open失敗・構文エラー・I/Oエラーのときはos_signal_io_errorで
 * <simple-error>をsignalする(以前は自前で1行表示してg_sym_eval_errorを
 * 返していたが、conditionのメッセージが同じ内容を運ぶため改めた)。
 * ロード中の個々のフォームが打ち切られた場合は理由を表示して続行し、
 * **戻り値はg_sym_tのまま**である(件数を見たい場合はos_load_from_streamを直接呼ぶ)。
 * @param args (path) pathはSTRING(9Pエクスポートルートから見た相対パス)
 * @param env 評価に使う環境(読み込んだ各S式もこのenvで評価される)
 * @return 最後まで読めればg_sym_t
 */
lisp_val_t cc_load(lisp_val_t args, lisp_val_t env);

/** LOADをglobal_environmentに関数として登録する */
void os_register_load(void);

#endif /* _LOAD_H_ */
