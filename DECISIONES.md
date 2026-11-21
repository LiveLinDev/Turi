# Decisiones de implementación

## 20 de noviembre de 2026

| Elección | Motivo y evidencia |
|---|---|
| Turi para el lenguaje, `.turi` para el código y `veloz` para la CLI | El encargo emplea Turi y fija las órdenes como `veloz check`. El sufijo permite reconocer las fuentes. |
| C++23 → IR propio de bloques y valores tipados → LLVM IR textual → Clang | Evita acoplar el frontend a los encabezados de una distribución LLVM. El binario generado ejecuta instrucciones nativas. La suite compara con un evaluador de pila independiente. |
| LLVM 23.1.4 local para desarrollo en Windows x64 | Pertenece a la rama 23.1.x fijada. La versión 23.1.6 del protocolo competitivo debe fijarse cuando se ejecute esa comparación. Comprobadas las [notas oficiales](https://releases.llvm.org/23.1.0/docs/ReleaseNotes.html) y la [publicación de 23.1.6](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.6). |
| Parámetros genéricos opacos y llamadas inferidas | Hace comprobable una plantilla sin llamadas y evita ambigüedad entre argumentos de tipo y el operador menor. Esta versión limita la monomorfización a 4096 especializaciones por archivo. |
| Structs por valor y enum inicializado con discriminante y espacio para los campos de sus variantes | Permite validar la semántica, la evaluación en orden y los accesos seguros antes de optimizar la disposición de los datos. El coste espacial de los enums queda pendiente de medición. |
| Primer formato de diagnósticos con rangos en bytes UTF-8 | Facilita aplicar una reparación en editores y agentes que leen el archivo como bytes. La suite aplica reparaciones reales y verifica el archivo resultante. |
| Clang `-O2` como compilación nativa inicial | Mantiene las comprobaciones de aritmética, la semántica de cortocircuito y el orden de las salidas. Las configuraciones de comparación del expediente se mantienen como trabajo futuro. |

La evidencia de las suites del repositorio es de corrección. Superar C, Rust o Goose en rendimiento y reducir tokens totales frente a otros lenguajes siguen siendo hipótesis.

## Primeras mediciones locales

- El usuario dio prioridad al rendimiento del programa y a los tokens totales hasta que el agente obtiene un resultado correcto. Los ensayos exploratorios se ejecutaron en su Ryzen con Windows. Se conserva el protocolo Intel/Linux del expediente para una comparación competitiva posterior.
- Se integraron arreglos de enteros con regiones de ámbito y dueños únicos. Un ensayo de temporales de aproximadamente 1 MiB por iteración conservó el pico residente mediano alrededor de 4,54 MiB al pasar de 4 a 2048 iteraciones. El contador del asignador terminó con cero reservas vivas en el programa auditado. Véase `tests/MEMORIA.md`.
- Se aceptó la eliminación del llenado que precedía a un recorrido completo. La validación reservada obtuvo 1,52× frente al compilador anterior en el caso aplicable. El intervalo del camino de respaldo incluyó 1. Se mantienen las trampas y la semántica de lectura con diez regresiones dirigidas. Véase `bench/local/RESULTADOS.md`.
- Un piloto sin documentación produjo sintaxis de Python en las doce primeras respuestas Turi. Se añadió el diagnóstico `E_PYTHON_DEF` con ejemplo de función y retorno explícito. La medición original se conserva con el compilador y el protocolo congelados.
