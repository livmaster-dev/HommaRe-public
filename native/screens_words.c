/* screens_words.c - the native screens' own words in the game's language.
 *
 * The views say most things in the game's words (its text files, ns_label);
 * the rest is written at the call as Russian and English (ns_tr). The other
 * languages come from the table below, by the English words (and the Russian
 * ones where one English word has two Russian forms); a language or a phrase
 * it lacks stays English. The language is the game's: its difficulties' names
 * say which install it is. */
#include "native_screens.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int ns_lang;
int ns_ru;

int ns_set_lang(const char *easy, const char *hard) {
    const unsigned char *u = (const unsigned char *)easy;
    if (u[0] >= 0xd0 && u[0] <= 0xd3)
        ns_lang = NS_RU; /* Cyrillic */
    else if (!strncmp(easy, "Leicht", 6))
        ns_lang = NS_DE;
    else if (!strncmp(easy, "Facile", 6))
        ns_lang = hard && !strncmp(hard, "Dura", 4) ? NS_IT : NS_FR;
    else if (!strncmp(easy, "\xc5\x81" "atw", 5)) /* Łatwy */
        ns_lang = NS_PL;
    else if ((easy[0] == 'l' || easy[0] == 'L') && !strncmp(easy + 1, "ehk", 3)) /* lehká */
        ns_lang = NS_CS;
    /* Könnyű; the Hungarian base game's data has no easy one, so its first is
     * "Normál". */
    else if (!strncmp(easy, "K\xc3\xb6nny", 6) || !strcmp(easy, "Norm\xc3\xa1l"))
        ns_lang = NS_HU;
    else if (!strncmp(easy, "F\xc3\xa1" "cil", 6)) /* Fácil */
        ns_lang = NS_ES;
    else if (!strncmp(easy, "\xe7\xae\x80\xe5\x8d\x95", 6)) /* 简单 */
        ns_lang = NS_ZH;
    else
        ns_lang = NS_EN;
    return ns_ru = ns_lang == NS_RU;
}

int ns_read_lang(Scratch *s) {
    char easy[64], hard[64];
    ns_difficulty_name(s, 0, easy, sizeof easy);
    ns_difficulty_name(s, 2, hard, sizeof hard);
    return ns_set_lang(easy, hard);
}

const char *ns_lang_code(void) {
    static const char *const code[NS_LANGS] = {"en", "ru", "de", "fr", "pl",
                                               "cs", "hu", "it", "es", "zh-Hans"};
    return code[ns_lang];
}

typedef struct {
    const char *en, *ru;              /* ru: only where `en` has two Russian forms */
    const char *tr[NS_LANGS - NS_DE]; /* de, fr, pl, cs, hu, it, es, zh-Hans */
} Words;

