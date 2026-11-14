// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu/render_story_items.h"

#include "menu/core/sprite_canvas.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ama::menu {

namespace {

constexpr std::size_t kExpectedStoryItemPatchCount = 120;
constexpr std::size_t kFirstFrameIndex = 0;

// Colors (BGRA order: b, g, r, a)
constexpr BgraColor kTransparentColor{0x00, 0x00, 0x00, 0x00};
constexpr BgraColor kOutgoingBubbleFill{232, 234, 206, 255};   // #CEEAE8
constexpr BgraColor kIncomingBubbleFill{255, 255, 255, 255};   // #FFFFFF
constexpr BgraColor kChatBubbleInk{38, 38, 38, 255};           // #262626
constexpr BgraColor kChatReadLabelInk{115, 115, 115, 255};     // #737373
constexpr BgraColor kChatSendBtnFill{94, 104, 115, 255};       // #73685E
constexpr BgraColor kChatSendBtnInk{188, 195, 201, 255};       // #C9C3BC
constexpr BgraColor kNotebookInk{56, 56, 56, 255};             // #383838
constexpr BgraColor kLetterInk{50, 52, 56, 255};               // #383432

// Chat geometry constants
constexpr int kChatReadEraseX = 0;
constexpr int kChatReadEraseY = 20;
constexpr int kChatReadEraseW = 20;
constexpr int kChatReadEraseBottomMargin = 2;
constexpr int kChatReadDrawX = 0;
constexpr int kChatReadDrawY = 20;
constexpr int kChatReadDrawW = 20;
constexpr int kChatReadDrawH = 16;

constexpr int kOutgoingSampleX = 24;
constexpr int kOutgoingSampleY = 20;
constexpr int kOutgoingFillX = 26;
constexpr int kOutgoingFillY = 7;
constexpr int kOutgoingFillRightMargin = 52;
constexpr int kOutgoingFillBottomMargin = 7;
constexpr int kOutgoingTextX = 30;
constexpr int kOutgoingTextY = 8;
constexpr int kOutgoingTextRightMargin = 52;
constexpr int kOutgoingTextBottomMargin = 8;

constexpr int kIncomingFillX = 58;
constexpr int kIncomingFillY = 6;
constexpr int kIncomingFillRightMargin = 8;
constexpr int kIncomingFillBottomMargin = 6;
constexpr int kIncomingTextX = 56;
constexpr int kIncomingTextY = 5;
constexpr int kIncomingTextRightMargin = 6;
constexpr int kIncomingTextBottomMargin = 5;

constexpr RectI kChatSendButtonEraseRect{398, 680, 44, 28};
constexpr RectI kChatSendButtonTextRect{394, 678, 52, 30};

// Font sizes and fit limits
constexpr float kChatReadFontSizePx = 9.5F;
constexpr float kChatSendFontSizePx = 14.0F;
constexpr float kChatSingleLineFontSizePx = 15.0F;
constexpr float kChatMultiLineFontSizePx = 12.5F;
constexpr float kNoteFontSizePx = 14.0F;
constexpr float kLetterStripFontSizePx = 13.5F;
constexpr float kStationery890FontSizePx = 13.5F;
constexpr float kStationeryA4FontSizePx = 38.0F;
constexpr float kStationeryRooftopFontSizePx = 40.0F;

constexpr float kDefaultMinSqueeze = 0.72F;
constexpr float kDefaultMinScale = 0.45F;
constexpr float kTightLineSpacing = 0.95F;
constexpr int kRegularWeight = 400;
constexpr int kSemiBoldWeight = 600;

constexpr int kGroupSheetColumns = 4;
constexpr int kGroupSheetCellsPerPage = 32;
constexpr int kGroupSheetMaxCellPx = 320;

enum class ChatBubbleKind {
    kOutgoingSingle,
    kIncomingSingle,
    kIncomingMultiLine,
};

struct ChatBubbleSpec {
    std::string_view filename;
    ChatBubbleKind kind;
    std::string_view english_text;
};

constexpr std::array<ChatBubbleSpec, 28> kChatBubbleSpecs = {{
    {"item_chat_01A.hg3", ChatBubbleKind::kOutgoingSingle, "Hello"},
    {"item_chat_01B.hg3", ChatBubbleKind::kIncomingSingle, "Hello"},
    {"item_chat_01C.hg3", ChatBubbleKind::kOutgoingSingle, "Oh, right"},
    {"item_chat_01D.hg3", ChatBubbleKind::kIncomingMultiLine,
     "Nice weather today, isn't it?\nTomorrow should be nice too"},
    {"item_chat_01E.hg3", ChatBubbleKind::kOutgoingSingle, "Maybe so"},
    {"item_chat_01F.hg3", ChatBubbleKind::kIncomingMultiLine,
     "Not sure about the day\nafter tomorrow, though"},
    {"item_chat_01G.hg3", ChatBubbleKind::kOutgoingSingle, "True"},
    {"item_chat_01H.hg3", ChatBubbleKind::kOutgoingSingle, "I love you, Rei"},
    {"item_chat_01I.hg3", ChatBubbleKind::kIncomingSingle, "I love you too"},
    {"item_chat_01J.hg3", ChatBubbleKind::kIncomingSingle, "Can't wait"},
    {"item_chat_02A.hg3", ChatBubbleKind::kIncomingSingle, "I got on the train"},
    {"item_chat_02B.hg3", ChatBubbleKind::kOutgoingSingle, "Is it crowded?"},
    {"item_chat_02C.hg3", ChatBubbleKind::kIncomingSingle, "Moderately"},
    {"item_chat_02D.hg3", ChatBubbleKind::kIncomingSingle, "Looks like we're off"},
    {"item_chat_02E.hg3", ChatBubbleKind::kOutgoingSingle, "Safe travels"},
    {"item_chat_02F.hg3", ChatBubbleKind::kIncomingSingle, "Yes. Snowy around me too"},
    {"item_chat_02G.hg3", ChatBubbleKind::kIncomingSingle, "Wrong message"},
    {"item_chat_02H.hg3", ChatBubbleKind::kIncomingSingle, "Ah"},
    {"item_chat_02I.hg3", ChatBubbleKind::kIncomingSingle, "Can't delete it"},
    {"item_chat_02J.hg3", ChatBubbleKind::kIncomingSingle, "Don't mindo"},
    {"item_chat_02K.hg3", ChatBubbleKind::kIncomingSingle, "Please don't mind it"},
    {"item_chat_02L.hg3", ChatBubbleKind::kOutgoingSingle, "Good effort"},
    {"item_chat_02M.hg3", ChatBubbleKind::kIncomingSingle, "Why?"},
    {"item_chat_03A.hg3", ChatBubbleKind::kOutgoingSingle, "Got it. Let's take it easy"},
    {"item_chat_03B.hg3", ChatBubbleKind::kIncomingSingle, "Yeah, thanks"},
    {"item_chat_03C.hg3", ChatBubbleKind::kIncomingSingle, "See you soon!"},
    {"item_chat_04A.hg3", ChatBubbleKind::kIncomingMultiLine,
     "I have something for you.\nPlease come to Kurayami\nSlope after school."},
    {"item_chat_04B.hg3", ChatBubbleKind::kOutgoingSingle, "Got it, I'll be there"},
}};

struct NoteStripSpec {
    std::string_view filename;
    std::string_view english_text;
};

constexpr std::array<NoteStripSpec, 26> kNoteStripSpecs = {{
    {"item_note_01A.hg3", "Is the courtyard okay for lunch today too?"},
    {"item_note_01B.hg3", "Yeah, let's do the courtyard."},
    {"item_note_01C.hg3", "Are you helping out at the inn today, Yuuhi?"},
    {"item_note_01D.hg3",
     "Yeah, looks like we have lots of guests,\nso I have to head straight back. Sorry."},
    {"item_note_01E.hg3", "Can't be helped if you're busy.\nDo your best."},
    {"item_note_01F.hg3",
     "Thanks. The landlady's been praising me a lot\nlately. She says my expression's brighter."},
    {"item_note_01G.hg3", "I can tell. You look so full of life, Yuuhi."},
    {"item_note_01H.hg3", "Yuuhi."},
    {"item_note_01I.hg3", "What?"},
    {"item_note_01J.hg3", "I've fallen even more in love with you now than before."},
    {"item_note_01K.hg3", "Geez, don't startle me out of nowhere like that!!"},
    {"item_note_01L.hg3", "Putting it in writing started feeling fun too."},
    {"item_note_01M.hg3", "Sorry."},
    {"item_note_01N.hg3", "Dummy."},
    {"item_note_01O.hg3", "I love you too."},
    {"item_note_02A.hg3", "Yuuhi"},
    {"item_note_02B.hg3", "What's wrong?"},
    {"item_note_02C.hg3", "When is your birthday, Yuuhi?"},
    {"item_note_02D.hg3", "Don't tell me it's today, is it?"},
    {"item_note_02E.hg3", "It's not today, though."},
    {"item_note_02F.hg3", "Though?"},
    {"item_note_02G.hg3", "January 15th."},
    {"item_note_02H.hg3", "If you'd told me, I could get something ready."},
    {"item_note_02I.hg3", "Because it'd sound like I'm asking for a gift."},
    {"item_note_02J.hg3",
     "Weren't you panicking earlier because you\ndidn't have anything ready for a classmate?"},
    {"item_note_02K.hg3", "Well, that's true, but..."},
}};

constexpr std::size_t kCompositeChildCount = 5;

struct NoteCompositeSpec {
    std::string_view filename;
    std::array<std::string_view, kCompositeChildCount> children;
};

constexpr std::array<NoteCompositeSpec, 3> kNoteCompositeSpecs = {{
    {"item_note_01P.hg3",
     {"item_note_01A.hg3", "item_note_01B.hg3", "item_note_01C.hg3",
      "item_note_01D.hg3", "item_note_01E.hg3"}},
    {"item_note_01Q.hg3",
     {"item_note_01F.hg3", "item_note_01G.hg3", "item_note_01H.hg3",
      "item_note_01I.hg3", "item_note_01J.hg3"}},
    {"item_note_01R.hg3",
     {"item_note_01K.hg3", "item_note_01L.hg3", "item_note_01M.hg3",
      "item_note_01N.hg3", "item_note_01O.hg3"}},
}};

struct TegamiStripSpec {
    std::string_view filename;
    HorizontalAlign align;
    std::string_view english_text;
};

// 56 standard single-box transparent letter strips (03E has dual body+signature boxes)
constexpr std::array<TegamiStripSpec, 56> kTegamiStripSpecs = {{
    {"item_tegami_01A.hg3", HorizontalAlign::kLeft, "Dear,"},
    {"item_tegami_01B.hg3", HorizontalAlign::kLeft,
     "The season when we long for warm sunshine has arrived."},
    {"item_tegami_01C.hg3", HorizontalAlign::kLeft,
     "Did this sudden letter surprise you?\n"
     "Still, I felt this was the best way to convey my feelings.\n"
     "It would make me very happy if you read it to the end."},
    {"item_tegami_01D.hg3", HorizontalAlign::kLeft, "First, let me tell you how I feel."},
    {"item_tegami_01E.hg3", HorizontalAlign::kLeft, "I like you."},
    {"item_tegami_01F.hg3", HorizontalAlign::kLeft,
     "Wait, that probably doesn't make it clear, does it?"},
    {"item_tegami_01G.hg3", HorizontalAlign::kLeft, "I am \"in love\" with you."},
    {"item_tegami_01H.hg3", HorizontalAlign::kLeft,
     "Having you by my side used to feel only natural.\n"
     "But I don't want it to end at just that anymore."},
    {"item_tegami_01I.hg3", HorizontalAlign::kLeft,
     "I finally realized these overflowing feelings of mine."},
    {"item_tegami_01J.hg3", HorizontalAlign::kLeft,
     "I want to close the distance between us, even just a little."},
    {"item_tegami_01K.hg3", HorizontalAlign::kLeft,
     "You are my precious younger brother.\n"
     "Yet now, you are someone I dearly, deeply love."},
    {"item_tegami_01L.hg3", HorizontalAlign::kLeft,
     "If you happen to feel the same way I do,\n"
     "please become my boyfriend."},
    {"item_tegami_01M.hg3", HorizontalAlign::kLeft,
     "I want the two of us to walk through life together.\n"
     "There is no rush for your answer. I'll wait for you."},
    {"item_tegami_01N.hg3", HorizontalAlign::kLeft,
     "Colder days are ahead of us, so please take good care of yourself."},
    {"item_tegami_01O.hg3", HorizontalAlign::kRight, "Yours,"},

    {"item_tegami_02A.hg3", HorizontalAlign::kLeft, "Dear,"},
    {"item_tegami_02B.hg3", HorizontalAlign::kLeft,
     "The season when we long for warm sunshine has arrived."},
    {"item_tegami_02C.hg3", HorizontalAlign::kLeft,
     "You told me... you had something to talk to me about.\n"
     "It might be different from what I'm imagining.\n"
     "Still, hoping \"what if...\" I picked up my pen."},
    {"item_tegami_02D.hg3", HorizontalAlign::kLeft,
     "Because I felt a letter would make it easier\n"
     "to convey my feelings than spoken words."},
    {"item_tegami_02E.hg3", HorizontalAlign::kLeft, "I like you."},
    {"item_tegami_02F.hg3", HorizontalAlign::kLeft,
     "Wait, simply saying \"like\" doesn't make it clear, does it?"},
    {"item_tegami_02G.hg3", HorizontalAlign::kLeft, "I am \"in love\" with you."},
    {"item_tegami_02H.hg3", HorizontalAlign::kLeft,
     "Having you by my side used to feel only natural.\n"
     "I thought of you as my precious younger brother.\n"
     "But I don't want it to end at just that anymore."},
    {"item_tegami_02I.hg3", HorizontalAlign::kLeft,
     "I see you as a man.\n"
     "I finally realized these overflowing feelings of mine."},
    {"item_tegami_02J.hg3", HorizontalAlign::kLeft,
     "That's why I want to close the distance between us.\n"
     "I want to be your girlfriend."},
    {"item_tegami_02K.hg3", HorizontalAlign::kLeft,
     "If you feel the same way I do, please become my boyfriend."},
    {"item_tegami_02L.hg3", HorizontalAlign::kLeft,
     "I want the two of us to keep walking forward together,\n"
     "in a new relationship as lovers."},
    {"item_tegami_02M.hg3", HorizontalAlign::kLeft,
     "If you confess to me... I know I'll be too happy\n"
     "to say a word, so I'm writing my answer here."},
    {"item_tegami_02N.hg3", HorizontalAlign::kLeft, "I hope I can give this to you."},
    {"item_tegami_02O.hg3", HorizontalAlign::kLeft,
     "Colder days are ahead of us, so please take good care of yourself."},
    {"item_tegami_02P.hg3", HorizontalAlign::kRight, "Yours,"},

    {"item_tegami_03A.hg3", HorizontalAlign::kLeft,
     "Thank you for carrying me piggyback all the way home.\n"
     "My heart was racing on the way back, but I was so happy."},
    {"item_tegami_03B.hg3", HorizontalAlign::kLeft,
     "It really hit me how much I love holding each other.\n"
     "My feelings are different from back then... but I still love you so much."},
    {"item_tegami_03C.hg3", HorizontalAlign::kLeft,
     "I made a bookmark from the autumn leaves I picked up at Kurose Onsen.\n"
     "Please use it if you'd like."},
    {"item_tegami_03D.hg3", HorizontalAlign::kLeft,
     "Not out of the blue, of course, but I'd be so happy if we could\n"
     "hold each other like that again and share that warm feeling."},

    {"item_tegami_04A.hg3", HorizontalAlign::kLeft,
     "I'm sorry that I always end up using letters\n"
     "to convey my feelings."},
    {"item_tegami_04B.hg3", HorizontalAlign::kLeft,
     "But I don't have the courage to put my feelings\n"
     "into words face-to-face... so I'm writing them down."},
    {"item_tegami_04C.hg3", HorizontalAlign::kLeft,
     "When I went to wake you up this morning,\n"
     "watching your sleeping face made me almost kiss you."},
    {"item_tegami_04D.hg3", HorizontalAlign::kLeft,
     "I didn't actually do it. Still, I really wanted to kiss you."},
    {"item_tegami_04E.hg3", HorizontalAlign::kLeft,
     "Because more than a younger brother, you are my boyfriend.\n"
     "As your girlfriend, I wanted to touch you more."},
    {"item_tegami_04F.hg3", HorizontalAlign::kLeft,
     "I hope we can take another step forward in our relationship\n"
     "and walk toward the future together."},
    {"item_tegami_04G.hg3", HorizontalAlign::kLeft,
     "Even so, I know writing something like this\n"
     "will probably leave you flustered."},
    {"item_tegami_04H.hg3", HorizontalAlign::kLeft,
     "I'm sorry for being such an immodest girlfriend."},

    {"item_tegami_05A.hg3", HorizontalAlign::kLeft,
     "It's been a while... though with you always right beside me,\n"
     "Chitose, saying that feels a little strange."},
    {"item_tegami_05B.hg3", HorizontalAlign::kLeft,
     "A long time has passed, but I still remember everything I told you back then."},
    {"item_tegami_05C.hg3", HorizontalAlign::kLeft,
     "Then and now, Chitose, you are the only woman for me. I love you."},
    {"item_tegami_05D.hg3", HorizontalAlign::kLeft,
     "Even after decades have passed, you haven't changed."},
    {"item_tegami_05E.hg3", HorizontalAlign::kLeft,
     "No matter how many years go by, your charm never fades."},
    {"item_tegami_05F.hg3", HorizontalAlign::kLeft,
     "I only wish the me from back then had realized that sooner."},
    {"item_tegami_05G.hg3", HorizontalAlign::kLeft,
     "To my dearest Chitose, with unchanging love for all my life."},

    {"item_tegami_06A.hg3", HorizontalAlign::kLeft,
     "The cherry blossom buds have begun to open, and today I graduate."},
    {"item_tegami_06B.hg3", HorizontalAlign::kLeft,
     "To the future me - how do you look back\n"
     "on this day, I wonder?"},
    {"item_tegami_06C.hg3", HorizontalAlign::kLeft,
     "I'm sure your heart is fluttering at a brand-new beginning.\n"
     "...Just like mine is right now."},
    {"item_tegami_06D.hg3", HorizontalAlign::kLeft,
     "What the future holds... of course I don't know yet.\n"
     "Still, I'm sure things will head in a wonderful direction."},
    {"item_tegami_06E.hg3", HorizontalAlign::kLeft,
     "I truly believe they will - though of course,\n"
     "you already know that by now, don't you?"},
    {"item_tegami_06F.hg3", HorizontalAlign::kLeft,
     "Because I have him right by my side!"},
}};

struct StationeryLineSpec {
    int band_y;
    HorizontalAlign align;
    std::string_view text;
};

// Shared geometry for 890x720 floral stationery letters (03lA, 04lA, 06lA)
constexpr int kFloralStationeryBandX = 104;
constexpr int kFloralStationeryBandW = 680;
constexpr int kFloralStationeryBandH = 29;
constexpr int kFloralStationeryTextX = 116;
constexpr int kFloralStationeryTextW = 656;
constexpr int kFloralStationerySignX = 560;
constexpr int kFloralStationerySignW = 216;
constexpr int kFloralCleanDonorY_03_06 = 560;
constexpr int kFloralCleanDonorY_04 = 98;

constexpr std::array<StationeryLineSpec, 11> kLetter03lALines = {{
    {32, HorizontalAlign::kLeft,
     "Thank you for carrying me piggyback all the way home."},
    {65, HorizontalAlign::kLeft,
     "My heart was racing on the way back, but I was so happy."},
    {131, HorizontalAlign::kLeft,
     "It really hit me how much I love holding each other."},
    {164, HorizontalAlign::kLeft,
     "My feelings are different from back then... but I still love you so much."},
    {230, HorizontalAlign::kLeft,
     "I made a bookmark from the autumn leaves I picked up at Kurose Onsen."},
    {263, HorizontalAlign::kLeft, "Please use it if you'd like."},
    {329, HorizontalAlign::kLeft,
     "Not out of the blue, of course, but I'd be so happy if we could"},
    {362, HorizontalAlign::kLeft,
     "hold each other like that again and share that warm feeling."},
    {428, HorizontalAlign::kLeft,
     "It's gotten chilly, so bundle up warm for bed."},
    {461, HorizontalAlign::kLeft, "Good night."},
    {527, HorizontalAlign::kRight, "From Chitose."},
}};

constexpr std::array<StationeryLineSpec, 14> kLetter04lALines = {{
    {32, HorizontalAlign::kLeft,
     "I'm sorry that I always end up using letters"},
    {65, HorizontalAlign::kLeft, "to convey my feelings."},
    {131, HorizontalAlign::kLeft,
     "But I don't have the courage to put my feelings"},
    {164, HorizontalAlign::kLeft,
     "into words face-to-face... so I'm writing them down."},
    {230, HorizontalAlign::kLeft,
     "When I went to wake you up this morning,"},
    {263, HorizontalAlign::kLeft,
     "watching your sleeping face made me almost kiss you."},
    {296, HorizontalAlign::kLeft,
     "I didn't actually do it. Still, I really wanted to kiss you."},
    {362, HorizontalAlign::kLeft,
     "Because more than a younger brother, you are my boyfriend."},
    {395, HorizontalAlign::kLeft,
     "As your girlfriend, I wanted to touch you more."},
    {461, HorizontalAlign::kLeft,
     "I hope we can take another step forward in our relationship"},
    {494, HorizontalAlign::kLeft, "and walk toward the future together."},
    {560, HorizontalAlign::kLeft,
     "Even so, I know writing something like this"},
    {593, HorizontalAlign::kLeft, "will probably leave you flustered."},
    {659, HorizontalAlign::kLeft,
     "I'm sorry for being such an immodest girlfriend."},
}};

constexpr std::array<StationeryLineSpec, 10> kLetter06lALines = {{
    {32, HorizontalAlign::kLeft,
     "The cherry blossom buds have begun to open, and today I graduate."},
    {98, HorizontalAlign::kLeft,
     "To the future me - how do you look back"},
    {131, HorizontalAlign::kLeft, "on this day, I wonder?"},
    {197, HorizontalAlign::kLeft,
     "I'm sure your heart is fluttering at a brand-new beginning."},
    {230, HorizontalAlign::kLeft, "...Just like mine is right now."},
    {296, HorizontalAlign::kLeft,
     "What the future holds... of course I don't know yet."},
    {329, HorizontalAlign::kLeft,
     "Still, I'm sure things will head in a wonderful direction."},
    {395, HorizontalAlign::kLeft,
     "I truly believe they will - though of course,"},
    {428, HorizontalAlign::kLeft,
     "you already know that by now, don't you?"},
    {494, HorizontalAlign::kLeft, "Because I have him right by my side!"},
}};

// High-res A4 parchment letter (05lA: 2480x3508)
constexpr RectI kA4StationeryDonorRect{280, 1564, 1920, 1012};
constexpr int kA4StationeryTargetX = 280;
constexpr int kA4StationeryTargetY = 552;
constexpr int kA4StationeryTextX = 305;
constexpr int kA4StationeryTextW = 1870;
constexpr int kA4StationeryBandH = 76;

constexpr std::array<StationeryLineSpec, 7> kLetter05lALines = {{
    {568, HorizontalAlign::kLeft,
     "It's been a while... though with you always right beside me, Chitose, saying that feels strange."},
    {660, HorizontalAlign::kLeft,
     "A long time has passed, but I still remember everything I told you back then."},
    {844, HorizontalAlign::kLeft,
     "Then and now, Chitose, you are the only woman for me. I love you."},
    {1028, HorizontalAlign::kLeft,
     "Even after decades have passed, you haven't changed."},
    {1120, HorizontalAlign::kLeft,
     "No matter how many years go by, your charm never fades."},
    {1212, HorizontalAlign::kLeft,
     "I only wish the me from back then had realized that sooner."},
    {1396, HorizontalAlign::kLeft,
     "To my dearest Chitose, with unchanging love for all my life."},
}};

constexpr std::array<std::string_view, 2> kChatPureArtFiles = {{
    "item_chat_02K2.hg3",
    "item_chat_bg.hg3",
}};

constexpr std::array<std::string_view, 2> kNotePureArtFiles = {{
    "item_note_bg.hg3",
    "item_note_bg_all.hg3",
}};

constexpr std::array<std::string_view, 4> kTegamiPureArtFiles = {{
    "item_tegami_bg.hg3",
    "item_tegami_line.hg3",
    "item_tegami_mask.hg3",
    "item_tegami_mask2.hg3",
}};

TextStyle MakeChatBubbleStyle(float size_px, HorizontalAlign halign, bool wrap) {
    TextStyle style;
    style.font_family = L"Segoe UI";
    style.weight = kRegularWeight;
    style.size_px = size_px;
    style.line_spacing = kTightLineSpacing;
    style.fill = kChatBubbleInk;
    style.halign = halign;
    style.valign = VerticalAlign::kCenter;
    style.wrap = wrap;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;
    return style;
}

TextStyle MakeChatReadStyle() {
    TextStyle style;
    style.font_family = L"Segoe UI";
    style.weight = kRegularWeight;
    style.size_px = kChatReadFontSizePx;
    style.fill = kChatReadLabelInk;
    style.halign = HorizontalAlign::kCenter;
    style.valign = VerticalAlign::kCenter;
    style.wrap = false;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;
    return style;
}

TextStyle MakeHandwrittenStyle(float size_px, BgraColor color, HorizontalAlign halign) {
    TextStyle style;
    style.font_family = L"Segoe Print";
    style.weight = kSemiBoldWeight;
    style.size_px = size_px;
    style.line_spacing = kTightLineSpacing;
    style.fill = color;
    style.halign = halign;
    style.valign = VerticalAlign::kCenter;
    style.wrap = false;
    style.min_squeeze = kDefaultMinSqueeze;
    style.min_scale = kDefaultMinScale;
    return style;
}

void ApplyCanvasToFirstFrame(ama::hg3::Hg3Container& hg3, const SpriteCanvas& canvas) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("HG-3 container has zero frames");
    }
    hg3.frames[kFirstFrameIndex].pixels = canvas.image();
}

