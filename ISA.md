# HP Saturn ISA

Instruction-set reference for the HP Saturn CPU as implemented by this
repo's Verilog core (`rtl/saturn_cpu.v` + `rtl/saturn_exec.v`) and the
vendored C reference (`x48ng/src/core/emulate.c`, `registers.c`). Every
opcode here is verified bit-for-bit between the two against the HP 48 GX
ROM.

---

## 1. Architectural state

| Name | Width | Purpose |
|------|-------|---------|
| `A`, `B`, `C`, `D`     | 64 bits (16 nibbles) | working registers |
| `R0`..`R4`             | 64 bits each         | scratch pad |
| `D0`, `D1`             | 20 bits              | data pointers into nibble memory |
| `P`                    | 4 bits               | field/nibble pointer (0..15) |
| `PC`                   | 20 bits              | program counter |
| `ST`                   | 4 bits               | status flags `{MP, SR, SB, XM}` |
| `PSTAT`                | 16 bits              | program status (application flags) |
| `CARRY`                | 1 bit                | carry flag |
| `HEXMODE`              | 1 bit                | 0 = BCD (base 10), 1 = HEX (base 16) |
| `RSTK` + `RSTKP`       | 8 × 20 b + `int4`    | return stack; pushes past 7 shift-drop the oldest |
| `IN_REG`, `OUT_REG`    | 16 nib / 12 nib      | keyboard / bus I/O registers |

`ST` bit positions: `XM=0`, `SB=1`, `SR=2`, `MP=3` (see `saturn_pkg.vh`).

---

## 2. Fields

A *field* is a contiguous nibble range `[start..end]` (inclusive). Most
arithmetic/logic opcodes operate only on the selected field and
preserve the register's other nibbles. Codes 8..14 duplicate 0..6 for
opcode-level fidelity.

| Code (`fs`) | Name | Nibbles | Length |
|------:|------|---------|-------:|
| 0  | `P`   | `P..P`                 | 1 |
| 1  | `WP`  | `0..P`                 | P+1 |
| 2  | `XS`  | `2..2` (exponent sign) | 1 |
| 3  | `X`   | `0..2`                 | 3 |
| 4  | `S`   | `15..15` (sign)        | 1 |
| 5  | `M`   | `3..14` (mantissa)     | 12 |
| 6  | `B`   | `0..1` (byte)          | 2 |
| 7  | `W`   | `0..15` (whole)        | 16 |
| 8..14 | mirror of 0..6 | | |
| 15 | `A`   | `0..4` (address)       | 5 |
| 16 | `IN`  | `0..3`                 | 4 |
| 17 | `OUT` | `0..2`                 | 3 |
| 18 | `OUTS`| `0..0`                 | 1 |

---

## 3. Encoding notation

Instructions are a stream of 4-bit nibbles read from memory starting at
`PC`. Tables below use the convention `n0 n1 n2 ...` for successive
nibbles, with `n0` being the first nibble fetched (low address).
Signed displacements are two's-complement.

- **`n_fs`** — field code (0..7) in a single nibble.
- **`n_reg`** — destination-register encoding.
- **`±dd`** / **`±ddd`** / **`±dddd`** — 2 / 3 / 4-nibble signed displacement.
- **`aaaaa`** — 5-nibble absolute address.
- **`kk…`** — immediate constant nibbles (low nibble first).

Instruction lengths are in **nibbles** (bytes = nibbles / 2).

---

## 4. Instruction encoding chart

### 4.1 First-nibble opcode map

What the **first nibble `n0`** alone tells you: the group, the typical
total length, and whether the next nibbles finish the opcode (`op`) or
are operand data (`fs`, `dd`, `ddd`, `aaaaa`, `kk…`).

| `n0` | Group                                        | n1 role | Full-op length (nibbles) |
|:---:|----------------------------------------------|---------|-------------------------|
| `0` | Misc. (returns, hex/dec, ST, PSTAT, P±1, RTI) | `op`    | 2 (`0E` → 4)            |
| `1` | Moves, DAT access, D0/D1 load / ±imm          | `op`    | 3..7                    |
| `2` | `P=n`                                         | `n`     | 2                       |
| `3` | `LC` — load (`n1+1`) nibbles into C           | count   | `2 + (n1+1)` = 3..18    |
| `4` | `GOC ±dd`                                     | disp lo | 3                       |
| `5` | `GONC ±dd`                                    | disp lo | 3                       |
| `6` | `GOTO ±ddd`                                   | disp lo | 4                       |
| `7` | `GOSUB ±ddd`                                  | disp lo | 4                       |
| `8` | I/O, status, compares, long branches          | `op`    | 3..22                   |
| `9` | compare+branch over field `fs`                | `fs`    | 5                       |
| `A` | field ADD/DEC or ZERO/COPY/EXCH (`fs<8` / `≥8`)| `fs`    | 3                       |
| `B` | field SUB/INC or SHL/SHR/NEG                  | `fs`    | 3                       |
| `C` | A-field ADD/DEC                               | `op`    | 2                       |
| `D` | A-field ZERO/COPY/EXCH                        | `op`    | 2                       |
| `E` | A-field SUB/INC                               | `op`    | 2                       |
| `F` | A-field SHL/SHR/NEG                           | `op`    | 2                       |

