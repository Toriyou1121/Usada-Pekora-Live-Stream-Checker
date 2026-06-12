/*
 * peko_chara.c - the living mascot: a visual-novel style Character
 * Presentation System built around ONE static sprite.
 *
 * Philosophy (like a commercial galgame): the character is CALM by default -
 * when nothing happens she only breathes. Motion is meaning: she moves when
 * she speaks, and she moves more when she feels something. Every effect is
 * time-based (GetTickCount + sine), eased toward its target each tick, and
 * pixel-snapped so the pixel art stays crisp.
 *
 *   layer 1  idle        breathing only (slow 3.4 s chest rise, ~1%)
 *   layer 2  speaking    small vertical bob + slight scale pulse while the
 *                        typewriter reveals text, plus an emphasis nudge
 *                        every ~2 s (like a VN sprite "stepping" on a beat)
 *   layer 3  emotion     a per-emotion recipe of AnimSteps (hop, shake,
 *                        jump-stretch, sink, sway, shrink...), a camera
 *                        zoom punch, an optional character-zone shake, and
 *                        an emotion icon popped above her head
 *   layer 4  focus       dialogue open = focused (zoom 1.03, full alpha);
 *                        closed = neutral (slightly dimmed, zoom 1.0)
 *
 * Usage in the game loop (peko_main.c):
 *   WM_TIMER (every 16 ms) -> chara_tick()  : UpdateAnimation + invalidates
 *   WM_PAINT compose       -> chara_draw()  : transformed sprite + icons
 *                             chara_dlg_draw(): the dialogue window
 *
 * The galgame dialogue itself is fully localized (EN / JA / ZH) and her
 * affection persists across sessions. Keys: T talk, 1-3 choose,
 * Space/Enter advance, Esc close.
 */
#include "peko.h"

HBITMAP g_hPekora; int g_pkW, g_pkH;

/* =====================================================================
 * VN CHARACTER PRESENTATION SYSTEM
 * ===================================================================== */

/* building-block motions; a recipe sequences up to 3 of them */
typedef enum {
    AN_NONE = 0,          /* still (breathing only)                       */
    AN_HOP,               /* one soft hop                  (timed)        */
    AN_DOUBLE_HOP,        /* excited double bounce         (timed)        */
    AN_SHAKE_X,           /* angry horizontal shake        (timed)        */
    AN_JUMP_STRETCH,      /* startled jump + stretch       (timed)        */
    AN_SHAKE_SCREEN,      /* character-zone camera shake   (timed)        */
    AN_SINK,              /* sad droop: sink + shrink      (persists)     */
    AN_SWAY,              /* pondering pendulum sway       (persists)     */
    AN_SHRINK_IN,         /* bashful shrink + tiny tremble (persists)     */
    AN_TURN,              /* pouty turn-away offset        (persists)     */
} AnimType;

typedef struct {
    AnimType type;
    DWORD    dur;         /* ms; 0 = persists while the emotion holds     */
    float    amp;         /* amplitude multiplier (1.0 = nominal)         */
} AnimStep;

#define MAX_STEPS 3

/* emotion icons popped above her head, each with its own little life */
typedef enum {
    ICON_NONE = 0, ICON_HEART, ICON_SWEAT, ICON_ANGER,
    ICON_QUESTION, ICON_EXCLAIM, ICON_NOTE
} IconType;

typedef struct {          /* one emotion's full presentation recipe       */
    AnimStep steps[MAX_STEPS];   /* played in order; AN_NONE/dur=0 ends   */
    int      icon;               /* ICON_* popped when the emotion fires  */
    float    zoom;               /* camera punch-in factor (decays ~0.5s) */
} EmotionAnim;

typedef struct {          /* live state, smoothed toward targets per tick */
    int    emotion;       /* EXPR_*                                       */
    DWORD  emotionMs;     /* when it was set                              */
    int    stepIdx;       /* position in the recipe                       */
    DWORD  stepMs;
    float  x, y;          /* rendered offset (px)                         */
    float  scaleX, scaleY;
    float  zoom;          /* camera zoom, eased                           */
    float  alpha;         /* focus: 255 in-dialogue, ~234 idle            */
    int    talking;       /* typewriter currently revealing               */
} CharacterState;

/* emotion -> animation mapping (indexed by ExprId) */
static const EmotionAnim EMO[EXPR_COUNT] = {
/* SHY         */ { {{AN_SHRINK_IN,     0, 0.7f}},                    ICON_HEART,    1.00f },
/* SMILE       */ { {{AN_NONE,          0, 0   }},                    ICON_NONE,     1.00f },
/* HAPPY       */ { {{AN_HOP,         420, 1.0f}, {AN_NONE,0,0}},     ICON_HEART,    1.01f },
/* ANGRY       */ { {{AN_SHAKE_X,     620, 1.0f}, {AN_NONE,0,0}},     ICON_ANGER,    1.03f },
/* POUT        */ { {{AN_TURN,          0, 1.0f}},                    ICON_ANGER,    1.00f },
/* EMBARRASSED */ { {{AN_SHRINK_IN,     0, 1.0f}},                    ICON_SWEAT,    1.00f },
/* THINKING    */ { {{AN_SWAY,          0, 1.0f}},                    ICON_QUESTION, 1.00f },
/* SURPRISED   */ { {{AN_JUMP_STRETCH,300, 1.0f},
                     {AN_SHAKE_SCREEN, 360, 1.0f}, {AN_NONE,0,0}},    ICON_EXCLAIM,  1.06f },
/* EXCITED     */ { {{AN_DOUBLE_HOP,  760, 1.0f}, {AN_NONE,0,0}},     ICON_NOTE,     1.04f },
/* SAD         */ { {{AN_SINK,          0, 1.0f}},                    ICON_SWEAT,    0.985f },
};

static CharacterState g_cs = { EXPR_SMILE, 0, 0, 0,
                               0, 0, 1.0f, 1.0f, 1.0f, 234.0f, 0 };
static DWORD g_exprHold = 0;          /* revert to base after this tick   */

/* character-zone shake (applies to sprite AND dialogue box) */
static float g_shakeAmp = 0;
static int   g_shkX = 0, g_shkY = 0;

/* the one live emotion icon */
#define ICON_LIFE 1100
static int   g_icon = ICON_NONE;
static DWORD g_iconMs = 0;

/* dialogue runtime (needed by UpdateAnimation for the talking layer) */
#define DLG_CPS 32
static int   g_dlgNode = -1;
static DWORD g_dlgTextMs = 0;
static RECT  g_dlgRect;
static int   g_lastScene = -1;

static RECT  g_pekoDst;               /* where she was drawn (hit test)   */
static DWORD g_squashMs = 0;          /* click poke pulse                 */

/* UpdateAnimation(): compute this tick's target transform from the active
 * recipe step + breathing + speaking, then ease the live state toward it. */
