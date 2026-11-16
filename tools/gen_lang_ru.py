#!/usr/bin/env python3
"""Generate app/lang_ru.h: the Russian screen texts (second language).

Each entry is the English text as it appears in the C sources and its Russian
replacement. Whole texts are looked up first; texts built at run time
("PINK 6", "LOAD SLOT 3") fall back to word-by-word. Russian capitals are
written as bytes 0x80..0x9F (А..Я) for app/font_lcd.h; Ё is written as Е.
Keep the Russian about as long as the English: labels must fit 56 px.
Usage: python tools/gen_lang_ru.py [out.h]
"""
import os, sys

RU = {
    # sections / pages
    'MIXER': 'МИКШЕР', 'MIX': 'МИКС', 'SEQUENCER': 'СЕКВЕНСОР', 'GLOBAL': 'ОБЩЕЕ', 'ACTIONS': 'ДЕЙСТВИЯ',
    'ARPEGGIATOR': 'АРПЕДЖИАТОР', 'ENGINE': 'ДВИЖОК', 'EDIT': 'ЗВУК', 'MASTER FX': 'ЭФФЕКТЫ', 'FM LAB': 'FM ЛАБ',
    'AMP ENV': 'ОГИБАЮЩАЯ', 'DX7 LFO': 'DX7 LFO', 'CHAIN': 'ЦЕПЬ', 'SAMPLER REC': 'ЗАПИСЬ СЭМПЛА',
    'SAMPLER CHOP': 'НАРЕЗКА', 'GLOBAL LFO': 'LFO', 'SCOPE': 'ОСЦИЛЛОГРАФ', 'SOUND': 'ЗВУК', 'BLUETOOTH': 'BLUETOOTH',
    'SAVE PROJECT': 'СОХРАНЕНИЕ', 'PATCH NAME': 'ИМЯ ЗВУКА', 'TRACK FX': 'ЭФФЕКТ ДОРОЖКИ', 'PRESETS': 'ПРЕСЕТЫ',
    'STEP EDIT': 'ШАГ', 'SEQ SETTINGS': 'НАСТРОЙКИ', 'SETTINGS': 'НАСТРОЙКИ', 'COMP': 'КОМПРЕССОР', 'FAST': 'БЫСТРО', 'MED': 'СРЕДНЕ', 'SLOW': 'МЕДЛЕННО', 'KEYS LATCH': 'УДЕРЖАНИЕ НОТ', 'LAST RUN PEAK': 'ПИК ПРОШЛОГО ЗАПУСКА', 'ENGINES': 'ДВИЖКИ', 'CLOCK / LENGTH': 'СИНХРО / ДЛИНА', 'MIDI ROUTING': 'MIDI',
    'MIDI ROUTING 1/2': 'MIDI 1/2', 'MIDI ROUTING 2/2': 'MIDI 2/2', 'TRACK ROLE': 'РОЛЬ ДОРОЖКИ',
    'TRACK ROLE / MIDI IN': 'РОЛЬ / MIDI ВХОД', 'SHARED FX': 'ОБЩИЕ ЭФФЕКТЫ', 'DELAY / SEQ TAP': 'ЗАДЕРЖКА',
    'MIDI HOST': 'MIDI УСТРОЙСТВА', 'SOURCE ENV': 'ОГИБАЮЩАЯ', 'TRACK LFO': 'LFO ДОРОЖКИ', 'VARIATION': 'ВАРИАЦИЯ',
    'PATTERN CHAIN ON': 'ЦЕПЬ ВКЛ', 'PATTERN CHAIN OFF': 'ЦЕПЬ ВЫКЛ', 'VOICE RATES': 'СКОРОСТИ ГОЛОСОВ',
    'VOICE LEVELS': 'УРОВНИ ГОЛОСОВ', 'PITCH RATES': 'ВЫСОТА: СКОРОСТИ', 'PITCH LEVELS': 'ВЫСОТА: УРОВНИ',
    'COMMON': 'ОБЩЕЕ', 'STEP DETAILS': 'ШАГ', 'START RECORDING': 'НАЧАТЬ ЗАПИСЬ', 'SAFE MODE': 'БЕЗОПАСНЫЙ РЕЖИМ',
    # mixer / tracks
    'VOLUME': 'ГРОМК', 'FILTER': 'ФИЛЬТР', 'SEND': 'ПОСЫЛ', 'STATE': 'СОСТ', 'MUTE': 'ТИХО', 'SOLO': 'СОЛО',
    'ON': 'ВКЛ', 'OFF': 'ВЫКЛ', 'BRIGHT': 'ЯРКО', 'TRACK': 'ДОРОЖКА', 'TRACKS': 'ДОРОЖКИ', 'LEVEL': 'УРОВЕНЬ',
    'TRACK 1': 'ДОРОЖКА 1', 'TRACK 2': 'ДОРОЖКА 2', 'TRACK 3': 'ДОРОЖКА 3', 'TRACK 4': 'ДОРОЖКА 4',
    'TRACK 5': 'ДОРОЖКА 5', 'TRACK 6': 'ДОРОЖКА 6', 'TRACK 7': 'ДОРОЖКА 7', 'TRACK 8': 'ДОРОЖКА 8',
    'KEYS 1-8 MUTE  9-16 SOLO': 'КЛАВИШИ 1-8 ТИХО  9-16 СОЛО',
    # sequencer
    'VELOCITY': 'СИЛА', 'GATE': 'ДЛИНА', 'PROB': 'ШАНС', 'NOTE': 'НОТА', 'NOTES': 'НОТЫ', 'STEPS': 'ШАГИ',
    'STEP': 'ШАГ', 'LENGTH': 'ДЛИНА', 'LAYER': 'СЛОЙ', 'CHORD': 'АККОРД', 'TIE': 'ЛИГА', 'LOCK': 'ЗАМОК',
    'VALUE': 'ЗНАЧЕНИЕ', 'SWING': 'СВИНГ', 'CLICK': 'МЕТРОНОМ', 'COUNT IN': 'ОТСЧЕТ', 'CLOCK': 'СИНХРО',
    'CLK IN': 'СИНХ ВХ', 'CLK OUT': 'СИНХ ВЫХ', 'PLAY': 'ИГРА', 'PAUSE': 'ПАУЗА', 'REC': 'ЗАПИСЬ', 'LIVE': 'ЖИВАЯ',
    'LIVE REC': 'ЗАПИСЬ ВЖИВУЮ', 'STEP REC': 'ЗАПИСЬ ПО ШАГАМ', 'COUNT IN': 'ОТСЧЕТ', 'NEXT LENGTH': 'ДЛИНА ДАЛЕЕ',
    'FOLLOW': 'СЛЕДИТЬ', 'PAGE': 'СТРАНИЦА', 'EMPTY STEP': 'ПУСТОЙ ШАГ', 'FIELD': 'ПОЛЕ', 'PROJECT': 'ПРОЕКТ',
    'REPEAT': 'ПОВТОР', 'MUTATE': 'МУТАЦИЯ', 'HUMAN': 'ЖИВОСТЬ', 'RATCHET': 'ДРОБЬ', 'CHANCE': 'ШАНС',
    '1 BAR': '1 ТАКТ', 'INT': 'ВНУТР', 'WAIT': 'ЖДЕМ', 'LOST': 'ПОТЕРЯ', 'WAIT MIDI CLOCK': 'ЖДЕМ MIDI СИНХРО',
    # global
    'OUTPUT': 'ВЫХОД', 'BPM': 'BPM', 'LOW CUT': 'СРЕЗ НИЗА', 'LANGUAGE': 'ЯЗЫК', 'ENGLISH': 'ENGLISH',
    'RUSSIAN': 'РУССКИЙ', 'CPU ': 'ЦП ',
    'THEME': 'ТЕМА', 'ORGANIC': 'ПРИРОДА', 'CLASSIC': 'КЛАССИКА', 'MATRIX': 'МАТРИЦА', 'INFO': 'ИНФО',
    'RAM FREE': 'СВОБОДНО', 'CPU PEAK': 'ПИК ЦП', 'BATTERY': 'БАТАРЕЯ', 'ENGINES': 'ДВИЖКИ', 'TO': 'В',
    'SCOPE': 'ОСЦИЛЛОГРАФ', 'CHOIR': 'ХОР',
    # FAQ (INFO > ALGORITHM), line by line
    'FAQ': 'СПРАВКА', 'MORE BUTTONS': 'ЕЩЁ КНОПКИ', 'ONE STEP BACK': 'НАЗАД НА ШАГ', 'HOLD: SCOPE': 'ДЕРЖАТЬ: ОСЦИЛЛОГРАФ', 'STEPS  HOLD: CHAIN': 'ШАГИ  ДЕРЖАТЬ: ЦЕПЬ', 'SOUND  HOLD: FM LAB': 'ЗВУК  ДЕРЖАТЬ: FM ЛАБ', 'EFFECTS': 'ЭФФЕКТЫ', 'TRACK FX  HOLD: MASTER': 'ДОРОЖКА  ДЕРЖАТЬ: МАСТЕР', 'LFO 1 AND 2': 'LFO 1 И 2', 'COPY / PASTE': 'КОПИЯ / ВСТАВКА', 'SAVE THE PROJECT': 'СОХРАНИТЬ ПРОЕКТ', 'START / STOP': 'СТАРТ / СТОП', 'RECORD': 'ЗАПИСЬ', 'OCTAVE DOWN': 'ОКТАВА ВНИЗ', 'OCTAVE UP': 'ОКТАВА ВВЕРХ', 'SELECT: SLOT': 'SELECT: СЛОТ', 'ALGORITHM: LETTER PLACE': 'ALGORITHM: МЕСТО БУКВЫ', 'K1 0-9  K2 A-M  K3 N-Z': 'Р1 0-9  Р2 A-M  Р3 N-Z', 'K4 SAVE / LOAD': 'Р4 SAVE / LOAD', 'BUTTONS': 'КНОПКИ', 'HOME: ONE STEP BACK': 'HOME: НАЗАД НА ШАГ', 'HOLD HOME: SCOPE': 'ДЕРЖАТЬ HOME: ОСЦИЛЛОГРАФ',
    'SEQ: STEPS  HOLD: CHAIN': 'SEQ: ШАГИ  ДЕРЖАТЬ: ЦЕПЬ', 'STEPS > PAGES  HOLD: CHAIN': 'ШАГИ > СТРАНИЦЫ  ДЕРЖАТЬ: ЦЕПЬ',
    'RATCH': 'ДРОБЬ', 'SPACE': 'ИНТЕРВАЛ', 'TRANSPOSE': 'ТРАНСПОЗ', 'EDIT: SOUND  FX: EFFECTS': 'EDIT: ЗВУК  FX: ЭФФЕКТЫ',
    'LFO: LFO  ARP: ARPEGGIO': 'LFO: LFO  ARP: АРПЕДЖИО', 'GLO: SETTINGS  SEL: COPY': 'GLO: НАСТРОЙКИ  SEL: КОПИЯ',
    'SAVE: SAVE  PLAY / REC': 'SAVE: СОХРАНИТЬ  PLAY / REC', 'SELECT AND KNOBS': 'SELECT И РУЧКИ',
    'SELECT: WHERE (TRACK, STEP,': 'SELECT: ГДЕ (ДОРОЖКА, ШАГ,', 'ROW, PAGE)': 'РЯД, СТРАНИЦА)',
    'KNOBS 1-4: WHAT YOU SEE': 'РУЧКИ 1-4: ТО, ЧТО ВИДНО', 'COLOURS MATCH THE BARS': 'ЦВЕТ = ЦВЕТ ПОЛОСКИ',
    'ALGORITHM: OPENS A PAGE': 'ALGORITHM: ОТКРЫВАЕТ', 'OR MOVES BY ROWS': 'ИЛИ ХОДИТ ПО РЯДАМ',
    'PRESETS: ENGINE / LENGTH': 'PRESETS: ДВИЖОК / ДЛИНА', 'SELECT: TRACK': 'SELECT: ДОРОЖКА', 'PRESETS: ENGINE': 'PRESETS: ДВИЖОК',
    'ALGORITHM: SOUND': 'ALGORITHM: ЗВУК', 'K1 VOLUME  K2 FILTER': 'Р1 ГРОМКОСТЬ  Р2 ФИЛЬТР',
    'K3 SEND  K4 ON/MUTE/SOLO': 'Р3 ПОСЫЛ  Р4 ВКЛ/ТИХО/СОЛО', 'SELECT: STEP  ALGORITHM: ROW': 'SELECT: ШАГ  ALGORITHM: РЯД',
    'K1 VELOCITY  K2 GATE': 'Р1 СИЛА  Р2 ДЛИНА', 'K1 VELOCITY  K2 LENGTH': 'Р1 СИЛА  Р2 ДЛИНА', 'OUT OF RANGE': 'ВНЕ ДИАПАЗОНА', 'ALGORITHM OPENS THE DEVICE LIST': 'АЛГОРИТМ: СПИСОК УСТРОЙСТВ', 'K3 CHANCE  K4 NOTE': 'Р3 ШАНС  Р4 НОТА', 'KEYS WRITE THE NOTE': 'КЛАВИШИ ПИШУТ НОТУ',
    'REC: LIVE WHILE PLAYING,': 'REC: ВЖИВУЮ ПРИ ИГРЕ,', 'STEP BY STEP WHEN STOPPED': 'ПО ШАГАМ ПРИ ОСТАНОВКЕ',
    'HOLD A KEY: A LONG NOTE': 'ДЕРЖИ КЛАВИШУ: ДЛИННАЯ НОТА', 'SOUND (EDIT)': 'ЗВУК (EDIT)',
    'SELECT: PAGES (ROUND)': 'SELECT: СТРАНИЦЫ ПО КРУГУ', 'DX7: TONE / ENV WINDOWS': 'DX7: ОКНА TONE / ENV',
    'HOLD EDIT: FM LAB': 'ДЕРЖАТЬ EDIT: FM ЛАБ', '808 / SAMPLER: SELECT PAD,': '808 / СЭМПЛЕР: SELECT ПЭД,',
    'ALGORITHM: PAGES': 'ALGORITHM: СТРАНИЦЫ', 'SAMPLER: REC RECORDS A PAD': 'СЭМПЛЕР: REC ПИШЕТ ПЭД',
    'FX AND LFO': 'FX И LFO', 'FX: SELECT EFFECT, K1-K4 IT': 'FX: SELECT ЭФФЕКТ, Р1-Р4 ЕГО', 'MIX 0 = OFF': 'МИКС 0 = ВЫКЛ',
    'LFO: SELECT LFO 1 / 2': 'LFO: SELECT LFO 1 / 2', 'K1 TARGET  K2 RATE': 'Р1 ЦЕЛЬ  Р2 ТЕМП', 'K3 DEPTH  K4 WAVE': 'Р3 ГЛУБИНА  Р4 ФОРМА',
    'K1 SLOT  K2 CURSOR': 'Р1 СЛОТ  Р2 КУРСОР', 'K3 LETTER  K4 SAVE / LOAD': 'Р3 БУКВА  Р4 SAVE / LOAD',
    'SAVE BUTTON DOES IT': 'КНОПКА SAVE ВЫПОЛНЯЕТ', 'CHANGES SAVE THEMSELVES': 'ПРАВКИ СОХРАНЯЮТСЯ САМИ',
    '3 S AFTER, NOT WHILE PLAYING': 'ЧЕРЕЗ 3 С, НЕ ВО ВРЕМЯ ИГРЫ', 'SELECT: THE ROW': 'SELECT: ПУНКТ', 'KNOBS: ITS VALUES': 'РУЧКИ: ЕГО ЗНАЧЕНИЯ',
    'ALGORITHM: OPENS ITS PAGE': 'ALGORITHM: ЕГО СТРАНИЦА', '(BLUETOOTH, SCOPE, INFO)': '(BLUETOOTH, ОСЦИЛЛОГРАФ, ИНФО)',
    'SAVE: SAVES': 'SAVE: СОХРАНИТЬ', 'UPDATE AND SAFE START': 'ОБНОВЛЕНИЕ И ЗАПУСК', 'THE UPDATE OFTEN NEEDS': 'ОБНОВЛЕНИЕ ЧАСТО СО',
    'A SECOND TRY: SAME PAGE,': 'ВТОРОГО РАЗА: ТА ЖЕ СТРАНИЦА,', 'CONNECT AND START AGAIN': 'CONNECT И START ЕЩЕ РАЗ',
    'HOLD OCT- AT POWER ON:': 'ДЕРЖИ OCT- ПРИ ВКЛЮЧЕНИИ:', 'SAFE START, NO PROJECTS': 'БЕЗОПАСНЫЙ СТАРТ, БЕЗ ПРОЕКТОВ',
    'CONNECT': 'СВЯЗАТЬ', 'SCAN': 'ПОИСК', 'DISCONNECT': 'ОТКЛЮЧИТЬ', 'MIDI OUT': 'MIDI ВЫХОД', 'MIDI SYNC': 'MIDI СИНХРО',
    'BT AUDIO': 'BT ЗВУК', 'BLE MIDI': 'BLE MIDI', 'HOST': 'ХОСТ', 'DEVICE': 'ВЕДОМЫЙ', 'WAITING FOR DEVICE': 'ЖДУ УСТРОЙСТВО', 'UNNAMED MIDI DEVICE': 'MIDI БЕЗ ИМЕНИ', 'NO DEVICES - SCAN': 'НЕТ УСТРОЙСТВ - ПОИСК', 'RADIO AVAILABLE ON DEVICE': 'РАДИО ТОЛЬКО НА ПРИБОРЕ',
    # sel / save
    'COPY': 'КОПИЯ', 'PASTE': 'ВСТАВИТЬ', 'CLEAR': 'СТЕРЕТЬ', 'FILL': 'ЗАПОЛНИТЬ', 'UNDO': 'ОТМЕНА', 'NAME': 'ИМЯ',
    'SAVE': 'SAVE', 'LOAD': 'ЗАГРУЗИТЬ', 'SLOT': 'СЛОТ', 'CURSOR': 'КУРСОР', 'LETTER': 'БУКВА', 'ACTION': 'ДЕЙСТВИЕ',
    'REPLACE DATA?': 'ЗАМЕНИТЬ?', 'PROJECT SAVED': 'ПРОЕКТ СОХРАНЕН', 'PROJECT LOADED': 'ПРОЕКТ ЗАГРУЖЕН',
    'SAVED': 'СОХРАНЕНО', 'SAVING': 'СОХРАНЯЮ', 'EMPTY SLOT': 'ПУСТОЙ СЛОТ', 'EMPTY': 'ПУСТО',
    'STEP COPIED': 'ШАГ СКОПИРОВАН', 'STEP PASTED': 'ШАГ ВСТАВЛЕН', 'PAGE COPIED': 'СТРАНИЦА СКОПИРОВАНА',
    'PAGE PASTED': 'СТРАНИЦА ВСТАВЛЕНА', 'TRACK COPIED': 'ДОРОЖКА СКОПИРОВАНА', 'NOTHING TO UNDO': 'НЕЧЕГО ОТМЕНЯТЬ',
    # sound editors and effects
    'CUTOFF': 'СРЕЗ', 'RESO': 'РЕЗОН', 'DECAY': 'СПАД', 'ATTACK': 'АТАКА', 'SUSTAIN': 'УДЕРЖ', 'RELEASE': 'ЗАТУХ',
    'ACCENT': 'АКЦЕНТ', 'WAVE': 'ВОЛНА', 'TUNE': 'СТРОЙ', 'DRIVE': 'ДРАЙВ', 'SLIDE': 'СКОЛЬЖ', 'TONE': 'ТОН',
    'TYPE': 'ТИП', 'TIME': 'ВРЕМЯ', 'FEEDBACK': 'ОБР.СВЯЗЬ', 'FDBK': 'ОБР.СВ', 'DEPTH': 'ГЛУБИНА', 'RATE': 'ТЕМП',
    'TARGET': 'ЦЕЛЬ', 'SHAPE': 'ФОРМА', 'MODE': 'РЕЖИМ', 'HOLD': 'УДЕРЖ', 'OCTAVES': 'ОКТАВЫ', 'HUMANIZE': 'ЖИВОСТЬ',
    'RANDOM': 'СЛУЧАЙНО', 'PLAYED': 'КАК ИГРАЛ', 'UP': 'ВВЕРХ', 'DOWN': 'ВНИЗ', 'UPDN IN': 'ВВЕРХ-ВНИЗ',
    'UPDN EX': 'ВВЕРХ-ВНИЗ 2', 'MIX': 'МИКС', 'REVERB': 'РЕВЕРБ', 'DELAY': 'ЗАДЕРЖКА', 'CHORUS': 'ХОРУС',
    'PHASER': 'ФЭЙЗЕР', 'RING': 'КОЛЬЦО', 'EQ': 'ЭКВ', 'SOFT': 'МЯГКО', 'HARD': 'ЖЕСТКО', 'FOLD': 'СГИБ',
    'CRUSH': 'КРОШКА', 'LOW': 'НИЗ', 'MID': 'СЕРЕДИНА', 'HIGH': 'ВЕРХ', 'FREQ': 'ЧАСТОТА', 'SINE': 'СИНУС',
    'SQUARE': 'КВАДРАТ', 'TRIANGLE': 'ТРЕУГ', 'SAW': 'ПИЛА', 'SAW UP': 'ПИЛА ВВЕРХ', 'SAW DOWN': 'ПИЛА ВНИЗ',
    'TRI': 'ТРЕУГ', 'SQR': 'КВАДР', 'NOISE': 'ШУМ', 'SOURCE': 'ИСТОЧНИК', 'THRESH': 'ПОРОГ', 'MONITOR': 'СЛУШАТЬ',
    'ARMED': 'ГОТОВ', 'START': 'СТАРТ', 'SLICES': 'КУСКИ', 'EVEN': 'РОВНО', 'TRANSIENT': 'ПО УДАРАМ',
    'SAMPLE SAVED': 'СЭМПЛ СОХРАНЕН', 'RAM FULL': 'ПАМЯТЬ ПОЛНА', 'SAMPLE MEMORY FULL': 'НЕТ МЕСТА ДЛЯ СЭМПЛОВ',
    'ENGINE MEMORY FULL': 'НЕ ХВАТАЕТ ПАМЯТИ', 'SOURCE MEMORY FULL': 'НЕ ХВАТАЕТ ПАМЯТИ', 'VOICES': 'ГОЛОСА',
    'BASS DRUM': 'БОЧКА', 'SNARE DRUM': 'МАЛЫЙ', 'LOW TOM': 'ТОМ НИЗ', 'MID TOM': 'ТОМ СРЕД', 'HIGH TOM': 'ТОМ ВЕРХ',
    'RIM SHOT': 'РИМШОТ', 'HAND CLAP': 'ХЛОПОК', 'COWBELL': 'КОЛОКОЛ', 'CYMBAL': 'ТАРЕЛКА', 'OPEN HAT': 'ХЭТ ОТКР',
    'CLOSED HAT': 'ХЭТ ЗАКР', 'KIT': 'НАБОР', 'PAN': 'ПАНОРАМА', 'RETURN': 'ВОЗВРАТ', 'FINE': 'ТОЧНО', 'RATIO': 'ОТНОШ',
    'DETUNE': 'РАССТРОЙ', 'VOICE MODE': 'ГОЛОСА', 'POLY': 'ПОЛИ', 'MONO': 'МОНО', 'DIVISION': 'ДОЛЯ',
    # MICRO D
    'WAVES': 'ВОЛНЫ', 'THICK': 'ТОЛЩЕ', 'MOVE': 'ДВИЖ', 'SHARK': 'АКУЛА', 'AIR': 'ВОЗДУХ', 'KEY': 'КЛАВИША',
    'PEAK': 'ПИК', 'DIRT': 'ГРЯЗЬ', 'FALL': 'СПАД', 'RESTART': 'ЗАНОВО', 'SPEED': 'СКОРОСТЬ', 'PINK': 'РОЗОВЫЙ',
    'WHITE': 'БЕЛЫЙ', 'PITCH': 'ВЫСОТА', 'ALL': 'ВСЕ', 'ALL 1': 'ВСЕ 1', 'DARK': 'ТЕМНО', 'KEY DOWN': 'НАЖАЛ',
    'KEY UP': 'ОТПУСТИЛ', '= YOUR SOUND': '= ТВОЙ ЗВУК', 'TWO WAVES DRIFT = THICK': 'ДВЕ ВОЛНЫ ПЛЫВУТ = ТОЛЩЕ',
    'ONE WAVE: TURN THICK UP': 'ОДНА ВОЛНА: КРУТИ ТОЛЩЕ', 'ONE FILTER FOR ALL': 'ОДИН ФИЛЬТР НА ВСЕ',
    # bluetooth / midi
    'CONNECTED': 'ПОДКЛЮЧЕНО', 'CONNECTING': 'ПОДКЛЮЧАЮ', 'SCANNING': 'ПОИСК', 'IDLE': 'ЖДУ', 'TIMED OUT': 'НЕТ ОТВЕТА',
    '> SCAN': '> ПОИСК', '> DISCONNECT': '> ОТКЛЮЧИТЬ', '> CONNECT SELECTED': '> ПОДКЛЮЧИТЬ', '> SETTINGS': '> НАСТРОЙКИ',
    'NO DEVICES - SELECT SCAN BELOW': 'НЕТ УСТРОЙСТВ - ВЫБЕРИ ПОИСК', 'LOOKING FOR MIDI DEVICES...': 'ИЩУ MIDI УСТРОЙСТВА...',
    'LOCAL': 'ЛОКАЛЬНО', 'ROLE': 'РОЛЬ', 'HOST': 'ХОСТ', 'AUTO': 'АВТО',
    'BUSY - RETRY': 'ЗАНЯТО - ЕЩЕ РАЗ', 'ERROR': 'ОШИБКА', 'DONE': 'ГОТОВО',
    # BINAURAL (preset names DELTA..FOCUS stay Latin: 'BETA' is also in the INFO version line)
    'BINAURAL': 'БИНАУРАЛ', 'BEATS': 'РИТМЫ', 'HEADPHONES': 'НАУШНИКИ', 'HEADPHONES ONLY': 'ТОЛЬКО НАУШНИКИ',
    'TIMER': 'ТАЙМЕР', 'PRESET': 'ПРЕСЕТ', 'BEAT': 'РИТМ', 'CARRIER': 'НЕСУЩАЯ', 'WIDTH': 'ШИРИНА',
    'FEED': 'КРОСС', 'XFEED': 'КРОСС', 'CROSSFEED': 'КРОССФИД', 'RAIN': 'ДОЖДЬ', 'END': 'КОНЕЦ', 'CUSTOM': 'СВОЙ',
    '3D SOUND': '3D ЗВУК',
}