### 4.2 Group-0 opcode map (second nibble `n1`)

| `n1`  | `0`   | `1`  | `2`   | `3`   | `4`     | `5`     | `6`    | `7`    | `8`   | `9`  | `A`  | `B`   | `C`   | `D`   | `E`      | `F` |
|-------|-------|------|-------|-------|---------|---------|--------|--------|-------|------|------|-------|-------|-------|----------|-----|
| Mnem. |RTNSXM|RTN  |RTNSC |RTNCC |SETHEX  |SETDEC  |RSTK=C |C=RSTK |CLRST |C=ST |ST=C |CSTEX |P=P+1 |P=P-1 |AND/OR fs |RTI |

### 4.3 Group-8 opcode map (second nibble `n1`)

| `n1`  | `0`         | `1`       | `2`       | `3`        | `4`       | `5`       | `6`          | `7`          | `8`          | `9`          | `A`          | `B`          | `C`           | `D`          | `E`          | `F`          |
|-------|-------------|-----------|-----------|------------|-----------|-----------|--------------|--------------|--------------|--------------|--------------|--------------|---------------|--------------|--------------|--------------|
|Mnem.  | I/O+service | shifts/field | CLRSTmask | ?ST=1 msk | ST=0 n   | ST=1 n   | ?ST=0 n+br | ?ST=1 n+br | ?P#n +br   | ?P=n +br   | ?A=B A +br | ?A>B A +br | GOLONG ±dddd | GOTO abs   | GOSUBL ±dddd | GOSBVL abs |
|Length | 3..22       | 3..6      | 3         | 5          | 3         | 3         | 5            | 5            | 5            | 5            | 5            | 5            | 6             | 7            | 6            | 7            |

### 4.4 Encoding formats (nibble layout)

Each row shows the nibbles laid out left-to-right as fetched from
memory (`n0` first). Lowercase letters mark operand nibbles; uppercase
letters mark fixed-opcode nibbles.

    Fmt  │ Layout                            │ Length │ Example                     │ Instructions
    ─────┼───────────────────────────────────┼────────┼─────────────────────────────┼────────────────────────────────
    A1   │ [n0][n1]                          │   2    │ `01`   = RTN                │ Most of group 0, 2, C, D, E, F
    A2   │ [0][E][fs][op]                    │   4    │ `0E 7 0` = A=A&B W          │ Group 0E field AND/OR
    B1   │ [n0][n1][n2]                      │   3    │ `A 7 0` = A=A+B W           │ Groups 1x (most), 4/5, 80/82,
         │                                   │        │                             │   84/85, 9/A/B
    B2   │ [1][5][op3][op4]                  │   4    │ `1 5 F C` = C=DAT1 13nib    │ DAT recall/store 4-nib form
    C1   │ [6][ddd]                          │   4    │ `6 F F 0` = GOTO -16        │ GOTO  (3-nib signed disp)
    C1'  │ [7][ddd]                          │   4    │ `7 0 1 0` = GOSUB +16       │ GOSUB (3-nib signed disp)
    D1   │ [8][0][op][n3]                    │   4    │ `8 0 C 5` = C=P 5           │ 80Cx / 80Dx / 80Fx one-param
    D2   │ [8][0][8][n3]...                  │ 4..22  │ see below (0808x family)    │ INTON, RSI, LA, BUSCB, ABIT*,
         │                                   │        │                             │   CBIT*, ?ABIT, ?CBIT, PC=(A),
         │                                   │        │                             │   BUSCD, PC=(C), INTOFF
    D3   │ [8][1][op][...]                   │ 3..6   │ `8 1 8 F 2 F` = C=C+16 A    │ 81x shifts / field ops
    E1   │ [8][3..9][n2][n3][n4]             │   5    │ `8 6 0 F E` = ?ST=0 0 -2    │ ST / P bit-tests + branch
    E2   │ [8][A..B][n2][n3][n4]             │   5    │ `8 A 2 0 1` = ?A=C A +1     │ A-field compares + branch
    F1   │ [8][C][dddd]                      │   6    │ `8 C 0 0 0 F` = GOLONG …    │ GOLONG  (4-nib signed)
    F1'  │ [8][E][dddd]                      │   6    │ `8 E 0 0 2 0` = GOSUBL …    │ GOSUBL  (4-nib signed)
    G1   │ [8][D][aaaaa]                     │   7    │ `8 D 0 0 0 4 0` = GOTO abs  │ GOTO  absolute
    G1'  │ [8][F][aaaaa]                     │   7    │ `8 F 0 1 0 0 0` = GOSBVL … │ GOSBVL absolute
    H1   │ [9][fs][op][n3][n4]               │   5    │ `9 0 2 0 3` = ?A=C P +3     │ Group-9 compare+branch
    I1   │ [1][6..8,C][n2]                   │   3    │ `1 6 7` = D0=D0+8           │ D0/D1 ±imm
    I2   │ [1][9..B,D..F][k1 k0 …]           │ 4/6/7  │ `1 B 0 1 2 3 4` = D0=(5)…   │ D0/D1 immediate load
    J1   │ [3][n1][k0 k1 …]                  │ 3..18  │ `3 0 5` = LC(1) 5           │ LC load-constant to C
    J2   │ [8][0][8][2][n4][k0 k1 …]         │ 6..22  │ `8 0 8 2 2 1 2 3` = LA(3)   │ LA load-constant to A