static void UpdateAnimation(DWORD now)
{
    const EmotionAnim *ea = &EMO[g_cs.emotion];

    /* advance the step machine; trigger one-shot effects at step start */
    static int lastEmo = -1, lastStep = -1;
    const AnimStep *st = &ea->steps[g_cs.stepIdx];
    DWORD el = now - g_cs.stepMs;
    while (st->dur && el >= st->dur && g_cs.stepIdx < MAX_STEPS - 1) {
        g_cs.stepIdx++;
        g_cs.stepMs = now;
        st = &ea->steps[g_cs.stepIdx];
        el = 0;
    }
    if (lastEmo != g_cs.emotion || lastStep != g_cs.stepIdx) {
        lastEmo = g_cs.emotion; lastStep = g_cs.stepIdx;
        if (st->type == AN_SHAKE_SCREEN)
            g_shakeAmp = S(5) * st->amp;          /* kick the camera once */
    }

    float tx = 0, ty = 0, tsx = 1.0f, tsy = 1.0f;
    float k = st->amp;
    float p = st->dur ? (float)el / st->dur : 0;
    switch (st->type) {
    case AN_HOP:
        ty -= S(10) * k * sinf(p * 3.14159f);
        break;
    case AN_DOUBLE_HOP:
        ty -= S(11) * k * fabsf(sinf(p * 6.28318f));
        break;
    case AN_SHAKE_X:
        tx += S(3) * k * (1.0f - p) * (((el / 45) & 1) ? 1.0f : -1.0f);
        break;
    case AN_JUMP_STRETCH: {
        float e = sinf(p * 3.14159f);
        ty -= S(14) * k * e;
        tsy += 0.10f * k * e;
        tsx -= 0.06f * k * e;
        break;
    }
    case AN_SINK:                                  /* sad droop, slow sway */
        ty += S(4) * k;
        tsx *= 1.0f - 0.015f * k;
        tsy *= 1.0f - 0.020f * k;
        tx += S(2) * k * sinf(now / 1300.0f);
        break;
    case AN_SWAY:                                  /* pondering pendulum  */
        tx += S(3) * k * sinf(now / 700.0f);
        break;
    case AN_SHRINK_IN:                             /* bashful + tremble   */
        tsx *= 1.0f - 0.03f * k;
        tsy *= 1.0f - 0.03f * k;
        tx += S(3) * k;
        ty += 1.0f * k * sinf(now / 180.0f);
        break;
    case AN_TURN:                                  /* hmph */
        tx += S(5) * k;
        break;
    default:
        break;
    }

    /* layer 1: breathing, always on (slow, ~1% chest rise) */
    tsy *= 1.0f + 0.010f * sinf(now * 6.28318f / 3400.0f);

    /* layer 2: speaking - bob + scale pulse + a beat emphasis every ~2 s */
    if (g_cs.talking) {
        ty -= S(1) + S(1) * sinf(now * 6.28318f / 420.0f);
        tsy += 0.004f * sinf(now * 6.28318f / 560.0f);
        DWORD ph = (now - g_dlgTextMs) % 2100;
        if (ph < 240) ty -= S(3) * sinf(ph / 240.0f * 3.14159f);
    }

    /* layer 3/4: camera - emotion zoom punch decaying into the focus zoom */
    float focus = (g_dlgNode >= 0) ? 1.03f : 1.0f;
    float punch = 1.0f;
    DWORD eel = now - g_cs.emotionMs;
    if (EMO[g_cs.emotion].zoom != 1.0f && eel < 480)
        punch = 1.0f + (EMO[g_cs.emotion].zoom - 1.0f) *
                       (float)(1.0 - ease(eel / 480.0));
    float tzoom  = focus * punch;
    float talpha = (g_dlgNode >= 0) ? 255.0f : 234.0f;

    /* smooth transitions: ease the live state toward this tick's targets */
    g_cs.x      += (tx     - g_cs.x)      * 0.28f;
    g_cs.y      += (ty     - g_cs.y)      * 0.28f;
    g_cs.scaleX += (tsx    - g_cs.scaleX) * 0.25f;
    g_cs.scaleY += (tsy    - g_cs.scaleY) * 0.25f;
    g_cs.zoom   += (tzoom  - g_cs.zoom)   * 0.18f;
    g_cs.alpha  += (talpha - g_cs.alpha)  * 0.15f;

    /* decaying high-frequency camera shake */
    if (g_shakeAmp > 0.3f) {
        g_shkX = (int)(g_shakeAmp * sinf(now * 0.9f));
        g_shkY = (int)(g_shakeAmp * 0.6f * sinf(now * 1.3f + 1.7f));
        g_shakeAmp *= 0.94f;
    } else {
        g_shakeAmp = 0; g_shkX = g_shkY = 0;
    }
}

/* ---------- phrase pools (bubble flavor text) ---------- */
static const wchar_t *PH_BASE[] = {
    L"こんぺこ〜！", L"ぺこ ❤", L"Yo! Yo! Yo!", L"HA↑HA↑HA↑",
    L"野うさぎ集合〜！", L"ぺこらだぴょん", L"おつぺこ〜！",
    L"がんばルビー！", L"だいじょうV！", L"ぺこ？"
};
static const wchar_t *PH_MORNING[] = {
    L"おはぺこ〜！", L"朝から偉いぺこ！", L"今日も一日がんばるぺこ！"
};
static const wchar_t *PH_NIGHT[] = {
    L"こんばんぺこ〜！", L"夜更かしは程々にぺこよ？", L"おやすみぺこ…💤"
};
static const wchar_t *PH_LIVE[] = {
    L"配信中ぺこ〜！見に来て！", L"今ライブやってるぺこ！",
    L"野うさぎ、集合ぺこ！🥕"
};
static const wchar_t *PH_CLICK[] = {
    L"ぺこっ！？", L"わっ、びっくりした！", L"えへへ〜 ❤",
    L"くすぐったいぺこ！", L"なになに？", L"もっと撫でてもいいぺこよ？",
    L"ぺこぺこ〜♪", L"甘えん坊め！"
};
static const wchar_t *PH_PET[] = {
    L"ふにゃ〜…", L"気持ちいいぺこ…", L"えへへ ❤", L"そこそこ〜"
};
static const wchar_t *PH_SING[] = {
    L"♪〜", L"ふんふんふ〜ん♪", L"いい曲ぺこね〜♪"
};
#define N(a) ((int)(sizeof(a)/sizeof((a)[0])))

/* ---------- omikuji ---------- */
typedef struct { const wchar_t *txt; int expr; } Fortune;
static const Fortune FORTUNES[] = {
    { L"大吉ぺこ〜！🥕 最高の一日!",      EXPR_EXCITED },
    { L"中吉ぺこ！いいことあるよ",        EXPR_SMILE },
    { L"小吉ぺこ。まあまあぺこね",        EXPR_SMILE },
    { L"吉ぺこ！平和が一番",              EXPR_HAPPY },
    { L"末吉ぺこ…焦らずいこう",           EXPR_SAD },
    { L"ぺこ吉！？レア運勢ぺこ！",        EXPR_SURPRISED },
    { L"カーロット吉！にんじん食べよ",    EXPR_HAPPY }
};
#define FORTUNE_COUNT N(FORTUNES)

/* ---------- galgame dialogue data, localized EN / JA / ZH ----------
 * Flat node table; a scene is an entry node. Choices carry an affection
 * delta and jump to a reaction node whose emotion is her reaction.
 * Text/label index = LangId (LANG_EN, LANG_JA, LANG_ZH). */
typedef struct { const wchar_t *label[LANG_COUNT]; int next; int dAff; } DlgChoice;
typedef struct { const wchar_t *text[LANG_COUNT]; int expr; int nch; DlgChoice ch[3]; } DlgNode;

