# Cobertura del motor híbrido

Generado por `python tools/pyfront/cobertura.py`. Una función cuenta si el programa la llamó; es nativa si alguna de sus llamadas corrió en código máquina.

| Conjunto | Programas | Idénticos a CPython | Funciones nativas | Bucles nativos | CPython (s) | Motor (s) |
|---|---:|---:|---:|---:|---:|---:|
| modelo | 60 | 60 | 32/60 (53 %) | 5/6 (83 %) | 3.6 | 7.3 |
| reales | 40 | 39 | 12/91 (13 %) | 9/39 (23 %) | 62.8 | 221.6 |

## Motivos por los que una función se queda en Python

| Veces | Motivo |
|---:|---|
| 11 | expresión no admitida: sys.argv |
| 10 | más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| 10 | más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| 9 | usa el nombre argparse, que no es una variable local con tipo conocido |
| 7 | dos nombres para la misma lista o diccionario, y se modifica |
| 4 | random no es el módulo random |
| 4 | es un generador |
| 3 | se llama con valores que no cruzan a nativo (list) |
| 2 | se llama con valores que no cruzan a nativo (list, list) |
| 2 | función no admitida: Random |
| 2 | no se pudo deducir el tipo del resultado de sieve_primes |
| 2 | math.isqrt |
| 2 | el compilador de Turi no lo aceptó: {"id":"E_NAME","message":"Los nombres de valores empiezan en minúscula (camelCase o snake_case).","file" |
| 2 | sorted de un tipo no admitido |
| 2 | for con destino compuesto no admitido |
| 2 | no se pudo deducir el tipo del resultado de build_chain |
| 2 | función decorada |
| 2 | expresión no admitida: sys.stdin |
| 2 | construcción dinámica: Nonlocal |
| 2 | float() de un texto |
| 1 | más lento que Python (4 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| 1 | la lista arr se guarda dentro de otra y puede cambiar después |
| 1 | más lento que Python (4 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| 1 | más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| 1 | más lento que Python (11 µs frente a 5 µs): pasar los datos cuesta más que el cálculo |

## Distintos de CPython

- `reales/ordenar`: el motor tardó demasiado

## Por programa

| Programa | Funciones | Bucles | CPython (s) | Motor (s) | Motivos |
|---|---:|---:|---:|---:|---|
| modelo/all_unique | 0/1 | 0/0 | 0.06 | 0.09 | all_unique: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/average | 1/1 | 0/0 | 0.06 | 0.12 |  |
| modelo/balanced_parens | 0/1 | 0/0 | 0.06 | 0.10 | balanced_parens: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/best_profit | 1/1 | 1/1 | 0.07 | 0.15 |  |
| modelo/caesar_shift | 1/1 | 0/0 | 0.06 | 0.14 |  |
| modelo/capitalize_words | 1/1 | 0/0 | 0.06 | 0.13 |  |
| modelo/celsius_to_fahrenheit | 0/1 | 0/0 | 0.06 | 0.11 | celsius_to_fahrenheit: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/char_code_sum | 1/1 | 0/0 | 0.06 | 0.13 |  |
| modelo/clamp_values | 0/1 | 0/0 | 0.06 | 0.11 | clamp_values: más lento que Python (4 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| modelo/collapse_spaces | 1/1 | 0/0 | 0.06 | 0.13 |  |
| modelo/collatz_steps | 1/1 | 0/0 | 0.06 | 0.13 |  |
| modelo/count_char | 0/1 | 0/0 | 0.06 | 0.09 | count_char: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/count_inversions | 0/1 | 0/1 | 0.06 | 0.09 | count_inversions: la lista arr se guarda dentro de otra y puede cambiar después; bucle de merge_count (línea 11): más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/count_vowels | 1/1 | 0/0 | 0.07 | 0.11 |  |
| modelo/count_words | 0/1 | 0/0 | 0.06 | 0.10 | count_words: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/digit_sum | 1/1 | 0/0 | 0.06 | 0.11 |  |
| modelo/dot_product | 0/1 | 0/0 | 0.05 | 0.12 | dot_product: más lento que Python (4 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/factorial | 1/1 | 0/0 | 0.05 | 0.12 |  |
| modelo/fibonacci | 1/1 | 0/0 | 0.06 | 0.12 |  |
| modelo/filter_positive | 1/1 | 0/0 | 0.07 | 0.14 |  |
| modelo/fizz_buzz | 1/1 | 0/0 | 0.06 | 0.14 |  |
| modelo/greatest_common_divisor | 1/1 | 0/0 | 0.06 | 0.12 |  |
| modelo/integer_sqrt | 0/1 | 1/1 | 0.07 | 0.14 | integer_sqrt: más lento que Python (0 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/interleave | 1/1 | 1/1 | 0.07 | 0.20 |  |
| modelo/is_anagram | 0/1 | 0/0 | 0.06 | 0.10 | is_anagram: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/is_palindrome | 0/1 | 0/0 | 0.06 | 0.10 | is_palindrome: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/is_prime | 0/1 | 0/0 | 0.06 | 0.12 | is_prime: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/join_uppercase | 0/1 | 0/0 | 0.06 | 0.12 | join_uppercase: más lento que Python (11 µs frente a 5 µs): pasar los datos cuesta más que el cálculo |
| modelo/last_index_of | 1/1 | 0/0 | 0.06 | 0.15 |  |
| modelo/leap_year | 0/1 | 0/0 | 0.06 | 0.10 | leap_year: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/longest_common_prefix | 0/1 | 0/0 | 0.07 | 0.13 | longest_common_prefix: más lento que Python (10 µs frente a 4 µs): pasar los datos cuesta más que el cálculo |
| modelo/longest_run | 1/1 | 1/1 | 0.07 | 0.16 |  |
| modelo/longest_word | 0/1 | 0/0 | 0.05 | 0.12 | longest_word: más lento que Python (5 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/max_of_list | 0/1 | 0/0 | 0.06 | 0.09 | max_of_list: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/max_subarray_sum | 1/1 | 0/0 | 0.06 | 0.12 |  |
| modelo/merge_sorted | 1/1 | 1/1 | 0.06 | 0.16 |  |
| modelo/missing_number | 1/1 | 0/0 | 0.07 | 0.12 |  |
| modelo/most_frequent | 0/1 | 0/0 | 0.06 | 0.12 | most_frequent: recorrer un diccionario o conjunto: su orden aquí no es el de Python |
| modelo/pairwise_differences | 1/1 | 0/0 | 0.07 | 0.12 |  |
| modelo/pascal_row | 1/1 | 0/0 | 0.06 | 0.13 |  |
| modelo/power_mod | 1/1 | 0/0 | 0.06 | 0.11 |  |
| modelo/prime_factors | 1/1 | 0/0 | 0.06 | 0.16 |  |
| modelo/remove_duplicates | 1/1 | 0/0 | 0.06 | 0.18 |  |
| modelo/reverse_words | 0/1 | 0/0 | 0.06 | 0.11 | reverse_words: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/rotate_left | 1/1 | 0/0 | 0.07 | 0.13 |  |
| modelo/run_length_encode | 1/1 | 0/0 | 0.06 | 0.12 |  |
| modelo/running_maximum | 1/1 | 0/0 | 0.06 | 0.13 |  |
| modelo/second_largest | 0/1 | 0/0 | 0.06 | 0.13 | second_largest: más lento que Python (9 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| modelo/sort_ascending | 0/1 | 0/0 | 0.07 | 0.11 | sort_ascending: más lento que Python (2 µs frente a 2 µs): pasar los datos cuesta más que el cálculo |
| modelo/string_repeat | 0/1 | 0/0 | 0.07 | 0.10 | string_repeat: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/strip_digits | 1/1 | 0/0 | 0.05 | 0.12 |  |
| modelo/sum_even | 1/1 | 0/0 | 0.06 | 0.13 |  |
| modelo/swap_case | 0/1 | 0/0 | 0.06 | 0.12 | swap_case: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/to_binary | 0/1 | 0/0 | 0.05 | 0.09 | to_binary: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/to_hex | 0/1 | 0/0 | 0.06 | 0.09 | to_hex: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/to_roman | 1/1 | 0/0 | 0.05 | 0.13 |  |
| modelo/triangle_kind | 0/1 | 0/0 | 0.05 | 0.10 | triangle_kind: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo |
| modelo/two_sum | 0/1 | 0/0 | 0.06 | 0.11 | two_sum: más lento que Python (8 µs frente a 3 µs): pasar los datos cuesta más que el cálculo |
| modelo/word_lengths | 0/1 | 0/0 | 0.06 | 0.09 | word_lengths: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo |
| modelo/words_starting_with_vowel | 1/1 | 0/0 | 0.05 | 0.11 |  |
| reales/agenda | 0/4 | 0/4 | 0.14 | 0.18 | parse_line: usa el nombre LINE_RE, que no es una variable local con tipo conocido; find_overlaps: se llama con valores que no cruzan a nativo (list); print_day_summary: se llama con valores que no cruzan a nativo (str, list); main: no se pudo deducir un tipo (('dict', None, None)); bucle de find_ove |
| reales/alumnos | 0/2 | 0/0 | 0.60 | 0.67 | make_students: random no es el módulo random; main: random no es el módulo random |
| reales/anagramas | 0/2 | 0/0 | 0.05 | 0.09 | anagram_groups: no se pudo deducir el tipo del resultado de anagram_groups; main: no se pudieron deducir los tipos con que se llama a anagram_groups |
| reales/arbol_bst | 0/1 | 0/0 | 0.17 | 0.20 | main: random.Random |
| reales/banco | 0/1 | 0/0 | 0.11 | 0.14 | main: expresión no admitida: sys.argv |
| reales/bolos | 0/1 | 0/1 | 0.54 | 0.57 | generate_game: no se pudo deducir el tipo del resultado de generate_game; bucle de generate_game (línea 58): se llama con valores que no cruzan a nativo (int, BowlingGame, Random) |
| reales/cache_lru | 0/0 | 0/0 | 0.21 | 0.22 |  |
| reales/colas | 0/2 | 0/0 | 0.14 | 0.17 | simulate: función no admitida: Random; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/collatz | 1/2 | 0/0 | 0.95 | 0.53 | main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/criba | 0/2 | 0/0 | 0.10 | 0.13 | sieve_primes: no se pudo deducir el tipo del resultado de sieve_primes; main: no se pudo deducir el tipo del resultado de sieve_primes |
| reales/dados | 0/2 | 0/0 | 0.39 | 0.42 | roll_sums: no se pudo deducir el tipo del resultado de roll_sums; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/dijkstra | 0/3 | 0/0 | 0.10 | 0.13 | cell_cost: más lento que Python (1 µs frente a 0 µs): pasar los datos cuesta más que el cálculo; dijkstra: guardar en una lista o diccionario un valor de otro tipo; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/empleados | 0/2 | 0/0 | 1.61 | 1.66 | build_org: no se pudo deducir el tipo del resultado de build_org; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/expresiones | 1/2 | 0/0 | 0.06 | 0.13 | main: expresión no admitida: sys.argv |
| reales/factores | 0/4 | 3/4 | 0.14 | 0.65 | smallest_prime_factors: math.isqrt; analyse: tupla de menos de dos o de más de cuatro elementos; parse_args: el compilador de Turi no lo aceptó: {"id":"E_NAME","message":"Los nombres de valores empiezan en minúscula (camelCase o snake_case).","file"; main: no se pudieron deducir los tipos con que se |
| reales/figuras | 0/2 | 0/0 | 0.19 | 0.21 | make_shapes: no se pudo deducir el tipo del resultado de make_shapes; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/generadores | 0/2 | 0/0 | 0.09 | 0.12 | fibonacci: es un generador; primes: es un generador |
| reales/intervalos | 1/3 | 0/1 | 0.41 | 0.43 | merge_intervals: sorted de un tipo no admitido; main: expresión no admitida: sys.argv; bucle de merge_intervals (línea 26): sorted de un tipo no admitido |
| reales/inventario | 0/2 | 0/0 | 0.15 | 0.17 | generate_inventory: no se pudo deducir el tipo del resultado de generate_inventory; main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/kmedias | 0/5 | 2/7 | 0.39 | 0.56 | generate_points: no se pudo deducir el tipo del resultado de generate_points; init_centroids: random.sample; nearest_centroid: for con destino compuesto no admitido; run_kmeans: la lista centroids se guarda dentro de otra y puede cambiar después; main: usa el nombre argparse, que no es una variable  |
| reales/laberinto | 0/0 | 0/0 | 0.04 | 0.08 |  |
| reales/lcs | 1/5 | 1/2 | 0.26 | 0.38 | make_strings: función no admitida: Random; lcs_table: dos nombres para la misma lista o diccionario, y se modifica; parse_args: el compilador de Turi no lo aceptó: {"id":"E_NAME","message":"Los nombres de valores empiezan en minúscula (camelCase o snake_case).","file"; main: dos nombres para la mism |
| reales/levenshtein | 1/3 | 1/3 | 0.24 | 0.26 | levenshtein: dos nombres para la misma lista o diccionario, y se modifica; main: expresión no admitida: sys.argv; bucle de levenshtein (línea 32): dos nombres para la misma lista o diccionario, y se modifica; bucle de main (línea 57): no se pudieron deducir los tipos con que se llama a generate_pair |
| reales/lista_enlazada | 0/1 | 0/0 | 0.18 | 0.19 | itertools_slice: es un generador |
| reales/markov | 0/2 | 0/0 | 0.08 | 0.11 | build_chain: no se pudo deducir el tipo del resultado de build_chain; main: no se pudo deducir el tipo del resultado de build_chain |
| reales/matrices | 3/5 | 1/2 | 0.24 | 0.34 | matmul: dos nombres para la misma lista o diccionario, y se modifica; main: expresión no admitida: sys.argv; bucle de matmul (línea 20): dos nombres para la misma lista o diccionario, y se modifica |
| reales/mochila | 0/2 | 0/0 | 0.09 | 0.12 | solve: función decorada; main: función decorada |
| reales/montecarlo | 1/2 | 0/0 | 0.27 | 0.15 | main: expresión no admitida: sys.argv |
| reales/ordenar | 0/0 | 0/0 | 1.04 | 159.98 |  |
| reales/palabras | 1/2 | 0/0 | 0.08 | 0.14 | main: expresión no admitida: sys.stdin |
| reales/particulas | 0/2 | 0/0 | 0.06 | 0.33 | simulate: random no es el módulo random; main: random no es el módulo random |
| reales/reinas | 0/2 | 0/1 | 0.07 | 0.09 | count_queens: construcción dinámica: Nonlocal; main: expresión no admitida: sys.argv; bucle de main (línea 45): construcción dinámica: Nonlocal |
| reales/rle | 1/2 | 1/1 | 1.44 | 0.50 | main: usa el nombre argparse, que no es una variable local con tipo conocido |
| reales/romanos | 0/3 | 0/3 | 0.06 | 0.11 | to_roman: usa el nombre ROMAN_VALUES, que no es una variable local con tipo conocido; from_roman: construcción dinámica: DictComp; main: expresión no admitida: sys.argv; bucle de to_roman (línea 32): se llama con valores que no cruzan a nativo (list, int, list); bucle de from_roman (línea 43): se ll |
| reales/rpn | 0/3 | 0/0 | 0.06 | 0.13 | evaluate: float() de un texto; format_result: más lento que Python (1 µs frente a 1 µs): pasar los datos cuesta más que el cálculo; main: expresión no admitida: sys.stdin |
| reales/sudoku | 1/4 | 0/0 | 0.32 | 0.39 | find_empty: no se pudo deducir el tipo del resultado de find_empty; is_valid: más lento que Python (3 µs frente a 2 µs): pasar los datos cuesta más que el cálculo; main: no se pudo deducir un tipo (None) |
| reales/topologico | 0/2 | 0/4 | 51.06 | 50.16 | build_dag: no se pudo deducir el tipo del resultado de build_dag; main: expresión no admitida: sys.argv; bucle de build_dag (línea 16): se llama con valores que no cruzan a nativo (dict, int, Random, int); bucle de topological_order (línea 26): se llama con valores que no cruzan a nativo (dict, dict |
| reales/union_find | 0/1 | 0/2 | 0.40 | 0.42 | main: expresión no admitida: sys.argv; bucle de find (línea 20): se llama con valores que no cruzan a nativo (int, UnionFind); bucle de find (línea 23): se llama con valores que no cruzan a nativo (int, UnionFind, int) |
| reales/ventas_csv | 0/3 | 0/0 | 0.12 | 0.15 | parse_amount: float() de un texto; main: función no admitida: defaultdict; read_rows: es un generador |
| reales/vida | 0/3 | 0/4 | 0.16 | 0.18 | step: se llama con valores que no cruzan a nativo (set); seed_board: expresión no admitida: {(x, y) for x in range(width) for y in range(hei; main: expresión no admitida: sys.argv; bucle de step (línea 20): se llama con valores que no cruzan a nativo (int, dict, int, int); bucle de step (línea 19):  |