void AppendGroupCell(std::vector<imaging::ContactSheetCell>& cells,
                     std::string_view entry_name,
                     const ama::hg3::Hg3Container& hg3) {
    if (hg3.frames.empty()) {
        return;
    }
    const auto& f = hg3.frames[kFirstFrameIndex];
    if (f.pixels.pixels.empty()) {
        return;
    }
    imaging::ContactSheetCell cell;
    cell.image = f.pixels;
    cell.label = std::string(entry_name) + " " + std::to_string(f.stdinfo.width) + "x" +
                 std::to_string(f.stdinfo.height);
    cells.push_back(std::move(cell));
}

void SaveGroupContactSheet(const std::vector<imaging::ContactSheetCell>& cells,
                           const DomainRenderOptions& options,
                           std::string_view stem) {
    if (!options.export_contact_sheets || options.contact_sheet_dir.empty() || cells.empty()) {
        return;
    }
    imaging::ContactSheetLayout layout;
    layout.columns = kGroupSheetColumns;
    layout.cells_per_page = kGroupSheetCellsPerPage;
    layout.max_cell_px = kGroupSheetMaxCellPx;
    SaveContactSheetGrid(cells, options.contact_sheet_dir, std::string(stem), layout);
}

void RenderChatBubbleContainer(ama::hg3::Hg3Container& hg3, const ChatBubbleSpec& spec) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("chat bubble container has zero frames");
    }
    SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);
    const int w = canvas.width();
    const int h = canvas.height();

    if (spec.kind == ChatBubbleKind::kOutgoingSingle) {
        const RectI read_erase{kChatReadEraseX, kChatReadEraseY, kChatReadEraseW,
                               std::max(1, h - kChatReadEraseY - kChatReadEraseBottomMargin)};
        canvas.Fill(read_erase, kTransparentColor);
        canvas.DrawText("Read",
                        RectI{kChatReadDrawX, kChatReadDrawY, kChatReadDrawW, kChatReadDrawH},
                        MakeChatReadStyle());

        const BgraColor outgoing_fill =
            (w > kOutgoingSampleX && h > kOutgoingSampleY)
                ? imaging::GetPixel(canvas.image(), kOutgoingSampleX, kOutgoingSampleY)
                : kOutgoingBubbleFill;
        const RectI bubble_fill{
            kOutgoingFillX, kOutgoingFillY,
            std::max(1, w - kOutgoingFillX - kOutgoingFillRightMargin),
            std::max(1, h - kOutgoingFillY - kOutgoingFillBottomMargin)};
        canvas.Fill(bubble_fill, outgoing_fill);

        const RectI text_rect{
            kOutgoingTextX, kOutgoingTextY,
            std::max(1, w - kOutgoingTextX - kOutgoingTextRightMargin),
            std::max(1, h - kOutgoingTextY - kOutgoingTextBottomMargin)};
        canvas.DrawText(
            std::string(spec.english_text), text_rect,
            MakeChatBubbleStyle(kChatSingleLineFontSizePx, HorizontalAlign::kCenter, false));
        ApplyCanvasToFirstFrame(hg3, canvas);
        return;
    }

    const RectI bubble_fill{
        kIncomingFillX, kIncomingFillY,
        std::max(1, w - kIncomingFillX - kIncomingFillRightMargin),
        std::max(1, h - kIncomingFillY - kIncomingFillBottomMargin)};
    canvas.Fill(bubble_fill, kIncomingBubbleFill);

    const RectI text_rect{
        kIncomingTextX, kIncomingTextY,
        std::max(1, w - kIncomingTextX - kIncomingTextRightMargin),
        std::max(1, h - kIncomingTextY - kIncomingTextBottomMargin)};
    const bool is_multi = (spec.kind == ChatBubbleKind::kIncomingMultiLine);
    const float font_px = is_multi ? kChatMultiLineFontSizePx : kChatSingleLineFontSizePx;
    const HorizontalAlign align =
        is_multi ? HorizontalAlign::kLeft : HorizontalAlign::kCenter;
    canvas.DrawText(std::string(spec.english_text), text_rect,
                    MakeChatBubbleStyle(font_px, align, false));
    ApplyCanvasToFirstFrame(hg3, canvas);
}

