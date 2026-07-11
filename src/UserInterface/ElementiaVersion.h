#pragma once
//
// ELEMENTIA CLIENT BUILD VERSION — DIE eine Stelle fuer die Versionsnummer des
// Spiel-Clients (Elementia.exe).
//
// Bei JEDEM ausgelieferten Client-Build (insbesondere Hotfixes) diesen Wert
// monoton um 1 erhoehen. Der Client meldet ihn als CG::VERSION_GATE
// (component=CLIENT) direkt vor CG_LOGIN2 (und vor CG_LOGIN3 im klassischen
// AccountConnector-Pfad). Der Server erzwingt serverseitig
// MIN_CLIENT_VERSION aus share/conf/game.txt — liegt dieser Build darunter,
// wird der Login mit "UPDVER" abgewiesen.
//
// Pendant fuer den Launcher: src/game-client/src/net/version.js (LAUNCHER_VERSION).
//
#define ELEMENTIA_CLIENT_VERSION 1
