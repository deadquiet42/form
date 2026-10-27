# FORM

An instrument made of light. A small body of points sits still and silent until you touch it, then bends, twists, swells and sings, with tuned strikes on every face. Every launch is a slightly different FORM. Headphones recommended.

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