### 4.5 Field-code encoding (3 bits, `fs`)

When an opcode embeds a 4-bit `fs` nibble (e.g. group 9/A/B, `81` sub-
dispatch, `0E` field AND/OR), only bits 2:0 pick the field; bit 3
selects a secondary sub-table within the same opcode (`A`/`B`/`9`:
`fs≥8` → ordered compare / shift-neg / ordered branch family; `A`/`B`
`fs<8` → arithmetic/move family).

| `fs[2:0]` | Field | Range       |
|:---:|-------|-------------|
| `0` | `P`   | `P..P`      |
| `1` | `WP`  | `0..P`      |
| `2` | `XS`  | `2..2`      |
| `3` | `X`   | `0..2`      |
| `4` | `S`   | `15..15`    |
| `5` | `M`   | `3..14`     |
| `6` | `B`   | `0..1`      |
| `7` | `W`   | `0..15`     |

### 4.6 Displacement sign-extension

| Encoding | Field | Bits | Range | Branches that use it |
|----------|-------|-----:|-------|----------------------|
| `dd`     | 2 nib (8 b)  | 8  | -128..+127 | `GOC`, `GONC`, all 8x branch+test |
| `ddd`    | 3 nib (12 b) | 12 | -2048..+2047 | `GOTO`, `GOSUB` |
| `dddd`   | 4 nib (16 b) | 16 | -32768..+32767 | `GOLONG`, `GOSUBL` |
| `aaaaa`  | 5 nib (20 b) | 20 | 0..0xFFFFF (absolute) | `GOTO abs`, `GOSBVL` |

For conditional branches, a displacement of **`0` is reserved** and
treated as `RTN` (pop `RSTK` instead of jumping).

---

## 5. Group 0 — Miscellaneous (2 nibbles each)

| Opcode | Mnemonic | Operation | Flags |
|--------|----------|-----------|-------|
| `00`   | `RTNSXM` | `ST[XM]=1; PC = pop_rstk()`               | — |
| `01`   | `RTN`    | `PC = pop_rstk()`                         | — |
| `02`   | `RTNSC`  | `PC = pop_rstk(); CARRY = 1`              | `C←1` |
| `03`   | `RTNCC`  | `PC = pop_rstk(); CARRY = 0`              | `C←0` |
| `04`   | `SETHEX` | `HEXMODE = 1`                             | — |
| `05`   | `SETDEC` | `HEXMODE = 0`                             | — |
| `06`   | `RSTK=C` | `push_rstk(C[A])`                         | — |
| `07`   | `C=RSTK` | `C[A] = pop_rstk()`                       | — |
| `08`   | `CLRST`  | `PSTAT[11:0] = 0`                         | — |
| `09`   | `C=ST`   | `C[11:0] = PSTAT[11:0]`                   | — |
| `0A`   | `ST=C`   | `PSTAT[11:0] = C[11:0]`                   | — |
| `0B`   | `CSTEX`  | swap `C[11:0]` ↔ `PSTAT[11:0]`            | — |
| `0C`   | `P=P+1`  | `P = (P+1) mod 16; CARRY = (P wraps)`     | `C` |
| `0D`   | `P=P-1`  | `P = (P-1) mod 16; CARRY = (P wraps)`     | `C` |
| `0F`   | `RTI`    | `PC = pop_rstk()` (returns from interrupt) | — |

### 0E — Field logic (4 nibbles: `0 E fs n`)

Logical AND / OR on a field; **carry is not updated**.

| `n` | Op | | `n` | Op |
|---|---|---|---|---|
| `0` | `A=A&B`  |  | `8` | `A=A\|B` |
| `1` | `B=B&C`  |  | `9` | `B=B\|C` |
| `2` | `C=C&A`  |  | `A` | `C=C\|A` |
| `3` | `D=D&C`  |  | `B` | `D=D\|C` |
| `4` | `B=B&A`  |  | `C` | `B=B\|A` |
| `5` | `C=C&B`  |  | `D` | `C=C\|B` |
| `6` | `A=A&C`  |  | `E` | `A=A\|C` |
| `7` | `C=C&D`  |  | `F` | `C=C\|D` |

---

## 6. Group 1 — Register / pointer / memory moves

