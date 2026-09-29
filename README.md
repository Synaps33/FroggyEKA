# FroggyEKA

Port **EKA2L1** (emulator Symbian OS / Nokia N-Gage) na baremetalowe konsole
MIPS32 **SF2000 / GB300 V2** (rodzina Froggy).

Projekt to zestaw patchy + skryptów, które nakłada się na:

| projekt | upstream | commit bazowy |
|---|---|---|
| EKA2L1 | https://github.com/EKA2L1/EKA2L1 | `dd3e221` |
| sf2000_multicore | https://github.com/Trademarked69/sf2000_multicore | — |

## Zawartość repozytorium

```
patches/
  eka2l1-sf2000.patch                  # cały port EKA2L1 (zmiany + nowe pliki)
  submodule-*.patch                    # poprawki w submodułach EKA2L1
  sf2000_multicore-sf2000.patch        # zmiany frontendu Multicore
  sf2000_multicore-cores-eka2l1.patch  # cores/eka2l1/Makefile (build rdzenia)
  sf2000_multicore-libretro-common.patch
files/                                 # nowe pliki portu (do podejrzenia bez patcha)
scripts/
  build-core.sh                        # budowa rdzenia dla GB300V2
  deploy-sd.sh                         # wgranie rdzenia i loadera na SD
```

## Jak zastosować

```bash
# EKA2L1
git clone https://github.com/EKA2L1/EKA2L1.git eka2l1
cd eka2l1 && git checkout dd3e221
git apply -p1 /path/to/FroggyEKA/patches/eka2l1-sf2000.patch
(cd src/external/ffmpeg         && git apply /path/to/FroggyEKA/patches/submodule-ffmpeg.patch)
(cd src/external/lunasvg        && git apply /path/to/FroggyEKA/patches/submodule-lunasvg.patch)
(cd src/external/miniBAE        && git apply /path/to/FroggyEKA/patches/submodule-miniBAE.patch)
(cd src/external/re2            && git apply /path/to/FroggyEKA/patches/submodule-re2.patch)
(cd src/external/spdlog         && git apply /path/to/FroggyEKA/patches/submodule-spdlog.patch)

# sf2000_multicore
git clone https://github.com/Trademarked69/sf2000_multicore.git
cd sf2000_multicore
git apply -p1 /path/to/FroggyEKA/patches/sf2000_multicore-sf2000.patch
git -C cores apply -p1 /path/to/FroggyEKA/patches/sf2000_multicore-cores-eka2l1.patch
git -C libs/libretro-common apply -p1 /path/to/FroggyEKA/patches/sf2000_multicore-libretro-common.patch
```

## Budowa i wdrożenie

Wymagania: toolchain Froggy (`/opt/frog-toolchain/mipsel-mti-elf_stable/…`),
CMake ≥ 3.8, oraz checkout EKA2L1 skonfigurowany raz przez
`eka2l1/cmake/sf2000.toolchain.cmake`.

```bash
EKA2L1_SRC=~/gb300/eka2l1 MULTICORE=~/gb300/sf2000_multicore ./scripts/build-core.sh
SD=/media/$USER/GB300 ./scripts/deploy-sd.sh
```

Na karcie SD rdzeń musi być w **obu** miejscach:

```
/cores/eka2l1/core_87000000
/system/Deimos/cores/eka2l1.sf2k
```

Loader (`bisrv.asd`, budowany razem przez `make` w `sf2000_multicore`) trafia do
`/bios/bisrv.asd`. Jego kopia zapasowa zostaje jako `bisrv.asd.bak`.

## Co zawiera port (najważniejsze)

### Pamięć (64 MB RAM, brak MMU)

* Wszystkie mapowania idą przez `calloc`/`free` z 48 MB sterty na buforze
  `gp_buf_64m` (16 MB zostaje dla firmware).
* Bufor translacji `ARMul_State` zmniejszony do 2 MB (z 125 MB), gdyż obiekt
  jest alokowany w całości; interpreter sam czyści bufor po zapełnieniu.
* Chunki Symbiana rezerwują **pełny** rozmiar wirtualny (1 MB granularności),
  ale host alokuje tylko żądany zakres (`host_alloc_size_`), z canarkami i
  twardymi ograniczeniami zapisów (`commit`/`decommit`/`page_bma_`/`fill`).
  Oszczędność ~26 MB.
* ROM (19 MB) ładowany bezpośrednio do bufora, bez podwójnego mapowania.
* Diagnostyka: `sbrk` loguje duże alokacje i OOM; `operator new` ≥16 MB i
  `free()` ze złym wskaźnikiem logują adres wywołującego; canarki chunków
  raportują `HOST GUARD BROKEN`.

### Czas i scheduler

* `std::thread` na SF2000 to atrapa — wątek `ntimer` nie istnieje, więc timer
  jest pompowany raz na klatkę z `retro_run` (`timing_->advance()`).
* Pętla emulacji przerywa jałowe kręcenie, gdy nie ma gotowego wątku
  (ogromna oszczędność: wcześniej do 5000 odczytów zegara na klatkę).
* Naprawiona rekurencja `codeseg::attach` (propagacja zależności tylko przy
  wskrzeszaniu kodegu — wzorzec z `deref`).

### Naprawione błędy pamięci (znalezione przez ASan na PC)

* `window_group::~window_group()` — `top_canvas` zwalniany przed destruktorem
  bazy, który pisał do zwolnionej pamięci (główna przyczyna korupcji sterty).
* `gdr_font_file_adapter::free_glyph_bitmap` — `new[]` + skalarne `delete`.
* `common::basic_stream` — brak wirtualnego destruktora.
* `kernel_system::free_msg` — odczyt zwolnionej wiadomości IPC przy teardownie.
* `memory_system::rom_map_` — niezainicjalizowany wskaźnik zwalniany w
  destruktorze.
* `load_mtm_list` — format pliku `Mtm Registry v2` zależny od `sizeof(size_t)`
  (desktop 8 B vs MIPS 4 B) → teraz stałe 64-bit + walidacja długości.
* Handler wyjątków gościa — wyrównane odczyty PC/LR (nieparzysty PC w Thumb
  powodował AdEL w kodzie diagnostycznym).

### Frontend

* Log zawsze tworzony (naprawa: `xlog` wymagał istniejącego pliku).
* Log na karcie: `/system/logs/Multicore.log`.

## Znane ograniczenia

* Wydajność: interpreter dyncom na MIPS jest wolny; gra (Call of Duty) dochodzi
  do ekranu tytułowego, ale animacje są dalekie od 60 fps. Kolejne kroki:
  większy cache translacji (pamięć jest ciasna), wyłączenie audio.
* Audio: ścieżka dźwiękowa może powodować błędy gościa (`MediaServer` czyta
  adres `0x8` po załadowaniu `sounddevice.mda`) — wątek jest wtedy bezpiecznie
  ubijany, gra działa dalej.

## Licencja

Kod EKA2L1: GPLv3 (patrz upstream). Patch/skrypty: jak upstream.
