"""Pide al modelo de la DGX Spark los programas de `corpus/reales/`.

Cada encargo describe un programa corriente, del tipo que la gente pide a una IA, sin decir nada de
Turi ni de restricciones: el modelo escribe el Python que escribiría siempre. La entrada es fija (un
tamaño por la línea de órdenes o un texto por la entrada estándar), así que la salida se puede
comparar con la de CPython.

Uso:  python tools/pyfront/corpus/pedir.py [--ssh spark] [--model ...] [nombres...]

Usa el túnel y la cortesía de `tools/aibench/run.py` (una petición cada vez, cediendo el paso a
cualquier otro tráfico del servidor).
"""
import argparse
import random
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "aibench"))
from run import Endpoint, PoliteGate, chat  # noqa: E402

COMMON = ("Write a complete Python 3 program (standard library only). {task} "
          "{io} Write it the way you normally would for real code. "
          "Reply with only the program in one ```python block.")
ARG = "It takes one optional integer N from the command line (default {n}) that sets the size of the work, " \
      "and prints its results to standard output."
STDIN = "It reads its input from standard input and prints its results to standard output."

# nombre, encargo, entrada ("arg", N) o ("stdin", generador)
TASKS = [
    ("inventario", "Model a small shop inventory with classes (Item with name, price and quantity; Inventory "
     "holding items in a dict). Generate N items deterministically, apply N/2 sales and restocks, then print "
     "the total value, the five most valuable items and the items with zero stock.", ("arg", 20000)),
    ("banco", "Simulate bank accounts. Define an Account class and a custom InsufficientFunds exception. "
     "Create 100 accounts, perform N deterministic transfers between them (use random.Random(7)), catching "
     "the exception when a balance would go negative, and print how many transfers failed and the final "
     "total and richest account.", ("arg", 50000)),
    ("palabras", "Count word frequencies in the text, ignoring case and punctuation, and print the 15 most "
     "common words with their counts, the number of distinct words and the longest word.", ("stdin", "text")),
    ("dijkstra", "Build a weighted grid graph of N x N nodes (weights from a simple formula of the coordinates) "
     "and compute shortest path distances from the top-left corner with Dijkstra's algorithm using heapq. "
     "Print the distance to the bottom-right corner and the sum of all distances.", ("arg", 120)),
    ("vida", "Run Conway's Game of Life on an unbounded board stored as a set of (x, y) tuples, starting from "
     "a deterministic pseudo-random 60x60 pattern, for N generations. Print the population every 10 "
     "generations.", ("arg", 100)),
    ("mochila", "Solve the 0/1 knapsack problem for N items with weights and values from a deterministic "
     "formula and capacity 10*N, using a recursive function with functools.lru_cache. Print the best value "
     "and the chosen item count.", ("arg", 120)),
    ("reinas", "Count the solutions of the N-queens problem with backtracking, using an inner recursive "
     "function that updates a counter of the enclosing function. Print the count for every board size "
     "from 4 to N.", ("arg", 9)),
    ("criba", "Compute all primes below N with the sieve of Eratosthenes. Print how many there are, the "
     "last ten, and the largest gap between consecutive primes.", ("arg", 3000000)),
    ("matrices", "Multiply two N x N matrices of floats generated with a simple formula, using lists of "
     "lists, then print the trace and the sum of all elements of the product rounded to 6 decimals.",
     ("arg", 160)),
    ("expresiones", "Write a tokenizer and a recursive descent parser (with classes for the AST nodes) for "
     "arithmetic expressions with + - * / parentheses and integers. Evaluate N generated expressions and "
     "print the sum of their values.", ("arg", 20000)),
    ("alumnos", "Use a dataclass Student(name, grade, scores). Generate N students deterministically, then "
     "print the average score per grade, the top 5 students by average (ties broken by name) using sorted "
     "with a key lambda, and the median score with statistics.median.", ("arg", 30000)),
    ("cache_lru", "Implement an LRU cache class with a fixed capacity on top of collections.OrderedDict, "
     "then replay N deterministic get/put operations on keys 0..999 and print the hit rate.", ("arg", 300000)),
    ("ventas_csv", "Read CSV lines (date,region,product,amount) and print the total amount per region and per "
     "product, sorted by total descending, using collections.defaultdict.", ("stdin", "csv")),
    ("arbol_bst", "Implement a binary search tree class with insert, contains and an in-order traversal "
     "written as a generator. Insert N pseudo-random keys (random.Random(3)), then print the size, height, "
     "the first ten keys in order and how many of 0..N-1 are present.", ("arg", 50000)),
    ("lista_enlazada", "Implement a singly linked list class with append, prepend, reverse, __len__ and "
     "__iter__. Build a list of N numbers, reverse it, and print its length, the sum and the first five "
     "values.", ("arg", 200000)),
    ("colas", "Simulate a bank with 3 tellers and customers arriving at random times (random.Random(11)) "
     "using collections.deque as the waiting line, for N customers. Print the average and maximum wait "
     "time.", ("arg", 100000)),
    ("montecarlo", "Estimate pi with a Monte Carlo simulation of N random points (random.seed(42)) and print "
     "the estimate with 6 decimals and the absolute error.", ("arg", 2000000)),
    ("rle", "Implement run-length encoding and decoding of strings. Generate N deterministic strings, encode "
     "and decode each one, check that the round trip works, and print the total original and encoded "
     "lengths with f-strings.", ("arg", 20000)),
    ("romanos", "Convert every integer from 1 to N to Roman numerals and back, checking the round trip, and "
     "print the longest numeral and the total number of characters.", ("arg", 3999)),
    ("sudoku", "Solve this sudoku with backtracking and print the solved grid: "
     "530070000600195000098000060800060003400080001700020006060000280000419005000080079. "
     "Solve it N times and print the number of recursive calls of the last run.", ("arg", 20)),
    ("topologico", "Generate a random DAG with N nodes (random.Random(5), edges only from lower to higher "
     "index), store it as a dict of lists, compute a topological order with Kahn's algorithm and the "
     "longest path length. Print both the length and the first ten nodes of the order.", ("arg", 20000)),
    ("union_find", "Implement a union-find (disjoint set) class with path compression and union by rank. "
     "Add N random edges among N nodes (random.Random(9)) and print the number of components and the size "
     "of the largest one.", ("arg", 200000)),
    ("kmedias", "Run k-means clustering (k=5) on N 2D points generated deterministically around 5 centers. "
     "Iterate 20 times and print the final centers rounded to 3 decimals and the size of each cluster.",
     ("arg", 20000)),
    ("levenshtein", "Compute the Levenshtein edit distance between N pairs of deterministic strings of "
     "length 60 with dynamic programming and print the total distance.", ("arg", 300)),
    ("lcs", "Compute the longest common subsequence of two deterministic strings of length N with dynamic "
     "programming and print its length and the subsequence itself.", ("arg", 1500)),
    ("markov", "Build a bigram Markov chain from the input text and generate 200 words starting from the "
     "first word using random.Random(1). Print the generated text wrapped at 70 characters.",
     ("stdin", "text")),
    ("rpn", "Evaluate the reverse Polish notation expressions in the input, one per line. Print each result, "
     "or an error message for lines that divide by zero or are malformed (use try/except).",
     ("stdin", "rpn")),
    ("bolos", "Implement a BowlingGame class that scores ten-pin bowling games (strikes, spares, tenth "
     "frame). Score N deterministic games and print the total and the best score.", ("arg", 50000)),
    ("particulas", "Write a Vector class with __add__, __sub__, __mul__ (by a scalar), length and __repr__, "
     "and simulate N steps of 50 particles under gravity bouncing inside a box. Print the final positions "
     "of the first three particles and the total kinetic energy.", ("arg", 2000)),
    ("figuras", "Define an abstract base class Shape (abc) with area and perimeter, and subclasses Circle, "
     "Rectangle and Triangle. Create N shapes deterministically and print the total area, the total "
     "perimeter and the count of each kind.", ("arg", 100000)),
    ("generadores", "Write a generator of Fibonacci numbers and a generator of prime numbers, and use "
     "itertools.islice and itertools.takewhile to print the sum of the first N primes and the Fibonacci "
     "numbers below 10**15.", ("arg", 20000)),
    ("dados", "Roll two dice N times (random.Random(2)) and print a histogram of the sums 2..12 with a "
     "line of '*' characters scaled to 50 characters for the most common sum.", ("arg", 500000)),
    ("collatz", "Find the starting number below N with the longest Collatz chain, caching chain lengths in "
     "a dict. Print the number and the chain length.", ("arg", 1000000)),
    ("laberinto", "Generate an N x N maze as a list of strings ('#' walls, '.' open) deterministically, then "
     "find the shortest path from the top-left to the bottom-right with BFS. Print the path length or "
     "'no path'.", ("arg", 300)),
    ("anagramas", "Group the words of the input into anagram groups using a dict keyed by the sorted "
     "letters, and print the groups with more than one word, largest first.", ("stdin", "words")),
    ("ordenar", "Implement merge sort and quicksort, sort the same N pseudo-random integers with both and "
     "with sorted(), check they agree, and print the first and last five values.", ("arg", 200000)),
    ("empleados", "Model employees with a class Employee and a subclass Manager (using super().__init__) "
     "that has a list of reports. Build an org of N employees, give raises by rule, and print the payroll "
     "per department and the manager with the most reports (use __str__ for printing).", ("arg", 30000)),
    ("factores", "Factorize every integer from 2 to N, and print the number with the most distinct prime "
     "factors, the most common largest prime factor and the count of primes.", ("arg", 200000)),
    ("intervalos", "Generate N random intervals (random.Random(4)), merge overlapping ones, and print the "
     "number of merged intervals, the longest one and the total covered length.", ("arg", 200000)),
    ("agenda", "Parse appointment lines 'YYYY-MM-DD HH:MM duration_minutes title' from the input, detect "
     "overlapping appointments per day, and print a summary per day with datetime.", ("stdin", "agenda")),
]


