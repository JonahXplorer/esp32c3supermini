#include "servo_voice_command.h"

#include <stdbool.h>
#include <string.h>

enum { MAX_COMMAND_BYTES = 256 };

typedef enum { PREFIX, WHOLE_WORDS } phrase_match_t;
typedef enum {
    NO_TARGET = 0,
    MASK = 1 << 0,
    HELMET = 1 << 1,
    SERVO = 1 << 2,
    ALL_TARGETS = MASK | HELMET | SERVO,
} target_t;

static bool ascii_letter(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

static size_t separator_bytes(const char *text)
{
    static const char *const utf8_separators[] = {
        "，", "。", "！", "？", "、", "：", "；", "　", "\xC2\xA0", "–", "—", NULL,
    };
    const unsigned char byte = *text;
    if (byte < 128) return strchr(" \t\r\n,.!?;:-", byte) != NULL;
    for (const char *const *separator = utf8_separators; *separator; ++separator) {
        const size_t length = strlen(*separator);
        if (strncmp(text, *separator, length) == 0) return length;
    }
    return 0;
}

static bool normalize(const char *transcript, char normalized[MAX_COMMAND_BYTES])
{
    size_t size = 0;
    bool space = false;
    const char *text = transcript;
    while (*text) {
        if ((size_t)(text - transcript) >= MAX_COMMAND_BYTES - 1) return false;
        const unsigned char byte = *text;
        const size_t separator = separator_bytes(text);
        if (separator) {
            space = size > 0;
            text += separator;
            continue;
        }

        // Normalize contractions but keep quotation marks out of the grammar.
        size_t apostrophe = 0;
        if (byte == '\'') apostrophe = 1;
        else if (byte >= 128 && strncmp(text, "’", sizeof("’") - 1) == 0) apostrophe = sizeof("’") - 1;
        if (apostrophe) {
            if (!size || space || !ascii_letter(normalized[size - 1]) || !ascii_letter(text[apostrophe])) return false;
            text += apostrophe;
            continue;
        }
        if (byte < 128 && !ascii_letter(byte) && !(byte >= '0' && byte <= '9')) return false;
        if (size + space >= MAX_COMMAND_BYTES - 1) return false;
        if (space) normalized[size++] = ' ';
        normalized[size++] = (char)(byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte);
        space = false;
        ++text;
    }
    normalized[size] = '\0';
    return size > 0;
}

static bool take_phrase(const char **text, const char *phrase, phrase_match_t match)
{
    const size_t length = strlen(phrase);
    if (strncmp(*text, phrase, length) != 0) return false;
    const char *rest = *text + length;
    if (match == WHOLE_WORDS && *rest && *rest != ' ') return false;
    *text = *rest == ' ' ? rest + 1 : rest;
    return true;
}

static bool take_one_of(const char **text, const char *const *phrases, phrase_match_t match)
{
    for (; *phrases; ++phrases) {
        if (take_phrase(text, *phrases, match)) return true;
    }
    return false;
}

static servo_voice_command_t parse_chinese(const char *text)
{
    static const char *const wake[] = {"jarvis", "jervis", "service", "llaves", "贾维斯", "贾维丝", NULL};
    static const char *const requests[] = {
        "请帮我", "请帮忙", "能不能帮我", "可以帮我", "能帮我", "麻烦你帮我", "麻烦帮我", "帮我", "帮忙", "请", NULL,
    };
    static const char *const targets[] = {
        "头盔面板", "头盔面罩", "脸部面板", "面具", "面罩", "面甲", "脸甲", "舵机",
        "face plate", "faceplate", "visor", "mask", "servos", "servo", NULL,
    };
    static const char *const open_verbs[] = {
        "抬起来", "抬起", "抬上去", "升起来", "升起", "掀起来", "掀开", "翻开", "打开", "开启", "开", NULL,
    };
    static const char *const close_verbs[] = {
        "放下来", "放下", "降下来", "降下", "拉下来", "拉下", "翻下来",
        "合上", "盖上", "扣上", "关闭", "关掉", "关上", "关", NULL,
    };
    static const char *const request_end[] = {"可以吗", "好吗", "行吗", "吗", NULL};

    (void)take_one_of(&text, wake, PREFIX);
    const bool polite = take_one_of(&text, requests, PREFIX);
    const bool target_first = take_phrase(&text, "把", PREFIX);
    if (target_first && !take_one_of(&text, targets, PREFIX)) return SERVO_VOICE_NONE;

    const bool open = take_one_of(&text, open_verbs, PREFIX);
    if (!open && !take_one_of(&text, close_verbs, PREFIX)) return SERVO_VOICE_NONE;
    (void)take_phrase(&text, "一下", PREFIX);
    if (!target_first && !take_one_of(&text, targets, PREFIX)) return SERVO_VOICE_NONE;
    (void)take_phrase(&text, "一下", PREFIX);
    (void)take_phrase(&text, "吧", PREFIX);
    if (polite) (void)take_one_of(&text, request_end, PREFIX);
    if (*text) return SERVO_VOICE_NONE;
    return open ? SERVO_VOICE_OPEN : SERVO_VOICE_CLOSE;
}

static bool one_edit_match(const char *word, size_t length, const char *target)
{
    const size_t target_length = strlen(target);
    if (length + 1 < target_length || length > target_length + 1) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!ascii_letter(word[i])) return false;
    }
    size_t shared = 0;
    while (shared < length && shared < target_length && word[shared] == target[shared]) ++shared;
    if (shared == length || shared == target_length) return true;
    if (length == target_length) {
        if (memcmp(word + shared + 1, target + shared + 1, length - shared - 1) == 0) return true;
        return shared + 1 < length && word[shared] == target[shared + 1] && word[shared + 1] == target[shared] &&
               memcmp(word + shared + 2, target + shared + 2, length - shared - 2) == 0;
    }
    if (length > target_length) return memcmp(word + shared + 1, target + shared, target_length - shared) == 0;
    return memcmp(word + shared, target + shared + 1, length - shared) == 0;
}

