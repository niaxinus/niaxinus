# Niaxinus

> Bash-kompatibilis szkriptnyelv, amely futtatható binárist fordít — Python-indent szintaxissal, maximális teljesítményre optimalizálva.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.3-green.svg)]()
[![Platform](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)]()
[![Tests](https://img.shields.io/badge/bash--compat-8%2F8%20PASS-brightgreen.svg)]()

---

## Mi ez?

A **Niaxinus** (`.nxs`) egy programnyelv, amelynek fordítója (`nxsc`) bash-szerű szkriptekből natív futtatható binárist készít.

**Fordítási pipeline:**
```
forrás.nxs  →  Lexer  →  Parser  →  Codegen  →  /tmp/*.c  →  gcc -O2  →  ELF bináris
```

**Interpreter mód (gyors fejlesztés / bash-compat tesztelés):**
```
forrás.nxs  →  Lexer  →  Parser  →  AST  →  exec_node()  →  kimenet
```

Főbb jellemzők:
- 🐚 **Bash-kompatibilis szintaxis** — pipeline, átirányítás, subshell, parancs-helyettesítés
- 🐍 **Python-stílusú indent** — nincs `fi`, `done`, `end`
- 🚀 **Natív bináris kimenet** — gcc `-O2` optimalizáció
- ⚡ **Thread pool** — az összes CPU mag kihasználása fordításkor
- 🧠 **ISA detektálás** — SSE4, AVX2, AES automatikus felismerés
- ⏱️ **Beépített `time:` blokk** — nanoszekundum pontosságú időmérés
- 🔧 **Shell builtins** — `cd`, `pwd`, `export`, `unset`, `read`, `shift`, `test/[`
- 🔀 **Pipeline & átirányítás** — `cmd1 | cmd2`, `> fajl`, `>> fajl`, `< fajl`
- 💠 **Paraméter-expanzió** — `${var:-default}`, `${#var}`, `${var%pat}` stb.

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

### Változók és paraméter-expanzió

```nxs
nev  = "Alice"
szam = "42"
echo "Nev: $nev, szam: $szam"

# Alap érték, ha üres
echo ${nev:-vendeg}

# Változó hossza
echo ${#nev}

# Utótag levágása
fajl = "kepek/foto.jpg"
echo ${fajl%.jpg}    # → kepek/foto
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

### Pipeline

```nxs
ls /usr/bin | grep "py" | sort | head -5
```

### Átirányítás

```nxs
echo "első sor"   > /tmp/kimenet.txt
echo "második sor" >> /tmp/kimenet.txt
sort < /tmp/kimenet.txt
```

### Parancs-helyettesítés

```nxs
datum = $(date +%Y-%m-%d)
echo "Ma: $datum"
```

### Subshell

```nxs
x = "szulo"
(
    x = "gyerek"
    echo "belul: $x"
)
echo "kivul: $x"
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
build/nxsc <forrás.nxs> <kimenet>      # fordítás binárisba
build/nxsc --run    <forrás.nxs>       # interpreter mód
build/nxsc --tokens <forrás.nxs>       # token dump
build/nxsc --ast    <forrás.nxs>       # AST dump
```

### Bash kompatibilitási diff tesztek

```bash
make test-bash-compat
# PASS control-flow
# PASS extern
# PASS hello
# PASS pipe
# PASS redir
# PASS expansion
# PASS builtins
# PASS cmdsub
# 8/8 tesztek sikerültek
```

---

## Beépített parancsok (builtins)

| Parancs | Leírás |
|---------|--------|
| `echo [-n] szöveg` | Kiírás stdout-ra; `-n`: newline nélkül |
| `cd [könyvtár]` | Könyvtárváltás; `cd -` visszamegy |
| `pwd` | Aktuális könyvtár |
| `export VAR[=érték]` | Exportálás környezeti változóként |
| `unset VAR` | Változó törlése |
| `read VAR` | Sor beolvasása stdin-ről |
| `shift [n]` | Pozicionális paraméterek eltolása |
| `true` / `false` / `:` | 0 / 1 / 0 kilépési kód |
| `test` / `[` | Feltétel kiértékelés |

---

## Könyvtárstruktúra

```
niaxinus/
├── src/              # Fordító forráskódja (C11)
│   ├── lexer.c/h     # Tokenizálás
│   ├── parser.c/h    # AST építés
│   ├── interp.c/h    # AST interpreter (--run mód)
│   ├── codegen.c/h   # C kódgenerálás + gcc hívás
│   ├── debug.c/h     # AST dump (--ast mód)
│   ├── sysinfo.c/h   # CPU / ISA detektálás
│   ├── threadpool.c/h# POSIX pthreads pool
│   ├── arena.c/h     # Lineáris memória allokátor
│   └── main.c        # CLI belépési pont
├── build/            # Fordítás kimenete
│   └── nxsc          # A fordító bináris
├── tests/
│   └── bash-compat/  # Bash-kompatibilitási tesztek (8 tesztpár)
├── workspace/        # NXS programok / projektek
├── docs/             # HTML dokumentáció
│   ├── index.html    # Főoldal
│   ├── reference.html# Nyelvi referencia
│   └── internals.html# Belső működés
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

| Oldal | Tartalom |
|-------|----------|
| [index.html](docs/index.html) | Kezdőlap, szintaxis áttekintés, gyors kezdés |
| [reference.html](docs/reference.html) | Teljes nyelvi referencia (minden builtin, expanzió, operátor) |
| [internals.html](docs/internals.html) | Belső architektúra, AST csomópontok, codegen, threadpool |

---

## Licensz

[MIT](LICENSE) — szabad felhasználás, módosítás és terjesztés megengedett.

