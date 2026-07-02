# Storyline — guide de l'opérateur du master

Déroulé opérationnel, soir par soir, pour la personne qui pilote les artefacts
depuis son cellulaire. Chaque cue narratif est associé à l'action exacte dans
l'interface. À relire avec la trame narrative complète avant chaque soirée.

> **Ce qui n'est PAS piloté par le master** : l'audio (haut-parleurs — manuel,
> aides de camp), le Miroir de Brume (lampe UV manuelle), la Corde des Liens
> (fil lumineux manuel), la Cloche (réelle) et le Diapason (autonome, RFID/IR).

---

## 0. Routine de connexion (à chaque utilisation)

1. Alimenter le master (ESP32) et attendre ~10 s.
2. Sur le cellulaire : WiFi **`MASTER_AP`**, mot de passe **`masterpass`**.
3. Ouvrir **`http://192.168.4.1`** dans le navigateur.
4. Basculer en vue **Détaillé** et vérifier :
   - le compteur « X/Y en ligne » de chaque section ;
   - les niveaux de **batterie** ;
   - presser **Time (sync)** — obligatoire après chaque redémarrage du master,
     sinon les balises ne connaissent pas l'heure (elles resteront allumées).
5. Revenir en vue **Prod** (ou passer en mode **Discret** pendant les scènes).

**Latence à connaître** : la nuit et en mode forcé, les balises réagissent
immédiatement. Le **jour**, elles dorment par cycles de 60 s : une commande
peut prendre jusqu'à une minute (badge « en attente » dans l'interface).
Anticiper les cues de jour d'une minute.

**Interruption d'urgence (mot « Lumière »)** : scène **Fin** (tout revient
calme : balises bleu profond, lanterne bougie, médaillon éteint), ou boutons
**Normal** (balises) / **Bougie** (lanterne) individuellement.

---

## 1. Samedi — « L'Appel des cordes brisées » + rituel des Balises

**Avant la veillée** (installation des balises aux 4 points cardinaux) :

- Vue Prod → section Balises → **Éteindre** (`FORCE_OFF`) : les balises sont
  plantées éteintes, « endormies depuis des siècles ».
- Vérifier en vue Détaillé que les 4 sont bien **en ligne** (elles répondent
  même éteintes).

**Pendant le rituel** (chaque groupe compte « un… sept » la main sur sa balise) :

- Vue **Détaillé**, tableau des accessoires : au « sept » de chaque groupe,
  presser **Idle** sur la ligne de **cette balise** (Nord, Sud, Est, Ouest).
  La lumière bleue se met à pulser à ce moment précis. Se placer discrètement
  à portée du WiFi du master.

**Fin de soirée** : rien à faire — les balises restent en veille bleue toute
la nuit (comportement automatique). Les jeunes les voient pulser des tentes.

---

## 2. Dimanche — « Le murmure qui copie » + commando « La Veille du bord noir »

**Récit au feu** : rien côté master.

**Lancement du commando** (« une balise a vacillé ») :

- Choisir la balise « touchée » (la plus proche de la zone du commando).
- Vue Détaillé → sa ligne : **Glitch**, puis **GlitchLock On** — la balise
  reste en glitch même quand le groupe se rassemble autour d'elle.