static const Words WORDS[] = {
    /* Main menu and its panels */
    {"Main Menu", 0,
     {"Hauptmenü", "Menu principal", "Główne menu", "Hlavní nabídka", "Főmenü", "Menu principale",
      "Menú principal", "主菜单"}},
    {"Single Player", 0,
     {"Einzelspieler", "Solo", "Jeden gracz", "Jeden hráč", "Egyjátékos", "Giocatore singolo",
      "Un jugador", "单人游戏"}},
    {"Multiplayer", 0,
     {"Mehrspieler", "Multijoueur", "Wielu graczy", "Hra více hráčů", "Többjátékos",
      "Multigiocatore", "Multijugador", "多人游戏"}},
    {"Options", 0,
     {"Optionen", "Options", "Opcje", "Nastavení", "Beállítások", "Opzioni", "Opciones", "选项"}},
    {"Credits", 0,
     {"Mitwirkende", "Crédits", "Twórcy", "Autoři", "Stáblista", "Riconoscimenti", "Créditos",
      "制作组"}},
    {"Exit", 0,
     {"Beenden", "Quitter", "Wyjście", "Ukončit", "Kilépés", "Esci", "Salir", "退出"}},
    {"Campaign", 0,
     {"Kampagne", "Campagne", "Kampania", "Kampaň", "Hadjárat", "Campagna", "Campaña", "战役"}},
    {"Campaigns", 0,
     {"Kampagnen", "Campagnes", "Kampanie", "Kampaně", "Hadjáratok", "Campagne", "Campañas", "战役"}},
    {"Scenarios", 0,
     {"Szenarien", "Scénarios", "Scenariusze", "Scénáře", "Forgatókönyvek", "Scenari", "Escenarios",
      "剧情"}},
    {"Load Game", 0,
     {"Spiel laden", "Charger une partie", "Wczytaj grę", "Načíst hru", "Játék betöltése",
      "Carica partita", "Cargar partida", "载入游戏"}},
    {"Custom Campaign", 0,
     {"Eigene Kampagne", "Campagne perso", "Kampania gracza", "Vlastní kampaň", "Saját hadjárat",
      "Campagna utente", "Campaña de usuario", "自定义战役"}},
    {"Hall of Fame", 0,
     {"Ruhmeshalle", "Classement", "Galeria sław", "Síň slávy", "Dicsőségcsarnok", "Hall of Fame",
      "Galería de la Fama", "名望堂"}},
    {"Hot Seat", 0,
     {"Hot-Seat", "Hot Seat", "Hot Seat", "Horké křeslo", "Hot-Seat", "Hot-seat", "Hot-seat",
      "热座"}},
    {"LAN", 0,
     {"LAN", "Réseau local", "Sieć lokalna", "Místní síť", "Helyi hálózat", "Rete locale",
      "Red local", "局域网"}},
    {"Standard", 0,
     {"Standard", "Standard", "Standardowa", "Standardní", "Standard", "Standard", "Estándar",
      "标准"}},
    {"Duel", 0,
     {"Duell", "Duel", "Pojedynek", "Souboj", "Párbaj", "Duello", "Duelo", "对决"}},
    {"Duel 3x3", 0,
     {"Duell 3x3", "Duel 3x3", "Pojedynek 3x3", "Souboj 3x3", "Párbaj 3x3", "Duello 3x3",
      "Duelo 3x3", "对决 3x3"}},
    {"Missions", 0,
     {"Missionen", "Missions", "Misje", "Mise", "Küldetések", "Missioni", "Misiones", "任务"}},
    {"Difficulty", 0,
     {"Schwierigkeit", "Difficulté", "Poziom trudności", "Obtížnost", "Nehézségi szint",
      "Difficoltà", "Dificultad", "游戏难度"}},
    {"Profile", 0,
     {"Profil", "Profil", "Profil", "Profil", "Profil", "Profilo", "Perfil", "档案"}},
    {"Change", 0,
     {"Wechseln", "Changer", "Zmień", "Změnit", "Csere", "Cambia", "Cambiar", "更换"}},
    {"New profile", 0,
     {"Neues Profil", "Nouveau profil", "Nowy profil", "Nový profil", "Új profil", "Nuovo profilo",
      "Nuevo perfil", "新档案"}},
    {"Profile name", 0,
     {"Profilname", "Nom du profil", "Nazwa profilu", "Název profilu", "Profil neve",
      "Nome del profilo", "Nombre del perfil", "档案名称"}},
    {"2 to 10 letters, digits or spaces", 0,
     {"2 bis 10 Buchstaben, Ziffern oder Leerzeichen", "De 2 à 10 lettres, chiffres ou espaces",
      "Od 2 do 10 liter, cyfr lub spacji", "2 až 10 písmen, číslic nebo mezer",
      "2–10 betű, számjegy vagy szóköz", "Da 2 a 10 lettere, cifre o spazi",
      "De 2 a 10 letras, números o espacios", "2 到 10 个字母、数字或空格"}},
    {"Back", 0,
     {"Zurück", "Retour", "Wróć", "Zpět", "Vissza", "Indietro", "Volver", "返回"}},
    {"Start", 0,
     {"Start", "Commencer", "Start", "Start", "Indítás", "Inizia", "Empezar", "开始"}},
    {"Next", 0,
     {"Nächster", "Suivant", "Następny", "Další", "Következő", "Successivo", "Siguiente", "下一个"}},
    {"Previous", 0,
     {"Vorheriger", "Précédent", "Poprzedni", "Předchozí", "Előző", "Precedente", "Anterior",
      "上一个"}},

    /* Custom game, hot seat, network game */
    {"Custom Game", 0,
     {"Einzelszenario", "Scénario isolé", "Gra własna", "Vlastní hra", "Egyedi játék",
      "Partita personalizzata", "Partida personalizada", "自定义游戏"}},
    {"Hot Seat: New Game", 0,
     {"Hot-Seat: Spiel erstellen", "Hot Seat : nouvelle partie", "Hot-seat: nowa gra",
      "Horké křeslo: nová hra", "Hot-Seat: új játék", "Hot-seat: nuova partita",
      "Hot-seat: nueva partida", "热座：新游戏"}},
    {"New Network Game", 0,
     {"Neues Netzwerkspiel", "Nouvelle partie en réseau", "Nowa gra sieciowa", "Nová síťová hra",
      "Új hálózati játék", "Nuova partita in rete", "Nueva partida en red", "新建网络游戏"}},
    {"Name", 0,
     {"Name", "Nom", "Nazwa", "Název", "Név", "Nome", "Nombre", "名称"}},
    {"Goal", 0,
     {"Ziel", "Objectif", "Cel", "Cíl", "Cél", "Obiettivo", "Objetivo", "目标"}},
    {"Goal:", 0,
     {"Ziel:", "Objectif :", "Cel:", "Cíl:", "Cél:", "Obiettivo:", "Objetivo:", "目标："}},
    {"Players", "Игроков",
     {"Spieler", "Joueurs", "Gracze", "Hráči", "Játékosok", "Giocatori", "Jugadores", "玩家"}},
    {"Players", "Игроки",
     {"Spieler", "Joueurs", "Gracze", "Hráči", "Játékosok", "Giocatori", "Jugadores", "玩家"}},
    {"Players:", 0,
     {"Spieler:", "Joueurs :", "Liczba graczy:", "Hráči:", "Játékosok:", "Giocatori:", "Jugadores:",
      "玩家："}},
    {"Size", 0,
     {"Größe", "Taille", "Rozmiar", "Velikost", "Méret", "Dimensione", "Tamaño", "大小"}},
    {"Size:", 0,
     {"Größe:", "Taille :", "Rozmiar mapy:", "Velikost:", "Méret:", "Dimensione:", "Tamaño:",
      "大小："}},
    {"Underground:", 0,
     {"Untergrund:", "Souterrain :", "Podziemia:", "Podzemí:", "Föld alatt:", "Sotterraneo:",
      "Subterráneo:", "地下："}},
    {"yes", 0,
     {"ja", "oui", "tak", "ano", "igen", "sì", "sí", "是"}},
    {"no", 0,
     {"nein", "non", "nie", "ne", "nem", "no", "no", "否"}},
    {"Map description", 0,
     {"Beschreibung der Karte", "Description de la carte", "Opis mapy", "Popis mapy",
      "Térkép leírása", "Descrizione della mappa", "Descripción del mapa", "地图说明"}},
    {"Multiplayer maps", 0,
     {"Mehrspieler-Karten", "Cartes multijoueur", "Mapy dla wielu graczy", "Mapy pro více hráčů",
      "Többjátékos térképek", "Mappe multigiocatore", "Mapas multijugador", "多人地图"}},
    {"Generate a Map", 0,
     {"Karte generieren", "Générer une carte", "Generuj mapę", "Vytvořit mapu", "Térkép generálása",
      "Genera una mappa", "Generar un mapa", "生成地图"}},
    {"Creating the game...", 0,
     {"Spiel wird erstellt...", "Création de la partie...", "Tworzenie gry...", "Vytváří se hra...",
      "Játék létrehozása...", "Creazione della partita...", "Creando la partida...", "正在创建游戏……"}},
    {"unlimited", 0,
     {"unbegrenzt", "illimité", "bez limitu", "bez omezení", "korlátlan", "illimitato", "ilimitado",
      "无限制"}},
    {"%d min", 0,
     {"%d Min.", "%d min", "%d min", "%d min", "%d perc", "%d min", "%d min", "%d 分钟"}},
    {"Version: ", 0,
     {"Version: ", "Version : ", "Wersja: ", "Verze: ", "Verzió: ", "Versione: ", "Versión: ",
      "版本："}},
    {"Turn time", 0,
     {"Zeitlimit", "Durée du tour", "Limit czasu", "Čas na tah", "Körlimit", "Tempo per turno",
      "Tiempo por turno", "回合时间"}},
    {"Turn time:", 0,
     {"Zeitlimit:", "Durée du tour :", "Limit czasu:", "Čas na tah:", "Körlimit:",
      "Tempo per turno:", "Tiempo por turno:", "回合时间："}},
    {"Difficulty:", 0,
     {"Schwierigkeit:", "Difficulté :", "Poziom trudności:", "Obtížnost:", "Nehézségi szint:",
      "Difficoltà:", "Dificultad:", "游戏难度："}},
    {"Animation speed", 0,
     {"Animationsgeschwindigkeit", "Vitesse des animations", "Szybkość animacji",
      "Rychlost animací", "Animáció sebessége", "Velocità animazioni", "Velocidad de animación",
      "动画速度"}},
    {"Game name", 0,
     {"Spielname", "Nom de la partie", "Nazwa gry", "Název hry", "Játék neve", "Nome della partita",
      "Nombre de la partida", "游戏名称"}},
    {"Private game", 0,
     {"Privates Spiel", "Partie privée", "Gra prywatna", "Soukromá hra", "Privát játék",
      "Partita privata", "Partida privada", "私人游戏"}},
    {"Password", "Пароль",
     {"Passwort", "Mot de passe", "Hasło", "Heslo", "Jelszó", "Password", "Contraseña", "密码"}},
    {"Ghost mode", 0,
     {"Geistermodus", "Mode fantôme", "Tryb ducha", "Přízračný mód", "Kísértet mód",
      "Modalità fantasma", "Modo fantasma", "幽灵模式"}},
    {"Quick combat", 0,
     {"Schneller Kampf", "Combat rapide", "Szybka walka", "Rychlý boj", "Gyors csata",
      "Combattimento rapido", "Combate rápido", "快速战斗"}},
    {"Autosave", 0,
     {"Automatisch speichern", "Sauvegarde auto", "Autozapis", "Automatické ukládání",
      "Automatikus mentés", "Salvataggio automatico", "Guardado automático", "自动存档"}},
    {"Dynamic battles", 0,
     {"Dynamische Kämpfe", "Combat temps limité", "Bitwy dynamiczne", "Dynamické bitvy",
      "Dinamikus csaták", "Battaglie dinamiche", "Batallas dinámicas", "动态战斗"}},
    {"Simultaneous turns", 0,
     {"Gleichzeitige Züge", "Tours simultanés", "Jednoczesne tury", "Souběžné tahy",
      "Egyidejű körök", "Turni simultanei", "Turnos simultáneos", "同步回合"}},
    {"Random", "Случайная",
     {"Zufällig", "Aléatoire", "Losowa", "Náhodná", "Véletlen", "Casuale", "Aleatoria", "随机"}},
    {"Random", "Случайный",
     {"Zufällig", "Aléatoire", "Losowy", "Náhodný", "Véletlen", "Casuale", "Aleatorio", "随机"}},
    {"Closed", 0,
     {"Geschlossen", "Fermé", "Zamknięte", "Zavřeno", "Zárva", "Chiuso", "Cerrado", "关闭"}},
    {"Open", 0,
     {"Offen", "Ouvert", "Otwarte", "Otevřeno", "Nyitva", "Aperto", "Abierto", "开放"}},
    {"Computer", 0,
     {"Computer", "Ordinateur", "Komputer", "Počítač", "Számítógép", "Computer", "Ordenador",
      "电脑"}},
    {"Human", 0,
     {"Mensch", "Humain", "Człowiek", "Člověk", "Ember", "Umano", "Humano", "玩家"}},
    {"%s player", 0,
     {"Spieler %s", "joueur %s", "gracz %s", "%s hráč", "%s játékos", "giocatore %s", "jugador %s",
      "%s玩家"}}, /* a colour's */
    {"Player", 0,
     {"Spieler", "Joueur", "Gracz", "Hráč", "Játékos", "Giocatore", "Jugador", "玩家"}},
    {"Colour", 0,
     {"Farbe", "Couleur", "Kolor", "Barva", "Szín", "Colore", "Color", "颜色"}},
    {"Faction", 0,
     {"Fraktion", "Faction", "Frakcja", "Frakce", "Frakció", "Fazione", "Facción", "种族"}},
    {"Hero", 0,
     {"Held", "Héros", "Bohater", "Hrdina", "Hős", "Eroe", "Héroe", "英雄"}},
    {"Bonus", 0,
     {"Bonus", "Bonus", "Bonus", "Bonus", "Bónusz", "Bonus", "Bonificación", "奖励"}},
    {"Artifact", 0,
     {"Artefakt", "Artefact", "Artefakt", "Artefakt", "Varázstárgy", "Artefatto", "Artefacto",
      "宝物"}},
    {"Gold", 0,
     {"Gold", "Or", "Złoto", "Zlato", "Arany", "Oro", "Oro", "黄金"}},
    {"Resources", 0,
     {"Rohstoffe", "Ressources", "Surowce", "Suroviny", "Nyersanyagok", "Risorse", "Recursos",
      "资源"}},
    {"red", 0,
     {"rot", "rouge", "czerwony", "červený", "piros", "rosso", "rojo", "红色"}},
    {"blue", 0,
     {"blau", "bleu", "niebieski", "modrý", "kék", "blu", "azul", "蓝色"}},
    {"green", 0,
     {"grün", "vert", "zielony", "zelený", "zöld", "verde", "verde", "绿色"}},
    {"yellow", 0,
     {"gelb", "jaune", "żółty", "žlutý", "sárga", "giallo", "amarillo", "黄色"}},
    {"orange", 0,
     {"orange", "orange", "pomarańczowy", "oranžový", "narancssárga", "arancione", "naranja",
      "橙色"}},
    {"teal", 0,
     {"türkis", "turquoise", "turkusowy", "tyrkysový", "türkiz", "verde acqua", "turquesa", "青色"}},
    {"purple", 0,
     {"lila", "violet", "fioletowy", "fialový", "lila", "viola", "morado", "紫色"}},
    {"tan", 0,
     {"braun", "brun", "brązowy", "hnědý", "barna", "marrone", "marrón", "棕色"}},
    {"Not ready", 0,
     {"Nicht bereit", "Pas prêt", "Nie gotowy", "Nepřipraven", "Nem áll készen", "Non pronto",
      "No preparado", "未准备"}},
    {"Ready", 0,
     {"Bereit", "Prêt", "Gotowy", "Připraven", "Kész", "Pronto", "Preparado", "已准备"}},
    {"Starting the game", 0,
     {"Spiel wird gestartet", "Lancement de la partie", "Rozpoczęcie gry", "Spouští se hra",
      "A játék indul", "Avvio della partita", "Iniciando la partida", "正在开始游戏"}},
    {"Getting the game's information...", 0,
     {"Informationen über das Spiel werden geladen...",
      "Récupération des informations sur la partie...", "Pobieranie informacji o grze...",
      "Načítají se informace o hře...", "A játék adatainak lekérése...",
      "Recupero delle informazioni sulla partita...", "Obteniendo la información de la partida...",
      "正在获取游戏信息……"}},
    {"Connecting...", 0,
     {"Verbinde...", "Connexion...", "Łączenie...", "Připojování...", "Csatlakozás...",
      "Connessione...", "Conectando...", "正在连接……"}},
    {"Game Setup", 0,
     {"Spieleinstellungen", "Paramètres de la partie", "Ustawienia gry", "Nastavení hry",
      "Játékbeállítások", "Impostazioni partita", "Configuración de la partida", "游戏设置"}},
    {"(you)", 0,
     {"(Ihr)", "(vous)", "(ty)", "(ty)", "(te)", "(tu)", "(tú)", "（你）"}},
    {"Remove from the game", 0,
     {"Aus dem Spiel entfernen", "Retirer de la partie", "Usuń z gry", "Odebrat ze hry",
      "Eltávolítás a játékból", "Rimuovi dalla partita", "Quitar de la partida", "移出游戏"}},
    {"Cancel", 0,
     {"Abbrechen", "Annuler", "Anuluj", "Zrušit", "Mégse", "Annulla", "Cancelar", "取消"}},
    {"Start game", 0,
     {"Spiel beginnen", "Commencer la partie", "Rozpocznij grę", "Spustit hru", "Játék indítása",
      "Inizia partita", "Iniciar partida", "开始游戏"}},

    /* Duels */
    {"Arena", 0,
     {"Arena", "Arène", "Arena", "Aréna", "Aréna", "Arena", "Arena", "竞技场"}},
    {"Battle speed", 0,
     {"Kampfgeschwindigkeit", "Vitesse du combat", "Szybkość bitwy", "Rychlost bitvy",
      "Csata sebessége", "Velocità della battaglia", "Velocidad de batalla", "战斗速度"}},
    {"Combat speed", 0,
     {"Kampfgeschwindigkeit", "Vitesse du combat", "Szybkość walki", "Rychlost boje",
      "Harc sebessége", "Velocità del combattimento", "Velocidad de combate", "战斗速度"}},
    {"Random hero", 0,
     {"Zufälliger Held", "Héros aléatoire", "Losowy bohater", "Náhodný hrdina", "Véletlen hős",
      "Eroe casuale", "Héroe aleatorio", "随机英雄"}},
    {"First player", 0,
     {"Erster Spieler", "Premier joueur", "Pierwszy gracz", "První hráč", "Első játékos",
      "Primo giocatore", "Primer jugador", "第一玩家"}},
    {"Second player", 0,
     {"Zweiter Spieler", "Second joueur", "Drugi gracz", "Druhý hráč", "Második játékos",
      "Secondo giocatore", "Segundo jugador", "第二玩家"}},
    {"3x3 mode", 0,
     {"3x3-Modus", "Mode 3x3", "Tryb 3x3", "Režim 3x3", "3x3 mód", "Modalità 3x3", "Modo 3x3",
      "3x3 模式"}},

    /* The gear menu */
    {"Battle", 0,
     {"Kampf", "Combat", "Bitwa", "Bitva", "Csata", "Battaglia", "Batalla", "战斗"}},
    {"Menu", 0,
     {"Menü", "Menu", "Menu", "Nabídka", "Menü", "Menu", "Menú", "菜单"}},
    {"Save Game", 0,
     {"Spiel speichern", "Sauvegarder la partie", "Zapisz grę", "Uložit hru", "Mentés",
      "Salva partita", "Guardar partida", "保存游戏"}},
    {"Restart", 0,
     {"Neu starten", "Recommencer", "Zacznij od nowa", "Začít znovu", "Újraindítás", "Ricomincia",
      "Reiniciar", "重新开始"}},
    {"Surrender", 0,
     {"Aufgeben", "Se rendre", "Poddaj się", "Vzdát se", "Megadás", "Arrenditi", "Rendirse", "投降"}},
    {"Quit Game", 0,
     {"Spiel beenden", "Quitter le jeu", "Wyjdź z gry", "Ukončit hru", "Kilépés a játékból",
      "Esci dal gioco", "Salir del juego", "退出游戏"}},
    {"Return to Game", 0,
     {"Zurück zum Spiel", "Retour au jeu", "Powrót do gry", "Návrat do hry", "Vissza a játékba",
      "Ritorna al gioco", "Volver a la partida", "返回游戏"}},

    /* Hall of fame */
    {"Rank", 0,
     {"Rang", "Rang", "Ranga", "Pořadí", "Rang", "Pos.", "Rango", "级别"}},
    {"Days", 0,
     {"Tage", "Jours", "Dni", "Dny", "Nap", "Giorni", "Días", "天数"}},
    {"Score", 0,
     {"Punkte", "Points", "Punkty", "Body", "Pontok", "Punti", "Puntos", "积分"}},
    {"Badge", 0,
     {"Bild", "Icône", "Ikona", "Ikona", "Ikon", "Icona", "Icono", "图标"}},
    {"Clear", 0,
     {"Löschen", "Effacer", "Wyczyść", "Vyčistit", "Törlés", "Cancella", "Borrar", "清除"}},
    {"Close", 0,
     {"Schließen", "Fermer", "Zamknij", "Zavřít", "Bezárás", "Chiudi", "Cerrar", "关闭"}},

    /* Save and load */
    {"Save name", 0,
     {"Name des Spielstands", "Nom de la sauvegarde", "Nazwa zapisu", "Název uložené hry",
      "Mentés neve", "Nome del salvataggio", "Nombre de la partida guardada", "存档名称"}},
    {"Type:", 0,
     {"Spielart:", "Type :", "Typ gry:", "Typ hry:", "Játéktípus:", "Tipo:", "Tipo:", "类型："}},
    {"Save", 0,
     {"Speichern", "Sauvegarder", "Zapisz", "Uložit", "Mentés", "Salva", "Guardar", "保存"}},
    {"Load", 0,
     {"Laden", "Charger", "Wczytaj", "Načíst", "Betöltés", "Carica", "Cargar", "载入"}},
    {"Delete", 0,
     {"Löschen", "Supprimer", "Usuń", "Smazat", "Törlés", "Elimina", "Borrar", "删除"}},

    /* Options and questions */
    {"Apply and Close", 0,
     {"Übernehmen und schließen", "Appliquer et fermer", "Zastosuj i zamknij", "Použít a zavřít",
      "Alkalmazás és bezárás", "Applica e chiudi", "Aplicar y cerrar", "应用并关闭"}},
    {"Apply", 0,
     {"Übernehmen", "Appliquer", "Zastosuj", "Použít", "Alkalmazás", "Applica", "Aplicar", "应用"}},
    {"Defaults", 0,
     {"Standard", "Par défaut", "Domyślne", "Výchozí", "Alapértelmezés", "Predefinite",
      "Por defecto", "默认"}},
    {"Yes", 0,
     {"Ja", "Oui", "Tak", "Ano", "Igen", "Sì", "Sí", "是"}},
    {"No", 0,
     {"Nein", "Non", "Nie", "Ne", "Nem", "No", "No", "否"}},

    /* LAN */
    {"Duel: Server List", 0,
     {"Duell: Serverliste", "Duel : liste des serveurs", "Pojedynek: lista serwerów",
      "Souboj: seznam serverů", "Párbaj: szerverlista", "Duello: elenco dei server",
      "Duelo: lista de servidores", "对决：服务器列表"}},
    {"Standard Mode: Server List", 0,
     {"Standardmodus: Serverliste", "Mode standard : liste des serveurs",
      "Tryb standardowy: lista serwerów", "Standardní režim: seznam serverů",
      "Standard mód: szerverlista", "Modalità standard: elenco dei server",
      "Modo estándar: lista de servidores", "标准模式：服务器列表"}},
    {"Standard Mode", 0,
     {"Standardmodus", "Mode standard", "Tryb standardowy", "Standardní režim", "Standard mód",
      "Modalità standard", "Modo estándar", "标准模式"}},
    {"Speed", 0,
     {"Geschwindigkeit", "Vitesse", "Szybkość", "Rychlost", "Sebesség", "Velocità", "Velocidad",
      "速度"}},
    {"Map", 0,
     {"Karte", "Carte", "Mapa", "Mapa", "Térkép", "Mappa", "Mapa", "地图"}},
    {"Running games", 0,
     {"Laufende Spiele", "Parties en cours", "Trwające gry", "Probíhající hry",
      "Folyamatban lévő játékok", "Partite in corso", "Partidas en curso", "进行中的游戏"}},
    {"Full games", 0,
     {"Volle Spiele", "Parties complètes", "Pełne gry", "Plné hry", "Megtelt játékok",
      "Partite al completo", "Partidas completas", "已满的游戏"}},
    {"No games on this channel yet", 0,
     {"Noch keine Spiele in diesem Kanal", "Aucune partie sur ce canal pour l’instant",
      "Na tym kanale nie ma jeszcze gier", "Na tomto kanálu zatím nejsou žádné hry",
      "Ezen a csatornán még nincs játék", "Ancora nessuna partita su questo canale",
      "Aún no hay partidas en este canal", "此频道暂无游戏"}},
    {"Joining the game...", 0,
     {"Spiel wird beigetreten...", "Connexion à la partie...", "Dołączanie do gry...",
      "Připojování ke hře...", "Csatlakozás a játékhoz...", "Ingresso nella partita...",
      "Uniéndose a la partida...", "正在加入游戏……"}},
    {"Changing the channel...", 0,
     {"Kanal wird gewechselt...", "Changement de canal...", "Zmiana kanału...", "Mění se kanál...",
      "Csatornaváltás...", "Cambio di canale...", "Cambiando de canal...", "正在切换频道……"}},
    {"Join", 0,
     {"Beitreten", "Rejoindre", "Dołącz", "Připojit", "Csatlakozás", "Partecipa", "Unirse", "加入"}},
    {"Create", 0,
     {"Erstellen", "Créer", "Stwórz", "Vytvořit", "Létrehozás", "Crea", "Crear", "创建"}},
    {"Channel", 0,
     {"Kanal", "Canal", "Kanał", "Kanál", "Csatorna", "Canale", "Canal", "频道"}},
    {"Player Name", 0,
     {"Spielername", "Nom du joueur", "Nazwa gracza", "Jméno hráče", "Játékos neve",
      "Nome del giocatore", "Nombre del jugador", "玩家名称"}},
    {"Leave", 0,
     {"Verlassen", "Quitter", "Opuść", "Odejít", "Kilépés", "Esci", "Salir", "离开"}},
    {"Your Name", 0,
     {"Euer Name", "Votre nom", "Twoje imię", "Tvé jméno", "Neved", "Il tuo nome", "Tu nombre",
      "你的名字"}},
    {"Password", "Ввод пароля",
     {"Passwort eingeben", "Saisie du mot de passe", "Wprowadź hasło", "Zadání hesla",
      "Jelszó megadása", "Inserisci la password", "Introducir contraseña", "输入密码"}},
    {"How other players see you", 0,
     {"So sehen Euch die anderen Spieler", "Le nom que voient les autres joueurs",
      "Tak widzą cię inni gracze", "Takto tě uvidí ostatní hráči", "Így látnak a többi játékosok",
      "Come ti vedono gli altri giocatori", "Así te ven los demás jugadores", "其他玩家看到的你的名字"}},
    {"New Game", 0,
     {"Neues Spiel", "Nouvelle partie", "Nowa gra", "Nová hra", "Új játék", "Nuova partita",
      "Nueva partida", "新游戏"}},

    /* ubi-gs */
    {"Online Profile", 0,
     {"Online-Profil", "Profil en ligne", "Profil online", "Online profil", "Online profil",
      "Profilo online", "Perfil en línea", "在线档案"}},
    {"Connecting to the server...", 0,
     {"Verbindung zum Server...", "Connexion au serveur...", "Łączenie z serwerem...",
      "Připojování k serveru...", "Csatlakozás a szerverhez...", "Connessione al server...",
      "Conectando con el servidor...", "正在连接服务器……"}},
    {"Logging in...", 0,
     {"Anmeldung...", "Identification...", "Logowanie...", "Přihlašování...", "Bejelentkezés...",
      "Accesso...", "Iniciando sesión...", "正在登录……"}},
    {"Checking the CD key...", 0,
     {"CD-Schlüssel wird geprüft...", "Vérification de la clé CD...", "Sprawdzanie klucza CD...",
      "Kontroluje se klíč CD...", "CD-kulcs ellenőrzése...", "Verifica della chiave CD...",
      "Comprobando la clave del CD...", "正在检查 CD 密钥……"}},
    {"Please enter the CD key", 0,
     {"Bitte gebt den CD-Schlüssel ein", "Veuillez entrer la clé CD", "Wprowadź klucz CD",
      "Zadej klíč CD", "Add meg a CD-kulcsot", "Inserisci la chiave CD",
      "Introduce la clave del CD", "请输入 CD 密钥"}},
    {"Any key works: a community server doesn't check it. Don't type your real one.", 0,
     {"Jeder Schlüssel geht: Ein Community-Server prüft ihn nicht. Gebt nicht Euren echten ein.",
      "N’importe quelle clé convient : un serveur communautaire ne la vérifie pas. N’entrez pas "
      "la vôtre.",
      "Każdy klucz zadziała: serwer społeczności go nie sprawdza. Nie wpisuj prawdziwego.",
      "Projde jakýkoli klíč: komunitní server ho nekontroluje. Nezadávej svůj skutečný.",
      "Bármilyen kulcs jó: a közösségi szerver nem ellenőrzi. Ne a valódit írd be.",
      "Va bene qualsiasi chiave: un server della comunità non la controlla. Non inserire quella "
      "vera.",
      "Sirve cualquier clave: un servidor de la comunidad no la comprueba. No escribas la tuya "
      "de verdad.", "任意密钥均可：社区服务器不会检查它。请勿输入你的真实密钥。"}},
    {"Any CD key works. A new name and password make an account on this server. The "
     "password reaches it only as a hash, but still make up a new one, not your Ubisoft "
     "login or one you use elsewhere.", 0,
     {"Jeder CD-Schlüssel geht. Ein neuer Name mit Passwort legt ein Konto auf diesem Server "
      "an. Das Passwort erreicht ihn nur als Hash, aber denkt Euch trotzdem ein neues aus, "
      "nicht Euer Ubisoft-Login und keins, das Ihr anderswo nutzt.",
      "N’importe quelle clé CD convient. Un nouveau nom avec un mot de passe crée un compte sur "
      "ce serveur. Le mot de passe ne lui parvient que sous forme de hachage, mais "
      "choisissez-en quand même un nouveau, ni votre identifiant Ubisoft, ni un mot de passe "
      "utilisé ailleurs.",
      "Każdy klucz CD zadziała. Nowa nazwa z hasłem zakłada konto na tym serwerze. Hasło trafia "
      "tam tylko jako skrót, ale i tak wymyśl nowe, nie login Ubisoft ani hasło używane gdzie "
      "indziej.",
      "Projde jakýkoli klíč CD. Nové jméno s heslem založí na tomto serveru účet. Heslo k němu "
      "dorazí jen jako hash, přesto si vymysli nové, ne své přihlášení k Ubisoftu ani heslo, "
      "které používáš jinde.",
      "Bármilyen CD-kulcs jó. Egy új név és jelszó fiókot hoz létre ezen a szerveren. A jelszó "
      "csak hash-ként jut el hozzá, de azért találj ki egy újat, ne a Ubisoft-belépésedet vagy "
      "máshol használt jelszót.",
      "Va bene qualsiasi chiave CD. Un nuovo nome con password crea un account su questo "
      "server. La password gli arriva solo come hash, ma scegline comunque una nuova, non il "
      "tuo login Ubisoft né una che usi altrove.",
      "Sirve cualquier clave del CD. Un nombre y una contraseña nuevos crean una cuenta en este "
      "servidor. La contraseña le llega solo como hash, pero aun así inventa una nueva, no tu "
      "cuenta de Ubisoft ni una que uses en otro sitio.",
      "任意 CD 密钥均可。新的名字和密码会在此服务器上创建账户。密码只以哈希形式发送，但仍请另设一个新密码，不要使用你的 Ubisoft 账户或在别处使用的密码。"}},
    {"Available Channels", 0,
     {"Verfügbare Kanäle", "Canaux disponibles", "Dostępne kanały", "Dostupné kanály",
      "Elérhető csatornák", "Canali disponibili", "Canales disponibles", "可用频道"}},
    {"Mode", 0,
     {"Modus", "Mode", "Tryb", "Režim", "Mód", "Modalità", "Modo", "模式"}},
    {"Joining the channel...", 0,
     {"Kanal wird betreten...", "Connexion au canal...", "Dołączanie do kanału...",
      "Připojování ke kanálu...", "Csatlakozás a csatornához...", "Ingresso nel canale...",
      "Uniéndose al canal...", "正在加入频道……"}},
};