void RenderChatFrameContainer(ama::hg3::Hg3Container& hg3) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("item_chat_flame.hg3 has zero frames");
    }
    SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);
    canvas.Fill(kChatSendButtonEraseRect, kChatSendBtnFill);

    TextStyle send_style;
    send_style.font_family = L"Segoe UI";
    send_style.weight = kSemiBoldWeight;
    send_style.size_px = kChatSendFontSizePx;
    send_style.fill = kChatSendBtnInk;
    send_style.halign = HorizontalAlign::kCenter;
    send_style.valign = VerticalAlign::kCenter;
    send_style.min_squeeze = kDefaultMinSqueeze;
    send_style.min_scale = kDefaultMinScale;

    canvas.DrawText("Send", kChatSendButtonTextRect, send_style);
    ApplyCanvasToFirstFrame(hg3, canvas);
}

void RenderTransparentStrip(ama::hg3::Hg3Container& hg3, std::string_view english_text,
                            float font_size_px, BgraColor ink_color, HorizontalAlign align) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("strip HG-3 container has zero frames");
    }
    SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);
    const RectI full_rect{0, 0, canvas.width(), canvas.height()};
    canvas.Fill(full_rect, kTransparentColor);
    canvas.DrawText(std::string(english_text), full_rect,
                    MakeHandwrittenStyle(font_size_px, ink_color, align));
    ApplyCanvasToFirstFrame(hg3, canvas);
}