static const DlgNode DLG[] = {
/* 0 -- present scene */
{ { L"Hmm? You look like you want to give Pekora a present, peko?",
    L"ねえねえ、ぺこらに何かプレゼントしたくなった顔してるぺこね？",
    L"嗯？你看起來想送佩克拉禮物的樣子 peko？" }, EXPR_THINKING, 3, {
  { { L"Here, a carrot 🥕", L"にんじんあげる🥕", L"送你紅蘿蔔🥕" },          1,  4 },
  { { L"Just my feelings...", L"気持ちだけ…", L"只有心意…" },                 2,  0 },
  { { L"You're the cutest ever", L"ぺこらが世界一かわいい",
      L"佩克拉是世界第一可愛" },                                             3,  6 } } },
{ { L"As expected of my nousagi! I'll enjoy it, peko~♪",
    L"さっすが野うさぎ！おいしくいただくぺこ〜♪",
    L"不愧是野兔！我就不客氣收下啦 peko〜♪" }, EXPR_HAPPY, 0, {{{0}}} },
{ { L"Feelings... huh. Fine, I'll accept them, peko.",
    L"気持ち…ね。まあ、受け取っておいてあげるぺこ",
    L"心意…啊。好吧，勉強收下 peko" }, EXPR_POUT, 0, {{{0}}} },
{ { L"W-What's that all of a sudden!? ...Thanks, peko... ❤",
    L"な、なに急に！？……ありがとぺこ…❤",
    L"什、什麼啦突然！？……謝謝 peko…❤" }, EXPR_EMBARRASSED, 0, {{{0}}} },
/* 4 -- tonight's game */
{ { L"What should tonight's game be?",
    L"今夜のゲーム、何がいいと思う？",
    L"今晚要玩什麼遊戲好呢？" }, EXPR_THINKING, 3, {
  { { L"A horror game", L"ホラーゲーム", L"恐怖遊戲" },                       5,  2 },
  { { L"Mario Kart, let's go!", L"マリカで勝負！", L"瑪利歐賽車對決！" },     6,  3 },
  { { L"You should sleep early", L"寝た方がいいよ", L"早點睡比較好喔" },      7, -2 } } },
{ { L"H-Horror!? ...I-It's not scary at all! Watch it by yourself!",
    L"ホ、ホラー！？…べ、別に怖くないし！ひとりで見ててよね！",
    L"恐、恐怖遊戲！？…才、才不可怕呢！你自己看啦！" }, EXPR_SURPRISED, 0, {{{0}}} },
{ { L"Nice, peko~! Loser owes a year of carrots!",
    L"いいぺこね〜！負けたらにんじん1年分だからね！",
    L"好欸 peko〜！輸的人要付一年份紅蘿蔔！" }, EXPR_EXCITED, 0, {{{0}}} },
{ { L"Huh!? Pekora isn't sleepy at all! ...fwa...",
    L"は！？ぺこらはまだ眠くないし！……ふぁ…",
    L"哈！？佩克拉才不睏呢！……呼啊…" }, EXPR_ANGRY, 0, {{{0}}} },
/* 8 -- what do you think of me */
{ { L"S-So... what do you think of Pekora...?",
    L"と、ところで…ぺこらのこと、どう思ってるぺこ…？",
    L"那、那個…你覺得佩克拉怎麼樣 peko…？" }, EXPR_SHY, 3, {
  { { L"I love you!", L"大好き！", L"最喜歡了！" },                           9,  8 },
  { { L"You're okay, I guess", L"普通かな", L"普通吧" },                     10, -3 },
  { { L"It's a secret", L"ヒミツ", L"祕密" },                                11,  3 } } },
{ { L"!! ...I-I see. Ehehe... maybe Pekora kinda likes you too",
    L"！！…そ、そっか。えへへ…ぺこらも…まあまあ好きかも",
    L"！！…是、是嗎。欸嘿嘿…佩克拉也…還算喜歡你啦" }, EXPR_EMBARRASSED, 0, {{{0}}} },
{ { L"O-Okay!? You call THE Pekora just 'okay'!?",
    L"ふっ、普通！？このぺこらをつかまえて普通とは何事ぺこ！",
    L"普、普通！？竟敢說本佩克拉普通！？" }, EXPR_ANGRY, 0, {{{0}}} },
{ { L"What's that!? Now I'm curious! Tell me! Heyyy~!",
    L"なにそれ気になる！教えて！ねえってば〜！",
    L"什麼啦超在意的！告訴我！喂～！" }, EXPR_POUT, 0, {{{0}}} },
/* 12 -- food quiz */
{ { L"Quiz time, peko! What's Pekora's favorite food?",
    L"クイズぺこ！ぺこらの好きな食べ物はな〜んだ？",
    L"猜謎時間 peko！佩克拉最喜歡的食物是什麼？" }, EXPR_SMILE, 3, {
  { { L"Carrots!", L"にんじん！", L"紅蘿蔔！" },                             13,  2 },
  { { L"Ramen!", L"ラーメン！", L"拉麵！" },                                 14,  4 },
  { { L"Nousagi hearts", L"野うさぎの心", L"野兔的心" },                     15,  5 } } },
{ { L"Bzzt! ...okay you're right, but put more thought into it, peko!",
    L"ぶっぶー！…いや合ってるけど！もうちょっとひねるぺこ！",
    L"答錯！…不對你是對的，但再動點腦筋嘛 peko！" }, EXPR_POUT, 0, {{{0}}} },
{ { L"Correct... I'd like to say, but midnight ramen is dangerous, peko...",
    L"正解…って言いたいけど、夜中のラーメンはやばいぺこね…",
    L"正確…雖然想這麼說，但半夜吃拉麵很危險 peko…" }, EXPR_EMBARRASSED, 0, {{{0}}} },
{ { L"I-I already have the nousagi hearts...! Wait, that's not it!",
    L"や、野うさぎの心はもう持ってるし…！じゃなくて！",
    L"野、野兔的心早就是佩克拉的了…！不對啦！" }, EXPR_SURPRISED, 0, {{{0}}} },
/* 16 -- HA↑HA↑HA↑ exam */
{ { L"Nousagi exam! Can you do the 'HA↑HA↑HA↑' properly, peko?",
    L"野うさぎ検定！『HA↑HA↑HA↑』、正しく言えるぺこ？",
    L"野兔檢定！你能正確喊出『HA↑HA↑HA↑』嗎 peko？" }, EXPR_SMILE, 3, {
  { { L"HA↑HA↑HA↑", L"HA↑HA↑HA↑", L"HA↑HA↑HA↑" },                           17,  5 },
  { { L"Ha... haha...", L"は…はは…", L"哈…哈哈…" },                          18,  1 },
  { { L"↑↑↓↓←→←→BA", L"↑↑↓↓←→←→BA", L"↑↑↓↓←→←→BA" },                         19,  3 } } },
{ { L"Perfect, peko!! You're a full-fledged nousagi now!",
    L"完璧ぺこ！！君はもう立派な野うさぎ！",
    L"完美 peko！！你已經是合格的野兔了！" }, EXPR_EXCITED, 0, {{{0}}} },
{ { L"Too quiet! One more time~! ...hehe, just kidding, peko",
    L"声が小さい！もう一回〜！……ふふ、冗談ぺこ",
    L"太小聲了！再來一次～！……呵呵，開玩笑的 peko" }, EXPR_SMILE, 0, {{{0}}} },
{ { L"That's the cheat code!? ...Knowing it is impressive though, peko",
    L"それコマンドの方ぺこ！？…でも知ってるのは偉いぺこね",
    L"那是密技指令吧 peko！？…不過你居然知道，算你厲害" }, EXPR_SURPRISED, 0, {{{0}}} },
/* 20 -- high-rank special */
{ { L"Thanks for always coming to see me... S-So... stay by my side from now on too, okay?",
    L"いつも来てくれて…その、ありがとぺこ。…これからも、そばにいてね？",
    L"謝謝你一直都來看佩克拉…那個…以後也要一直陪在我身邊喔？" }, EXPR_SHY, 2, {
  { { L"Of course!", L"もちろん！", L"當然！" },                             21, 10 },
  { { L"As long as you stream", L"ぺこらが配信する限りね",
      L"只要佩克拉繼續直播" },                                               22,  6 } } },
{ { L"Ehehe... it's a promise, peko! Pinky swear!",
    L"えへへ…約束だぺこよ！ゆびきりげんまん！",
    L"欸嘿嘿…約好了 peko！打勾勾！" }, EXPR_EMBARRASSED, 0, {{{0}}} },
{ { L"Then I'll just have to stream forever, peko!",
    L"じゃあ一生配信するしかないぺこね！",
    L"那佩克拉只好直播一輩子了 peko！" }, EXPR_EXCITED, 0, {{{0}}} },
};

