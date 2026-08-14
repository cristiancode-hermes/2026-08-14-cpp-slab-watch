# cpp-slab-watch — Slab Watch (Linux)

Monitor del **slab allocator del kernel de Linux** en **C++23** que lee `/proc/slabinfo` y muestra el estado de las cachés de objetos del kernel: top-N de cachés por memoria, objetos activos vs totales, slabs, tamaño de objeto, memoria consumida y semáforo de color por utilización. Cero dependencias externas: compilación directa con `g++`.

## Features

- **Parseo robusto de `/proc/slabinfo`**: banner de versión, líneas de header `#`, formato moderno con secciones `: tunables` y `: slabdata`, formato legacy de 6 campos (kernel antiguos sin slabdata), y líneas malformadas que se saltan sin romper el resto
- **Cálculo de memoria por caché**: `num_slabs × pagesperslab × page_size` (total) y `active_slabs × pagesperslab × page_size` (activa), con `sysconf(_SC_PAGESIZE)` real
- **Tabla top-N por memoria** (`--top N`): nombre truncado a 20 chars, objetos activos/totales, objsize, slabs, memoria y utilización
- **Semáforo de color por utilización**: verde ≤ 70%, amarillo ≤ 90%, rojo > 90%; ANSI desactivable (`--no-color`)
- **Modo watch** (`--watch N`): refresco en vivo cada N segundos con **tasas delta** (objetos/s y bytes/s por caché), tiempo transcurrido, limpieza de pantalla y `std::flush` (salida completa incluso con `timeout`/kill)
- **Modo demo** (`--demo`): muestra sintética embebida con 32 cachés realistas (`kmalloc-*`, `dentry`, `filp`, `inode_cache`, `TCP`, ...) cuyos contadores crecen — permite ver el tool en contenedores/VMs donde `/proc/slabinfo` está enmascarado
- Salida **JSON** completa para scripting (`--json`): `{caches[{name, active_objs, num_objs, objsize, objperslab, pagesperslab, active_slabs, num_slabs, active_mem, total_mem, utilization}], summary{page_size, caches, total_objs, active_objs, total_mem, active_mem, utilization}}`
- **34 tests de auto-verificación** (`--selftest`): parseo sintético (moderno, legacy, malformado, vacío), matemática de memoria, agregación, formateo, tasas y smoke test condicional contra `/proc` real
- Formato de números legible: `12.3k`, `987.7M`, `1.2G` (objetos) y `4.0K`, `25.2M`, `1.0G` (bytes)

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o slab-watch
```

## Usage

```bash
./slab-watch                 # snapshot: top 10 cachés por memoria
./slab-watch --top 6         # solo las 6 más grandes
./slab-watch --watch 2       # refresco en vivo cada 2s con tasas por segundo
./slab-watch --json          # JSON completo para scripting
./slab-watch --demo          # datos sintéticos (contenedores/VMs)
./slab-watch --no-color      # sin ANSI (pipes/logs)
./slab-watch --selftest      # auto-verificación (34 tests)
```

## Ejemplo (demo)

```
SLAB WATCH — Linux kernel slab allocator (proc/slabinfo)
  ------------------------------------------------------------------------------
  TOP CACHES BY MEMORY (8 of 32)
  NAME                     ACTIVE/TOTAL  OBJSIZE  SLABS   MEMORY    UTIL
  inode_cache            76.4k/81.9k       640  1607   50.2M   93.3%
  dentry                 84.5k/98.3k       192   615   19.2M   86.0%
  TCP                     4.1k/8.2k       2.1k  1171   18.3M   50.0%
  filp                   51.2k/65.5k       256   512   16.0M   78.1%
  radix_tree_node        12.3k/16.4k       576   586    9.2M   75.0%
  vm_area_struct         20.5k/32.8k       200   202    6.3M   62.5%
  kmalloc-256            12.8k/16.4k       256   128    4.0M   78.1%
  kmalloc-128            24.6k/32.8k       128   128    4.0M   75.0%
  ------------------------------------------------------------------------------
  SUMMARY  caches 32   objects 752.0k   slab memory 164.5M (active 128.4M)   util 77.3%   page 4096B
```

Con `--watch 1` las columnas muestran tasas por segundo:

```
  SUMMARY  caches 32   objects 770.4k   slab memory 167.1M   alloc 10.4k/s   util 76.8%
```

En sistemas reales, `/proc/slabinfo` es legible (0444). En contenedores donde el archivo está enmascarado (Permission denied), el tool indica el error y `--demo` muestra el modo con datos sintéticos.

## Estructura

```
src/main.cpp   — todo el código (monolítico, stdlib-only)
```

## Notas técnicas

- El formato de `/proc/slabinfo` es `name <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> : tunables <limit> <batchcount> <sharedfactor> : slabdata <active_slabs> <num_slabs> <sharedavail>`. Los kernels antiguos (2.6.x) omiten las secciones `tunables`/`slabdata` — el parser soporta ambos.
- `std::from_chars` **no salta whitespace** — los tokens se trimean antes de parsear.
- La memoria por caché se deriva de los slabs (`num_slabs × pagesperslab × PAGE_SIZE`), no de `objsize × num_objs` (que subestima por objetos parciales en el último slab).
- Las **tasas delta** se calculan como `(cur - prev) / dt` entre snapshots consecutivos en modo watch; el primer frame usa el intervalo nominal (el dt real del primer frame es el tiempo de parseo, microsegundos — daría tasas disparadas).
- En `--watch`, la salida se flushea (`std::cout << std::flush`) al final de cada frame — con redirección a pipe/archivo y `timeout`/kill no se pierde la salida.
- El smoke test de `/proc/slabinfo` real es condicional: en contenedores con el archivo enmascarado se reporta `SKIP`, nunca un fallo.
- `std::is_sorted` con `>=` exige orden estrictamente decreciente — con ties (valores iguales) falla. Para verificar orden descendente con ties permitidos hay que usar `>` como comparador.