void RenderTegami03EStrip(ama::hg3::Hg3Container& hg3) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("item_tegami_03E.hg3 has zero frames");
    }
    SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);
    const int w = canvas.width();
    const int h = canvas.height();
    canvas.Fill(RectI{0, 0, w, h}, kTransparentColor);

    constexpr int kBodyHeight = 48;
    constexpr int kSignWidth = 180;
    constexpr int kSignHeight = 27;
    constexpr int kSignRightMargin = 4;
    const RectI body_rect{0, 0, w, std::min(kBodyHeight, h)};
    const RectI sign_rect{std::max(0, w - kSignWidth - kSignRightMargin),
                          std::max(0, h - kSignHeight),
                          std::min(kSignWidth, w), std::min(kSignHeight, h)};

    canvas.DrawText("It's gotten chilly, so bundle up warm for bed.\nGood night.", body_rect,
                    MakeHandwrittenStyle(kLetterStripFontSizePx, kLetterInk,
                                         HorizontalAlign::kLeft));
    canvas.DrawText("From Chitose.", sign_rect,
                    MakeHandwrittenStyle(kLetterStripFontSizePx, kLetterInk,
                                         HorizontalAlign::kRight));
    ApplyCanvasToFirstFrame(hg3, canvas);
}

template <std::size_t LineCount>
void RenderFloralStationeryLetter(ama::hg3::Hg3Container& hg3, int clean_donor_y,
                                  const std::array<StationeryLineSpec, LineCount>& lines) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("floral stationery HG-3 container has zero frames");
    }
    const BgraImage donor_snapshot = hg3.frames[kFirstFrameIndex].pixels;
    SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);

    const RectI donor_rect{kFloralStationeryBandX, clean_donor_y,
                           kFloralStationeryBandW, kFloralStationeryBandH};

    for (const auto& line : lines) {
        canvas.BlitCleanRegion(donor_snapshot, donor_rect, kFloralStationeryBandX,
                               line.band_y);
        const RectI text_rect =
            (line.align == HorizontalAlign::kRight)
                ? RectI{kFloralStationerySignX, line.band_y,
                        kFloralStationerySignW, kFloralStationeryBandH}
                : RectI{kFloralStationeryTextX, line.band_y,
                        kFloralStationeryTextW, kFloralStationeryBandH};
        canvas.DrawText(std::string(line.text), text_rect,
                        MakeHandwrittenStyle(kStationery890FontSizePx, kLetterInk,
                                             line.align));
    }

    ApplyCanvasToFirstFrame(hg3, canvas);
}