static const char *const fillers[] = {"uh", "um", "uhh", "umm", "erm", "ah", NULL};

static target_t take_english_target(const char **text)
{
    static const char *const qualifiers[] = {
        "both of the", "both the", "the two", "my two", "both", "two", "the", "my", "your", "our", "this", "that", "a", NULL,
    };
    static const char *const masks[] = {
        "iron man helmet faceplate", "iron mans faceplate", "iron man faceplate", "iron man face plate",
        "iron man face mask", "iron man helmet visor", "iron man visor", "iron man mask",
        "ironman faceplate", "ironman face mask", "ironman visor", "ironman mask",
        "helmets face plate", "helmets faceplate", "helmet faceplate", "helmet face plate",
        "helmet face mask", "helmets mask", "helmet mask",
        "helmets visor", "helmet visor", "helmet shield", "helmet panel",
        "face plates", "face plate", "face plait", "face place", "face play", "face shield", "faceshield", "face mask",
        "face guard", "face cover",
        "facial mask", "faceplates", "faceplate", "visors", "visor", "vis or", "viz or", "vizer", "visar",
        "masks", "mask", "masque", "头盔面板", "头盔面罩", "面罩", "面具", "脸甲", "面甲", NULL,
    };
    static const char *const helmets[] = {"iron man helmet", "ironman helmet", "helmets", "helmet", NULL};
    static const char *const servos[] = {
        "servo motors", "servo motor", "servomotors", "servomotor", "servos", "servo",
        "serv o", "ser vo", "sir vo", "sirvo", "survo", "serbo", "舵机", NULL,
    };
    while (take_one_of(text, fillers, WHOLE_WORDS)) {}
    (void)take_one_of(text, qualifiers, WHOLE_WORDS);
    while (take_one_of(text, fillers, WHOLE_WORDS)) {}
    if (take_one_of(text, masks, WHOLE_WORDS)) return MASK;
    if (take_one_of(text, helmets, WHOLE_WORDS)) return HELMET;
    if (take_one_of(text, servos, WHOLE_WORDS)) return SERVO;

    // Limit fuzzy matching to the distinctive mask nouns. "Task" and "server"
    // are actual words a user could say, so short or generic targets stay exact.
    const size_t length = strcspn(*text, " ");
    if (one_edit_match(*text, length, "visor") || one_edit_match(*text, length, "faceplate")) {
        *text += length;
        if (**text) ++*text;
        return MASK;
    }
    return NO_TARGET;
}

static bool take_particle(const char **text, const char *particle)
{
    static const char *const modifiers[] = {"all the way", "back", "fully", "completely", NULL};
    const char *rest = *text;
    (void)take_one_of(&rest, modifiers, WHOLE_WORDS);
    if (!take_phrase(&rest, particle, WHOLE_WORDS)) return false;
    *text = rest;
    return true;
}

static bool at_english_end(const char *text)
{
    static const char *const endings[] = {
        "for me", "right now", "if you can", "thank you", "please", "thanks", "now", "fully", "jarvis", NULL,
    };
    while (take_one_of(&text, endings, WHOLE_WORDS) || take_one_of(&text, fillers, WHOLE_WORDS)) {}
    return !*text;
}

typedef struct {
    const char *verb;
    const char *particle;
    servo_voice_command_t command;
    unsigned targets;
} english_rule_t;

#define OPEN(verb, particle) {verb, particle, SERVO_VOICE_OPEN, ALL_TARGETS}
#define CLOSE(verb, particle) {verb, particle, SERVO_VOICE_CLOSE, ALL_TARGETS}