### 10x / 11x / 12x (3 nibbles) — R↔{A,C} full W-field

`n2`: register select. 0..4 → R0..R4 with A; 8..C → same with C;
5,6,7 are duplicates of 1,2,3; D,E,F duplicates of 9,A,B.

| Op pattern | Example mnemonic | Operation |
|------------|------------------|-----------|
| `1 0 n`    | `R0=A`           | `Rn = A` (W) |
| `1 1 n`    | `A=R0`           | `A = Rn`     |
| `1 2 n`    | `AR0EX`          | swap `A` ↔ `Rn` |

### 13x (3 nibbles) — D0 / D1 ↔ A / C

| `n2` | Mnem | Op | `n2` | Mnem | Op |
|---|---|---|---|---|---|
| 0 | `D0=A`   | `D0 = A[A]`             | 8 | `D0=AS`   | `D0[15:0] = A[B+X]` |
| 1 | `D1=A`   | `D1 = A[A]`             | 9 | `D1=AS`   | `D1[15:0] = A[B+X]` |
| 2 | `AD0EX`  | swap `D0` ↔ `A[A]`      | A | `AD0XS`   | swap `D0[15:0]` ↔ `A[B+X]` |
| 3 | `AD1EX`  | swap `D1` ↔ `A[A]`      | B | `AD1XS`   | swap `D1[15:0]` ↔ `A[B+X]` |
| 4 | `D0=C`   | `D0 = C[A]`             | C | `D0=CS`   | `D0[15:0] = C[B+X]` |
| 5 | `D1=C`   | `D1 = C[A]`             | D | `D1=CS`   | `D1[15:0] = C[B+X]` |
| 6 | `CD0EX`  | swap `D0` ↔ `C[A]`      | E | `CD0XS`   | swap `D0[15:0]` ↔ `C[B+X]` |
| 7 | `CD1EX`  | swap `D1` ↔ `C[A]`      | F | `CD1XS`   | swap `D1[15:0]` ↔ `C[B+X]` |

### 14x (3 nibbles) — DAT recall / store, field W or B

`n2 < 8` → W-field (5 nibbles); `n2 ≥ 8` → B-field (2 nibbles).
Low 3 bits of `n2` select operation (write vs read, A vs C, D0 vs D1).

| `n2 & 7` | Mnemonic | Op | Nibbles xferred |
|---|---|---|---|
| 0 | `DAT0=A` | `mem[D0] = A` | W=5 or B=2 |
| 1 | `DAT1=A` | `mem[D1] = A` | |
| 2 | `A=DAT0` | `A = mem[D0]` | |
| 3 | `A=DAT1` | `A = mem[D1]` | |
| 4 | `DAT0=C` | `mem[D0] = C` | |
| 5 | `DAT1=C` | `mem[D1] = C` | |
| 6 | `C=DAT0` | `C = mem[D0]` | |
| 7 | `C=DAT1` | `C = mem[D1]` | |

### 15x (4 nibbles) — DAT recall / store, field-code or explicit count

Form: `1 5 n2 n3`.

- `n2 < 8`: **field-code** variant; `n3` is the field code (`P`/`WP`/...). Transfer = field length.
- `n2 ≥ 8`: **explicit-count** variant; transfer = `n3 + 1` nibbles starting at nibble 0 of the register.

Low 3 bits of `n2` select the direction/reg/pointer same as 14x.

### 16x / 17x / 18x / 1Cx (3 nibbles) — D0/D1 ±= (n+1)

| Opcode | Mnemonic | Operation | Flags |
|--------|----------|-----------|-------|
| `1 6 n` | `D0=D0+(n+1)` | `D0 += n+1`, carry on overflow | `C` |
| `1 7 n` | `D1=D1+(n+1)` | `D1 += n+1`                    | `C` |
| `1 8 n` | `D0=D0-(n+1)` | `D0 -= n+1`, carry on underflow | `C` |
| `1 C n` | `D1=D1-(n+1)` | `D1 -= n+1`                    | `C` |

### 19x / 1Ax / 1Bx (4 / 6 / 7 nibbles) — load immediate into D0

| Opcode | Mnemonic | Operation | Length |
|--------|----------|-----------|-------:|
| `1 9 n3 n2`                 | `D0=(2) kk`   | `D0[7:0]  = {n3,n2}`                 | 4 |
| `1 A n5 n4 n3 n2`           | `D0=(4) kkkk` | `D0[15:0] = {n5,n4,n3,n2}`           | 6 |
| `1 B n6 n5 n4 n3 n2`        | `D0=(5) kkkkk`| `D0[19:0] = {n6,..,n2}`              | 7 |

### 1Dx / 1Ex / 1Fx — load immediate into D1

Same as 19/1A/1B but target is `D1`.

---

## 7. Group 2 — `P=n` (2 nibbles)

| Opcode | Mnemonic | Operation |
|--------|----------|-----------|
| `2 n`  | `P=n`    | `P = n` (0..15) |