typedef struct { int node; int minRank; } DlgScene;
static const DlgScene SCENES[] = {
    { 0, 0 }, { 4, 0 }, { 8, 0 }, { 12, 0 }, { 16, 0 }, { 20, 3 }
};
#define SCENE_COUNT N(SCENES)

/* ---------- misc runtime state ---------- */
static int     g_phrase = 0;
static DWORD   g_phraseMs;
static wchar_t g_overrideTxt[96];
static DWORD   g_overrideUntil;
static DWORD   g_lastClickMs, g_lastPetMs, g_lastPetPhraseMs;
static DWORD   g_clickBurstMs; static int g_clickBurst;
static DWORD   g_inviteAt = 0;
static int     g_inviting = 0;
static DWORD   g_noteMs = 0;

/* particles (hearts / carrots / sparkles / drifting ♪) */
#define MAX_PART 96
typedef struct {
    float x, y, vx, vy;
    DWORD born; int life;
    int   type;                  /* 0 heart, 1 carrot, 2 sparkle, 3 note  */
    int   alive;
} Particle;
static Particle g_part[MAX_PART];
static int      g_partAlive = 0;
static RECT     g_partRect;

/* petals */
#define N_PETALS 22
typedef struct { float x, y, ph, spd, drift; int size; } Petal;
static Petal g_petals[N_PETALS];
static int   g_petalsInit = 0;

/* voice clips */
#define MAX_VOICE 32
static wchar_t g_voice[MAX_VOICE][MAX_PATH];
static int     g_voiceCount = 0;

/* konami */
static const WORD KONAMI[] = { VK_UP, VK_UP, VK_DOWN, VK_DOWN,
                               VK_LEFT, VK_RIGHT, VK_LEFT, VK_RIGHT, 'B', 'A' };
static int g_konamiPos = 0;

static int rank_of(int pts)
{
    static const int th[5] = { 0, 50, 150, 400, 1000 };
    int r = 0;
    for (int i = 1; i < 5; ++i) if (pts >= th[i]) r = i;
    return r;
}

