# Validación de la primera optimización de arreglos

Antes de medir la variante se fijan dos cargas reservadas con entradas dinámicas 11, 127 y 997. El conjunto inicial de tres cargas se usó para diagnóstico. El resultado reservado compara dos compiladores Turi que reciben exactamente el mismo programa y las mismas opciones. Se conservará la mejora si mantiene los tests y consigue al menos un 5 % de mejora mediana en una carga reservada sin una regresión demostrable superior al 2 % en la otra. Los intervalos del 95 % se estiman mediante remuestreo de pares, estratificado por semilla.

- **Cobertura total.** 8000 arreglos de longitud variable, inicializados con un valor calculado en ejecución. El primer bucle escribe en orden todos los elementos. La derecha usa el valor inicial de la misma posición. Un segundo acceso lee otra posición dependiente de la semilla.
- **Cobertura parcial.** Misma construcción y semilla. El primer bucle comienza en uno y deja intacto el primer elemento. El camino de respaldo debe preservar su valor.

Se ejecutan 30 pares por carga, con tres calentamientos por binario, intercambiando el orden de ambas versiones en cada bloque. Todos los ejecutables se contrastan con un oráculo independiente. El medidor usa la misma CPU lógica y los mismos límites de tiempo que el ensayo original.

Después de esta decisión se repetirá la serie de tres cargas iniciales con el compilador que se conserve. Se añadirá una séptima fila, C++23 con vector estándar, compilado con Clang++ 23.1.4 `-O3 -march=x86-64-v3 -flto -DNDEBUG`. La serie inicial permanece disponible con sus seis filas originales. La nueva serie vuelve a medir todos los lenguajes en el mismo orden aleatorizado por bloque.