---

## 8. Group 3 — `LC n+1` (variable length)

Load `n+1` nibbles into `C`, starting at `C[P]` and wrapping.
`P` is unchanged.

| Opcode | Mnemonic | Length (nibbles) |
|--------|----------|-----------------:|
| `3 n k0 k1 .. k_n` | `LC(n+1) kk..` | `3 + n` |

---

## 9. Groups 4 / 5 — Conditional jump (3 nibbles)

8-bit signed displacement `dd = {n2, n1}`. An all-zero `dd` triggers
a return-stack pop (synthesised `RTN`).

| Opcode        | Mnemonic | Operation |
|---------------|----------|-----------|
| `4 dd`        | `GOC  ±dd` | if `CARRY`:  `PC += sext8(dd) + 1` else `PC += 3`. `dd=00` → `RTN`. |
| `5 dd`        | `GONC ±dd` | if `!CARRY`: `PC += sext8(dd) + 1` else `PC += 3`. `dd=00` → `RTN`. |

Special cases in group 4: `4 02 00` is the 3-nibble NOP (`PC += 3`,
regardless of carry).

---

## 10. Groups 6 / 7 — Unconditional branch / call (4 nibbles)

12-bit signed displacement `ddd = {n3, n2, n1}`.

| Opcode  | Mnemonic | Operation |
|---------|----------|-----------|
| `6 ddd` | `GOTO  ±ddd` | `PC = (PC + sext12(ddd) + 1) & 0xFFFFF` |
| `7 ddd` | `GOSUB ±ddd` | `push_rstk(PC + 4); PC = PC + sext12(ddd) + 4` |

`6 003`, `6 004` are reserved as NOPs (pc += 4 / += 5).

---

## 11. Group 8 — I/O, status, compares, long branches

### 80x (3 or 4 nibbles) — I/O and service

| Opcode | Mnemonic | Operation | Notes |
|--------|----------|-----------|-------|
| `8 0 0` | `OUT=CS`  | `OUT[OUTS] = C[OUTS]`          | 3 |
| `8 0 1` | `OUT=C`   | `OUT[OUT]  = C[OUT]`           | 3 |
| `8 0 2` | `A=IN`    | `A[IN] = IN`                   | 3 |
| `8 0 3` | `C=IN`    | `C[IN] = IN`                   | 3 |
| `8 0 4` | `UNCNFG`  | unconfigure memory controller  | 3 |
| `8 0 5` | `CONFIG`  | configure memory controller    | 3 |
| `8 0 6` | `C=ID`    | chip-ID into `C[A]`            | 3 |
| `8 0 7` | `SHUTDN`  | low-power wait                 | 3 |
| `8 0 9` | `C+P+1`   | `C[A] = C[A] + P + 1` (hex)    | 3, `C` |
| `8 0 A` | `RESET`   | peripheral reset               | 3 |
| `8 0 B` | `BUSCC`   | bus command                    | 3 |
| `8 0 C n` | `C=P n` | `C[n] = P`                     | 4 |
| `8 0 D n` | `P=C n` | `P = C[n]`                     | 4 |
| `8 0 E` | `SREQ?`   | `C[0]=0; ST[SR]=0`             | 3 |
| `8 0 F n` | `CPEX n`| swap `C[n]` ↔ `P`              | 4 |

### 8080..808F (4–7 nibbles) — sub-dispatch on `n3`

| `n3` | Length | Mnemonic | Operation |
|---|---:|---|---|
| `0` | 4 | `INTON`      | enable interrupts |
| `1` | 5 | `RSI`        | interrupt return + scan |
| `2` | 6+`n4` | `LA (n4+1) kk..`  | load `n4+1` nibbles into `A` at `A[P]` |
| `3` | 4 | `BUSCB`      | bus command |
| `4` | 5 | `ABIT=0 n`   | `A.bit[n] = 0` |
| `5` | 5 | `ABIT=1 n`   | `A.bit[n] = 1` |
| `6` | 7 | `?ABIT=0 n ±dd` | if `A.bit[n]==0`: branch `sext8({n6,n5})` |
| `7` | 7 | `?ABIT=1 n ±dd` | if `A.bit[n]==1`: branch |
| `8` | 5 | `CBIT=0 n`   | `C.bit[n] = 0` |
| `9` | 5 | `CBIT=1 n`   | `C.bit[n] = 1` |
| `A` | 7 | `?CBIT=0 n ±dd` | if `C.bit[n]==0`: branch |
| `B` | 7 | `?CBIT=1 n ±dd` | if `C.bit[n]==1`: branch |
| `C` | 4 | `PC=(A)`     | `PC = mem[A[A]]` (stub in RTL) |
| `D` | 4 | `BUSCD`      | bus command |
| `E` | 4 | `PC=(C)`     | `PC = mem[C[A]]` (stub in RTL) |
| `F` | 4 | `INTOFF`     | disable interrupts |

### 81x — Shifts and extended field ops

