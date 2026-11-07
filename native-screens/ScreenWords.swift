// ScreenWords.swift - the views' own words in German, French and Polish, by
// their English (ScreenStore.tr). Russian and English are written at the call;
// a phrase missing here stays English. The game side's words are in
// native/screens_words.c.

enum ScreenWords {
    static let table: [String: [String: String]] = [
        "2 to 10 letters, digits or spaces": ["de": "2 bis 10 Buchstaben, Ziffern oder Leerzeichen",
                                              "fr": "De 2 à 10 lettres, chiffres ou espaces",
                                              "pl": "Od 2 do 10 liter, cyfr lub spacji"],
        "Cancel": ["de": "Abbrechen", "fr": "Annuler", "pl": "Anuluj"],
        "Chat": ["de": "Chat", "fr": "Discussion", "pl": "Czat"],
        "Clear": ["de": "Löschen", "fr": "Effacer", "pl": "Wyczyść"],
        "Clear the scores?": ["de": "Ergebnisse löschen?", "fr": "Effacer les scores ?", "pl": "Wyczyścić wyniki?"],
        "Create": ["de": "Erstellen", "fr": "Créer", "pl": "Stwórz"],
        "Delete": ["de": "Löschen", "fr": "Supprimer", "pl": "Usuń"],
        "Delete the profile %@?": ["de": "Profil „%@“ löschen?", "fr": "Supprimer le profil « %@ » ?",
                                   "pl": "Usunąć profil „%@”?"],
        "Delete the save?": ["de": "Spielstand löschen?", "fr": "Supprimer la sauvegarde ?", "pl": "Usunąć zapis?"],
        "Done": ["de": "Fertig", "fr": "Terminé", "pl": "Gotowe"],
        "Game settings": ["de": "Spieleinstellungen", "fr": "Paramètres de la partie", "pl": "Ustawienia gry"],
        "Its saved games and campaign progress go with it.": [
            "de": "Seine Spielstände und der Fortschritt in den Kampagnen gehen verloren.",
            "fr": "Ses sauvegardes et sa progression dans les campagnes seront perdues.",
            "pl": "Zapisane gry i postępy w kampaniach tego profilu przepadną."],
        "Looking for games on this network…": ["de": "Suche Spiele in diesem Netzwerk…",
                                               "fr": "Recherche de parties sur ce réseau…",
                                               "pl": "Szukanie gier w tej sieci…"],
        "Map": ["de": "Karte", "fr": "Carte", "pl": "Mapa"],
        "Message": ["de": "Nachricht", "fr": "Message", "pl": "Wiadomość"],
        "Name": ["de": "Name", "fr": "Nom", "pl": "Nazwa"],
        "Next": ["de": "Nächster", "fr": "Suivant", "pl": "Następny"],
        "Play": ["de": "Spielen", "fr": "Jouer", "pl": "Graj"],
        "Player name": ["de": "Spielername", "fr": "Nom du joueur", "pl": "Nazwa gracza"],
        "Players": ["de": "Spieler", "fr": "Joueurs", "pl": "Gracze"],
        "Previous": ["de": "Vorheriger", "fr": "Précédent", "pl": "Poprzedni"],
        "Restore": ["de": "Zurücksetzen", "fr": "Rétablir", "pl": "Przywróć"],
        "Restore the defaults?": ["de": "Standardwerte wiederherstellen?", "fr": "Rétablir les valeurs par défaut ?",
                                  "pl": "Przywrócić ustawienia domyślne?"],
        "Send": ["de": "Senden", "fr": "Envoyer", "pl": "Wyślij"],
        "started": ["de": "läuft", "fr": "en cours", "pl": "trwa"],
        // The skill wheel (SkillWheel.swift), in the games' own words for
        // skills (Fertigkeiten, compétences, umiejętności) and perks.
        "Skill wheel": ["de": "Fertigkeitenrad", "fr": "Roue des compétences", "pl": "Koło umiejętności"],
        "Skill": ["de": "Fertigkeit", "fr": "Compétence", "pl": "Umiejętność"],
        "Racial skill": ["de": "Volksfertigkeit", "fr": "Compétence raciale", "pl": "Umiejętność rasowa"],
        "Perk": ["de": "Fähigkeit", "fr": "Capacité", "pl": "Zdolność"],
        "Perks": ["de": "Fähigkeiten", "fr": "Capacités", "pl": "Zdolności"],
        "Ultimate": ["de": "Ultimative Fähigkeit", "fr": "Capacité ultime", "pl": "Zdolność ostateczna"],
        "Needs": ["de": "Benötigt", "fr": "Nécessite", "pl": "Wymaga"],
        "Opens": ["de": "Ermöglicht", "fr": "Permet", "pl": "Odblokowuje"],
        "chance": ["de": "Chance", "fr": "chance", "pl": "szansa"],
        "The game has no skill table to show.": ["de": "Das Spiel hat keine Fertigkeitentabelle.",
                                                 "fr": "Le jeu n'a pas de table des compétences.",
                                                 "pl": "Gra nie ma tabeli umiejętności."],
        "Tap a skill or a perk to see what it does and what it needs. Pinch or double-tap to zoom.": [
            "de": "Tippe auf eine Fertigkeit oder Fähigkeit: was sie bewirkt und was sie benötigt. Zum Vergrößern zwei Finger spreizen oder doppelt tippen.",
            "fr": "Touchez une compétence ou une capacité pour voir son effet et ce qu'elle nécessite. Pincez ou touchez deux fois pour zoomer.",
            "pl": "Dotknij umiejętności lub zdolności, aby zobaczyć, co daje i czego wymaga. Rozsuń palce lub stuknij dwa razy, aby powiększyć."],
        // The hero on the wheel.
        "Hero": ["de": "Held", "fr": "Héros", "pl": "Bohater"],
        "level": ["de": "Stufe", "fr": "niveau", "pl": "poziom"],
        "Their class": ["de": "Seine Klasse", "fr": "Sa classe", "pl": "Jego klasa"],
        "learnt": ["de": "erlernt", "fr": "acquis", "pl": "poznane"],
        "can take": ["de": "wählbar", "fr": "possible", "pl": "do wzięcia"],
        "offered": ["de": "angeboten", "fr": "proposé", "pl": "proponowane"],
        "Learnt": ["de": "Erlernt", "fr": "Acquis", "pl": "Poznane"],
        "Can take": ["de": "Jetzt wählbar", "fr": "Possible maintenant", "pl": "Można wziąć"],
        "Offered now": ["de": "Jetzt angeboten", "fr": "Proposé maintenant", "pl": "Proponowane teraz"],
        "Not learnt": ["de": "Nicht erlernt", "fr": "Non acquis", "pl": "Nie poznane"],
    ]
}