void RenderHighResLetter05lA(ama::hg3::Hg3Container& hg3) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("item_tegami_05lA.hg3 has zero frames");
    }
    const BgraImage donor_snapshot = hg3.frames[kFirstFrameIndex].pixels;
    SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);

    canvas.BlitCleanRegion(donor_snapshot, kA4StationeryDonorRect,
                           kA4StationeryTargetX, kA4StationeryTargetY);

    for (const auto& line : kLetter05lALines) {
        const RectI text_rect{kA4StationeryTextX, line.band_y,
                              kA4StationeryTextW, kA4StationeryBandH};
        canvas.DrawText(std::string(line.text), text_rect,
                        MakeHandwrittenStyle(kStationeryA4FontSizePx, kLetterInk,
                                             line.align));
    }

    ApplyCanvasToFirstFrame(hg3, canvas);
}

void RenderRooftopNoteR01A(ama::hg3::Hg3Container& hg3) {
    if (hg3.frames.empty()) {
        throw std::runtime_error("item_tegami_R01A.hg3 has zero frames");
    }
    const BgraImage donor_snapshot = hg3.frames[kFirstFrameIndex].pixels;
    SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);

    constexpr RectI kRow3CleanDonor{550, 462, 1100, 78};
    constexpr int kRow4TargetY = 554;
    constexpr int kRow5TargetY = 646;

    constexpr RectI kRow7SignCleanDonor{1760, 830, 140, 78};
    constexpr int kRow6SignTargetX = 1760;
    constexpr int kRow6SignTargetY = 738;

    canvas.BlitCleanRegion(donor_snapshot, kRow3CleanDonor, kRow3CleanDonor.x, kRow4TargetY);
    canvas.BlitCleanRegion(donor_snapshot, kRow3CleanDonor, kRow3CleanDonor.x, kRow5TargetY);
    canvas.BlitCleanRegion(donor_snapshot, kRow7SignCleanDonor, kRow6SignTargetX,
                           kRow6SignTargetY);

    constexpr RectI kRow4TextRect{560, 554, 1320, 78};
    constexpr RectI kRow5TextRect{560, 646, 700, 78};
    constexpr RectI kRow6SignRect{1680, 738, 210, 78};

    canvas.DrawText(
        "I have something to tell you. Please come to the rooftop after school.",
        kRow4TextRect,
        MakeHandwrittenStyle(kStationeryRooftopFontSizePx, kLetterInk, HorizontalAlign::kLeft));
    canvas.DrawText(
        "I'll be waiting.", kRow5TextRect,
        MakeHandwrittenStyle(kStationeryRooftopFontSizePx, kLetterInk, HorizontalAlign::kLeft));
    canvas.DrawText(
        "Rei", kRow6SignRect,
        MakeHandwrittenStyle(kStationeryRooftopFontSizePx, kLetterInk, HorizontalAlign::kRight));

    ApplyCanvasToFirstFrame(hg3, canvas);
}

