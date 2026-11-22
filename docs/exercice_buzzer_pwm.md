# Exercice : jouer des notes sur un buzzer avec le PWM d'un timer

Objectif : jouer des notes, puis des mélodies, sur un buzzer passif, piloté par un
timer en PWM à travers un transistor. Les commandes passent par la CLI.

---

## 1. Rappels matériel

### Buzzer actif ou passif

| | Buzzer actif | Buzzer passif |
|---|---|---|
| À l'intérieur | Un oscillateur et un transducteur | Un transducteur seul |
| Signal à lui donner | Une tension continue | Un signal alternatif à la fréquence de la note |
| Résultat | Une seule note fixe, marche/arrêt | N'importe quelle note |
| Test | Sous tension continue, il sonne | Sous tension continue, il fait juste « clic » |

Pour jouer des notes, il faut le **passif**. L'actif se pilote par une simple
GPIO (voir la section 6).

### Pourquoi un transistor

Les buzzers des kits sont généralement **magnétiques**, avec une impédance
d'environ 16 à 42 Ω, et demandent 20 à 50 mA. Une GPIO du STM32 ne doit pas
fournir plus d'environ 20 mA, et le DAC à peine 1 mA. Il faut donc un
**transistor NPN en commutation** : la broche ne fournit que le petit courant de
base, et le transistor fait passer le courant du buzzer.

### Schéma

```
          5V (ou 3V3) sur CN6
              │
          ┌───┴───┐
       buzzer    diode 1N4148 (cathode vers le haut)
          └───┬───┘
              │ C
PA6 ── 1 kΩ ──┤ B     NPN (S8050, 2N2222, BC547…)
              │ E
             GND (commun avec la carte)
```

- **La résistance de base** (1 kΩ) limite le courant de la broche à environ
  2,6 mA, avec 3,3 V moins les 0,7 V de la jonction base-émetteur. C'est largement
  assez pour saturer le transistor.
- **La diode de roue libre** protège le transistor de la surtension de la bobine
  du buzzer quand le courant est coupé. Elle est inutile pour un buzzer piézo,
  mais sans danger.
- **Le brochage E, B, C** dépend du modèle. Vérifie la datasheet de ton
  transistor.
- **Alimentation** : utilise le **5V** ou le **3V3** du connecteur **CN6**.
  **Pas VREF+**, qui est la référence de l'ADC et du DAC : un buzzer la ferait
  onduler et fausserait les mesures. Avec 5V, le son est plus fort, et la broche
  du STM32 ne voit que la base, donc ce n'est pas un problème.

## 2. Le PWM et les notes

### Fréquence et rapport cyclique

Un timer en mode PWM produit un signal carré :

```
f_pwm  = f_timer / ((PSC + 1) × (ARR + 1))
duty   = CCR / (ARR + 1)
```

- TIM3 est sur APB1, à **170 MHz**.
- Avec **PSC = 169**, le compteur avance à **1 MHz**, et le calcul devient simple :
  `ARR = 1 000 000 / f − 1`.
  - Pour 440 Hz, on obtient ARR = 2272.
  - Pour 100 Hz, ARR = 9999. Pour 5 kHz, ARR = 199.

  Tout tient dans les 16 bits.
- **Rapport cyclique de 50 %**, soit `CCR = (ARR + 1) / 2`. C'est le son le plus
  fort pour un buzzer.
  - Un rapport plus faible (10 à 20 %) donne un volume un peu plus bas et un timbre
    différent. C'est un « volume » grossier.

### Fréquence des notes

La gamme tempérée vaut `f = 440 × 2^((n − 69) / 12)`, où `n` est le numéro MIDI
de la note (A4 = 69).

| Note | C4 | D4 | E4 | F4 | G4 | A4 | B4 | C5 |
|---|---|---|---|---|---|---|---|---|
| Hz | 261,6 | 293,7 | 329,6 | 349,2 | 392,0 | 440,0 | 493,9 | 523,3 |

Un dièse multiplie la fréquence par 2^(1/12) ≈ 1,0595. Une octave au-dessus, la
fréquence est doublée.

Un buzzer rend en général le mieux entre **2 et 4 kHz**, sa fréquence de
résonance, souvent indiquée sur la datasheet. Les octaves 5 à 7 sonnent plus fort
que l'octave 4.

## 3. CubeMX

**TIM3** :
- Channel1 en **PWM Generation CH1**, sur **PA6** (Arduino D12, sur CN5) ;
- Prescaler **169**, Counter Period **2272** (pour démarrer à 440 Hz) ;
- **auto-reload preload : Enable**. Ainsi, un changement d'ARR ne prend effet qu'à
  la fin de la période en cours, sans glitch ;
- Pulse **1136**, avec output compare preload activé ;
- pas d'interruption nécessaire.

