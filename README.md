# pt3player
pt3player

#EN:
This is pt3player, based on [https://github.com/Volutar/pt3player](https://github.com/Volutar/pt3player), which extends the playback functionality and adds Linux support.

Usage: playpt3.exe [--channel-map MODE] <file.pt3>.
Channel map modes:
  default      - 3 channels: A, B, C
  split-all    - 9 sub-channels: tone/noise/env per A,B,C
  compact      - 3 groups: Tones, Noise, Envelopes
  minimal      - 3 groups: Melody, Bass/FX, Noise
  buzzer-split - 4 groups: Normal, Buzzer, Noise, Envelope

#RU:
Это pt3player, основанный на [https://github.com/Volutar/pt3player](https://github.com/Volutar/pt3player), который расширяет функциональность воспроизведения и добавляет поддержку Linux.

Использование: playpt3.exe [--channel-map режим] <file.pt3>.
Режимы карты каналов:
  default — стандартные 3 канала (A, B, C) как сейчас
  split-all — каждый канал разделяется на тон, шум, огибающую (до 9 суб-каналов)
  compact — разделение на: тоны (все), шумы (все), огибающие (все) — 3 группы
  minimal — разделение на: мелодия (тон без envelope), басы/эффекты (с envelope), шум — 3 группы
  buzzer-split — отделяет каналы с buzzer-эффектом (быстрая envelope) от обычных
