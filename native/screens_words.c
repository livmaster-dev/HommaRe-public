/* screens_words.c - the native screens' own words in the game's language.
 *
 * The views say most things in the game's words (its text files, ns_label);
 * the rest is written at the call as Russian and English (ns_tr). German,
 * French and Polish come from the table below, by the English words (and the
 * Russian ones where one English word has two Russian forms); a language or a
 * phrase it lacks stays English. The language is the game's: its first
 * difficulty's name says which install it is. */
#include "native_screens.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int ns_lang;
int ns_ru;

int ns_set_lang(const char *easy) {
    const unsigned char *u = (const unsigned char *)easy;
    if (u[0] >= 0xd0 && u[0] <= 0xd3)
        ns_lang = NS_RU; /* Cyrillic */
    else if (!strncmp(easy, "Leicht", 6))
        ns_lang = NS_DE;
    else if (!strncmp(easy, "Facile", 6))
        ns_lang = NS_FR;
    else if (!strncmp(easy, "\xc5\x81" "atw", 5)) /* Łatwy */
        ns_lang = NS_PL;
    else
        ns_lang = NS_EN;
    return ns_ru = ns_lang == NS_RU;
}

const char *ns_lang_code(void) {
    static const char *const code[] = {"en", "ru", "de", "fr", "pl"};
    return code[ns_lang];
}

typedef struct {
    const char *en, *ru; /* ru: only where `en` has two Russian forms */
    const char *de, *fr, *pl;
} Words;

