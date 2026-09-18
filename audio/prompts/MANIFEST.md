# DFR0534 prompt set — Family Assistance Alert (bench v0)

Track index on the module = **copy order** onto its USB drive, so these must be
copied one at a time in this order (use `tools/load_dfr0534_prompts.sh`). The
index column is what `src/audio_dfr0534.h` `enum gs_prompt` plays.

| Track | File | Content | Source |
|---|---|---|---|
| 01 | `01_silence.wav` | 100 ms silence — power-up auto-play guard | synthesized |
| 02 | `02_pressed_20s.wav` | "Assistance button pressed, contacting care circle in 20 seconds." | macOS `say` (Samantha, 165 wpm) — **bench placeholder**; production voice = Polly neural (spec D12) |
| 03 | `03_tone.wav` | two 880 Hz beeps | synthesized |
| 04 | `04_contacting_10s.wav` | "Contacting care circle in 10 seconds." | say |
| 05 | `05_cancelled.wav` | "Cancelled." | say |
| 06 | `06_contacted.wav` | "Contacted care circle." | say |
| 07 | `07_retrying.wav` | "Could not reach your care circle yet. Still trying." | say |
| 08 | `08_test_ok.wav` | "Test complete. Care circle contacted." | say |
| 09 | `09_not_setup.wav` | "Assistance is not set up yet." | say |
| 10 | `10_fault.wav` | descending error tone | synthesized |

All files: WAV, 16 kHz, mono, 16-bit PCM (the module decodes 8–48 kHz WAV/MP3
in hardware). Regenerate with `tools/gen_prompts.sh` once the production voice
is chosen. The firmware's bench self-test logs the module's own index→name
mapping at power-up so a mis-ordered copy is visible immediately.