Avant de choisir la broche, vérifie dans le pinout qu'elle est libre et qu'elle
n'est pas en conflit, comme l'étaient ADC1_IN2 à IN4 avec LPUART1. Autres
candidates : TIM16_CH1 ou TIM1_CH1 (PA8, D7).

## 4. Firmware : progression conseillée

### Étape 1 : une note fixe

- Dans une commande `tone_on`, appeler `HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1)`.
  Dans `tone_off`, appeler `HAL_TIM_PWM_Stop`.
- Ça doit faire un la à 440 Hz. C'est le premier test du câblage.

### Étape 2 : `tone <Hz> [duty%]`

- Calculer `arr = 1000000 / f - 1` et `ccr = (arr + 1) * duty / 100`.
- Appliquer `__HAL_TIM_SET_AUTORELOAD(&htim3, arr)` et
  `__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, ccr)`.
- Limiter f à une plage raisonnable, par exemple 20 Hz à 20 kHz, et répondre
  `error:` sinon.
- `tone 0` coupe le son.

### Étape 3 : `tone <Hz> <ms>`

- Une durée suppose une attente. Un `vTaskDelay` **dans le handler CLI** bloquerait
  la CLI pendant la note. C'est acceptable pour une note courte, mais pas pour une
  mélodie.
- Meilleure solution : une **tâche `BUZZER`** avec une **queue** de notes
  `{freq, ms}`. La tâche joue chaque note, puis fait un `vTaskDelay(ms)`.
  - La commande CLI ne fait que pousser dans la queue et rend la main tout de
    suite.
  - C'est la même architecture que la première version d'`ato_analog`.

### Étape 4 : `play <notes>`

- Syntaxe proposée : `play C4:200 E4:200 G4:200 C5:400 R:200`.
  - `R` est un silence. `#` donne un dièse, par exemple `F#4:200`.
- Parser chaque mot avec `ato_cli_core_get_parameter` (le nombre de paramètres est
  variable, donc `expected_params = -1`).
- Convertir le nom de la note en numéro MIDI, puis en fréquence, avec la formule
  de la section 2. `powf` existe, ou bien une table de 12 fréquences complétée par
  des décalages d'octave.
- **Articulation** : couper le son 10 à 20 ms à la fin de chaque note. Sans cela,
  deux notes identiques consécutives se fondent en une seule.
- Attention à la longueur de ligne de la CLI : `CLI_CMD_SIZE` vaut 100. Pour une
  longue mélodie, on peut l'augmenter, ou stocker des mélodies dans le firmware et
  les lancer par un nom, avec `play tune1`.

### Étape 5 (bonus) : `stop` et volume

- `stop` vide la queue avec `xQueueReset` et coupe le son.
- `volume <0..50>` règle le rapport cyclique par défaut.

## 5. Vérification

1. **Sans buzzer**, relier PA6 à **PA0** et utiliser la scope existante :
   - lancer `tone 440 50` (ou `tone_on`), puis `acq 20000 2000` et `dump`, ou le
     script `scope.py`. On doit voir un carré de 0 à 3,3 V, avec une période de
     2,27 ms et un rapport cyclique de 50 %.
   - Faire varier le duty et vérifier la largeur des créneaux.
   - Le signal du PWM est entre 0 et 3,3 V, donc sans danger pour l'entrée de
     l'ADC.
2. **Avec le buzzer** : `tone 440`, puis `tone 2000`. La deuxième doit sonner plus
   fort, car elle est plus proche de la résonance.
3. **Gamme** : `play C5:300 D5:300 E5:300 F5:300 G5:300 A5:300 B5:300 C6:600`.
4. **La CLI reste réactive pendant une mélodie** : `ps` doit répondre tout de suite
   et afficher la tâche `BUZZER`.

## 6. Variante : le buzzer actif

- Même montage à transistor. La base est pilotée par une **GPIO en sortie
  push-pull**, et non par le PWM.
- `HAL_GPIO_WritePin(..., GPIO_PIN_SET)` le fait sonner, `RESET` l'arrête.
- On peut faire des bips rythmés (`beep <n> <ms>`) avec la même tâche et la même
  queue.
- Expérience amusante : piloter le buzzer actif en PWM à quelques dizaines de Hz.
  On entend sa note fixe hachée.

## 7. Pour aller plus loin

- **Un vrai haut-parleur avec le DAC** : avec un petit amplificateur audio
  (PAM8403, LM386) et un haut-parleur de 8 Ω, on joue des **sinus** générés par le
  DAC et le DMA, comme dans `gen sine`, et le volume se règle avec l'amplitude.
  - On peut mélanger deux notes (somme de deux sinus) pour faire des accords, ou
    appliquer une enveloppe (attaque et déclin) pour un son plus naturel.
- **Format RTTTL** : le format texte des sonneries de téléphones Nokia, qu'on peut
  parser pour jouer des mélodies existantes.