static const Words WORDS[] = {
    /* Main menu and its panels */
    {"Main Menu", 0, "Hauptmenü", "Menu principal", "Główne menu"},
    {"Single Player", 0, "Einzelspieler", "Solo", "Jeden gracz"},
    {"Multiplayer", 0, "Mehrspieler", "Multijoueur", "Wielu graczy"},
    {"Options", 0, "Optionen", "Options", "Opcje"},
    {"Credits", 0, "Mitwirkende", "Crédits", "Twórcy"},
    {"Exit", 0, "Beenden", "Quitter", "Wyjście"},
    {"Campaign", 0, "Kampagne", "Campagne", "Kampania"},
    {"Campaigns", 0, "Kampagnen", "Campagnes", "Kampanie"},
    {"Scenarios", 0, "Szenarien", "Scénarios", "Scenariusze"},
    {"Load Game", 0, "Spiel laden", "Charger une partie", "Wczytaj grę"},
    {"Custom Campaign", 0, "Eigene Kampagne", "Campagne perso", "Kampania gracza"},
    {"Hall of Fame", 0, "Ruhmeshalle", "Classement", "Galeria sław"},
    {"Hot Seat", 0, "Hot-Seat", "Hot Seat", "Hot Seat"},
    {"LAN", 0, "LAN", "Réseau local", "Sieć lokalna"},
    {"Standard", 0, "Standard", "Standard", "Standardowa"},
    {"Duel", 0, "Duell", "Duel", "Pojedynek"},
    {"Duel 3x3", 0, "Duell 3x3", "Duel 3x3", "Pojedynek 3x3"},
    {"Missions", 0, "Missionen", "Missions", "Misje"},
    {"Difficulty", 0, "Schwierigkeit", "Difficulté", "Poziom trudności"},
    {"Profile", 0, "Profil", "Profil", "Profil"},
    {"Change", 0, "Wechseln", "Changer", "Zmień"},
    {"New profile", 0, "Neues Profil", "Nouveau profil", "Nowy profil"},
    {"Profile name", 0, "Profilname", "Nom du profil", "Nazwa profilu"},
    {"2 to 10 letters, digits or spaces", 0, "2 bis 10 Buchstaben, Ziffern oder Leerzeichen",
     "De 2 à 10 lettres, chiffres ou espaces", "Od 2 do 10 liter, cyfr lub spacji"},
    {"Back", 0, "Zurück", "Retour", "Wróć"},
    {"Start", 0, "Start", "Commencer", "Start"},
    {"Next", 0, "Nächster", "Suivant", "Następny"},
    {"Previous", 0, "Vorheriger", "Précédent", "Poprzedni"},

    /* Custom game, hot seat, network game */
    {"Custom Game", 0, "Einzelszenario", "Scénario isolé", "Gra własna"},
    {"Hot Seat: New Game", 0, "Hot-Seat: Spiel erstellen", "Hot Seat : nouvelle partie", "Hot-seat: nowa gra"},
    {"New Network Game", 0, "Neues Netzwerkspiel", "Nouvelle partie en réseau", "Nowa gra sieciowa"},
    {"Name", 0, "Name", "Nom", "Nazwa"},
    {"Goal", 0, "Ziel", "Objectif", "Cel"},
    {"Goal:", 0, "Ziel:", "Objectif :", "Cel:"},
    {"Players", "Игроков", "Spieler", "Joueurs", "Gracze"},
    {"Players", "Игроки", "Spieler", "Joueurs", "Gracze"},
    {"Players:", 0, "Spieler:", "Joueurs :", "Liczba graczy:"},
    {"Size", 0, "Größe", "Taille", "Rozmiar"},
    {"Size:", 0, "Größe:", "Taille :", "Rozmiar mapy:"},
    {"Underground:", 0, "Untergrund:", "Souterrain :", "Podziemia:"},
    {"yes", 0, "ja", "oui", "tak"},
    {"no", 0, "nein", "non", "nie"},
    {"Map description", 0, "Beschreibung der Karte", "Description de la carte", "Opis mapy"},
    {"Multiplayer maps", 0, "Mehrspieler-Karten", "Cartes multijoueur", "Mapy dla wielu graczy"},
    {"Generate a Map", 0, "Karte generieren", "Générer une carte", "Generuj mapę"},
    {"Creating the game...", 0, "Spiel wird erstellt...", "Création de la partie...", "Tworzenie gry..."},
    {"unlimited", 0, "unbegrenzt", "illimité", "bez limitu"},
    {"%d min", 0, "%d Min.", "%d min", "%d min"},
    {"Turn time", 0, "Zeitlimit", "Durée du tour", "Limit czasu"},
    {"Turn time:", 0, "Zeitlimit:", "Durée du tour :", "Limit czasu:"},
    {"Difficulty:", 0, "Schwierigkeit:", "Difficulté :", "Poziom trudności:"},
    {"Animation speed", 0, "Animationsgeschwindigkeit", "Vitesse des animations", "Szybkość animacji"},
    {"Game name", 0, "Spielname", "Nom de la partie", "Nazwa gry"},
    {"Private game", 0, "Privates Spiel", "Partie privée", "Gra prywatna"},
    {"Password", "Пароль", "Passwort", "Mot de passe", "Hasło"},
    {"Ghost mode", 0, "Geistermodus", "Mode fantôme", "Tryb ducha"},
    {"Quick combat", 0, "Schneller Kampf", "Combat rapide", "Szybka walka"},
    {"Autosave", 0, "Automatisch speichern", "Sauvegarde auto", "Autozapis"},
    {"Dynamic battles", 0, "Dynamische Kämpfe", "Combat temps limité", "Bitwy dynamiczne"},
    {"Simultaneous turns", 0, "Gleichzeitige Züge", "Tours simultanés", "Jednoczesne tury"},
    {"Random", "Случайная", "Zufällig", "Aléatoire", "Losowa"},
    {"Random", "Случайный", "Zufällig", "Aléatoire", "Losowy"},
    {"Closed", 0, "Geschlossen", "Fermé", "Zamknięte"},
    {"Open", 0, "Offen", "Ouvert", "Otwarte"},
    {"Computer", 0, "Computer", "Ordinateur", "Komputer"},
    {"Human", 0, "Mensch", "Humain", "Człowiek"},
    {"%s player", 0, "Spieler %s", "joueur %s", "gracz %s"}, /* a colour's */
    {"Player", 0, "Spieler", "Joueur", "Gracz"},
    {"Colour", 0, "Farbe", "Couleur", "Kolor"},
    {"Faction", 0, "Fraktion", "Faction", "Frakcja"},
    {"Hero", 0, "Held", "Héros", "Bohater"},
    {"Bonus", 0, "Bonus", "Bonus", "Bonus"},
    {"Artifact", 0, "Artefakt", "Artefact", "Artefakt"},
    {"Gold", 0, "Gold", "Or", "Złoto"},
    {"Resources", 0, "Rohstoffe", "Ressources", "Surowce"},
    {"red", 0, "rot", "rouge", "czerwony"},
    {"blue", 0, "blau", "bleu", "niebieski"},
    {"green", 0, "grün", "vert", "zielony"},
    {"yellow", 0, "gelb", "jaune", "żółty"},
    {"orange", 0, "orange", "orange", "pomarańczowy"},
    {"teal", 0, "türkis", "turquoise", "turkusowy"},
    {"purple", 0, "lila", "violet", "fioletowy"},
    {"tan", 0, "braun", "brun", "brązowy"},
    {"Not ready", 0, "Nicht bereit", "Pas prêt", "Nie gotowy"},
    {"Ready", 0, "Bereit", "Prêt", "Gotowy"},
    {"Starting the game", 0, "Spiel wird gestartet", "Lancement de la partie", "Rozpoczęcie gry"},
    {"Getting the game's information...", 0, "Informationen über das Spiel werden geladen...",
     "Récupération des informations sur la partie...", "Pobieranie informacji o grze..."},
    {"Connecting...", 0, "Verbinde...", "Connexion...", "Łączenie..."},
    {"Game Setup", 0, "Spieleinstellungen", "Paramètres de la partie", "Ustawienia gry"},
    {"(you)", 0, "(Ihr)", "(vous)", "(ty)"},
    {"Remove from the game", 0, "Aus dem Spiel entfernen", "Retirer de la partie", "Usuń z gry"},
    {"Cancel", 0, "Abbrechen", "Annuler", "Anuluj"},
    {"Start game", 0, "Spiel beginnen", "Commencer la partie", "Rozpocznij grę"},

    /* Duels */
    {"Arena", 0, "Arena", "Arène", "Arena"},
    {"Battle speed", 0, "Kampfgeschwindigkeit", "Vitesse du combat", "Szybkość bitwy"},
    {"Combat speed", 0, "Kampfgeschwindigkeit", "Vitesse du combat", "Szybkość walki"},
    {"Random hero", 0, "Zufälliger Held", "Héros aléatoire", "Losowy bohater"},
    {"First player", 0, "Erster Spieler", "Premier joueur", "Pierwszy gracz"},
    {"Second player", 0, "Zweiter Spieler", "Second joueur", "Drugi gracz"},
    {"3x3 mode", 0, "3x3-Modus", "Mode 3x3", "Tryb 3x3"},

    /* The gear menu */
    {"Battle", 0, "Kampf", "Combat", "Bitwa"},
    {"Menu", 0, "Menü", "Menu", "Menu"},
    {"Save Game", 0, "Spiel speichern", "Sauvegarder la partie", "Zapisz grę"},
    {"Restart", 0, "Neu starten", "Recommencer", "Zacznij od nowa"},
    {"Surrender", 0, "Aufgeben", "Se rendre", "Poddaj się"},
    {"Quit Game", 0, "Spiel beenden", "Quitter le jeu", "Wyjdź z gry"},
    {"Return to Game", 0, "Zurück zum Spiel", "Retour au jeu", "Powrót do gry"},

    /* Hall of fame */
    {"Rank", 0, "Rang", "Rang", "Ranga"},
    {"Days", 0, "Tage", "Jours", "Dni"},
    {"Score", 0, "Punkte", "Points", "Punkty"},
    {"Badge", 0, "Bild", "Icône", "Ikona"},
    {"Clear", 0, "Löschen", "Effacer", "Wyczyść"},
    {"Close", 0, "Schließen", "Fermer", "Zamknij"},

    /* Save and load */
    {"Save name", 0, "Name des Spielstands", "Nom de la sauvegarde", "Nazwa zapisu"},
    {"Type:", 0, "Spielart:", "Type :", "Typ gry:"},
    {"Save", 0, "Speichern", "Sauvegarder", "Zapisz"},
    {"Load", 0, "Laden", "Charger", "Wczytaj"},
    {"Delete", 0, "Löschen", "Supprimer", "Usuń"},

    /* Options and questions */
    {"Apply and Close", 0, "Übernehmen und schließen", "Appliquer et fermer", "Zastosuj i zamknij"},
    {"Apply", 0, "Übernehmen", "Appliquer", "Zastosuj"},
    {"Defaults", 0, "Standard", "Par défaut", "Domyślne"},
    {"Yes", 0, "Ja", "Oui", "Tak"},
    {"No", 0, "Nein", "Non", "Nie"},

    /* LAN */
    {"Duel: Server List", 0, "Duell: Serverliste", "Duel : liste des serveurs", "Pojedynek: lista serwerów"},
    {"Standard Mode: Server List", 0, "Standardmodus: Serverliste", "Mode standard : liste des serveurs",
     "Tryb standardowy: lista serwerów"},
    {"Standard Mode", 0, "Standardmodus", "Mode standard", "Tryb standardowy"},
    {"Speed", 0, "Geschwindigkeit", "Vitesse", "Szybkość"},
    {"Map", 0, "Karte", "Carte", "Mapa"},
    {"Running games", 0, "Laufende Spiele", "Parties en cours", "Trwające gry"},
    {"Full games", 0, "Volle Spiele", "Parties complètes", "Pełne gry"},
    {"No games on this channel yet", 0, "Noch keine Spiele in diesem Kanal",
     "Aucune partie sur ce canal pour l’instant", "Na tym kanale nie ma jeszcze gier"},
    {"Joining the game...", 0, "Spiel wird beigetreten...", "Connexion à la partie...", "Dołączanie do gry..."},
    {"Changing the channel...", 0, "Kanal wird gewechselt...", "Changement de canal...", "Zmiana kanału..."},
    {"Join", 0, "Beitreten", "Rejoindre", "Dołącz"},
    {"Create", 0, "Erstellen", "Créer", "Stwórz"},
    {"Channel", 0, "Kanal", "Canal", "Kanał"},
    {"Player Name", 0, "Spielername", "Nom du joueur", "Nazwa gracza"},
    {"Leave", 0, "Verlassen", "Quitter", "Opuść"},
    {"Your Name", 0, "Euer Name", "Votre nom", "Twoje imię"},
    {"Password", "Ввод пароля", "Passwort eingeben", "Saisie du mot de passe", "Wprowadź hasło"},
    {"How other players see you", 0, "So sehen Euch die anderen Spieler", "Le nom que voient les autres joueurs",
     "Tak widzą cię inni gracze"},
    {"New Game", 0, "Neues Spiel", "Nouvelle partie", "Nowa gra"},

    /* ubi-gs */
    {"Online Profile", 0, "Online-Profil", "Profil en ligne", "Profil online"},
    {"Connecting to the server...", 0, "Verbindung zum Server...", "Connexion au serveur...", "Łączenie z serwerem..."},
    {"Logging in...", 0, "Anmeldung...", "Identification...", "Logowanie..."},
    {"Checking the CD key...", 0, "CD-Schlüssel wird geprüft...", "Vérification de la clé CD...",
     "Sprawdzanie klucza CD..."},
    {"Please enter the CD key", 0, "Bitte gebt den CD-Schlüssel ein", "Veuillez entrer la clé CD", "Wprowadź klucz CD"},
    {"Any key works: a community server doesn't check it. Don't type your real one.", 0,
     "Jeder Schlüssel geht: Ein Community-Server prüft ihn nicht. Gebt nicht Euren echten ein.",
     "N’importe quelle clé convient : un serveur communautaire ne la vérifie pas. N’entrez pas la vôtre.",
     "Każdy klucz zadziała: serwer społeczności go nie sprawdza. Nie wpisuj prawdziwego."},
    {"Any CD key works. A new name and password make an account on this server. "
     "The password reaches it only as a hash, but still make up a new one, "
     "not your Ubisoft login or one you use elsewhere.", 0,
     "Jeder CD-Schlüssel geht. Ein neuer Name mit Passwort legt ein Konto auf diesem Server an. "
     "Das Passwort erreicht ihn nur als Hash, aber denkt Euch trotzdem ein neues aus, "
     "nicht Euer Ubisoft-Login und keins, das Ihr anderswo nutzt.",
     "N’importe quelle clé CD convient. Un nouveau nom avec un mot de passe crée un compte sur ce serveur. "
     "Le mot de passe ne lui parvient que sous forme de hachage, mais choisissez-en quand même un nouveau, "
     "ni votre identifiant Ubisoft, ni un mot de passe utilisé ailleurs.",
     "Każdy klucz CD zadziała. Nowa nazwa z hasłem zakłada konto na tym serwerze. "
     "Hasło trafia tam tylko jako skrót, ale i tak wymyśl nowe, "
     "nie login Ubisoft ani hasło używane gdzie indziej."},
    {"Available Channels", 0, "Verfügbare Kanäle", "Canaux disponibles", "Dostępne kanały"},
    {"Mode", 0, "Modus", "Mode", "Tryb"},
    {"Joining the channel...", 0, "Kanal wird betreten...", "Connexion au canal...", "Dołączanie do kanału..."},
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
    const char *out = en;
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; ++i) {
        const Words *w = &WORDS[i];
        if (strcmp(w->en, en) || (w->ru && strcmp(w->ru, ru)))
            continue;
        out = ns_lang == NS_DE ? w->de : ns_lang == NS_FR ? w->fr : w->pl;
        break;
    }
    if (out == en && ns_trace())
        fprintf(stderr, "[screens] no %s words for \"%s\"\n", ns_lang_code(), en);
    /* An empty slot, or the last one probed when all were taken. */
    e->ru = ru;
    e->en = en;
    e->out = out;
    e->lang = ns_lang;
    return out;
}

const char *ns_tr(const char *ru, const char *en) {
    if (ns_lang == NS_EN || ns_lang == NS_RU)
        return ns_lang == NS_RU ? ru : en;
    return lookup(ru, en);
}