def encode(ru):
    out = []
    for ch in ru.replace('Ё', 'Е'):
        o = ord(ch)
        if 0x410 <= o <= 0x42f: out.append('\\x%02x' % (0x80 + o - 0x410))
        elif o < 128: out.append('\\"' if ch == '"' else ch)
        else: raise SystemExit('cannot encode %r in %r' % (ch, ru))
    # \x escapes swallow following hex digits: split the literal there
    s, prev_hex = '', False
    for piece in out:
        if prev_hex and piece[0] in '0123456789abcdefABCDEF': s += '""'
        s += piece; prev_hex = piece.startswith('\\x')
    return s

def main():
    dst = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'app', 'lang_ru.h')
    keys = sorted(RU)
    lines = ['/* Generated by tools/gen_lang_ru.py: English screen text -> Russian, sorted',
             ' * by the English (strcmp order). Russian capitals are bytes 0x80..0x9F. */',
             '#ifndef FM1_LANG_RU_H', '#define FM1_LANG_RU_H',
             'static const char *const lang_ru[][2] = {']
    for k in keys:
        lines.append('    {"%s", "%s"},' % (k.replace('"', '\\"'), encode(RU[k])))
    lines += ['};', '#endif', '']
    with open(dst, 'w', newline='\n', encoding='ascii') as f: f.write('\n'.join(lines))
    print('lang_ru.h: %d texts' % len(keys))

if __name__ == '__main__':
    main()
