# Comparación Ryzen y DGX Spark

Se fijaron 100 programas de enteros y arreglos en diez familias antes de cronometrarlos. Los archivos Python son la especificación ejecutable. Se generan fuentes legibles y equivalentes de C, C++, Goose, Rust, Go y Turi. Se incluyen ceros y datos dependientes de la semilla, lecturas, mutación, clonación y reservas por ámbito. El generador aplica las operaciones nativas de cada lenguaje. Los índices y el dominio evitan comportamiento indefinido en C. El oráculo interpreta los algoritmos en Python, por lo que detecta errores de traducción y compilación pero comparte la especificación algorítmica.

Cada CPU recibe 100 casos, dos semillas base por caso y cuatro subsemillas por bloque. La Spark usa otra serie de semillas y hasta cuatro elementos adicionales por caso. Dos calentamientos preceden diez bloques con el orden de los lenguajes aleatorizado por caso. Una calibración separada en C ajusta el número común de iteraciones a unos dos milisegundos. Se retienen los calentamientos, los valores de control, errores y resultados individuales. Se verifica el resultado de cada proceso contra el oráculo. El reloj interno cubre el núcleo, incluida la reserva que solicita el programa. Excluye arranque del proceso y lectura de argumentos. Cada variante corre en un proceso nuevo sujeto a afinidad de un procesador lógico. Go recibe GOMAXPROCS=1.

La velocidad relativa es tiempo C dividido entre tiempo Turi en el mismo bloque. Se usa la mediana de los diez cocientes por programa y la media geométrica entre programas con el mismo peso. El intervalo descriptivo por caso es el bootstrap emparejado del 95 % con 2.000 remuestreos y semilla fija. Se registra ventaja por caso si el límite inferior supera 1,01, desventaja si el superior queda por debajo de 1/1,01 e indeterminado en los demás casos. Estos intervalos individuales no corrigen por 100 comparaciones. La comparación del compilador anterior y el candidato usa el mismo protocolo. La regla predefinida de conservación exige pruebas funcionales correctas y al menos 1 % de mejora geométrica del candidato sobre el anterior.

Cada proceso entrega su pico de memoria residente mediante PeakWorkingSetSize o VmHWM. Una ejecución independiente e instrumentada de C y Turi mide cantidad de reservas, bytes solicitados, pico de bytes vivos y bytes vivos al retorno. El conteo cubre las reservas del código generado y su gestión de arreglos. El RSS incluye el proceso y su runtime. Los ceros de las columnas de instrumentación en las ejecuciones de tiempo son marcadores inactivos.

Las pruebas remotas usan un solo núcleo de CPU a prioridad ociosa y un directorio temporal privado. El supervisor descarta el caso en curso al detectar trabajo externo, espera una ventana de reposo y lo repite. La GPU permanece disponible para las aplicaciones de la Spark. Los datos guardados del supervisor son contadores de actividad sin contenido de solicitudes.

La segunda campaña suma 1.709 a las semillas antes de evaluar el nuevo asignador. Incluye juntos el compilador inicial, el intermedio y el candidato. El reloj habitual de Go en Windows devolvió cero en una muestra de una primera pasada parcial. Se archivaron sus 20 casos previos y se repitió toda la matriz final de Windows con QueryPerformanceCounter explícito, la misma fuente de tiempo que usa el arnés C. También se ejecutó la auditoría de ambos asignadores en el Ryzen con las mismas 100 entradas finales.

En la Spark, una consulta a las métricas comprueba internamente la ficha del modelo y abre otras conexiones. El primer supervisor se detuvo antes de aceptar muestras. La versión siguiente observó conexiones y GPU pasivamente durante los casos y consultó los contadores fuera de las tandas. Tras un corte de SSH, se recuperaron 40 casos verificados de la primera campaña y se completaron los restantes. En la segunda campaña el contador pasó de 5.880 a 5.881 solicitudes admitidas y completadas. La nueva actividad se observó durante una espera entre tandas. La separación máxima entre lecturas del supervisor fue de 0,321 segundos. Una simulación local también comprobó el descarte de un caso activo, el descarte de una tanda por una petición breve y la salida al perder el latido SSH. Se comprobó la retirada de los cuatro directorios de trabajo remotos. [Registro de la campaña final](resultados/final/spark/guard-ZVqMvn1F.jsonl).

## Reproducción en la estación de desarrollo

Los seis fuentes generados, las dos listas de entradas y la especificación ejecutable se conservan en [fuentes.zip](resultados/final/fuentes.zip). Las versiones de herramientas y los SHA-256 de cada binario figuran en [entorno.json](resultados/final/entorno.json). El generador materializa C, C++, Rust, Go, Goose y Turi a partir del mismo AST. Las variantes conservan la semántica normal de su lenguaje para memoria y aritmética. Las entradas medidas están dentro del dominio definido de todos los programas.

```powershell
python bench/amplia/generate.py build-amplia2/reproduccion
python bench/amplia/run.py --root build-amplia2/ryzen --out build-amplia2/repeticion
python bench/amplia/test_guard.py
.venv/Scripts/python.exe bench/amplia/seguridad.py
.venv/Scripts/python.exe bench/amplia/report.py
```

Los directorios de repetición pueden reanudarse. Una carpeta de salida nueva fuerza una medición completa. Para construir las versiones actuales, `bench/amplia/build.py` usa las instalaciones locales de Clang, Go y Rust, el Goose fijado por el proyecto y el Zig de la venv. En el equipo Linux se transfieren ejecutables listos y el plan congelado. La reproducción de inferencia usa `tools/eval/contexto/session.py` con los modelos locales.