static void today_str(wchar_t *out, int cap)
{
    time_t t = time(NULL); struct tm *lt = localtime(&t);
    if (lt) _snwprintf(out, cap, L"%04d-%02d-%02d",
                       lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    else    wcsncpy(out, L"?", cap);
}

void bubble_say(const wchar_t *txt, DWORD holdMs)
{
    wcsncpy(g_overrideTxt, txt, 95); g_overrideTxt[95] = 0;
    g_overrideUntil = GetTickCount() + holdMs;
    InvalidateRect(g_hMain, &R_peko, FALSE);
}

int chara_dlg_active(void) { return g_dlgNode >= 0; }

/* the context emotion she settles back into */
static int base_expr(void)
{
    if (g_dlgNode >= 0) return DLG[g_dlgNode].expr;
    if (g_playing == 1) return EXPR_HAPPY;
    if (g_state == ST_OK && g_itemCount > 0 && g_items[0].isLive) return EXPR_HAPPY;
    if (g_state == ST_CHECKING) return EXPR_THINKING;
    return EXPR_SMILE;
}

/* set an emotion; holdMs > 0 = an EVENT (plays its motion, pops its icon,
 * reverts to the context emotion afterwards). holdMs = 0 = context change
 * (no icon, recipe restarts only if the emotion actually changed). */
void chara_set_expr(int expr, DWORD holdMs)
{
    DWORD now = GetTickCount();
    if (expr < 0 || expr >= EXPR_COUNT) return;
    int changed = (expr != g_cs.emotion);
    g_cs.emotion = expr;
    if (changed || holdMs) {
        g_cs.emotionMs = now;
        g_cs.stepIdx = 0;
        g_cs.stepMs = now;
    }
    g_exprHold = holdMs ? now + holdMs : 0;
    if (holdMs && EMO[expr].icon != ICON_NONE) {
        g_icon = EMO[expr].icon;
        g_iconMs = now;
    }
    InvalidateRect(g_hMain, &R_peko, FALSE);
}

/* ---------- init ---------- */

static void scan_voices(void)
{
    g_voiceCount = 0;
    wchar_t dir[MAX_PATH], spec[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *s = (n && n < MAX_PATH) ? wcsrchr(dir, L'\\') : NULL;
    if (s) wcscpy(s + 1, L"voice"); else wcscpy(dir, L"voice");
    _snwprintf(spec, MAX_PATH, L"%ls\\*.wav", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_voiceCount >= MAX_VOICE) break;
        _snwprintf(g_voice[g_voiceCount], MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        g_voiceCount++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void voice_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    DWORD u = (DWORD)(v * 65535 / 100);
    waveOutSetVolume((HWAVEOUT)(UINT_PTR)0, MAKELONG(u, u));
}

void chara_init(void)
{
    g_hPekora = load_image_res(IMG_PEKORA, &g_pkW, &g_pkH, 0x00000000);
    g_phrase  = rand() % N(PH_BASE);
    g_phraseMs = GetTickCount();
    g_inviteAt = GetTickCount() + 120000 + (rand() % 180000);  /* 2-5 min */
    scan_voices();
    voice_set_volume(g_cfg.voiceVol);
}

/* ---------- particles ---------- */

static void part_extend(float x, float y)
{
    RECT r = { (int)x - S(18), (int)y - S(18), (int)x + S(18), (int)y + S(18) };
    if (IsRectEmpty(&g_partRect)) g_partRect = r;
    else UnionRect(&g_partRect, &g_partRect, &r);
}

static void spawn_particle(float x, float y, int type, float spread, float up)
{
    for (int i = 0; i < MAX_PART; ++i) {
        if (g_part[i].alive) continue;
        Particle *p = &g_part[i];
        p->x = x; p->y = y;
        p->vx = ((rand() % 200) - 100) / 100.0f * spread;
        p->vy = -up - (rand() % 100) / 60.0f;
        p->born = GetTickCount();
        p->life = (type == 3 ? 1100 : 550) + rand() % 350;
        p->type = type;
        p->alive = 1;
        g_partAlive++;
        part_extend(x, y);
        return;
    }
}

static void draw_heart(HDC dc, int cx, int cy, int r, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + 1, cy + 1);
    Ellipse(dc, cx, cy - r, cx + r + 1, cy + 1);
    POINT tri[3] = { { cx - r, cy - r / 4 }, { cx + r, cy - r / 4 }, { cx, cy + r } };
    Polygon(dc, tri, 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br);
}

static void draw_carrot(HDC dc, int cx, int cy, int r, double ang)
{
    POINT tip = { cx + (int)(r * 1.4 * cos(ang)), cy + (int)(r * 1.4 * sin(ang)) };
    POINT b1  = { cx + (int)(r * 0.7 * cos(ang + 2.6)), cy + (int)(r * 0.7 * sin(ang + 2.6)) };
    POINT b2  = { cx + (int)(r * 0.7 * cos(ang - 2.6)), cy + (int)(r * 0.7 * sin(ang - 2.6)) };
    HBRUSH br = CreateSolidBrush(C_CARROT);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    POINT tri[3] = { tip, b1, b2 };
    Polygon(dc, tri, 3);
    HBRUSH gr = CreateSolidBrush(RGB(110, 200, 120));
    SelectObject(dc, gr);
    Ellipse(dc, (b1.x + b2.x) / 2 - r / 3, (b1.y + b2.y) / 2 - r / 3,
                (b1.x + b2.x) / 2 + r / 3, (b1.y + b2.y) / 2 + r / 3);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(gr);
}

static void particles_draw(HDC dc)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_PART; ++i) {
        if (!g_part[i].alive) continue;
        Particle *p = &g_part[i];
        DWORD el = now - p->born;
        double kk = 1.0 - (double)el / p->life;
        if (kk <= 0) continue;
        int r = S(5) + (int)(S(3) * kk);
        switch (p->type) {
        case 0: draw_heart(dc, (int)p->x, (int)p->y, r,
                           kk > 0.5 ? C_SAKURA_D : C_SAKURA); break;
        case 1: draw_carrot(dc, (int)p->x, (int)p->y, r + S(2),
                            (el / 90.0) + i); break;
        case 3: {
            RECT tr = { (int)p->x - S(14), (int)p->y - S(14),
                        (int)p->x + S(14), (int)p->y + S(14) };
            draw_text(dc, (i & 1) ? L"♪" : L"♫", &tr, g_fUI,
                      kk > 0.5 ? C_PEKODK : C_SKY,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        default: {
            HBRUSH br = CreateSolidBrush(RGB(255, 232, 120));
            HGDIOBJ ob = SelectObject(dc, br);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, (int)p->x - r / 2, (int)p->y - r / 2,
                        (int)p->x + r / 2, (int)p->y + r / 2);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
        } }
    }
}

/* ---------- petals ---------- */

static void petals_init(RECT *rc)
{
    for (int i = 0; i < N_PETALS; ++i) {
        Petal *p = &g_petals[i];
        p->x = (float)(rand() % (rc->right > 0 ? rc->right : 1));
        p->y = (float)(rand() % (rc->bottom > 0 ? rc->bottom : 1));
        p->ph = (float)(rand() % 628) / 100.0f;
        p->spd = 0.5f + (rand() % 100) / 90.0f;
        p->drift = ((rand() % 100) - 50) / 120.0f;
        p->size = S(4) + rand() % S(4);
    }
    g_petalsInit = 1;
}

void petals_draw(HDC dc, RECT *rc)
{
    if (!g_cfg.petalsOn) return;
    if (!g_petalsInit) petals_init(rc);
    for (int i = 0; i < N_PETALS; ++i) {
        Petal *p = &g_petals[i];
        int px = (int)(p->x + sin(p->ph) * S(10));
        int py = (int)p->y;
        COLORREF c = (i & 1) ? RGB(255, 205, 222) : C_SAKURA;
        HBRUSH br = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        int w = p->size, h = p->size * 2 / 3 + 1;
        if (((int)(p->ph * 2)) & 1) { int t = w; w = h; h = t; }
        Ellipse(dc, px - w, py - h, px + w, py + h);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(br);
    }
}

/* ---------- per-tick animation (the "game loop" hook) ---------- */

int chara_tick(HWND hWnd, DWORD now)
{
    if (g_overrideUntil && now >= g_overrideUntil) {
        g_overrideUntil = 0;
        InvalidateRect(hWnd, &R_peko, FALSE);
    }
    if (!g_overrideUntil && now - g_phraseMs >= 5000) {
        g_phraseMs = now;
        g_phrase++;
        InvalidateRect(hWnd, &R_peko, FALSE);
    }

    /* emotion: revert to the context emotion once a held reaction expires */
    if (g_exprHold && now >= g_exprHold) {
        g_exprHold = 0;
        chara_set_expr(base_expr(), 0);
    } else if (!g_exprHold && g_dlgNode < 0 && g_cs.emotion != base_expr()) {
        chara_set_expr(base_expr(), 0);
    }

    /* the VN presentation system update */
    g_cs.talking = 0;
    if (g_dlgNode >= 0) {
        size_t len = wcslen(DLG[g_dlgNode].text[g_lang]);
        size_t shown = (size_t)((now - g_dlgTextMs) * DLG_CPS / 1000);
        g_cs.talking = (shown < len);
        if (shown < len + 4) InvalidateRect(hWnd, &g_dlgRect, FALSE);
    }
    UpdateAnimation(now);

    /* hot = something visibly moving beyond breathing -> repaint every tick */
    int hot = g_cs.talking || g_shakeAmp > 0 ||
              now - g_cs.emotionMs < 1400 ||
              (g_icon != ICON_NONE && now - g_iconMs < ICON_LIFE) ||
              (g_squashMs && now - g_squashMs < 280);
    if (hot) {
        RECT inv = R_peko;
        inv.top -= S(52);                  /* room for the emotion icon */
        InvalidateRect(hWnd, &inv, FALSE);
        if (g_shkX | g_shkY) InvalidateRect(hWnd, &g_dlgRect, FALSE);
    } else if (GetForegroundWindow() == hWnd) {
        /* idle: breathing only, ~12 fps is plenty for a 1% chest rise */
        static DWORD lastBr = 0;
        if (now - lastBr >= 80) { lastBr = now; InvalidateRect(hWnd, &R_peko, FALSE); }
    }

    /* singing along: float a ♪ every ~1.3 s while music plays */
    if (g_playing == 1 && now - g_noteMs >= 1200 + (DWORD)(rand() % 600)) {
        g_noteMs = now;
        if (!IsRectEmpty(&g_pekoDst))
            spawn_particle((float)(g_pekoDst.left + S(20) + rand() % 60),
                           (float)(g_pekoDst.top + S(20)), 3, 0.4f, 1.2f);
        if (!g_overrideUntil && (rand() % 4) == 0)
            bubble_say(PH_SING[rand() % N(PH_SING)], 2200);
    }

    /* once in a while she invites you to chat */
    if (g_dlgNode < 0 && now >= g_inviteAt) {
        g_inviteAt = now + 180000 + (rand() % 240000);   /* next: 3-7 min */
        g_inviting = 1;
        bubble_say(LANGS[g_lang].dlg_invite, 6000);
        chara_set_expr(EXPR_SHY, 6000);
    }

    /* particles */
    if (g_partAlive > 0) {
        RECT prev = g_partRect;
        SetRectEmpty(&g_partRect);
        for (int i = 0; i < MAX_PART; ++i) {
            if (!g_part[i].alive) continue;
            Particle *p = &g_part[i];
            if (now - p->born >= (DWORD)p->life) { p->alive = 0; g_partAlive--; continue; }
            p->x += p->vx;
            p->y += p->vy;
            p->vy += (p->type == 3) ? 0.01f : 0.12f;
            part_extend(p->x, p->y);
        }
        RECT inv = prev;
        if (!IsRectEmpty(&g_partRect)) UnionRect(&inv, &prev, &g_partRect);
        if (!IsRectEmpty(&inv)) InvalidateRect(hWnd, &inv, FALSE);
    }

    /* petals */
    if (g_cfg.petalsOn && GetForegroundWindow() == hWnd) {
        static DWORD lastPet = 0;
        if (now - lastPet >= 33) {
            lastPet = now;
            RECT rc; GetClientRect(hWnd, &rc);
            if (!g_petalsInit) petals_init(&rc);
            for (int i = 0; i < N_PETALS; ++i) {
                Petal *p = &g_petals[i];
                p->y  += p->spd * 1.6f;
                p->x  += p->drift;
                p->ph += 0.045f;
                if (p->y > rc.bottom + 8) {
                    p->y = -8.0f;
                    p->x = (float)(rand() % (rc.right > 0 ? rc.right : 1));
                }
                if (p->x < -12) p->x = (float)rc.right + 8;
                if (p->x > rc.right + 12) p->x = -8;
            }
            return 1;
        }
    }
    return 0;
}

/* ---------- interactions ---------- */

static void play_voice(void)
{
    if (g_voiceCount <= 0) return;
    PlaySoundW(g_voice[rand() % g_voiceCount], NULL,
               SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

static void affection_add(int pts)
{
    int r0 = rank_of(g_cfg.affection);
    g_cfg.affection += pts;
    if (g_cfg.affection < 0) g_cfg.affection = 0;
    int r1 = rank_of(g_cfg.affection);
    if (r1 > r0) {
        wchar_t msg[96];
        _snwprintf(msg, 96, L"⭐ %ls ⭐", LANGS[g_lang].rank[r1]);
        bubble_say(msg, 4000);
        chara_set_expr(EXPR_EXCITED, 4000);
        for (int i = 0; i < 12; ++i)
            spawn_particle((float)((g_pekoDst.left + g_pekoDst.right) / 2),
                           (float)g_pekoDst.top + S(20), 2, 2.2f, 2.2f);
    }
    cfg_save();
}

int chara_click(HWND hWnd, POINT p)
{
    if (!PtInRect(&g_pekoDst, p)) return 0;
    DWORD now = GetTickCount();
    if (now - g_lastClickMs < 350) return 1;
    g_lastClickMs = now;
    g_squashMs = now;

    if (now - g_clickBurstMs > 3000) { g_clickBurstMs = now; g_clickBurst = 0; }
    g_clickBurst++;

    wchar_t today[16]; today_str(today, 16);
    if (wcscmp(today, g_cfg.lastGreetDay) != 0) {
        wcsncpy(g_cfg.lastGreetDay, today, 15);
        bubble_say(L"こんぺこ〜！今日も来てくれたんだね ❤", 4200);
        chara_set_expr(EXPR_EXCITED, 4200);
        affection_add(10);
    } else if (g_clickBurst >= 5) {
        g_clickBurst = 0;
        bubble_say(L"もう！くすぐったいってば！", 3000);
        chara_set_expr(EXPR_ANGRY, 3000);
        affection_add(1);
    } else {
        bubble_say(PH_CLICK[rand() % N(PH_CLICK)], 2600);
        chara_set_expr((rand() % 3) ? EXPR_HAPPY : EXPR_SURPRISED, 2600);
        affection_add(2);
    }

    for (int i = 0; i < 8; ++i)
        spawn_particle((float)p.x, (float)p.y, (rand() % 3 == 0) ? 1 : 0, 1.6f, 1.6f);
    play_voice();
    InvalidateRect(hWnd, &R_peko, FALSE);
    return 1;
}

void chara_drag(HWND hWnd, POINT p)
{
    if (!PtInRect(&g_pekoDst, p)) return;
    DWORD now = GetTickCount();
    if (now - g_lastPetMs < 160) return;
    g_lastPetMs = now;
    spawn_particle((float)p.x, (float)p.y, 0, 0.8f, 1.0f);
    affection_add(1);
    chara_set_expr(EXPR_SHY, 1600);
    if (now - g_lastPetPhraseMs > 2500) {
        g_lastPetPhraseMs = now;
        bubble_say(PH_PET[rand() % N(PH_PET)], 2200);
        if ((rand() % 3) == 0) chara_set_expr(EXPR_EMBARRASSED, 2200);
    }
    (void)hWnd;
}

void chara_konami_key(WPARAM vk)
{
    if (vk == KONAMI[g_konamiPos]) {
        if (++g_konamiPos >= (int)(sizeof KONAMI / sizeof KONAMI[0])) {
            g_konamiPos = 0;
            bubble_say(L"HA↑HA↑HA↑ にんじんの雨ぺこ！！", 3500);
            chara_set_expr(EXPR_EXCITED, 3500);
            RECT rc; GetClientRect(g_hMain, &rc);
            for (int i = 0; i < 36; ++i) {
                float x = (float)(rand() % (rc.right > 0 ? rc.right : 1));
                spawn_particle(x, (float)(-(rand() % 200)), 1, 1.2f, -1.5f);
            }
            play_voice();
        }
    } else g_konamiPos = (vk == KONAMI[0]) ? 1 : 0;
}

int chara_fortune_hit(HWND hWnd, POINT p)
{
    if (g_tab != TAB_BOARD) return 0;
    int cx = R_board.right - S(16), cy = R_board.bottom - S(16);
    int dx = p.x - cx, dy = p.y - cy, r = S(16);
    if (dx * dx + dy * dy > r * r) return 0;

    wchar_t today[16]; today_str(today, 16);
    int idx;
    if (wcscmp(today, g_cfg.fortuneDay) == 0 && g_cfg.fortuneIdx >= 0)
        idx = g_cfg.fortuneIdx % FORTUNE_COUNT;
    else {
        idx = rand() % FORTUNE_COUNT;
        wcsncpy(g_cfg.fortuneDay, today, 15);
        g_cfg.fortuneIdx = idx;
        cfg_save();
    }
    wchar_t msg[96];
    _snwprintf(msg, 96, L"%ls%ls", LANGS[g_lang].fortune_pre, FORTUNES[idx].txt);
    bubble_say(msg, 5000);
    chara_set_expr(FORTUNES[idx].expr, 5000);
    for (int i = 0; i < 6; ++i)
        spawn_particle((float)p.x, (float)p.y, 2, 1.4f, 1.4f);
    (void)hWnd;
    return 1;
}

void chara_on_live(void)
{
    bubble_say(PH_LIVE[rand() % N(PH_LIVE)], 6000);
    chara_set_expr(EXPR_SURPRISED, 1800);        /* !? ... then happy (base) */
}

/* ---------- galgame dialogue logic ---------- */

static void dlg_goto(int node)
{
    g_dlgNode = node;
    g_dlgTextMs = GetTickCount();
    if (node >= 0) chara_set_expr(DLG[node].expr, 0);
    g_overrideUntil = 0;                          /* bubble yields to the box */
    InvalidateRect(g_hMain, NULL, FALSE);
}

void chara_dlg_action(HWND hWnd, const wchar_t *act)
{
    if (wcscmp(act, L"!talk") == 0) {
        g_inviting = 0;
        int rank = rank_of(g_cfg.affection);
        int pick = -1, guard = 0;
        while (guard++ < 32) {
            int s = rand() % SCENE_COUNT;
            if (SCENES[s].minRank > rank) continue;
            if (SCENE_COUNT > 2 && s == g_lastScene) continue;
            pick = s; break;
        }
        if (pick < 0) pick = 0;
        g_lastScene = pick;
        dlg_goto(SCENES[pick].node);
        play_voice();
        return;
    }
    if (g_dlgNode < 0) return;
    if (wcscmp(act, L"!dlgx") == 0) {
        g_dlgNode = -1;
        g_exprHold = 0;
        chara_set_expr(base_expr(), 0);
        InvalidateRect(g_hMain, NULL, FALSE);
        return;
    }
    if (wcscmp(act, L"!dlgskip") == 0) {
        size_t len = wcslen(DLG[g_dlgNode].text[g_lang]);
        size_t shown = (size_t)((GetTickCount() - g_dlgTextMs) * DLG_CPS / 1000);
        if (shown >= len && DLG[g_dlgNode].nch == 0) {
            chara_dlg_action(hWnd, L"!dlgx");     /* paging past the end */
        } else {
            g_dlgTextMs = GetTickCount() - 60000; /* reveal the full line */
            InvalidateRect(g_hMain, &g_dlgRect, FALSE);
        }
        return;
    }
    if (wcsncmp(act, L"!dlg:", 5) == 0) {
        int k = _wtoi(act + 5);
        const DlgNode *nd = &DLG[g_dlgNode];
        if (k < 0 || k >= nd->nch) return;
        if (nd->ch[k].dAff) affection_add(nd->ch[k].dAff);
        if (nd->ch[k].dAff >= 5)
            for (int i = 0; i < 6; ++i)
                spawn_particle((float)((g_pekoDst.left + g_pekoDst.right) / 2),
                               (float)g_pekoDst.top + S(30), 0, 1.6f, 1.6f);
        dlg_goto(nd->ch[k].next);
        play_voice();
    }
}

/* the compact galgame window in her corner; shakes with the camera */
void chara_dlg_draw(HDC dc, RECT *client)
{
    (void)client;
    SetRectEmpty(&g_dlgRect);
    if (g_dlgNode < 0) return;
    const DlgNode *nd = &DLG[g_dlgNode];
    const wchar_t *text = nd->text[g_lang];
    DWORD now = GetTickCount();

    int l = R_peko.left;
    int r = (g_pekoDst.left > l + S(180)) ? g_pekoDst.left - S(8)
                                          : R_peko.right - S(110);
    if (r - l > S(430)) l = r - S(430);
    int b = R_peko.bottom - S(2);
    int t = b - S(124);
    if (t < R_peko.top) t = R_peko.top;
    l += g_shkX; r += g_shkX; t += g_shkY; b += g_shkY;
    g_dlgRect = (RECT){ l - S(8), t - S(18), r + S(8), b + S(8) };

    fill_round(dc, l, t, r, b, 14, RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 2, C_SAKURA);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, l, t, r, b, 14, 14);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
    fill_round(dc, l + S(10), t - S(14), l + S(92), t + S(8), 8, C_SAKURA_D);
    RECT nr = { l + S(10), t - S(14), l + S(92), t + S(8) };
    draw_text(dc, L"ぺこら 🥕", &nr, g_fSmall, RGB(255, 255, 255),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    add_link(l, t, r, b, L"!dlgskip");

    RECT xr = { r - S(26), t + S(5), r - S(6), t + S(25) };
    draw_text(dc, L"✕", &xr, g_fSmall, C_NAVY_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    add_link(xr.left, xr.top, xr.right, xr.bottom, L"!dlgx");

    size_t len = wcslen(text);
    size_t shown = (size_t)((now - g_dlgTextMs) * DLG_CPS / 1000);
    int done = shown >= len;
    RECT tr = { l + S(12), t + S(14), r - S(28), b - S(34) };
    if (!done) {
        wchar_t buf[256];
        if (shown > 255) shown = 255;
        wcsncpy(buf, text, shown); buf[shown] = 0;
        draw_text(dc, buf, &tr, g_fSmall, C_NAVY, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        draw_text(dc, text, &tr, g_fSmall, C_NAVY, DT_LEFT | DT_TOP | DT_WORDBREAK);

        if (nd->nch > 0) {
            int cy = b - S(30), cx = r - S(8);
            for (int k = nd->nch - 1; k >= 0; --k) {
                const wchar_t *lbl = nd->ch[k].label[g_lang];
                SIZE sz; HGDIOBJ of = SelectObject(dc, g_fSmall);
                GetTextExtentPoint32W(dc, lbl, (int)wcslen(lbl), &sz);
                SelectObject(dc, of);
                int cw = sz.cx + S(20);
                if (cx - cw < l + S(6)) { cx = r - S(8); cy -= S(30); }
                cx -= cw;
                fill_round(dc, cx, cy, cx + cw, cy + S(24), 10,
                           k == 0 ? C_PEKO : (k == 1 ? C_SAKURA_D : C_GREEN));
                RECT cr = { cx, cy, cx + cw, cy + S(24) };
                draw_text(dc, lbl, &cr, g_fSmall, RGB(255, 255, 255),
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                wchar_t actbuf[16];
                _snwprintf(actbuf, 16, L"!dlg:%d", k);
                add_link(cx, cy, cx + cw, cy + S(24), actbuf);
                cx -= S(8);
            }
        } else {
            RECT er = { r - S(96), b - S(30), r - S(8), b - S(6) };
            fill_round(dc, er.left, er.top, er.right, er.bottom, 10,
                       (now / 500) % 2 ? C_SAKURA_D : C_SAKURA);
            draw_text(dc, LANGS[g_lang].dlg_end, &er, g_fSmall,
                      RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            add_link(er.left, er.top, er.right, er.bottom, L"!dlgx");
        }
    }
}

/* ---------- emotion icon (independent pop / float / pulse / fade) ---------- */

static void draw_emote_icon(HDC dc, DWORD now)
{
    if (g_icon == ICON_NONE) return;
    DWORD el = now - g_iconMs;
    if (el >= ICON_LIFE) { g_icon = ICON_NONE; return; }
    float t = el / (float)ICON_LIFE;

    /* pop in with overshoot, shrink out at the end (pixel-friendly exit) */
    float sc = (t < 0.18f) ? sinf(t / 0.18f * 1.5708f) * 1.25f
             : (t < 0.30f) ? 1.25f - (t - 0.18f) / 0.12f * 0.25f : 1.0f;
    if (t > 0.78f) sc *= (1.0f - (t - 0.78f) / 0.22f);
    if (sc <= 0.05f) return;

    int cx = (g_pekoDst.left + g_pekoDst.right) / 2 + S(22);
    int cy = g_pekoDst.top - S(16);

    switch (g_icon) {
    case ICON_HEART:                              /* floats up, blushing pink */
        draw_heart(dc, cx, cy - (int)(t * S(14)), (int)(S(9) * sc), C_SAKURA_D);
        break;
    case ICON_SWEAT: {                            /* slides down the temple   */
        int sx = cx + S(12), sy = cy + S(6) + (int)(t * S(10));
        int rw = (int)(S(5) * sc), rh = (int)(S(7) * sc);
        HBRUSH br = CreateSolidBrush(RGB(110, 175, 240));
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, sx - rw, sy - rh / 2, sx + rw, sy + rh);
        POINT tri[3] = { { sx - rw + 1, sy }, { sx + rw - 1, sy }, { sx, sy - rh - rh / 2 } };
        Polygon(dc, tri, 3);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
        break;
    }
    case ICON_ANGER: {                            /* manga 💢, pulsing        */
        float pulse = sc * (1.0f + 0.20f * sinf(el * 0.025f));
        int rr = (int)(S(9) * pulse);
        HPEN pn = CreatePen(PS_SOLID, S(3), C_RED);
        HGDIOBJ op = SelectObject(dc, pn);
        for (int kx = 0; kx < 4; ++kx) {          /* four radiating claws */
            double a = 0.785 + kx * 1.5708;
            MoveToEx(dc, cx + (int)(rr * 0.45 * cos(a)), cy + (int)(rr * 0.45 * sin(a)), NULL);
            LineTo(dc, cx + (int)(rr * cos(a)), cy + (int)(rr * sin(a)));
        }
        SelectObject(dc, op); DeleteObject(pn);
        break;
    }
    case ICON_QUESTION: {                         /* tilting "?"              */
        RECT qr = { cx - S(16) + (int)(sinf(t * 12.56f) * S(2)), cy - S(22),
                    cx + S(16) + (int)(sinf(t * 12.56f) * S(2)), cy + S(10) };
        draw_text(dc, L"?", &qr, g_fDate, C_PEKODK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case ICON_EXCLAIM: {                          /* sharp "!"                */
        RECT er = { cx - S(16), cy - S(24) - (int)(sc * S(2)),
                    cx + S(16), cy + S(8) };
        draw_text(dc, L"!", &er, g_fDate, C_RED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case ICON_NOTE: {                             /* drifts up-right, swaying */
        RECT mr = { cx - S(14) + (int)(t * S(10) + sinf(t * 9.42f) * S(3)),
                    cy - S(16) - (int)(t * S(16)),
                    cx + S(14) + (int)(t * S(10)), cy + S(8) - (int)(t * S(16)) };
        draw_text(dc, L"♪", &mr, g_fDate, C_PEKODK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    }
}

/* ---------- drawing ---------- */

static void draw_bubble(HDC dc, int l, int t, int r, int b, const wchar_t *txt)
{
    fill_round(dc, l, t, r, b, 16, RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, 2, C_SAKURA);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, l, t, r, b, 16, 16);
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ obr = SelectObject(dc, wb);
    POINT tail[3] = { { r - S(14), b - S(20) }, { r + S(10), b - S(8) }, { r - S(14), b - S(4) } };
    Polygon(dc, tail, 3);
    SelectObject(dc, op); SelectObject(dc, ob); SelectObject(dc, obr);
    DeleteObject(pen); DeleteObject(wb);
    RECT tr = { l + S(10), t, r - S(10), b };
    draw_text(dc, txt, &tr, g_fUI, C_SAKURA_D,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK);
}

static const wchar_t *current_phrase(void)
{
    if (g_overrideUntil) return g_overrideTxt;
    int live = (g_state == ST_OK && g_itemCount > 0 && g_items[0].isLive);
    time_t t = time(NULL); struct tm *lt = localtime(&t);
    int hr = lt ? lt->tm_hour : 12;
    if (live && (g_phrase % 3) == 0)  return PH_LIVE[g_phrase % N(PH_LIVE)];
    if (hr >= 5 && hr < 11 && (g_phrase % 4) == 0)
        return PH_MORNING[g_phrase % N(PH_MORNING)];
    if ((hr >= 22 || hr < 5) && (g_phrase % 4) == 0)
        return PH_NIGHT[g_phrase % N(PH_NIGHT)];
    return PH_BASE[g_phrase % N(PH_BASE)];
}

void chara_draw(HDC dc)
{
    if (!g_hPekora || g_pkW <= 0 || g_pkH <= 0) { SetRectEmpty(&g_pekoDst); return; }
    int rw = R_peko.right - R_peko.left, rh = R_peko.bottom - R_peko.top;
    if (rw < 20 || rh < 20) { SetRectEmpty(&g_pekoDst); return; }
    DWORD now = GetTickCount();

    int maxh = g_pkH * 13 / 10;                /* modest 1.3x upscale cap */
    int dh = rh > maxh ? maxh : rh;
    int dw = g_pkW * dh / g_pkH;
    if (dw > rw) { dw = rw; dh = g_pkH * dw / g_pkW; }

    /* click poke pulse on top of the system's transform */
    double psx = 1.0, psy = 1.0;
    if (g_squashMs && now - g_squashMs < 260) {
        double p = (now - g_squashMs) / 260.0;
        double amp = sin(p * 3.14159) * 0.10;
        psx += amp; psy -= amp;
    }

    /* final size: state scale x camera zoom, snapped to even pixels so the
     * pixel art doesn't shimmer during slow breathing */
    int ddw = (int)(dw * g_cs.scaleX * g_cs.zoom * psx) & ~1;
    int ddh = (int)(dh * g_cs.scaleY * g_cs.zoom * psy) & ~1;
    int dx = R_peko.right - ddw + (int)g_cs.x + g_shkX;
    int dy = R_peko.bottom - ddh + (int)g_cs.y + g_shkY;

    /* speech bubble (hidden while the dialogue box is open) */
    int bx2 = dx - S(14);
    if (g_dlgNode < 0 && bx2 - R_peko.left > S(120)) {
        int bw2 = bx2 - R_peko.left, bh2 = S(64);
        if (bw2 > S(300)) bw2 = S(300);
        int bl = bx2 - bw2, bt = dy + S(30);
        draw_bubble(dc, bl, bt, bl + bw2, bt + bh2, current_phrase());
    }

    {
        HDC m = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(m, g_hPekora);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(g_cs.alpha + 0.5f), AC_SRC_ALPHA };
        AlphaBlend(dc, dx, dy, ddw, ddh, m, 0, 0, g_pkW, g_pkH, bf);
        SelectObject(m, ob); DeleteDC(m);
    }

    g_pekoDst = (RECT){ dx, dy, dx + ddw, dy + ddh };

    draw_emote_icon(dc, now);

    /* "let's chat" chip floating by her head (pulses when she's inviting) */
    {
        wchar_t lbl[40];
        _snwprintf(lbl, 40, L"💬 %ls", LANGS[g_lang].talk_label);
        SIZE sz; HGDIOBJ of = SelectObject(dc, g_fSmall);
        GetTextExtentPoint32W(dc, lbl, (int)wcslen(lbl), &sz);
        SelectObject(dc, of);
        int cw = sz.cx + S(20), chh = S(26);
        int cx = dx + ddw / 2 - cw / 2, cy = dy - chh - S(6);
        if (cy < R_peko.top) cy = R_peko.top;
        if (cx + cw > R_peko.right) cx = R_peko.right - cw;
        COLORREF cc = C_PEKO;
        if (g_inviting && (now / 400) % 2) cc = C_SAKURA_D;
        fill_round(dc, cx, cy, cx + cw, cy + chh, 12, cc);
        RECT cr = { cx, cy, cx + cw, cy + chh };
        draw_text(dc, lbl, &cr, g_fSmall, RGB(255, 255, 255),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_link(cx, cy, cx + cw, cy + chh, L"!talk");
    }

    /* affection meter */
    {
        int rank = rank_of(g_cfg.affection);
        int hx = R_peko.left + S(4), hy = R_peko.bottom - S(20);
        if (dx - R_peko.left > S(150)) {
            for (int i = 0; i < 5; ++i)
                draw_heart(dc, hx + i * S(16), hy, S(6),
                           i <= rank ? C_SAKURA_D : RGB(225, 220, 228));
            RECT rr2 = { hx + 5 * S(16) + S(8), hy - S(11),
                         dx - S(8), hy + S(11) };
            draw_text(dc, LANGS[g_lang].rank[rank], &rr2, g_fSmall, C_NAVY_DIM,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    particles_draw(dc);
}
