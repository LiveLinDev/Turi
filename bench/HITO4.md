# Protocolo previo para el modelo de memoria

**Estado: protocolo, sin medidas competitivas.** La máquina local de desarrollo es un AMD Ryzen 7 7700X con Windows. El expediente exige un Intel físico con Linux para publicar la comparación. Las verificaciones de salida sí se pueden ejecutar en el equipo de desarrollo.

La pregunta del hito 4 es cuánto cuestan las elecciones de memoria de Turi respecto a Rust y Goose. Las ocho cargas siguientes se fijan antes de obtener sus resultados. Las implementaciones deben leer los parámetros en ejecución, usar el mismo algoritmo, emitir el mismo checksum e incluir el caso vacío cuando corresponda. Se debe guardar cada fallo y cada tamaño de entrada por separado.

| Carga | Qué se mide | Entradas pequeñas, medianas y grandes |
|---|---|---|
| Recorrido lineal de enteros | Acceso secuencial, coste de comprobación de índices | 4096 / 65536 / 1048576 elementos |
| Temporales por petición | Recuperación del espacio al salir y pico en un servicio prolongado | 1000 / 10000 / 100000 peticiones con longitud determinada por un generador fijo |
| Copia seguida de mutación | Coste de conservar instantáneas independientes | 64 / 1024 / 16384 elementos por instantánea |
| Árbol generado por niveles | Construcción, recorrido y liberación de valores de vida conjunta | Profundidad 10 / 15 / 20 |
| Grafo cíclico editable | Altas, bajas y reutilización de índices con generación | 1024 / 16384 / 131072 nodos iniciales |
| LRU acotada | Estabilidad de memoria y reciclaje durante sustituciones | Capacidad 256 / 4096 / 65536 |
| DAG compartido de expresiones | Compartición acíclica y liberación al desaparecer la última raíz | 4096 / 65536 / 524288 nodos |
| Canalización de lotes entre tareas | Transferencia de propiedad y coste de publicar una raíz congelada | 16 / 256 / 4096 lotes de 1024 enteros |

Fijar los fuentes, semillas, criterios de elegibilidad, códigos de versión y checksums antes de las ejecuciones cronometradas. Separar entrenamiento PGO, selección y evaluación. Ejecutar los perfiles y el protocolo estadístico de `EXPEDIENTE.md §B7` sobre la misma máquina. Registrar tiempos, RSS máximo y `memory.peak`, con intervalos por carga. La canalización se publica por separado según el número de hilos. El hardware, el acceso y las implementaciones de cada carga son requisitos de la comparación.