| Opcode | Length | Mnemonic | Operation |
|--------|-------:|----------|-----------|
| `8 1 0` | 3 | `ASLC` | A shift left 1 nibble circular (W) |
| `8 1 1` | 3 | `BSLC` | B ...  |
| `8 1 2` | 3 | `CSLC` | C ...  |
| `8 1 3` | 3 | `DSLC` | D ...  |
| `8 1 4` | 3 | `ASRC` | A shift right 1 nibble circular (W); sets `ST[SB]` |
| `8 1 5` | 3 | `BSRC` | |
| `8 1 6` | 3 | `CSRC` | |
| `8 1 7` | 3 | `DSRC` | |
| `8 1 8 fs op4 kk` | 6 | `R = R ± CON` | `op4[3]=0 → +`, `1 → -`; `op4[1:0]` selects A/B/C/D; `kk = n5+1` (1..16), always HEX |
| `8 1 9 fs op4`    | 5 | `R SRB fs`    | 1-bit right shift across field; sets `ST[SB]` |
| `8 1 A fs sub kk` | 6 | `R{x}={A,C}` / `{A,C}=R{x}` / `AR{x}EX` etc. field `fs` | see below |
| `8 1 B op`        | 4 | PC-register moves | see below |
| `8 1 C` | 3 | `ASRB` | A 1-bit right shift across W; sets `ST[SB]` |
| `8 1 D` | 3 | `BSRB` | |
| `8 1 E` | 3 | `CSRB` | |
| `8 1 F` | 3 | `DSRB` | |

**`8 1 A fs n4 n5` sub-dispatch** (6 nibbles):

| `n4` | Operation family |
|---|---|
| `0` | `Rx = A` or `Rx = C` (field `fs`); `n5` picks R0..R4 and A/C |
| `1` | `A = Rx` or `C = Rx` |
| `2` | `AR{x}EX` / `CR{x}EX` (exchange) |

**`8 1 B n3` sub-dispatch** (4 nibbles):

| `n3` | Mnemonic | Operation |
|---|---|---|
| `2` | `PC=A`   | `PC = A[A]` |
| `3` | `PC=C`   | `PC = C[A]` |
| `4` | `A=PC`   | `A[A] = PC + 4` |
| `5` | `C=PC`   | `C[A] = PC + 4` |
| `6` | `APCEX`  | swap `A[A]` ↔ `PC + 4` |
| `7` | `CPCEX`  | swap `C[A]` ↔ `PC + 4` |

### 82x (3 nibbles) — CLRST mask

`n2` is a 4-bit mask; each set bit clears one ST bit (bit 0 → XM,
bit 1 → SB, bit 2 → SR, bit 3 → MP).

### 83x (3 or 5 nibbles) — `?ST=1 mask ±dd`

If **all** selected ST bits are 0 → `CARRY = 1` and branch by
`sext8({n4,n3})`; else `PC += 5`. A zero displacement pops the RSTK.

### 84x / 85x (3 nibbles) — `ST=0 n` / `ST=1 n`

Set or clear `PSTAT[n2]` (0..15).

### 86x / 87x (5 nibbles) — `?ST=0 n ±dd` / `?ST=1 n ±dd`

Conditional branch on `PSTAT[n2]`. Displacement `{n4,n3}`, sign-
extended 8-bit. `dd=00` → RTN.

### 88x / 89x (5 nibbles) — `?P#n ±dd` / `?P=n ±dd`

Conditional branch on `P` compared to `n2`.

### 8Ax (5 nibbles) — Equality/zero compares on A-field + branch

| `n2` | Test (A-field) |
|---|---|
| `0`..`7` | `?A=B` `?B=C` `?A=C` `?C=D` / `?A#B` `?B#C` `?A#C` `?C#D` |
| `8`..`B` | `?A=0` `?B=0` `?C=0` `?D=0` |
| `C`..`F` | `?A#0` `?B#0` `?C#0` `?D#0` |

Displacement `{n4,n3}`, sign-extended 8-bit; `dd=00` → RTN.

### 8Bx (5 nibbles) — Ordered compares on A-field + branch

| `n2` | Test (A-field) |
|---|---|
| `0`..`3` | `?A>B` `?B>C` `?C>A` `?D>C` |
| `4`..`7` | `?A<B` `?B<C` `?C<A` `?D<C` |
| `8`..`B` | `?A>=B` `?B>=C` `?C>=A` `?D>=C` |
| `C`..`F` | `?A<=B` `?B<=C` `?C<=A` `?D<=C` |

### 8Cx / 8Dx / 8Ex / 8Fx — Long / absolute branches

| Opcode | Mnemonic | Operation | Length |
|--------|----------|-----------|-------:|
| `8 C dddd`  | `GOLONG  ±dddd`   | `PC += sext16(dddd) + 2`         | 6 |
| `8 D aaaaa` | `GOTO abs`        | `PC = aaaaa` (5-nibble absolute) | 7 |
| `8 E dddd`  | `GOSUBL ±dddd`    | `push PC+6; PC += sext16(dddd)+6` | 6 |
| `8 F aaaaa` | `GOSBVL abs`      | `push PC+7; PC = aaaaa`          | 7 |

