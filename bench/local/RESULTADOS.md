# Rendimiento local en Windows · 20 de noviembre de 2026

## Compilador integrado

**693 ejecuciones con checksum correcto**, incluidas 63 de calentamiento. Se repitieron las tres cargas y se añadió C++23. Los valores son medianas de 30 procesos, en milisegundos.

| Carga | Turi | C | C seguro | C++ | Goose | Rust | Go |
|---|---:|---:|---:|---:|---:|---:|---:|
| Recurrencia escalar | 34,00 | 35,88 | 37,40 | 36,91 | 39,06 | **32,77** | 37,58 |
| Recorrido de arreglo | 18,85 | 18,97 | 18,47 | 19,62 | **18,33** | 19,13 | 23,00 |
| Temporales por iteración | **29,69** | 34,43 | 34,22 | 35,46 | 33,49 | 34,52 | 66,55 |

El cociente pareado C/Turi en la recurrencia fue **1,029** (IC 95 % 1,016–1,053) y en temporales **1,164** (1,106–1,214). En el recorrido del arreglo fue 0,953 (0,880–1,028), que deja abierto el orden entre ambos lenguajes. El cociente Goose/Turi fue 1,144, 1,043 y 1,170, respectivamente. El intervalo de Goose también incluye 1 en el recorrido. Rust fue más rápido en el cálculo escalar. [Tiempos y checksums de las 693 ejecuciones](resultados/ryzen-7700x-integrado.json)

## Cambio medido

El compilador detecta una declaración `repeat` seguida de un recorrido que escribe exactamente una vez todas las posiciones del arreglo. Conserva el orden de evaluación del elemento inicial, la longitud, la reserva y sus posibles trampas. Las lecturas de la posición actual pueden reutilizar el valor inicial. Si el bucle es parcial, hay una lectura cruzada o se expone todo el arreglo, conserva el llenado ordinario.

La validación independiente usó dos funciones y tres semillas diferentes de las de diagnóstico. Con cobertura total, el tiempo bajó de 55,40 a 36,67 ms, con un cociente pareado de **1,52×** (IC 95 % 1,40–1,70). Con cobertura parcial, las medianas fueron 40,74 y 41,96 ms y el cociente 0,963 (0,884–1,034). La estimación puntual del camino de respaldo fue más lenta, con incertidumbre suficiente para no distinguir una regresión de ruido. El cambio cumplió el criterio previo. [Criterio fijado](VALIDACION.md) · [132 ejecuciones de validación](resultados/validacion-primera-escritura.json)

## Compilación inicial

**Primera serie, 594 ejecuciones de procesos con salida correcta.** Incluye 54 calentamientos. Se midieron 30 ejecuciones por celda con tres semillas que llegaron por stdin a binarios ya compilados. Cada cifra de la tabla es una mediana en milisegundos. Menos es mejor.

| Carga | Turi | C | C con comprobaciones | Goose | Rust | Go |
|---|---:|---:|---:|---:|---:|---:|
| Recurrencia escalar | 33,26 | 34,66 | 35,38 | 38,82 | **31,68** | 37,16 |
| Arreglo de 262.144 enteros | 18,81 | 17,00 | 17,84 | 17,69 | **16,96** | 23,10 |
| Temporales por iteración | 34,25 | **31,86** | 31,96 | 33,14 | 33,46 | 67,22 |

Frente al C ordinario, la mediana del cociente pareado C/Turi fue 1,048 para el cálculo escalar (IC 95 % 1,022–1,062), 0,901 para el arreglo (0,815–0,949) y 0,939 para temporales (0,875–0,963). En esta serie Turi superó a Goose en el escalar. Para el arreglo y los temporales el intervalo frente a Goose incluye 1.

### Configuración

AMD Ryzen 7 7700X, Windows x64, un hilo de aplicación, afinidad a una CPU lógica y cronometraje de extremo a extremo con arranque del proceso. Clang 23.1.4 compiló el LLVM IR de Turi, el C y el C independiente generado por Goose con `-O3 -march=x86-64-v3 -flto -fuse-ld=lld`. Goose v2026.4 generó ese C con `-O2 --standalone`. La configuración de release de Goose usa aritmética envolvente. El C seguro comprueba sumas, multiplicaciones, índices y reservas. Las semillas fijadas permanecen en el dominio definido de todas las implementaciones. [Distribución oficial de Goose](https://github.com/aardappel/goose/releases/tag/v2026.4)

Rust 1.97.1 empleó `opt-level=3`, LTO completo, una unidad de código, x86-64-v3 y aborto en panic. Go 1.26.5 se compiló con `GOAMD64=v3` y su recolector predeterminado. Las versiones son las disponibles en el equipo de desarrollo. Los fuentes y opciones exactas se conservan en esta carpeta.

### Reproducción

```powershell
python bench/local/build.py build/veloz.exe
python bench/local/goose/build.py
.\bench\local\rust\build.ps1
.\bench\local\go\build.ps1
python bench/local/measure.py build/nueva-serie.json
```

Para reproducir la serie integrada añade `--with-cpp` a `build.py` y a `measure.py`. Para reconstruir el comparador previo al cambio usa el commit `ec6fc4a`. El protocolo reservado se repite con `python bench/local/validate_change.py COMPILADOR_ANTERIOR COMPILADOR_ACTUAL build/validacion.json`.

El medidor evita sobrescribir una serie existente. El registro de esta serie contiene las 594 filas, hashes de ejecutables, semillas, salida, tiempos, estado y análisis pareado. [Resultados brutos](resultados/ryzen-7700x-inicial.json) · [Protocolo y oráculos](PROTOCOLO.md)