static servo_voice_command_t parse_english(const char *text, bool question_mark)
{
    static const char *const prefixes[] = {
        "do me a favor and", "go ahead and", "would you", "could you", "can you", "will you",
        "id like you to", "i would like you to", "i want you to", "i need you to", "help me", "please", "kindly",
        "all right", "alright", "okay", "ok", "hey there", "hey", "jar ves", "jar viss", "jarvis", "jervis",
        "javis", "jarvice", "service", "llaves", "贾维斯", "贾维丝", NULL,
    };
    static const english_rule_t rules[] = {
        OPEN("open", "up"), OPEN("open", NULL), OPEN("mind opening", NULL), OPEN("do you mind opening", NULL),
        OPEN("op en", NULL), OPEN("opn", NULL), OPEN("oh pen", NULL),
        OPEN("raise", "up"), OPEN("raise", NULL), OPEN("rise", NULL), OPEN("mind raising", NULL),
        OPEN("lift", "up"), OPEN("lift", NULL), OPEN("retract", NULL), OPEN("mind lifting", NULL),
        OPEN("do you mind raising", NULL), OPEN("do you mind lifting", NULL),
        CLOSE("close", "up"), CLOSE("close", NULL), CLOSE("mind closing", NULL), CLOSE("do you mind closing", NULL),
        CLOSE("cloze", NULL), CLOSE("clothes", NULL), CLOSE("shut", NULL),
        CLOSE("lower", "down"), CLOSE("lower", NULL), CLOSE("mind lowering", NULL), CLOSE("do you mind lowering", NULL),
        CLOSE("drop", "down"), CLOSE("drop", NULL), CLOSE("seal", NULL),
        OPEN("flip", "up"), CLOSE("flip", "down"), OPEN("flip", "open"),
        CLOSE("flip", "shut"), CLOSE("flip", "closed"),
        OPEN("pop", "up"), OPEN("pop", "open"), OPEN("fold", "up"), CLOSE("fold", "down"),
        OPEN("pull", "up"), CLOSE("pull", "down"), OPEN("pull", "open"),
        OPEN("push", "up"), CLOSE("push", "down"), CLOSE("push", "closed"),
        OPEN("put", "up"), CLOSE("put", "down"), OPEN("bring", "up"), CLOSE("bring", "down"),
        OPEN("slide", "up"), CLOSE("slide", "down"), OPEN("move", "up"), CLOSE("move", "down"),
        OPEN("move", "to position b"), CLOSE("move", "to position a"),
        OPEN("move", "to b"), CLOSE("move", "to a"),
        OPEN("set", "to position b"), CLOSE("set", "to position a"),
        // A bare helmet can refer to its electronics. On/off needs a mask or servo.
        {"turn", "on", SERVO_VOICE_OPEN, MASK | SERVO}, {"turn", "off", SERVO_VOICE_CLOSE, MASK | SERVO},
        {"switch", "on", SERVO_VOICE_OPEN, MASK | SERVO}, {"switch", "off", SERVO_VOICE_CLOSE, MASK | SERVO},
        {"power", "on", SERVO_VOICE_OPEN, SERVO}, {"power", "off", SERVO_VOICE_CLOSE, SERVO},
    };

    while (take_one_of(&text, prefixes, WHOLE_WORDS) || take_one_of(&text, fillers, WHOLE_WORDS)) {}
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
        const english_rule_t *rule = &rules[i];
        const char *rest = text;
        if (!take_phrase(&rest, rule->verb, WHOLE_WORDS)) continue;
        while (take_one_of(&rest, fillers, WHOLE_WORDS)) {}
        const bool particle_first = rule->particle && take_particle(&rest, rule->particle);
        const target_t target = take_english_target(&rest);
        if (!(rule->targets & target)) continue;
        if (rule->particle && !particle_first && !take_particle(&rest, rule->particle)) continue;
        if (at_english_end(rest)) return rule->command;
    }

    // Also accept short commands such as "visor up", with an explicit target
    // and direction. A question such as "visor up?" can be a status query.
    if (!question_mark && take_english_target(&text) != NO_TARGET) {
        if (take_phrase(&text, "up", WHOLE_WORDS)) return at_english_end(text) ? SERVO_VOICE_OPEN : SERVO_VOICE_NONE;
        if (take_phrase(&text, "down", WHOLE_WORDS) && at_english_end(text)) return SERVO_VOICE_CLOSE;
    }
    return SERVO_VOICE_NONE;
}

#undef OPEN
#undef CLOSE

servo_voice_command_t servo_voice_command_parse(const char *transcript)
{
    char normalized[MAX_COMMAND_BYTES];
    if (!transcript || !normalize(transcript, normalized)) return SERVO_VOICE_NONE;
    const servo_voice_command_t chinese = parse_chinese(normalized);
    if (chinese != SERVO_VOICE_NONE) return chinese;
    return parse_english(normalized, strchr(transcript, '?') || strstr(transcript, "？"));
}
