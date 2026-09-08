#pragma once

#include <cstdint>
#include <string>

// 予定の件名・場所を表示前に整える層。旧PC版(scheduler_sender.py)の
// to_halfwidth() = unicodedata.normalize('NFKC', text) 相当を、
// 表示に効く範囲だけデバイス側へ移したもの。
//
// M5にもIDFにも依存させない。ホスト上での単体テストを後から入れられるようにする。

// UTF-8の文字列を正規化する。
// - 全角スペース(U+3000)を半角スペースにする
// - 全角英数記号(U+FF01〜U+FF5E)を半角にする
// - 半角カナ(U+FF61〜U+FF9F)を全角カナにする(濁点・半濁点は合成する)
// - 前後の空白を落とす
std::string normalizeText(const std::string& src);
