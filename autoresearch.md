# Comparación experimental de Turi

Se fijaron 100 algoritmos por CPU, dos calentamientos y diez bloques medidos por algoritmo. Se exigió corrección funcional y una mejora geométrica del 1 % para conservar cada cambio de rendimiento. El protocolo y los resultados individuales están en [la campaña de Ryzen y Spark](bench/amplia/RESULTADOS.md).

| Versión medida con las entradas de reserva | Tiempo Turi / tiempo C, ambas CPU |
|---|---:|
| Compilador inicial | 1,2701 |
| Pruebas de índices y profundidad de llamadas | 1,2562 |
| Regiones adaptativas y tratamiento de errores | 1,2351 |

Cada carga y cada CPU tienen el mismo peso. Las tres versiones participaron en los mismos diez bloques de la campaña final. Se conservaron las dos iteraciones. La comparación emparejada de reservas en el Ryzen encontró 57 mejoras de pico, 5 aumentos y 2 empates entre los 64 programas que solicitaron memoria.

Para repetir la matriz local ya compilada se puede ejecutar `sh autoresearch.sh`. La verificación funcional se ejecuta con `sh autoresearch.checks.sh`. El registro de experimentos de la estación de trabajo está en `.autoresearch/results.jsonl` y su panel en `.autoresearch/report.html`. La medición en la Spark usa su propio supervisor y los controles de actividad descritos en el [protocolo](bench/amplia/PROTOCOLO.md).

Las comparaciones de IA miden el coste efectivo de prellenado y decodificación, las reparaciones y la corrección en casos privados. Se conserva el resultado del modelo con y sin tarjeta y con la caché fría y caliente. [Protocolo de generación](tools/eval/contexto/PROTOCOLO.md).