void EmitPatchedHg3(const ExtractedAssetEntry& src_entry, const ama::hg3::Hg3Container& hg3,
                    const DomainRenderOptions& options,
                    std::vector<MenuPatchEntrySpec>& out_records,
                    std::vector<imaging::ContactSheetCell>& group_cells) {
    if (options.export_contact_sheets && !options.contact_sheet_dir.empty()) {
        ExportHg3ContactSheetPages(src_entry.entry_name, hg3, options.contact_sheet_dir);
        AppendGroupCell(group_cells, src_entry.entry_name, hg3);
    }
    out_records.push_back(GameAssetSource::BuildHg3PatchEntry(src_entry, hg3));
}

class StoryItemsDomainRenderer final : public IMenuDomainRenderer {
public:
    std::string DomainName() const override { return "story_items"; }

    std::vector<MenuPatchEntrySpec> RenderDomain(
        const GameAssetSource& source, const DomainRenderOptions& options) override {
        std::vector<MenuPatchEntrySpec> records;
        records.reserve(kExpectedStoryItemPatchCount);

        std::vector<imaging::ContactSheetCell> chat_cells;
        std::vector<imaging::ContactSheetCell> note_cells;
        std::vector<imaging::ContactSheetCell> tegami_cells;

        // 1. Smartphone chat overlays (29 translated + 2 pure-art audited)
        for (const auto& spec : kChatBubbleSpecs) {
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 = source.LoadHg3(std::string(spec.filename), &src_entry);
            RenderChatBubbleContainer(hg3, spec);
            EmitPatchedHg3(src_entry, hg3, options, records, chat_cells);
        }
        {
            constexpr std::string_view kChatFrameName = "item_chat_flame.hg3";
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(kChatFrameName), &src_entry);
            RenderChatFrameContainer(hg3);
            EmitPatchedHg3(src_entry, hg3, options, records, chat_cells);
        }
        if (options.export_contact_sheets && !options.contact_sheet_dir.empty()) {
            for (const std::string_view keep_name : kChatPureArtFiles) {
                AppendGroupCell(chat_cells, keep_name, source.LoadHg3(std::string(keep_name)));
            }
            SaveGroupContactSheet(chat_cells, options, "story_items_chat");
        }

