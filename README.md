# FORM

An experimental audiovisual sculpture. Pure maths: a body of points, a drone and tuned strikes, all generated in real time and shaped by the controls. A nod to the demoscene and to 90s and 00s drone and IDM.

Needs a dual stick layout like the R36S: D-pad, A B X Y, L1 R1 L2 R2, two analogue sticks that click for L3 and R3, Select and Start. That includes the R36S and its clones, Anbernic RG35XX H, RG40XX H, RG351MP, RG353P, RG353M, RG353V, RG503, RG406H and RG556, Powkiddy X55, Retroid Pocket 5 and AYN Odin 2. Devices without two sticks or L3 and R3 lose part of it.

One C file, SDL2 only. No samples, fonts or other assets.

Download: https://deadquiet42.github.io/form/

## Build

```
gcc -O2 -s form.c -o form $(sdl2-config --cflags --libs) -lm -lpthread
```

The PortMaster release is cross compiled with clang and lld for aarch64, against glibc 2.17 symbol versions.

## Run

```
./form
```

Set `FORM_SEED` to a number to replay a particular launch; the seed is printed at startup.

## Licence

Freeware. See [LICENSE](LICENSE).
