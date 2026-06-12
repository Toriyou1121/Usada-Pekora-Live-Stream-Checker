/*
 * peko_lang.c - every visible string in EN / JA / ZH.
 * The Lang button cycles through LANG_ORDER; fonts switch to match.
 */
#include "peko.h"

const LangPack LANGS[LANG_COUNT] = {
    {   /* ---------------- English ---------------- */
        L"Peko Board  " PEKO_VERSION_W L"  - Usada Pekora Fan Board",
        L"Check", L"Channel", L"English", L"Join",
        L"CONNECTING TO YOUTUBE ...",
        L"CONNECTION ERROR - YouTube unreachable.\nOffline, or blocked here "
        L"(e.g. mainland China) - a working VPN / proxy is required.",
        L"Press  CHECK  to load Pekora's live & upcoming schedule.",
        L"ON AIR NOW", L"OFFLINE", L"  (+%d upcoming)",
        L"No live or upcoming streams. Pekora is on a break, peko!",
        L"chat", L"LIVE", L"SOON", L"NO COVER",
        L"Click \"watch\" for the stream, or \"chat\" for live chat.",
        L"Peko Board " PEKO_VERSION_W L" - still under development, peko!",
        L"Japan time (JST)", L"My computer's time",
        L"Japan local time",
        { L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
          L"Thursday", L"Friday", L"Saturday" },
        /* tabs */
        L"BOARD", L"REPLAYS", L"MUSIC",
        /* X card */
        L"Pekora on X (Twitter)",
        L"@usadapekora", L"Latest posts", L"#PekoArt",
        /* replays */
        L"RECENT STREAM REPLAYS",
        L"No replays found yet - press CHECK on the Board tab first, peko!",
        L"Click a card to watch the replay on YouTube.",
        /* music */
        L"PEKO MUSIC PLAYER",
        L"Put MP3 / WAV files into the \"music\" folder next to the exe,\n"
        L"then restart Peko Board. Pekora will sing for you, peko!",
        L"NOW PLAYING", L"(nothing playing)",
        /* countdown */
        L"Next stream in",
        /* settings */
        L"Window size", L"Volume ...", L"Auto check",
        L"Off", L"Every 5 min", L"Every 10 min",
        L"Live notification", L"Sakura petals", L"Minimize to tray",
        L"Music", L"Voice",
        L"Pekora is LIVE now, peko! Click to watch!",
        L"Open Peko Board", L"Check now", L"Exit",
        { L"Rookie nousagi", L"Nousagi", L"Fine nousagi",
          L"Elite nousagi", L"Legendary nousagi" },
        L"Today's peko-fortune: ",
        L"Chat",
        L"The end ▼",
        L"Hey hey, wanna chat? 💬"
    },
    {   /* ---------------- Japanese ---------------- */
        L"Peko Board  " PEKO_VERSION_W L"  〜兎田ぺこら ファンボード〜",
        L"確認", L"チャンネル", L"日本語", L"メンバー",
        L"YouTube に接続中 ...",
        L"接続エラー - YouTube に接続できません。\nオフライン、または規制中 "
        L"(例: 中国本土)。VPN / プロキシが必要です。",
        L"「確認」を押すと配信中・配信予定を読み込みます。",
        L"配信中", L"オフライン", L"  (ほか予定 %d 件)",
        L"配信中・配信予定はありません。ぺこらは休憩中ぺこ!",
        L"チャット", L"LIVE", L"予定", L"カバーなし",
        L"「watch」で配信ページ、「チャット」でライブチャットへ。",
        L"Peko Board " PEKO_VERSION_W L" - まだ開発中ぺこ!",
        L"日本時間 (JST)", L"このPCの時間",
        L"現地時間",
        { L"日曜日", L"月曜日", L"火曜日", L"水曜日",
          L"木曜日", L"金曜日", L"土曜日" },
        /* tabs */
        L"ボード", L"アーカイブ", L"ミュージック",
        /* X card */
        L"ぺこらの X (Twitter)",
        L"@usadapekora", L"最新ポスト", L"#ぺこらーと",
        /* replays */
        L"最近の配信アーカイブ",
        L"アーカイブがまだありません。先にボードで「確認」を押してねぺこ!",
        L"カードをクリックすると YouTube でアーカイブを再生します。",
        /* music */
        L"ぺこ ミュージックプレイヤー",
        L"exe の隣の「music」フォルダに MP3 / WAV を入れて\n"
        L"再起動してね。ぺこらが歌うぺこ!",
        L"再生中", L"(停止中)",
        /* countdown */
        L"次の配信まで",
        /* settings */
        L"画面サイズ", L"音量 ...", L"自動チェック",
        L"オフ", L"5分ごと", L"10分ごと",
        L"配信通知", L"桜の花びら", L"トレイに最小化",
        L"ミュージック", L"ボイス",
        L"ぺこらが配信中ぺこ! クリックして見に行こう!",
        L"Peko Board を開く", L"今すぐ確認", L"終了",
        { L"野うさぎ見習い", L"野うさぎ", L"立派な野うさぎ",
          L"エリート野うさぎ", L"伝説の野うさぎ" },
        L"今日のぺこみくじ: ",
        L"おしゃべり",
        L"おわり ▼",
        L"ねえねえ、おしゃべりしない？💬"
    },
    {   /* ---------------- Traditional Chinese ---------------- */
        L"Peko Board  " PEKO_VERSION_W L"  〜兔田佩克拉 粉絲看板〜",
        L"檢查", L"頻道", L"繁體中文", L"加入會員",
        L"正在連線 YouTube ...",
        L"連線錯誤 - 無法連線 YouTube。\n離線,或此處被封鎖 "
        L"(例如中國大陸),需要可用的 VPN / 代理。",
        L"按「檢查」載入佩克拉的直播與預定開播。",
        L"直播中", L"未開播", L"  (另有預定 %d 場)",
        L"目前沒有直播或預定開播。佩克拉休息中 peko!",
        L"聊天室", L"LIVE", L"預定", L"無封面",
        L"點「watch」開啟直播頁面,「聊天室」開啟即時聊天。",
        L"Peko Board " PEKO_VERSION_W L" - 仍在開發中 peko!",
        L"日本時間 (JST)", L"本機時間",
        L"當地時間",
        { L"星期日", L"星期一", L"星期二", L"星期三",
          L"星期四", L"星期五", L"星期六" },
        /* tabs */
        L"看板", L"直播回放", L"音樂",
        /* X card */
        L"佩克拉的 X (Twitter)",
        L"@usadapekora", L"最新貼文", L"#ぺこらーと",
        /* replays */
        L"最近的直播回放",
        L"還沒有回放資料。請先在看板按「檢查」peko!",
        L"點擊卡片即可在 YouTube 觀看回放。",
        /* music */
        L"佩克拉 音樂播放器",
        L"把 MP3 / WAV 放進 exe 旁邊的「music」資料夾,\n"
        L"再重新啟動。佩克拉唱歌給你聽 peko!",
        L"播放中", L"(未播放)",
        /* countdown */
        L"距離下次直播",
        /* settings */
        L"視窗大小", L"音量 ...", L"自動檢查",
        L"關閉", L"每 5 分鐘", L"每 10 分鐘",
        L"開播通知", L"櫻花花瓣", L"最小化到系統列",
        L"音樂", L"語音",
        L"佩克拉開播了 peko! 點擊前往觀看!",
        L"開啟 Peko Board", L"立即檢查", L"結束",
        { L"見習野兔", L"野兔", L"優秀野兔",
          L"菁英野兔", L"傳說野兔" },
        L"今日的 peko 籤: ",
        L"聊天",
        L"結束 ▼",
        L"吶吶，要不要聊天？💬"
    }
};

/* Lang button cycle order: Japanese -> English -> Traditional Chinese. */
static const int LANG_ORDER[LANG_COUNT] = { LANG_JA, LANG_EN, LANG_ZH };

int lang_next(int cur)
{
    for (int i = 0; i < LANG_COUNT; ++i)
        if (LANG_ORDER[i] == cur)
            return LANG_ORDER[(i + 1) % LANG_COUNT];
    return LANG_ORDER[0];
}