        // 2. Written notebook strips (26 individual strips + 3 composite scroll planes + 2 pure-art)
        std::unordered_map<std::string, ama::hg3::Hg3Frame> rendered_note_strips;
        rendered_note_strips.reserve(kNoteStripSpecs.size());

        for (const auto& spec : kNoteStripSpecs) {
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 = source.LoadHg3(std::string(spec.filename), &src_entry);
            RenderTransparentStrip(hg3, spec.english_text, kNoteFontSizePx, kNotebookInk,
                                   HorizontalAlign::kLeft);
            rendered_note_strips.emplace(std::string(spec.filename),
                                         hg3.frames[kFirstFrameIndex]);
            EmitPatchedHg3(src_entry, hg3, options, records, note_cells);
        }

        for (const auto& comp_spec : kNoteCompositeSpecs) {
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(comp_spec.filename), &src_entry);
            if (hg3.frames.empty()) {
                throw std::runtime_error("composite note HG-3 has zero frames");
            }
            const ama::hg3::Hg3StdInfo& parent_info = hg3.frames[kFirstFrameIndex].stdinfo;
            SpriteCanvas canvas(hg3.frames[kFirstFrameIndex].pixels);
            canvas.Fill(RectI{0, 0, canvas.width(), canvas.height()}, kTransparentColor);

