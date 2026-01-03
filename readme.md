# GLSL-sličan kompajler -> LLVM IR

Ovaj projekat predstavlja istraživačko-edukativni kompajler za GLSL-sličan jezik koji parsira shader-like kod i generiše korektan, verifikabilan LLVM IR. Fokus projekta je na preciznoj semantici jezika, ispravnom modelovanju tipova, izraza i kontrole toka. Sve optimizacije se za sad prepuštaju LLVM infrastrukturi.

Projekat je trenutno u CPU-like fazi izvršavanja. GPU shader pipeline još uvek nije implementiran, ali je jasno planiran kao sledeći korak.

## Status projekta i plan razvoja

### Trenutna faza

U trenutnoj fazi implementirano je:

- **GLSL-inspirisan jezik**, nezavisan od konkretnog GPU pipeline-a
- **Generisanje LLVM IR-a** koji prolazi LLVM verifier
- **Podrška za**:
  - skalarne tipove, vektore, matrice i strukture
  - swizzle operacije (čitanje i pisanje)
  - built-in matematičke i vektorske funkcije
  - kontrolu toka (if, for, while, break, return)
  - eksperimentalno ciljanje RISC-V backend-a (CPU)

### Sledeće faze (planirano)

**Razdvajanje CPU i GPU modela izvršavanja**

**Dodavanje GPU pipeline nivoa**:
- različiti tipovi shadera (vertex, fragment, compute)
- resursi: teksture, sampleri, uniform i storage bufferi
- OpenGL-like operacije sa host strane

**Implementacija GPU backend-a**:
- potencijalno SPIR-V ili sličan IR

**OpenMP backend za CPU**:
- imitacija SIMD/SIMT ponašanja
- poređenje CPU i GPU izvršavanja istih shader-a

## Ciljevi dizajna

Ciljevi projekta su:

- dizajn jezika inspirisan GLSL-om, ali bez zavisnosti od GPU specifikacije
- precizno modelovanje semantike izraza, tipova i kontrole toka
- generisanje strogo korektnog i verifikabilnog LLVM IR-a
- jasna separacija parsera, AST-a, codegen-a i pomoćnih modula

Projekat je istraživačkog karaktera i nije zamišljen kao zamena za postojeći GLSL/SPIR-V toolchain.

## Model izvršavanja i ograničenja

Jezik koristi CPU-like model izvršavanja zasnovan na LLVM IR-u:

- svaka funkcija se kompajlira u LLVM Function
- kontrolni tok je eksplicitno modelovan pomoću BasicBlock-ova
- svi skokovi i povratne instrukcije su eksplicitno generisani u LLVM IR-u;
funkcije tipa void koje nemaju eksplicitni return u izvornom jeziku automatski se završavaju instrukcijom ret void

U ovoj fazi nije implementirano:

- SIMT / warp / wavefront izvršavanje
- shader stage semantika
- ugrađene GLSL promenljive (gl_Position, gl_FragCoord)
- implicitna paralelizacija niti

## Zavisnosti

- **LLVM 18**
- **C++20** kompatibilan kompajler
- **fmt** biblioteka

## Trenutno korišćenje

### Generisanje LLVM IR-a

```bash
./shader_codegen < input/shader.src
```

Generiše se fajl `module.ll`.

### Provera ispravnosti

```bash
llvm-as module.ll -o /dev/null
```

### Interaktivno testiranje kompajalera

#### `make run-codegen`

Pokreće glavni kompajler u interaktivnom modu:

```bash
make run-codegen
```

Kompajler čita shader kod sa standardnog ulaza i generiše LLVM IR na standardnom izlazu. Možete direktno unositi kod ili preusmeriti ulaz iz fajla:

```bash
./shader_codegen < input/shader.src
```

#### `make run-codegen-opt`

Pokreće kompajler i direktno propušta generisani IR kroz LLVM optimizator:

```bash
make run-codegen-opt
```

Ekvivalentno je sledećem pipeline-u:

```bash
./shader_codegen < input/shader.src | opt -O3 -S
```

Generiše optimizovani LLVM IR koji se ispisuje na standardni izlaz. Koristi se za brzu proveru optimizovanog koda bez kreiranja privremenih fajlova.