/* The words for (ru, en) in the language, the table searched once per pair
 * of literals: the views ask for the same ones on every document. */
typedef struct {
    const char *ru, *en, *out;
    int lang;
} Seen;

static const char *lookup(const char *ru, const char *en) {
    enum { SLOTS = 512, PROBES = 8 };
    static Seen seen[SLOTS];
    const uintptr_t h = ((uintptr_t)en >> 3) ^ ((uintptr_t)ru >> 5);
    Seen *e = NULL;
    for (int probe = 0; probe < PROBES; ++probe) {
        e = &seen[(h + (uintptr_t)probe) % SLOTS];
        if (!e->en)
            break;
        if (e->en == en && e->ru == ru && e->lang == ns_lang)
            return e->out;
    }
    const char *out = NULL;
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0] && !out; ++i) {
        const Words *w = &WORDS[i];
        if (!strcmp(w->en, en) && (!w->ru || !strcmp(w->ru, ru)))
            out = w->tr[ns_lang - NS_DE];
    }
    /* A game text usually stands in front of the words (text_or), so a
     * phrase noted here may still never be shown. */
    if (!out && ns_trace())
        fprintf(stderr, "[screens] no %s words for \"%s\"\n", ns_lang_code(), en);
    /* An empty slot, or the last one probed when all were taken. */
    e->ru = ru;
    e->en = en;
    e->out = out ? out : en;
    e->lang = ns_lang;
    return e->out;
}

const char *ns_tr(const char *ru, const char *en) {
    if (ns_lang == NS_EN || ns_lang == NS_RU)
        return ns_lang == NS_RU ? ru : en;
    return lookup(ru, en);
}