---

## 12. Group 9 — Compare+branch with arbitrary field (5 nibbles)

Form: `9 fs op dd`.

- If `fs < 8`: equality compares over field `fs`, same subtable as 8Ax.
- If `fs ≥ 8`: ordered compares over field `fs & 7`, same subtable as 8Bx.

If `CARRY` set after the compare: `PC += sext8(dd) + 3` (or RTN on
`dd==0`); else `PC += 5`.

---

## 13. Group A — Field ADD/DEC + MOVE (3 nibbles each)

Form: `A fs op`.

### `fs < 8` — field arithmetic (carry updated)

| `op` | Mnemonic | | `op` | Mnemonic |
|---|---|---|---|---|
| `0` | `A=A+B fs`  |  | `8` | `B=B+A fs` |
| `1` | `B=B+C fs`  |  | `9` | `C=C+B fs` |
| `2` | `C=C+A fs`  |  | `A` | `A=A+C fs` |
| `3` | `D=D+C fs`  |  | `B` | `C=C+D fs` |
| `4` | `A=A+A fs`  |  | `C` | `A=A-1 fs` |
| `5` | `B=B+B fs`  |  | `D` | `B=B-1 fs` |
| `6` | `C=C+C fs`  |  | `E` | `C=C-1 fs` |
| `7` | `D=D+D fs`  |  | `F` | `D=D-1 fs` |

### `fs ≥ 8` (actual field = `fs & 7`) — ZERO / COPY / EXCH

| `op` | Mnemonic | | `op` | Mnemonic |
|---|---|---|---|---|
| `0` | `A=0 fs`   |  | `8` | `B=A fs` |
| `1` | `B=0 fs`   |  | `9` | `C=B fs` |
| `2` | `C=0 fs`   |  | `A` | `A=C fs` |
| `3` | `D=0 fs`   |  | `B` | `C=D fs` |
| `4` | `A=B fs`   |  | `C` | `ABEX fs` |
| `5` | `B=C fs`   |  | `D` | `BCEX fs` |
| `6` | `C=A fs`   |  | `E` | `ACEX fs` |
| `7` | `D=C fs`   |  | `F` | `CDEX fs` |

---

## 14. Group B — Field SUB/INC + SHL/SHR/NEG (3 nibbles each)

Form: `B fs op`.

### `fs < 8` — field arithmetic (carry updated)

| `op` | Mnemonic | | `op` | Mnemonic |
|---|---|---|---|---|
| `0` | `A=A-B fs`  |  | `8` | `B=B-A fs` |
| `1` | `B=B-C fs`  |  | `9` | `C=C-B fs` |
| `2` | `C=C-A fs`  |  | `A` | `A=A-C fs` |
| `3` | `D=D-C fs`  |  | `B` | `C=C-D fs` |
| `4` | `A=A+1 fs`  |  | `C` | `A=B-A fs` |
| `5` | `B=B+1 fs`  |  | `D` | `B=C-B fs` |
| `6` | `C=C+1 fs`  |  | `E` | `C=A-C fs` |
| `7` | `D=D+1 fs`  |  | `F` | `D=C-D fs` |

### `fs ≥ 8` (actual field = `fs & 7`) — shifts and negate

