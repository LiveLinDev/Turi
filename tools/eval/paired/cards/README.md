# Piloto emparejado con tarjeta breve

Esta cohorte repite las mismas 12 tareas de compilación estática con Qwen 2.5 7B y las mismas versiones de Turi, Go y Rust del piloto sin tarjeta. Los enunciados, firmas, ejemplo público, orden, modelo, semilla, presupuesto, máximo de tres intentos, criterio de compilación y aislamiento se heredan del arnés vecino. Cada solicitud y cada respuesta se conservan en este directorio. Las fuentes propuestas nunca se ejecutan.

Se decidió probar una tarjeta después de observar que las doce respuestas iniciales del modelo para Turi en la primera cohorte empezaban por def. Las tres tarjetas explican los mismos conceptos y presentan la misma función adjust escrita en cada lenguaje. El ejemplo modifica una copia local en un bucle y devuelve un entero. Es independiente de las soluciones a las doce tareas. La prosa aclara las reglas de tipos, mutabilidad y retorno respectivas.

| Lenguaje | Tokens de la tarjeta sola |
|---|---:|
| Turi | 279 |
| Go | 283 |
| Rust | 285 |

count_tokens.py lee únicamente el vocabulario y las fusiones BPE del GGUF local de Qwen, con normalización NFC, pretokenización Qwen2 y codificación por bytes. token_counts.json fija los conteos y el digest del modelo. Los resultados contabilizan el prompt completo que recibe el modelo en cada generación, incluida la tarjeta y, en reparaciones, la respuesta previa y el diagnóstico.

En un reintento se repiten explícitamente la firma esperada y el primer diagnóstico limitado a 200 caracteres. En la condición anterior la firma permanecía en el primer mensaje del contexto. Por eso el contraste de primer intento aísla la adición de la tarjeta y la comparación con reparaciones incluye también ese recordatorio.

~~~powershell
python tools/eval/paired/cards/count_tokens.py
python tools/eval/paired/cards/paired_cards.py verify
python tools/eval/paired/cards/paired_cards.py run
python tools/eval/paired/cards/paired_cards.py replay
python tools/eval/paired/cards/paired_cards.py report
~~~

La verificación previa compila la función de ejemplo de cada tarjeta con la firma esperada. replay contrasta cada respuesta guardada con su calificación sin llamar al modelo. report separa los rechazos de contrato (formato de función o construcciones excluidas) de los errores emitidos por compiladores. manifest.json identifica el protocolo, el arnés compartido, los binarios y las tarjetas.

Las fuentes de tareas y el conjunto del piloto son públicos. El modelo ve solo la tarea presente y su ejemplo público. La métrica no incluye corrección funcional, pruebas ocultas ni ejecución de soluciones. La muestra se limita a una familia de modelos, doce problemas escalares y una generación determinista por intento.

## Resultados del 20 de noviembre de 2026

Las 51 solicitudes tienen conteos de tokens y las 51 calificaciones se reprodujeron sin inferencia adicional. El incremento de entrada del primer intento respecto al mismo problema sin tarjeta fue exactamente 279 tokens en cada tarea Turi, 283 en cada tarea Go y 285 en cada tarea Rust.

| Lenguaje | Compilan primero | Compilan hasta 3 | Intentos | Entrada inicial | Salida inicial | Entrada acumulada | Salida acumulada | Total |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Turi | 7/12 | 8/12 | 22 | 4.642 | 633 | 9.590 | 1.102 | 10.692 |
| Go | 8/12 | 11/12 | 17 | 4.696 | 672 | 7.348 | 1.096 | 8.444 |
| Rust | 12/12 | 12/12 | 12 | 4.750 | 679 | 4.750 | 679 | 5.429 |

| Lenguaje | Rechazo de contrato inicial | Rechazo del compilador inicial | Rechazo de contrato total | Rechazo del compilador total |
|---|---:|---:|---:|---:|
| Turi | 0 | 5 | 0 | 14 |
| Go | 0 | 4 | 2 | 4 |
| Rust | 0 | 0 | 0 | 0 |

Turi pasó de 0 a 7 compilaciones iniciales frente a la condición sin tarjeta. Todos sus candidatos con tarjeta llegaron al compilador. Los cinco fallos iniciales fueron un if usado como expresión, la omisión de return, un rango inclusivo escrito al estilo de Rust y dos intentos de reasignar parámetros inmutables. En los tres intentos acumulados, los diagnósticos sumaron ocho E_PARSE, cinco E_IMMUTABLE y un E_DUP_NAME.

Go pasó de 9 a 8 compilaciones iniciales y de 10 a 11 tras las reparaciones. Los cuatro fallos iniciales fueron diagnósticos de tipos o nombres. Rust pasó de 11 a 12 compilaciones iniciales. En esta selección Turi consumió más tokens totales y compiló menos funciones que Go y Rust con tarjeta. Las cifras incluyen los intentos fallidos y todas las entradas repetidas.
