# Exercice : oscilloscope en temps réel sur l'USART2

Objectif : afficher en direct, dans une fenêtre Python, l'entrée et la sortie du
circuit testé, rafraîchies une dizaine de fois par seconde, comme sur un
oscilloscope.

Cet exercice part du mode capture déjà en place, fourni par `ato_analog`
(`gen`, `acq`, `dump`) et par `tools/scope/scope.py`.

---

## 1. Le débit : pourquoi ça ne passe pas aujourd'hui

Le débit nécessaire vaut `fs × 2 voies × la taille d'un point`. On le compare au
débit utile de l'UART, qui est d'environ `bauds / 10` octets par seconde (un bit
de start et un bit de stop par octet).

| Configuration | Débit utile | Besoin à fs = 20 kHz |
|---|---|---|
| CLI actuelle : 115200 bauds, CSV hex de 9 octets par point | ~11,5 Ko/s | 180 Ko/s, donc trop |
| USART2 à 921600 bauds (maximum du CP2102), binaire de 3 octets par point | ~92 Ko/s | 60 Ko/s, ça passe |

On a donc besoin de deux choses : un **débit plus élevé** et un **format binaire
compact**.

Le binaire ne peut pas passer dans la CLI, car des octets `\r`, `\n` ou
d'échappement y casseraient le texte. Il faut un **port dédié aux données**,
l'USART2. La CLI de l'USART1 reste en texte et sert à envoyer les commandes.

## 2. Principe : capture en salves, déclenchement côté Python

Un oscilloscope numérique ne transmet pas tout en continu. Il **capture une
salve** de n points, l'affiche, puis recommence.

- **Le générateur tourne en continu.** Le DAC et TIM7 ne s'arrêtent pas entre deux
  images, donc le circuit reste en régime établi. Ce n'est pas le cas de `acq`,
  qui démarre et arrête tout à chaque capture.
- **L'ADC est réarmé à chaque image** avec `ato_adc_start_dma` puis
  `ato_adc_wait_dma`. Pendant que l'ADC n'est pas armé, ses triggers sont
  simplement ignorés.
- **Le déclenchement (edge trigger) se fait en Python.** Dans chaque image, on
  cherche le premier front montant de l'entrée qui passe par le niveau milieu, et
  on affiche à partir de là. Sans cela, l'image « défile », car le début de la
  capture n'est pas aligné sur la période du signal.
  - Il faut donc capturer au moins **une période de plus** que ce qu'on affiche.
- **Cadence** : n = 1000 et fs = 20 kHz donnent 50 ms de capture et environ 33 ms
  d'envoi, soit environ 12 images/s.

## 3. CubeMX

**USART2** sur PB3 (TX) et PB4 (RX), en mode Asynchronous :

- **921600 bauds**, 8 bits, sans parité, 1 bit de stop ;
- DMA **RX en circulaire** et **TX en normal**, en octets. `ato_uart_init` vérifie
  la présence des deux par un assert, même si l'on n'utilise que l'émission ;
- `USART2 global interrupt` activé, avec une priorité ≥ 5. Les callbacks HAL
  appellent des fonctions `FromISR`.

Câblage avec le CP2102 :

- PB3 (TX) va sur le RXD du CP2102 ;
- PB4 (RX) va sur son TXD ;
- relier les deux masses (GND).

## 4. Firmware

### 4.1 Format de trame (binaire, little-endian)

| Champ | Type | Contenu |
|---|---|---|
| magic | 4 octets | `"SCP1"`, pour se resynchroniser |
| seq | u32 | numéro de trame, qui permet de détecter les trames perdues |
| fs_mhz | u32 | fs réelle en mHz (200 kHz × 1000 tient dans un u32) |
| f_mhz | u32 | fréquence réelle du signal en mHz |
| n | u16 | nombre de points |
| vref_mv | u16 | 3300 |
| points | n × 3 octets | deux valeurs de 12 bits par point, voir ci-dessous |
| crc | u16 | CRC16 de tout ce qui précède, magic compris |

Les deux valeurs de 12 bits d'un point se rangent dans 3 octets :
- `b0 = in >> 4` ;
- `b1 = ((in & 0xF) << 4) | (out >> 8)` ;
- `b2 = out & 0xFF`.

Choisir un CRC16 que Python sait aussi calculer facilement, par exemple
CRC-16/CCITT-FALSE : polynôme 0x1021, valeur initiale 0xFFFF. Le calculer
**en logiciel** est plus simple pour commencer. Le G4 a aussi un périphérique
CRC, mais il demande davantage de configuration.

### 4.2 Tâche `SCOPE`

Elle peut vivre dans `ato_analog.c` ou dans un nouveau `ato_scope.c`.

- Elle possède une instance `ato_uart_t` sur l'USART2. Son contexte peut rester
  `static`, puisque le module n'a qu'une instance.
- **Piège** : `ato_uart_write_wait()` dort sur la notification de la tâche
  enregistrée par `ato_uart_start_rx(u, task)`. La tâche `SCOPE` doit donc appeler
  elle-même `ato_uart_start_rx(&uart2, xTaskGetCurrentTaskHandle())` au début de
  son exécution.
- La tâche attend une demande de démarrage, par une notification ou un sémaphore.
  Puis, tant que le mode run est actif, elle boucle :
  1. prendre le mutex `s_ctx.lock` ;
  2. si les paramètres de `gen` ont changé (un compteur de génération suffit),
     reconstruire la table et redémarrer le DMA du DAC ;
  3. lancer `ato_adc_start_dma(samples, 2*n)` puis `ato_adc_wait_dma(...)` ;
  4. encoder la trame dans un buffer d'environ 3 Ko pour n ≤ 1000 ;
  5. rendre le mutex ;
  6. appeler `ato_uart_write_wait(&uart2, trame, taille)`.
  - Le mutex est libéré **avant** l'envoi, pour que la CLI puisse agir entre deux
    images.
