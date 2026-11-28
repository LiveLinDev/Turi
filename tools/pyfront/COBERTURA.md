# Cobertura del motor híbrido

Generado por `python tools/pyfront/cobertura.py`. Una función cuenta si el programa la llamó; es nativa si alguna de sus llamadas corrió en código máquina.

| Conjunto | Programas | Idénticos a CPython | Funciones nativas | Bucles nativos | CPython (s) | Motor (s) |
|---|---:|---:|---:|---:|---:|---:|
| modelo | 60 | 60 | 30/60 (50 %) | 6/8 (75 %) | 3.1 | 6.3 |
| reales | 40 | 40 | 15/143 (10 %) | 11/48 (23 %) | 65.8 | 72.8 |

## Motivos por los que una función se queda en Python

| Veces | Motivo |
|---:|---|
| 11 | usa el nombre argparse, que no es una variable local con tipo conocido |
| 9 | clase: hereda de otra clase |
| 8 | más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| 8 | usa el nombre sys, que no es una variable local con tipo conocido |
| 7 | más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| 7 | clase: atributos de clase o sentencias en el cuerpo de la clase |
| 7 | dos nombres para la misma lista o diccionario, y se modifica |
| 6 | no se pudo deducir el tipo del resultado de parse_args |
| 6 | no se pudo deducir el tipo del resultado de Parser__factor |
| 6 | clase: __init__ asigna atributos dentro de un if o un bucle |
| 5 | más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| 5 | random.Random |
| 4 | random no es el módulo random |
| 4 | es un generador |
| 3 | especificación de formato no admitida: %H:%M |
| 2 | más lento que Python (2 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| 2 | más lento que Python (3 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| 2 | más lento que Python (10 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| 2 | comparación entre tipos distintos o no admitidos |
| 2 | más lento que Python (2 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| 2 | función no admitida: round |
| 2 | in con tipos distintos o con flotantes |
| 2 | función no admitida: Random |
| 2 | no se pudo deducir el tipo del resultado de sieve_primes |
| 2 | llamada a Token____init__ con un argumento de otro tipo |

## Por programa

| Programa | Funciones | Bucles | CPython (s) | Motor (s) | Motivos |
|---|---:|---:|---:|---:|---|
| modelo/all_unique | 0/1 | 0/0 | 0.06 | 0.11 | all_unique: más lento que Python (2 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/average | 0/1 | 0/0 | 0.05 | 0.11 | average: más lento que Python (3 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/balanced_parens | 0/1 | 0/0 | 0.06 | 0.11 | balanced_parens: más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/best_profit | 1/1 | 1/1 | 0.05 | 0.11 |  |
| modelo/caesar_shift | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/capitalize_words | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/celsius_to_fahrenheit | 0/1 | 0/0 | 0.06 | 0.09 | celsius_to_fahrenheit: más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/char_code_sum | 1/1 | 0/0 | 0.05 | 0.09 |  |
| modelo/clamp_values | 0/1 | 0/0 | 0.06 | 0.10 | clamp_values: más lento que Python (3 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/collapse_spaces | 1/1 | 0/0 | 0.05 | 0.10 |  |
| modelo/collatz_steps | 1/1 | 0/0 | 0.05 | 0.10 |  |
| modelo/count_char | 0/1 | 0/0 | 0.05 | 0.09 | count_char: más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/count_inversions | 0/1 | 0/1 | 0.05 | 0.10 | count_inversions: la lista arr se guarda dentro de otra y puede cambiar después; bucle de merge_count (línea 11): más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/count_vowels | 1/1 | 0/0 | 0.05 | 0.10 |  |
| modelo/count_words | 0/1 | 0/0 | 0.05 | 0.10 | count_words: más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/digit_sum | 1/1 | 0/0 | 0.05 | 0.10 |  |
| modelo/dot_product | 0/1 | 0/0 | 0.05 | 0.10 | dot_product: más lento que Python (6 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/factorial | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/fibonacci | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/filter_positive | 1/1 | 0/0 | 0.06 | 0.11 |  |
| modelo/fizz_buzz | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/greatest_common_divisor | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/integer_sqrt | 0/1 | 1/1 | 0.05 | 0.11 | integer_sqrt: más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/interleave | 1/1 | 1/1 | 0.05 | 0.11 |  |
| modelo/is_anagram | 0/1 | 0/0 | 0.05 | 0.09 | is_anagram: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/is_palindrome | 0/1 | 0/0 | 0.05 | 0.10 | is_palindrome: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/is_prime | 0/1 | 0/0 | 0.04 | 0.10 | is_prime: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/join_uppercase | 0/1 | 0/0 | 0.05 | 0.11 | join_uppercase: más lento que Python (10 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| modelo/last_index_of | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/leap_year | 0/1 | 0/0 | 0.05 | 0.11 | leap_year: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/longest_common_prefix | 0/1 | 0/0 | 0.05 | 0.11 | longest_common_prefix: más lento que Python (10 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| modelo/longest_run | 1/1 | 1/1 | 0.05 | 0.11 |  |
| modelo/longest_word | 0/1 | 0/0 | 0.05 | 0.11 | longest_word: más lento que Python (14 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| modelo/max_of_list | 0/1 | 0/0 | 0.05 | 0.10 | max_of_list: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/max_subarray_sum | 1/1 | 0/0 | 0.05 | 0.10 |  |
| modelo/merge_sorted | 1/1 | 1/1 | 0.05 | 0.10 |  |
| modelo/missing_number | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/most_frequent | 0/1 | 0/0 | 0.05 | 0.10 | most_frequent: recorrer un diccionario o conjunto: su orden aquí no es el de Python |
| modelo/pairwise_differences | 1/1 | 0/0 | 0.06 | 0.11 |  |
| modelo/pascal_row | 0/1 | 0/1 | 0.05 | 0.10 | pascal_row: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo; bucle de pascal_row (línea 3): devuelve el parámetro row después de reasignarlo |
| modelo/power_mod | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/prime_factors | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/remove_duplicates | 1/1 | 0/0 | 0.06 | 0.11 |  |
| modelo/reverse_words | 0/1 | 0/0 | 0.06 | 0.11 | reverse_words: más lento que Python (2 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/rotate_left | 1/1 | 0/0 | 0.05 | 0.12 |  |
| modelo/run_length_encode | 1/1 | 0/0 | 0.06 | 0.11 |  |
| modelo/running_maximum | 1/1 | 1/1 | 0.05 | 0.10 |  |
| modelo/second_largest | 0/1 | 0/0 | 0.06 | 0.11 | second_largest: más lento que Python (8 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| modelo/sort_ascending | 0/1 | 0/0 | 0.05 | 0.10 | sort_ascending: más lento que Python (5 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/string_repeat | 0/1 | 0/0 | 0.05 | 0.09 | string_repeat: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/strip_digits | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/sum_even | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/swap_case | 0/1 | 0/0 | 0.05 | 0.10 | swap_case: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/to_binary | 0/1 | 0/0 | 0.05 | 0.10 | to_binary: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/to_hex | 0/1 | 0/0 | 0.05 | 0.11 | to_hex: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/to_roman | 1/1 | 0/0 | 0.05 | 0.11 |  |
| modelo/triangle_kind | 0/1 | 0/0 | 0.05 | 0.10 | triangle_kind: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/two_sum | 0/1 | 0/0 | 0.05 | 0.11 | two_sum: más lento que Python (8 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/word_lengths | 0/1 | 0/0 | 0.05 | 0.11 | word_lengths: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/words_starting_with_vowel | 1/1 | 0/0 | 0.05 | 0.11 |  |
| reales/agenda | 0/5 | 0/4 | 0.19 | 0.31 | Appointment__time_range: especificación de formato no admitida: %H:%M; parse_line: usa el nombre LINE_RE, que no es una variable local con tipo conocido; find_overlaps: sorted con una clave no comparable; print_day_summary: especificación de formato no admitida: %H:%M; main: no se pudo deducir un ti |
| reales/alumnos | 0/3 | 0/0 | 0.67 | 0.66 | make_students: random no es el módulo random; main: random no es el módulo random; Student.__init__: clase: campo de dataclass con valor por defecto calculado |
| reales/anagramas | 0/2 | 0/0 | 0.06 | 0.10 | anagram_groups: no se pudo deducir el tipo del resultado de anagram_groups; main: no se pudieron deducir los tipos con que se llama a anagram_groups |
| reales/arbol_bst | 0/2 | 0/0 | 0.19 | 0.22 | main: random.Random; BST.__init__: clase: atributos de clase o sentencias en el cuerpo de la clase |
| reales/banco | 0/5 | 0/0 | 0.10 | 0.17 | Account__deposit: más lento que Python (2 µs frente a 0 µs): pasar los datos cuesta más que el cálculo; Account__withdraw: más lento que Python (2 µs frente a 0 µs): pasar los datos cuesta más que el cálculo; run_transfers: función no admitida: round; main: usa el nombre sys, que no es una variable  |
| reales/bolos | 0/3 | 0/1 | 0.58 | 0.61 | generate_game: no se pudo deducir el tipo del resultado de generate_game; main: usa el nombre argparse, que no es una variable local con tipo conocido; BowlingGame.__init__: clase: atributos de clase o sentencias en el cuerpo de la clase; bucle de generate_game (línea 58): se llama con valores que n |
| reales/cache_lru | 0/5 | 0/1 | 0.22 | 0.26 | LRUCache__get: método no admitido: get; LRUCache__put: in con tipos distintos o con flotantes; run_workload: random.Random; parse_args: no se pudo deducir el tipo del resultado de parse_args; main: no se pudo deducir el tipo del resultado de parse_args; bucle de run_workload (línea 46): in con tipos |
| reales/colas | 0/2 | 0/0 | 0.16 | 0.19 | simulate: función no admitida: Random; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/collatz | 1/2 | 0/0 | 1.06 | 0.52 | main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/criba | 0/2 | 0/0 | 0.11 | 0.21 | sieve_primes: no se pudo deducir el tipo del resultado de sieve_primes; main: no se pudo deducir el tipo del resultado de sieve_primes |
| reales/dados | 0/2 | 0/0 | 0.41 | 0.42 | roll_sums: no se pudo deducir el tipo del resultado de roll_sums; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/dijkstra | 0/3 | 0/1 | 0.10 | 0.14 | cell_cost: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo; dijkstra: guardar en una lista o diccionario un valor de otro tipo; main: usa el nombre argparse, que no es una variable local con tipo conocido; bucle de dijkstra (línea 30): se llama con valores que no |
| reales/empleados | 0/4 | 0/0 | 1.70 | 1.73 | Employee__give_raise: función no admitida: round; build_org: no se pudo deducir el tipo del resultado de build_org; main: usa el nombre argparse, que no es una variable local con tipo conocido; Manager.__init__: clase: hereda de otra clase |
| reales/expresiones | 1/12 | 0/3 | 0.06 | 0.12 | Lexer__tokens: llamada a Token____init__ con un argumento de otro tipo; Parser__peek: error interno (ValueError: 'Token' is not in list); Parser__advance: error interno (ValueError: 'Token' is not in list); Parser__expect: expresión no admitida: tok.kind; Parser__parse: no se pudo deducir el tipo de |
| reales/factores | 0/4 | 2/4 | 0.14 | 0.17 | smallest_prime_factors: math.isqrt; analyse: tupla de menos de dos o de más de cuatro elementos; parse_args: el compilador de Turi no lo aceptó: {"id":"E_NAME","message":"Los nombres de valores empiezan en minúscula (camelCase o snake_case).","file"; main: tupla de menos de dos o de más de cuatro el |
| reales/figuras | 0/6 | 0/0 | 0.17 | 0.22 | make_shapes: no se pudo deducir el tipo del resultado de make_shapes; main: usa el nombre argparse, que no es una variable local con tipo conocido; Shape.__init__: clase: hereda de otra clase; Circle.__init__: clase: hereda de otra clase; Rectangle.__init__: clase: hereda de otra clase; Triangle.__i |
| reales/generadores | 0/4 | 0/0 | 0.10 | 0.14 | parse_args: no se pudo deducir el tipo del resultado de parse_args; main: no se pudo deducir el tipo del resultado de parse_args; fibonacci: es un generador; primes: es un generador |
| reales/intervalos | 1/3 | 0/1 | 0.43 | 0.46 | merge_intervals: sorted de un tipo no admitido; main: usa el nombre sys, que no es una variable local con tipo conocido; bucle de merge_intervals (línea 26): sorted de un tipo no admitido |
| reales/inventario | 0/9 | 0/0 | 0.15 | 0.18 | generate_inventory: no se pudo deducir el tipo del resultado de generate_inventory; main: usa el nombre argparse, que no es una variable local con tipo conocido; Item.__init__: clase: método value con decorador; Inventory.__init__: clase: __init__ asigna atributos dentro de un if o un bucle; Invento |
| reales/kmedias | 0/5 | 2/7 | 0.41 | 0.49 | generate_points: no se pudo deducir el tipo del resultado de generate_points; init_centroids: random.sample; nearest_centroid: for con destino compuesto no admitido; run_kmeans: la lista centroids se guarda dentro de otra y puede cambiar después; main: usa el nombre argparse, que no es una variable  |
| reales/laberinto | 0/0 | 0/0 | 0.05 | 0.09 |  |
| reales/lcs | 1/5 | 2/3 | 0.24 | 3.17 | make_strings: función no admitida: Random; lcs_table: dos nombres para la misma lista o diccionario, y se modifica; parse_args: el compilador de Turi no lo aceptó: {"id":"E_NAME","message":"Los nombres de valores empiezan en minúscula (camelCase o snake_case).","file"; main: dos nombres para la mism |
| reales/levenshtein | 1/3 | 1/3 | 0.27 | 0.28 | levenshtein: dos nombres para la misma lista o diccionario, y se modifica; main: usa el nombre sys, que no es una variable local con tipo conocido; bucle de levenshtein (línea 32): dos nombres para la misma lista o diccionario, y se modifica; bucle de main (línea 57): no se pudieron deducir los tipo |
| reales/lista_enlazada | 0/4 | 0/0 | 0.18 | 0.23 | parse_args: no se pudo deducir el tipo del resultado de parse_args; main: no se pudo deducir el tipo del resultado de parse_args; LinkedList.__init__: clase: atributos de clase o sentencias en el cuerpo de la clase; itertools_slice: es un generador |
| reales/markov | 0/2 | 0/0 | 0.08 | 0.11 | build_chain: no se pudo deducir el tipo del resultado de build_chain; main: no se pudo deducir el tipo del resultado de build_chain |
| reales/matrices | 3/5 | 1/2 | 0.26 | 0.27 | matmul: dos nombres para la misma lista o diccionario, y se modifica; main: usa el nombre sys, que no es una variable local con tipo conocido; bucle de matmul (línea 20): dos nombres para la misma lista o diccionario, y se modifica |
| reales/mochila | 0/2 | 0/0 | 0.11 | 0.20 | solve: firma o ámbito de función interna con capturas; main: firma o ámbito de función interna con capturas |
| reales/montecarlo | 1/2 | 0/0 | 0.32 | 0.14 | main: usa el nombre sys, que no es una variable local con tipo conocido |
| reales/ordenar | 1/3 | 0/4 | 1.15 | 0.76 | quicksort: is / is not; main: usa el nombre argparse, que no es una variable local con tipo conocido; bucle de quicksort (línea 50): más lento que Python (1841 µs frente a 3 µs): pasar los datos cuesta más que el cálculo; bucle de quicksort (línea 52): más lento que Python (1969 µs frente a 2 µs): p |
| reales/palabras | 1/2 | 0/0 | 0.07 | 0.12 | main: expresión no admitida: sys.stdin |
| reales/particulas | 0/6 | 0/0 | 0.05 | 0.17 | simulate: random no es el módulo random; main: random no es el módulo random; Vector.__init__: clase: atributos de clase o sentencias en el cuerpo de la clase; Vector.__add__: clase: atributos de clase o sentencias en el cuerpo de la clase; Vector.__sub__: clase: atributos de clase o sentencias en e |
| reales/reinas | 1/2 | 1/1 | 0.05 | 0.11 | main: usa el nombre sys, que no es una variable local con tipo conocido |
| reales/rle | 1/2 | 1/1 | 1.55 | 0.44 | main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/romanos | 0/3 | 0/3 | 0.05 | 0.20 | to_roman: usa el nombre ROMAN_VALUES, que no es una variable local con tipo conocido; from_roman: construcción dinámica: DictComp; main: usa el nombre sys, que no es una variable local con tipo conocido; bucle de to_roman (línea 32): se llama con valores que no cruzan a nativo (list, int, list); buc |
| reales/rpn | 0/4 | 0/0 | 0.06 | 0.13 | evaluate: float() de un texto; format_result: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo; main: expresión no admitida: sys.stdin; RPNError.__init__: clase: hereda de otra clase |
| reales/sudoku | 1/5 | 0/0 | 0.30 | 0.39 | find_empty: más lento que Python (4 µs frente a 2 µs): pasar los datos cuesta más que el cálculo; is_valid: más lento que Python (6 µs frente a 3 µs): pasar los datos cuesta más que el cálculo; SudokuSolver__solve: el atributo grid de SudokuSolver es de un tipo que no cruza a nativo; main: la lista  |
| reales/topologico | 0/2 | 0/4 | 53.29 | 57.88 | build_dag: no se pudo deducir el tipo del resultado de build_dag; main: random.Random; bucle de build_dag (línea 16): se llama con valores que no cruzan a nativo (dict, int, Random, int); bucle de topological_order (línea 26): se llama con valores que no cruzan a nativo (dict, dict, int); bucle de t |
| reales/union_find | 1/2 | 1/1 | 0.45 | 0.50 | main: random.Random |
| reales/ventas_csv | 0/3 | 0/0 | 0.11 | 0.16 | parse_amount: float() de un texto; main: función no admitida: defaultdict; read_rows: es un generador |
| reales/vida | 0/3 | 0/4 | 0.14 | 0.18 | step: se llama con valores que no cruzan a nativo (set); seed_board: comprensión con varios for; main: random.Random; bucle de step (línea 20): se llama con valores que no cruzan a nativo (int, dict, int, int); bucle de step (línea 19): se llama con valores que no cruzan a nativo (dict, int, int); b |
