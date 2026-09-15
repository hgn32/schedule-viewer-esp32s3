#include "text_util.h"

namespace {

void utf8Append(std::string* dst, uint32_t cp) {
    if (cp < 0x80) {
        dst->push_back((char)cp);
    } else if (cp < 0x800) {
        dst->push_back((char)(0xC0 | (cp >> 6)));
        dst->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        dst->push_back((char)(0xE0 | (cp >> 12)));
        dst->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        dst->push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        dst->push_back((char)(0xF0 | (cp >> 18)));
        dst->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        dst->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        dst->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// 半角カナ(U+FF61〜U+FF9F)から全角カナへの対応表。添字はcp - 0xFF61。
const uint32_t kHalfKana[] = {
    0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3, // ｡｢｣､･ｦｧｨ
    0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC, // ｩｪｫｬｭｮｯｰ
    0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA,                         // ｱｲｳｴｵ
    0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,                         // ｶｷｸｹｺ
    0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,                         // ｻｼｽｾｿ
    0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,                         // ﾀﾁﾂﾃﾄ
    0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE,                         // ﾅﾆﾇﾈﾉ
    0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,                         // ﾊﾋﾌﾍﾎ
    0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2,                         // ﾏﾐﾑﾒﾓ
    0x30E4, 0x30E6, 0x30E8,                                         // ﾔﾕﾖ
    0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED,                         // ﾗﾘﾙﾚﾛ
    0x30EF, 0x30F3, 0x309B, 0x309C,                                 // ﾜﾝﾞﾟ
};

// 濁点が付く全角カナ。清音はすべて+1で濁音になるが、小書き文字が間に挟まる
// (ッがタとツの間にある等)ため、規則ではなく表で持つ。付かない文字は0を返す。
uint32_t withDakuten(uint32_t cp) {
    static const uint32_t kVoiceable[] = {
        0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,  // カキクケコ
        0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,  // サシスセソ
        0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,  // タチツテト
        0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,  // ハヒフヘホ
    };
    for (uint32_t c : kVoiceable) {
        if (c == cp) return cp + 1;
    }
    if (cp == 0x30A6) return 0x30F4;  // ウ -> ヴ
    return 0;
}

// 半濁点が付く全角カナ(ハ行のみ)。付かない文字は0を返す。
uint32_t withHandakuten(uint32_t cp) {
    static const uint32_t kSemiVoiceable[] = {
        0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,  // ハヒフヘホ
    };
    for (uint32_t c : kSemiVoiceable) {
        if (c == cp) return cp + 2;
    }
    return 0;
}

}  // namespace

size_t utf8NextChar(const std::string& s, size_t i, uint32_t* out) {
    if (out == nullptr || i >= s.size()) return 0;

    const unsigned char c = (unsigned char)s[i];
    const size_t rest = s.size() - i;

    if (c < 0x80) {
        *out = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && rest >= 2) {
        *out = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[i + 1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && rest >= 3) {
        *out = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) |
               (uint32_t)(s[i + 2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && rest >= 4) {
        *out = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) |
               ((uint32_t)(s[i + 2] & 0x3F) << 6) | (uint32_t)(s[i + 3] & 0x3F);
        return 4;
    }
    *out = 0xFFFD;
    return 1;
}

std::string normalizeText(const std::string& src) {
    std::string out;
    out.reserve(src.size());

    size_t i = 0;
    while (i < src.size()) {
        uint32_t cp = 0;
        i += utf8NextChar(src, i, &cp);

        if (cp == 0x3000) {          // 全角スペース
            cp = 0x20;
        } else if (cp >= 0xFF01 && cp <= 0xFF5E) {  // 全角英数記号
            cp -= 0xFEE0;
        } else if (cp >= 0xFF61 && cp <= 0xFF9F) {  // 半角カナ
            cp = kHalfKana[cp - 0xFF61];

            // 直後の濁点・半濁点は前の文字へ合成する(NFKCと同じ扱い)。
            if (i < src.size()) {
                uint32_t next = 0;
                const size_t adv = utf8NextChar(src, i, &next);
                if (next == 0xFF9E) {  // ﾞ
                    const uint32_t merged = withDakuten(cp);
                    if (merged != 0) {
                        cp = merged;
                        i += adv;
                    }
                } else if (next == 0xFF9F) {  // ﾟ
                    const uint32_t merged = withHandakuten(cp);
                    if (merged != 0) {
                        cp = merged;
                        i += adv;
                    }
                }
            }
        }

        utf8Append(&out, cp);
    }

    // 前後の空白を落とす(旧PC版のclean_text()末尾のstrip()相当)。
    const size_t head = out.find_first_not_of(" \t\r\n");
    if (head == std::string::npos) return "";
    const size_t tail = out.find_last_not_of(" \t\r\n");
    return out.substr(head, tail - head + 1);
}
