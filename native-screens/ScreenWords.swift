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
    ]
}