### End-to-end testiranje (RISC-V backend)

#### `make test`

Alias za `make run-riscv`. Izvršava kompletan pipeline od kompilacije shader-a do izvršavanja na RISC-V arhitekturi putem emulatora:

```bash
make test
```

**Šta se dešava (korak po korak):**

1. **Generisanje IR-a** (`module.ll`)
   - Kompajler `irgen` čita `input/shader.src`
   - Generiše neoptimizovan LLVM IR

2. **Optimizacija** (`module.opt.ll`)
   - LLVM `opt` primenjuje `-O3` optimizacije
   - Kreira optimizovani IR modul

3. **Kompilacija u RISC-V objekat** (`shader.o`)
   - `llc` kompajler backend generiše RISC-V mašinski kod
   - Target: `riscv64-unknown-linux-gnu`
   - Float ABI: hard (sa HW podrškom za FP instrukcije)
   - ISA ekstenzije: `+d,+f` (double i float)

4. **Kreiranje deljene biblioteke** (`librvshade.so`)
   - RISC-V cross-compiler linkuje objekat u `.so`
   - Position-independent code (PIC)

5. **Kompilacija host programa** (`test_host.rv`)
   - Cross-kompajlira `test/riscv/test_host.cpp`
   - Linkuje sa `librvshade.so`
   - Dinamički linkovanje sa rpath=`$ORIGIN`

6. **Izvršavanje kroz QEMU**
   - `qemu-riscv64` emulira RISC-V izvršavanje
   - Program poziva `shade_wrapper` funkciju iz shader-a
   - Renderuje 2048×2048 sliku
   - Generiše `result/riscv_out.ppm`
   - Ispisuje vreme renderovanja

#### Fajlovi koji učestvuju u `make test`

**Ulazni fajlovi:**

- [`input/shader.src`](input/shader.src) – izvorni shader kod (funkcija `shade`)
- [`test/riscv/test_host.cpp`](test/riscv/test_host.cpp) – C++ host program za testiranje

**Generisani fajlovi:**

- `module.ll` – neoptimizovan LLVM IR
- `module.opt.ll` – optimizovan LLVM IR
- `shader.o` – RISC-V objekat fajl
- `librvshade.so` – RISC-V deljena biblioteka
- `test_host.rv` – RISC-V izvršni fajl
- `result/riscv_out.ppm` – renderovana slika

**Host program (`test_host.cpp`):**

Program iterira kroz svaki piksel slike (2048×2048), izračunava UV koordinate (0–1), poziva `shade_wrapper(u, v, rgba)` iz kompajliranog shader-a, i čuva rezultat u PPM formatu.

Funkcija `shade_wrapper` je C linkage wrapper koji poziva `shade` funkciju iz shader koda.

#### Zavisnosti

Za `make test` potrebno je:

- **LLVM toolchain**: `llc`, `opt`
- **RISC-V cross-kompajler**: `riscv64-linux-gnu-g++`
- **QEMU user-mode emulator**: `qemu-riscv64`
- **RISC-V sysroot**: `/usr/riscv64-linux-gnu`

#### Dodatni target-i

```bash
make ir         # samo generiše module.ll
make opt        # samo generiše module.opt.ll
make so         # samo generiše librvshade.so
```

### Optimizacija

```bash
opt -O3 module.ll -S -o module.opt.ll
```

Koristi se novi LLVM pass manager.

## Specifikacija jezika (trenutna faza)

### Tipovi

- **Skalari**: `float`, `double`, `int`, `uint`, `bool`
- **Vektori**: `vec2`, `vec3`, `vec4`
- **Matrice**: kvadratne i nekvadratne dimenzija 2–4
- **Strukture**: korisnički definisani `struct`
- **Nizovi**: lokalni i uniform

### Operatori i izrazi

#### Aritmetički operatori

`+`, `-`, `*`, `/`, unarni `-`

Podržani nad skalarima i vektorima.

#### Relacioni operatori

`<`, `<=`, `>`, `>=`, `==`, `!=`

Implementirani preko `fcmp` i `icmp`.

#### Logički operatori

- `&&`, `||` sa short-circuit evaluacijom
- `!` logička negacija

