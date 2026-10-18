// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// choice_options.h -- the 34 options of the game's 17 choice menus, as
// analysys/dump_choice_menus.py reports them: the line the engine holds and
// the English the shipped table pairs with it.  Both columns are UTF-8, the
// way the table on disk is.
//
// analysys/ut_option_image.py reads this list, checks both columns against
// translations.tsv, and measures the English against the option tile.  Keep
// the row shape it parses: menu, branch, "japanese", "english".

namespace choice_options {

struct Option {
    int         menu;
    int         branch;
    const char* japanese;
    const char* english;
};

inline const Option OPTIONS[] = {
    { 1, 1,
      "聞かなかった事にしよう。",
      "I'll pretend I didn't hear that." },
    { 1, 2,
      "借りじゃないだろ。",
      "It's not a debt." },
    { 2, 1,
      "だからって、何で精神的妹なんて名乗るんだよ。",
      "Even so, why the hell would you call yourself my spiritual little sister?" },
    { 2, 2,
      "兄妹の様な仲の良さねぇ……。",
      "Close as siblings, huh......" },
    { 3, 1,
      "だけど、極端なお人好しでもある。",
      "But she's also extremely soft-hearted." },
    { 3, 2,
      "でも、それが鳥谷だ。",
      "But that's Toritani for you." },
    { 4, 1,
      "別に他意は無いが。",
      "Not that I had any ulterior motive." },
    { 4, 2,
      "いや、普通に興味があってだな。",
      "Just genuine curiosity." },
    { 5, 1,
      "雫の寝ぼけを見られるのは良いな。",
      "Being able to see Shizuku half-asleep is nice." },
    { 5, 2,
      "寝ぼけた雫の表情は生き生きとしているな……。",
      "Shizuku's half-asleep face is so lively..." },
    { 6, 1,
      "すまん。すまん。熱かったか？",
      "Sorry. Sorry. Was it hot?" },
    { 6, 2,
      "落とすなよ。もったいないから。",
      "Don't drop it. It'd be a waste." },
    { 7, 1,
      "ああ、やっぱり氷川かよ。",
      "Ah, so it really is Hikawa." },
    { 7, 2,
      "里奈って……氷川かよ……。",
      "Rina... as in Hikawa..." },
    { 8, 1,
      "忘れるなんてもったいない。",
      "Forgetting it would be a waste." },
    { 8, 2,
      "おもしろくて忘れられない。",
      "It's too funny to forget." },
    { 9, 1,
      "こんな事で我が身が危険な目に遭うのも考え物だ。",
      "Putting my own life in danger over something like this is something to think about." },
    { 9, 2,
      "たしかに氷川が危険な目に遭うのも考え物だ。",
      "It's true that putting Hikawa in danger is also something to think about." },
    { 10, 1,
      "違う！",
      "It's not like that!" },
    { 10, 2,
      "俺は女好きだ！",
      "I like women!" },
    { 11, 1,
      "たしかに氷川もやってれば客はもっと喜んだかもな。",
      "Honestly, if Hikawa did it too, the customers would probably be even happier." },
    { 11, 2,
      "まぁ、一年生だし、氷川までこんな事する必要はないよ。",
      "Well, she's a first-year -- there's no need to make Hikawa do this kind of thing." },
    { 12, 1,
      "想像などしないから大丈夫だが。",
      "It's fine because I'm not imagining anything." },
    { 12, 2,
      "自制心があるから大丈夫だが。",
      "It's fine because I have self-control." },
    { 13, 1,
      "なんやかんや俺も感謝しているからな。",
      "In one way or another, I'm grateful too." },
    { 13, 2,
      "ここでやっておかないと後で何言われるかわからんからな。",
      "If I don't do it now, who knows what I'll hear about it later." },
    { 14, 1,
      "そうか。",
      "I see." },
    { 14, 2,
      "雫がそう思って暮らしてくれているのなら、俺はうれしい。",
      "If Shizuku is living her life believing that, then I'm glad." },
    { 15, 1,
      "優美は我慢してくれている。",
      "Yumi is holding herself back." },
    { 15, 2,
      "優美の気持ちを考えたら。",
      "When I consider how Yumi feels..." },
    { 16, 1,
      "まぁ、たしかにお前のスカートの長さならパンツも見えないだろうし",
      "Well, at your skirt length your panties won't show, so" },
    { 16, 2,
      "あまりスカートが皺にならない様にしてくれれば問題ないさ",
      "as long as you keep the skirt from getting too wrinkled, there's no problem" },
    { 17, 1,
      "ああ、俺は間違っていた",
      "Ah, I was wrong." },
    { 17, 2,
      "分からない",
      "I don't know." },
};

inline const int OPTION_COUNT =
    static_cast<int>(sizeof(OPTIONS) / sizeof(*OPTIONS));

}  // namespace choice_options