def make_input(kind, rng):
    words = ("the quick brown fox jumps over lazy dog alpha beta gamma delta river mountain cloud stone "
             "light shadow garden window paper silver golden ocean forest bird song night day market city "
             "listen silent enlist tinsel inlets stone tones notes onset seton").split()
    if kind == "text":
        lines = []
        for _ in range(3000):
            line = " ".join(rng.choice(words) for _ in range(rng.randint(5, 14)))
            lines.append(line.capitalize() + rng.choice([".", ",", "!", "?", ";"]))
        return "\n".join(lines) + "\n"
    if kind == "csv":
        regions, products = ["north", "south", "east", "west"], ["apple", "pear", "plum", "fig", "kiwi"]
        rows = [f"2026-{rng.randint(1, 12):02d}-{rng.randint(1, 28):02d},{rng.choice(regions)},"
                f"{rng.choice(products)},{rng.randint(1, 50000) / 100:.2f}" for _ in range(40000)]
        return "date,region,product,amount\n" + "\n".join(rows) + "\n"
    if kind == "rpn":
        out = []
        for i in range(4000):
            a, b, c = rng.randint(-50, 50), rng.randint(-9, 9), rng.randint(1, 20)
            out.append(rng.choice([f"{a} {b} /", f"{a} {b} + {c} *", f"{a} {c} - {b} * {c} +", f"{a} +",
                                   f"{a} {b} {c} * -", f"{c} {b} /"]))
        return "\n".join(out) + "\n"
    if kind == "words":
        return "\n".join(rng.choice(words) for _ in range(5000)) + "\n"
    if kind == "agenda":
        out = []
        for i in range(3000):
            out.append(f"2026-11-{rng.randint(1, 28):02d} {rng.randint(8, 18):02d}:{rng.choice(['00', '15', '30', '45'])} "
                       f"{rng.choice([15, 30, 45, 60, 90])} meeting {i}")
        return "\n".join(out) + "\n"
    raise ValueError(kind)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ssh", default="spark")
    parser.add_argument("--model", default="nvidia/Qwen3.8-Flash-Next-NVFP4")
    parser.add_argument("names", nargs="*")
    options = parser.parse_args()
    folder = HERE / "reales"
    folder.mkdir(exist_ok=True)
    rng = random.Random(20261125)
    inputs = {}
    for name, _, (kind, value) in TASKS:
        if kind == "stdin":
            inputs[name] = make_input(value, rng)
    endpoint = Endpoint(ssh=options.ssh)
    try:
        gate = PoliteGate(endpoint)
        for name, task, (kind, value) in TASKS:
            if options.names and name not in options.names:
                continue
            program = folder / f"{name}.py"
            if kind == "stdin":
                (folder / f"{name}.in").write_text(inputs[name], encoding="utf-8", newline="\n")
            else:
                (folder / f"{name}.args").write_text(str(value) + "\n", encoding="utf-8")
            if program.exists():
                continue
            io = ARG.format(n=value) if kind == "arg" else STDIN
            prompt = COMMON.format(task=task, io=io)
            content, usage, seconds = chat(endpoint, gate, options.model,
                                           [{"role": "user", "content": prompt}], 6000, None)
            match = re.search(r"```(?:python)?\n(.*?)```", content, re.S)
            code = (match.group(1) if match else content).strip() + "\n"
            program.write_text(code, encoding="utf-8", newline="\n")
            print(f"{name}: {len(code.splitlines())} líneas, {seconds:.0f} s", flush=True)
    finally:
        endpoint.close()


if __name__ == "__main__":
    main()