            for (const std::string_view child_name : comp_spec.children) {
                const auto it = rendered_note_strips.find(std::string(child_name));
                if (it == rendered_note_strips.end()) {
                    throw std::runtime_error("missing rendered child note strip: " +
                                             std::string(child_name));
                }
                const ama::hg3::Hg3Frame& child_frame = it->second;
                const int dst_x = child_frame.stdinfo.offset_x - parent_info.offset_x;
                const int dst_y = child_frame.stdinfo.offset_y - parent_info.offset_y;
                const RectI src_rect{0, 0, child_frame.pixels.width,
                                     child_frame.pixels.height};
                canvas.CompositeSubRect(child_frame.pixels, src_rect, dst_x, dst_y);
            }

            ApplyCanvasToFirstFrame(hg3, canvas);
            EmitPatchedHg3(src_entry, hg3, options, records, note_cells);
        }
        if (options.export_contact_sheets && !options.contact_sheet_dir.empty()) {
            for (const std::string_view keep_name : kNotePureArtFiles) {
                AppendGroupCell(note_cells, keep_name, source.LoadHg3(std::string(keep_name)));
            }
            SaveGroupContactSheet(note_cells, options, "story_items_note");
        }

        // 3. Handwritten heroine letters (57 transparent strips + 5 full-page letters + 4 pure-art)
        for (const auto& spec : kTegamiStripSpecs) {
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 = source.LoadHg3(std::string(spec.filename), &src_entry);
            RenderTransparentStrip(hg3, spec.english_text, kLetterStripFontSizePx, kLetterInk,
                                   spec.align);
            EmitPatchedHg3(src_entry, hg3, options, records, tegami_cells);
        }
        {
            constexpr std::string_view kTegami03EName = "item_tegami_03E.hg3";
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(kTegami03EName), &src_entry);
            RenderTegami03EStrip(hg3);
            EmitPatchedHg3(src_entry, hg3, options, records, tegami_cells);
        }
        {
            constexpr std::string_view kTegami03lAName = "item_tegami_03lA.hg3";
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(kTegami03lAName), &src_entry);
            RenderFloralStationeryLetter(hg3, kFloralCleanDonorY_03_06, kLetter03lALines);
            EmitPatchedHg3(src_entry, hg3, options, records, tegami_cells);
        }
        {
            constexpr std::string_view kTegami04lAName = "item_tegami_04lA.hg3";
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(kTegami04lAName), &src_entry);
            RenderFloralStationeryLetter(hg3, kFloralCleanDonorY_04, kLetter04lALines);
            EmitPatchedHg3(src_entry, hg3, options, records, tegami_cells);
        }
        {
            constexpr std::string_view kTegami05lAName = "item_tegami_05lA.hg3";
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(kTegami05lAName), &src_entry);
            RenderHighResLetter05lA(hg3);
            EmitPatchedHg3(src_entry, hg3, options, records, tegami_cells);
        }
        {
            constexpr std::string_view kTegami06lAName = "item_tegami_06lA.hg3";
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(kTegami06lAName), &src_entry);
            RenderFloralStationeryLetter(hg3, kFloralCleanDonorY_03_06, kLetter06lALines);
            EmitPatchedHg3(src_entry, hg3, options, records, tegami_cells);
        }
        {
            constexpr std::string_view kTegamiR01AName = "item_tegami_R01A.hg3";
            ExtractedAssetEntry src_entry;
            ama::hg3::Hg3Container hg3 =
                source.LoadHg3(std::string(kTegamiR01AName), &src_entry);
            RenderRooftopNoteR01A(hg3);
            EmitPatchedHg3(src_entry, hg3, options, records, tegami_cells);
        }
        if (options.export_contact_sheets && !options.contact_sheet_dir.empty()) {
            for (const std::string_view keep_name : kTegamiPureArtFiles) {
                AppendGroupCell(tegami_cells, keep_name, source.LoadHg3(std::string(keep_name)));
            }
            SaveGroupContactSheet(tegami_cells, options, "story_items_tegami");
        }

        return records;
    }
};

}  // namespace

std::unique_ptr<IMenuDomainRenderer> CreateStoryItemsDomainRenderer() {
    return std::make_unique<StoryItemsDomainRenderer>();
}

}  // namespace ama::menu