- Pile de la tâche : pas de gros tableaux locaux. Les buffers sont `static`.

### 4.3 Commandes CLI

- **`run <fs> <n>`**
  - n est limité à 1000 environ, pour garder une bonne cadence.
  - Refuser le signal `step`, qui est un événement unique : utiliser `square`.
  - Régler TIM7, activer le trigger du DAC, lancer le DMA du DAC, démarrer le
    timer, puis réveiller la tâche `SCOPE`.
- **`stop`**
  - Demander l'arrêt à la tâche.
  - Attendre la fin de l'image en cours : prendre le mutex suffit.
  - Arrêter le timer et le DMA du DAC, désactiver le trigger, remettre le DAC au
    repos. La séquence est la même qu'à la fin de `acq_command`.
- **`gen` pendant run** : ne change que les paramètres et incrémente le compteur
  de génération. La tâche applique le changement à l'image suivante.
- **`acq` et `dump` pendant run** : répondre `error: scope running`.

### 4.4 Pièges déjà rencontrés dans ce projet

- **Trigger du DAC** : le bit `TENx` ne se modifie que canal désactivé. Il faut
  donc appeler `HAL_DAC_Stop`, puis `dac_trigger_enable`, puis
  `HAL_DAC_Start_DMA`.
- **Prescaler du timer** : il est bufferisé. Il faut forcer un update event
  (`EGR = UG`) **avant** d'armer le DAC et l'ADC, sinon le TRGO de cet update les
  déclencherait.
- **Overrun ADC** : en fin de DMA, le timer continue de déclencher l'ADC jusqu'au
  `Stop_DMA`. Le compteur `errors` d'`ato_adc` augmente alors, ce qui est sans
  conséquence. En mode run, le timer ne s'arrête jamais : ce sera systématique.
- **Entrée en l'air** : une voie non câblée recopie le canal précédent. Ce n'est
  pas un bug du code.

## 5. Python : mode `--live`

```
uv run scope.py --live --port <CLI USART1> --data <CP2102> \
    --shape sine --freq 160 --vpp 2000 --fs 20000 --n 1000 --rc 10e3 100e-9
```

- **Thread de lecture** sur le port de données, à 921600 bauds :
  1. chercher `b"SCP1"` dans le flux ;
  2. lire l'en-tête avec `struct.unpack("<4sIIIHH", ...)` ;
  3. lire les `3*n` octets de points et les 2 octets du CRC ;
  4. vérifier le CRC. Si la trame est corrompue, la jeter et chercher le magic
     suivant ;
  5. déballer les points avec numpy :

     ```python
     b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.uint16)
     vin = (b[:, 0] << 4) | (b[:, 1] >> 4)
     vout = ((b[:, 1] & 0xF) << 8) | b[:, 2]
     ```
  6. ne garder que **la dernière trame** reçue, avec un verrou ou une
     `queue.Queue(maxsize=1)`.
- **Affichage** : `matplotlib.animation.FuncAnimation` avec un intervalle
  d'environ 50 ms. Pour la vitesse, créer les `Line2D` une seule fois puis appeler
  `set_data`, et utiliser `blit=True`.
- **Déclenchement** : trouver le premier `i` tel que
  `vin[i-1] < mid <= vin[i]`, avec `mid = (vin.min() + vin.max()) / 2`, puis
  afficher la fenêtre `[i, i + n_affiché)`.
  - Option : un peu d'**hystérésis**, pour ne pas déclencher sur le bruit.
- **Théorie RC** : réutiliser `rc_lowpass()` et `sine_fit()`. En régime établi,
  la valeur initiale `v0` de `rc_lowpass` peut être prise sur la sortie mesurée au
  point de déclenchement.
- **Démarrage et arrêt** : envoyer `gen …` puis `run …` par la classe `Scope`
  existante. Envoyer `stop` dans un `finally`, ou sur l'événement
  `close_event` de la figure.
- **Affichage de contrôle** : le nombre d'images/s, le nombre de trames perdues
  (trou dans `seq`) et le nombre de CRC faux.

## 6. Vérification

1. **Débit** : `run 20000 1000` sans Python, en regardant le flux avec
   `python -m serial.tools.miniterm`. On doit voir passer de l'illisible ; on
   cherche juste « SCP1 » dans le flux.
2. **PA4 relié à PA0 et PB14** : les deux courbes doivent être superposées et
   l'image stable. Vérifier qu'aucun CRC n'est faux.
3. **`gen sine 50 …` puis `gen sine 500 …` pendant run** : le signal doit changer
   sans redémarrage.
4. **Passe-bas RC** : faire varier la fréquence en direct et voir l'amplitude
   baisser et le déphasage augmenter autour de fc ≈ 160 Hz.
5. **`stop`** : le flux s'arrête, `acq` et `dump` refonctionnent, `ps` et
   `stats` sont normaux.

## 7. Pour aller plus loin

- **Double buffer** : capturer l'image k+1 par DMA pendant l'envoi de l'image k.
  On visera environ 20 images/s, la limite devenant `max(capture, envoi)`.
- **Déclenchement dans le firmware** : n'envoyer que la fenêtre utile, ce qui
  réduit le débit nécessaire.
- **USB CDC natif** (PA11/PA12, environ 1 Mo/s) : permet du vrai flux continu
  jusqu'à ~100 kHz.
- **pyqtgraph** à la place de matplotlib, pour un rafraîchissement plus rapide.
- **Mode Bode automatique** : un balayage de fréquences en sinus, avec la mesure
  de gain et de phase par `sine_fit`, puis le tracé en log-log.