| `op` | Mnemonic | | `op` | Mnemonic |
|---|---|---|---|---|
| `0` | `ASL fs` |  | `8` | `A=-A fs` (2's comp) |
| `1` | `BSL fs` |  | `9` | `B=-B fs` |
| `2` | `CSL fs` |  | `A` | `C=-C fs` |
| `3` | `DSL fs` |  | `B` | `D=-D fs` |
| `4` | `ASR fs` |  | `C` | `A=-A-1 fs` (1's comp) |
| `5` | `BSR fs` |  | `D` | `B=-B-1 fs` |
| `6` | `CSR fs` |  | `E` | `C=-C-1 fs` |
| `7` | `DSR fs` |  | `F` | `D=-D-1 fs` |

`ASR/BSR/CSR/DSR` each set `ST[SB]` if any bit fell off.

---

## 15. Group C — A-field ADD/DEC (2 nibbles)

Same sub-table as Group A/`fs<8` but with the field fixed to `A` and
only `n1` selects the op. Always 2 nibbles, CARRY updated.

| `n1` | Mnemonic | | `n1` | Mnemonic |
|---|---|---|---|---|
| `0` | `A=A+B A`  |  | `8` | `B=B+A A` |
| `1` | `B=B+C A`  |  | `9` | `C=C+B A` |
| `2` | `C=C+A A`  |  | `A` | `A=A+C A` |
| `3` | `D=D+C A`  |  | `B` | `C=C+D A` |
| `4` | `A=A+A A`  |  | `C` | `A=A-1 A` |
| `5` | `B=B+B A`  |  | `D` | `B=B-1 A` |
| `6` | `C=C+C A`  |  | `E` | `C=C-1 A` |
| `7` | `D=D+D A`  |  | `F` | `D=D-1 A` |

---

## 16. Group D — A-field ZERO / COPY / EXCH (2 nibbles)

Same as Group A/`fs≥8`, field fixed to `A`.

| `n1` | Mnemonic | | `n1` | Mnemonic |
|---|---|---|---|---|
| `0` | `A=0 A`   |  | `8` | `B=A A` |
| `1` | `B=0 A`   |  | `9` | `C=B A` |
| `2` | `C=0 A`   |  | `A` | `A=C A` |
| `3` | `D=0 A`   |  | `B` | `C=D A` |
| `4` | `A=B A`   |  | `C` | `ABEX A` |
| `5` | `B=C A`   |  | `D` | `BCEX A` |
| `6` | `C=A A`   |  | `E` | `ACEX A` |
| `7` | `D=C A`   |  | `F` | `CDEX A` |

---

## 17. Group E — A-field SUB/INC (2 nibbles)

Same as Group B/`fs<8`, field fixed to `A`.

| `n1` | Mnemonic | | `n1` | Mnemonic |
|---|---|---|---|---|
| `0` | `A=A-B A`  |  | `8` | `B=B-A A` |
| `1` | `B=B-C A`  |  | `9` | `C=C-B A` |
| `2` | `C=C-A A`  |  | `A` | `A=A-C A` |
| `3` | `D=D-C A`  |  | `B` | `C=C-D A` |
| `4` | `A=A+1 A`  |  | `C` | `A=B-A A` |
| `5` | `B=B+1 A`  |  | `D` | `B=C-B A` |
| `6` | `C=C+1 A`  |  | `E` | `C=A-C A` |
| `7` | `D=D+1 A`  |  | `F` | `D=C-D A` |

---

## 18. Group F — A-field SHL/SHR/NEG (2 nibbles)

Same as Group B/`fs≥8`, field fixed to `A`.

| `n1` | Mnemonic | | `n1` | Mnemonic |
|---|---|---|---|---|
| `0` | `ASL A` |  | `8` | `A=-A A` |
| `1` | `BSL A` |  | `9` | `B=-B A` |
| `2` | `CSL A` |  | `A` | `C=-C A` |
| `3` | `DSL A` |  | `B` | `D=-D A` |
| `4` | `ASR A` |  | `C` | `A=-A-1 A` |
| `5` | `BSR A` |  | `D` | `B=-B-1 A` |
| `6` | `CSR A` |  | `E` | `C=-C-1 A` |
| `7` | `DSR A` |  | `F` | `D=-D-1 A` |

---

## 19. Decimal vs hex mode

BCD is the Saturn's default numerical mode (`HEXMODE=0`). In that
mode, the ALU arithmetic operations that touch a field (`ADD`, `SUB`,
`INC`, `DEC`, `NEG1`, `NEG2`) propagate carry at base 10 instead of
base 16. `SETHEX` / `SETDEC` toggle the mode. `ADDCON` / `SUBCON`
(used by opcode `8 0 9` `C+P+1` and by `818x`) are **unconditionally
hex** to match the C reference's `add_register_constant`.

---

## 20. Opcodes not implemented in pure RTL

These instructions depend on state that lives in `saturn_t` outside the
CPU — memory-mapping configuration, keyboard scan, bus commands,
interrupt-pending flag — and are delegated to the C reference in
`--core=rtl` / `--core=lockstep` mode via `sim/step_dispatch.c`'s
fallback list:

- `0F` `RTI` (needs `saturn.int_pending`)
- `80 0` `OUT=CS` / `80 1` `OUT=C`
- `80 2` `A=IN`  / `80 3` `C=IN` (needs `do_in()` keyboard scan)
- `80 4` `UNCNFG` / `80 5` `CONFIG` / `80 6` `C=ID`
- `80 7` `SHUTDN`
- `80 8` 0808 sub-dispatch (`INTON`, `RSI`, `LA`, `BUSCB`, `ABIT*`,
  `CBIT*`, `PC=(A)`, `PC=(C)`, `BUSCD`, `INTOFF`)
- `80 A` `RESET` / `80 B` `BUSCC`
- `80 E` `SREQ?` (touches `ST[SR]`)

The RTL carries stubs that advance the PC correctly so standalone
`vsim` runs stay in sync; the behavioral effects are supplied by the C
core.

---

## 21. Reference

- `x48ng/src/core/emulate.c` — canonical C implementation (`step_instruction_XX`).
- `x48ng/src/core/registers.c` — field arithmetic (`add_register`, `sub_register`, etc.).
- `rtl/saturn_exec.v` — combinational dispatch matching the tables above.
- HP's original *Saturn Programming Manual* (widely mirrored) remains the
  definitive source for edge cases beyond what `emulate.c` models.