Short-circuit je implementiran pomoću:
- uslovnih skokova (br)
- phi čvorova
- eksplicitnih merge blokova

### Kontrola toka

Podržano:

- `if` / `else`
- `while`, `for`
- `break`
- `return`

Svaki BasicBlock mora imati terminator.
`void` funkcije bez eksplicitnog `return` automatski dobijaju `ret void`.

LLVM verifier se koristi za proveru ispravnosti IR-a.

### Built-in funkcije

Implementirane preko LLVM intrinsics i helper funkcija:

- `sin`, `cos`, `sqrt`, `floor`, `fract`
- `dot`, `length`, `normalize`
- `mix`, `clamp`, `min`, `max`, `mod`

Validira se:
- broj argumenata
- tipovi
- dimenzije vektora

### Swizzle operacije

- **Čitanje**: `shufflevector`
- **Pisanje**: validacija + `insertelement` lanac

Primeri:

```glsl
// Swizzle čitanje
vec3 v = c.xyz;      // ekstrakcija komponenti
vec3 v = c.xxx;      // replikacija komponente (splat)
vec2 p = v.xy;       // parcijalni vektor
vec3 rev = v.zyx;    // permutacija

// Swizzle pisanje
v.xy = vec2(1.0, 2.0);
v.zyx = vec3(3.0, 2.0, 1.0);
```

## Konstruktori i helper sloj

### Konstruktori

- **Vektori**: splat i kombinacije
- **Matrice**: identitet, dijagonala, kolone
- **Strukture**: `insertvalue` sekvence

### Helper moduli

**call_helpers**:
- pozivi funkcija
- built-in funkcije
- konstruktori

**assignment_helpers**:
- validacija l-value izraza
- swizzle assignment
- dodela članova struktura

Cilj helper sloja je da AST ostane čist, a složena logika centralizovana.

### Uniform promenljive

Uniform promenljive predstavljaju ulazne parametre shader-a koji su konstantni tokom jednog renderovanja.

**Implementacija:**

- generišu se kao LLVM `GlobalVariable` sa `ExternalLinkage`
- inicijalizuju se sa `getNullValue`
- konstante tokom jednog izvršavanja shader funkcije
- host program ih postavlja pre svakog poziva

**Definisanje uniform promenljivih:**

```glsl
uniform vec3 lightPos;
uniform float time;
uniform mat4x4 MVP;
```

Uniform promenljive se deklarišu na početku shader koda koristeći `uniform` kvalifikator, nakon čega sledi tip i ime promenljive.

**Problem poravnanja (alignment):**

Kada host program (C++) prenosi uniform podatke shader-u, mora da poštuje LLVM-ovo poravnanje tipova. Vec3 tipovi su problematični jer LLVM koristi 16-bajtno poravnanje za vektore (zbog SIMD), dok C++ `struct` sa tri float-a zauzima samo 12 bajtova.

**Rešenje:**

Host strukture moraju da dodaju padding polje (`_pad`) nakon vec3 tipova:

```cpp
struct Vec3Uniform {
    float x, y, z;
    float _pad;  // poravnanje na 16B
};
```

Ovo osigurava da veličina strukture bude 16 bajtova, što odgovara LLVM očekivanjima. Bez ovog padding-a dolazi do neusklađenosti memorijskog layout-a između host programa i kompajliranog shader koda.

## Error handling

Determinističan i eksplicitan:

- `logError` + prekid codegen-a

Hvata:
- sintaksne greške
- tipne greške
- nevalidne pozive
- nevalidan swizzle ili assignment
- nepostojeća polja struktura

Trenutno nema linija/kolona u porukama o grešci (planirano unapređenje).

## Primeri testova

### Aritmetika

```glsl
fn float test(float a, float b) {
    return a + b * 2.0;
}
```

### Short-circuit

```glsl
fn bool test(bool a, bool b) {
    return a && b || !a;
}
```

### Swizzle

```glsl
fn vec3 test() {
    vec3 v = vec3(1.0, 2.0, 3.0);
    v.xy = vec2(5.0, 6.0);
    return v;
}
```
Testovi su sačuvani u tests.md fajlu