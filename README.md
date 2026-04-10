# Niaxinus

> Bash-kompatibilis szkriptnyelv, amely futtatható binárist fordít — Python-indent szintaxissal, maximális teljesítményre optimalizálva.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.2-green.svg)]()
[![Platform](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)]()

---

## Mi ez?

A **Niaxinus** (`.nxs`) egy programnyelv, amelynek fordítója (`nxsc`) bash-szerű szkriptekből natív futtatható binárist készít.

**Fordítási pipeline:**
```
forrás.nxs  →  Lexer  →  Parser  →  Codegen  →  /tmp/*.c  →  gcc -O2  →  ELF bináris
```

Főbb jellemzők:
- 🐚 **Bash-kompatibilis szintaxis** — változók, if/while/for, külső parancsok
- 🐍 **Python-stílusú indent** — nincs `fi`, `done`, `end`
- 🚀 **Natív bináris kimenet** — gcc `-O2` optimalizáció
- ⚡ **Thread pool** — az összes CPU mag kihasználása fordításkor
- 🧠 **ISA detektálás** — SSE4, AVX2, AES automatikus felismerés
- ⏱️ **Beépített `time:` blokk** — nanoszekundum pontosságú időmérés

---

## Gyors kezdés

### Követelmények

| Csomag | Verzió |
|--------|--------|
| gcc    | ≥ 9    |
| make   | ≥ 4    |
| Linux x86-64 | — |

### Build

```bash
git clone <repo>
cd niaxinus
make
# fordító: build/nxsc
```

### Hello World

```bash
# hello.nxs
nev = "Világ"
echo "Hello, $nev!"
```

```bash
build/nxsc hello.nxs ./hello
./hello
# → Hello, Világ!
```

---

## Szintaxis

### Változók

```nxs
nev  = "Alice"
szam = "42"
echo "Nev: $nev, szam: $szam"
```

### Aritmetika

```nxs
a = "10"
b = "3"
osszeg = $(( a + b ))
echo "Eredmeny: $osszeg"
```

Támogatott operátorok: `+  -  *  /  %  **`

### if / elif / else

```nxs
x = "7"

if [ $x -gt 10 ]:
    echo "nagy"
elif [ $x -gt 5 ]:
    echo "közepes"
else:
    echo "kis"
```

### while

```nxs
i = "1"
while [ $i -le 5 ]:
    echo "iteráció: $i"
    i = $(( i + 1 ))
```

### for loop

```nxs
# Lista iteráció
for szin in piros zold kek:
    echo "szín: $szin"

# Numerikus tartomány
for i 1 10:
    echo "szám: $i"
```

### Külső parancsok

```nxs
ls workspace/
uname -a
grep "kulcsszo" fajl.txt
```

### time blokk

```nxs
time:
    for i 1 1000:
        echo "tick"
# → time: 1823456 ns
```

---

## nxsc CLI

```
build/nxsc <forrás.nxs> <kimenet>
```

---

## Könyvtárstruktúra

```
niaxinus/
├── src/              # Fordító forráskódja (C11)
│   ├── lexer.c/h     # Tokenizálás
│   ├── parser.c/h    # AST építés
│   ├── codegen.c/h   # C kódgenerálás + gcc hívás
│   ├── sysinfo.c/h   # CPU / ISA detektálás
│   ├── threadpool.c/h# POSIX pthreads pool
│   ├── arena.c/h     # Lineáris memória allokátor
│   └── main.c        # CLI belépési pont
├── build/            # Fordítás kimenete
│   └── nxsc          # A fordító bináris
├── workspace/        # NXS programok / projektek
├── docs/             # HTML dokumentáció
│   └── index.html
├── Makefile
├── LICENSE
└── README.md
```

---

## Dokumentáció

Nyisd meg a `docs/index.html` fájlt böngészőben:

```bash
xdg-open docs/index.html
```

---

## Licensz

[MIT](LICENSE) — szabad felhasználás, módosítás és terjesztés megengedett.