**Résolution** (le « cœur d'ancrage » est replacé dans la balise) :

- Au moment où les louveteaux replacent l'objet : **GlitchLock Off** puis
  **Idle** sur cette balise (`FORCE_IDLE` déverrouille et rend le contrôle au
  cycle normal). Effet vécu : la balise « se raccorde » et redevient bleue
  sous leurs yeux.

---

## 3. Lundi — « Le fil qui tient dans le noir » + commando « Le Cercle qui répond »

Soirée essentiellement manuelle (sons des Porteurs d'Échos, fragment sonore).

- Option d'ambiance : un **Glitch** bref (puis **Idle**) sur la balise du côté
  du bois d'où « viennent les sons », au moment choisi par Rama.
- Rien d'autre côté master.

---

## 4. Mardi — « L'épreuve de ceux qui restent liés » + « Le passage des sans-lune »

Le commando se déroule **hors du camp, hors de portée du master** (la Corde
pulse par fil lumineux manuel, la Cloche est réelle).

- Avant le départ : vérifier que les balises sont en veille bleue normale
  (section Balises → **Normal** si un mode forcé traînait).
- Pendant le commando : rien — le camp reste « protégé » (balises bleues),
  ce que voient ceux qui restent.

---

## 5. Mercredi — « La lumière que l'on transmet »

**LE cue de la soirée** : Rama sonne la Cloche ; les balises passent
momentanément du bleu au blanc brillant, « rechargées par le son ».

- Vue Prod → section Balises → **Recharge** (`FORCE_RECHARGE`) **au moment
  exact où la Cloche sonne** : flash blanc ~1,5 s puis retour automatique au
  bleu stable. Aucune seconde commande à envoyer.
- Répétable si Rama refait sonner la Cloche.

---

## 6. Jeudi — transmission (soirée douce, pas de commando)

- La **Lanterne des Portées** entre en jeu (trouvée dans la journée) :
  vue Prod → section Lanterne → **Bougie** pour la veillée de chants — lumière
  chaude vacillante au centre du cercle.
- Option pendant la découverte sur le sentier : **Révélation** (mode Discret →
  Lanterne) pour les ombres mouvantes, puis retour **Bougie**.
- Balises : rien (routine nocturne automatique).

---

## 7. Vendredi (journée) — les signes que le Windigo se rapproche

Manifestations ponctuelles pendant la chasse au Médaillon (⚠️ latence jusqu'à
60 s le jour — anticiper) :

1. « Les balises clignotent en ambre par moments » : section Balises de la vue
   Détaillé → **Alerte** sur une ou deux balises (via les boutons de la liste
   « Alerte balises ») ; laisser 1–2 minutes ; puis **Normal** — de jour elles
   se rendorment (économie batterie pour le soir).
2. Garder ces épisodes **courts et rares** : la montée se joue le soir.

**Fin d'après-midi, avant la finale** : brancher/charger ce qui doit l'être,
vérifier « 4/4 · 1/1 · 1/1 en ligne » et les batteries, resynchroniser
l'heure (**Time (sync)**), puis passer le cellulaire en mode **Discret**.

---

## 8. Vendredi soir — LA NUIT DU GRAND RAPPEL

Tout se pilote depuis le mode **Discret** (ou la vue Prod) avec les boutons de
scènes préprogrammées. **Une scène = un cue. Dans l'ordre :**

| # | Cue narratif | Bouton | Effet |
|---|---|---|---|
| 1 | Procession, assemblée autour du feu | **Prépa** | balises ambre fixe (éveillées), lanterne bougie, médaillon au repos |
| 2 | Installation des artefacts — « les balises tiennent » | **Installation** | balises bleu stable ; lanterne en révélation (posée au centre) |
| 3 | L'attaque — « il teste le cercle » | **Attaque** | lanterne s'éteint lentement puis clignote rouge (Windigo) |
| 3b | Le Windigo teste chaque point du périmètre | **Séquence auto** (ou taps balise par balise) | les balises passent en ambre clignotant une à une, 2,5 s d'écart |
| 3c | « Le Miroir. Tournez le Miroir. » | — | manuel (UV) |
| 3d | « La Cloche. Maintenant. » | **Cloche** | toutes les balises reviennent au bleu stable d'un coup |
| 4 | Contact Baguette ↔ Médaillon | **Accord final** | médaillon : comète montant ~7 s vers l'or ; balises arc-en-ciel pulsant ; lanterne blanc brillant. **Presser à l'instant du contact** — la montée de 7 s EST l'effet dramatique, la note d'Akéla arrive sur le sommet |
| 5 | Retour au calme — « La Symphonie est jouée » | **Fin** | balises bleu profond très lent, lanterne bougie dorée, médaillon au repos |

Notes de régie :

- Pendant la phase 3, **rythmer** : quelques alertes individuelles valent
  mieux qu'un déluge. La lanterne Windigo porte l'ambiance à elle seule.
- Le gros bouton **FINALE** (glitch général + déclenchement du médaillon,
  tout simultané) est un **plan B chaos**, pas le déroulé nominal — les
  scènes ci-dessus suivent le récit. Ne pas le presser par accident.
- Si une commande ne part pas (badge « en attente ») : les appareils sont
  éveillés le soir, c'est presque toujours la portée WiFi du cellulaire vers
  le **master** — se rapprocher du master, pas de la balise.
- Après la scène **Fin**, laisser tel quel : les balises reprennent leur
  veille de nuit d'elles-mêmes après un **Normal** (à envoyer une fois les
  jeunes couchés, pour la dernière nuit sous protection du Cercle).

---

## 9. Après le camp

- Tout éteindre : vue Prod → **Éteindre** (balises), **Éteindre** (lanterne),
  **Arrêter** (médaillon), puis couper l'alimentation du master.
