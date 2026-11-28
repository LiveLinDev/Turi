"""Prefijos de fila y teselas con carriles por fila para productos periódicos."""

from math import fsum, gcd, ulp
from struct import pack, unpack
import sys


SHORT_TERMS = 16


def words_source(data):
    rows = [', '.join(f'u64({word:#x})' for word in data[start:start + 8])
            for start in range(0, len(data), 8)]
    return ',\n        '.join(rows)


def bits(value):
    return unpack('<Q', pack('<d', value))[0]


def value(word):
    return unpack('<d', pack('<Q', word))[0]


def short_data(period, first, second, checkpoint, cycles, compensated):
    stride = (period + 7) & ~7
    phase_at = 2 * period + 8
    phases = checkpoint[phase_at:phase_at + period]
    phase_step = phases[1]
    if gcd(phase_step, period) != 1:
        return None
    inverse = pow(phase_step, -1, period)
    a_row, a_col, a_mod, a_seed = first
    b_row, b_col, b_mod, b_seed = second
    left = [[((a_row * row + a_col * k + a_seed) % a_mod) / a_mod for row in range(period)]
            for k in range(SHORT_TERMS)]
    right = [[((b_row * k + b_col * col + b_seed) % b_mod) / b_mod for col in range(period)]
             for k in range(SHORT_TERMS)]
    offsets = checkpoint[phase_at + period:]
    columns = []
    cells = [[0.0] * period for _ in range(period)]
    for col in range(period):
        deltas = []
        for row, phase in enumerate(phases):
            word = offsets[(row * stride + col) // 8]
            delta = (word >> (8 * (col & 7))) & 255
            cells[row][col] = value(checkpoint[phase + col] + delta - 128)
            deltas.append(delta)
        deltas.extend([128] * (stride - period))
        for at in range(0, stride, 8):
            columns.append(sum(delta << (8 * lane) for lane, delta in enumerate(deltas[at:at + 8])))

    high, low, diagonal = [], [], []
    for rest in range(SHORT_TERMS):
        for row, entries in enumerate(cells):
            hi = lo = 0.0
            for term in entries * cycles:
                next_sum = hi + term
                if compensated:
                    lo += (hi - next_sum) + term if term <= hi else (term - next_sum) + hi
                hi = next_sum
            high.append(bits(hi))
            if compensated:
                low.append(bits(lo))
            diagonal.append(bits(entries[row]))
            if rest + 1 < SHORT_TERMS:
                factor = left[rest][row]
                cells[row] = [x + factor * y for x, y in zip(entries, right[rest])]
        high.extend([0] * (stride - period))
        if compensated:
            low.extend([0] * (stride - period))

    payload = high + low
    diag_at = len(payload)
    payload.extend(diagonal)
    seed_at = len(payload)
    payload.extend(checkpoint[phases[i % period]] for i in range(phase_at))
    payload.extend(col * inverse % period for col in range(period))
    payload.extend(columns)
    left_at = len(payload)
    for row in left:
        payload.extend(map(bits, row))
        payload.extend([0] * (stride - period))
    right_at = len(payload)
    for row in right:
        payload.extend(map(bits, row[:SHORT_TERMS]))
    return payload, diag_at, seed_at, left_at, right_at


def compact_interval_data(period, generators, checkpoint, short, compensated):
    """Agrupa el prefijo de columna, la diagonal y los pesos de una cola."""
    stride = (period + 7) & ~7
    data, diag_at, _, _, _ = short
    phase_at = 2 * period + 8
    phases = checkpoint[phase_at:phase_at + period]
    corrections = checkpoint[phase_at + period:]
    seeds = []
    for row, phase in enumerate(phases):
        seeds.append([value(checkpoint[phase + col] +
                            ((corrections[(row * stride + col) // 8] >> (8 * (col & 7))) & 255) - 128)
                      for col in range(SHORT_TERMS)])

    a_row, a_col, _, a_seed = generators[0]
    b_row, b_col, b_mod, b_seed = generators[1]
    digits = []
    for k in range(SHORT_TERMS):
        residues = [(a_row * row + a_col * k + a_seed) % period for row in range(period)]
        residues.extend([0] * (stride - period))
        digits.extend(sum(digit << (8 * lane) for lane, digit in enumerate(residues[at:at + 8]))
                      for at in range(0, stride, 8))
    right = [[((b_row * k + b_col * col + b_seed) % b_mod) / b_mod for col in range(SHORT_TERMS)]
             for k in range(SHORT_TERMS)]
    records = []
    for terms in range(SHORT_TERMS):
        at = stride * terms
        initial = [fsum((value(data[at + row]),
                         value(data[SHORT_TERMS * stride + at + row]) if compensated else 0.0,
                         *seeds[row][:terms])) for row in range(period)]
        records.extend(map(bits, initial))
        records.extend([0] * (stride - period))
        begin = diag_at + period * terms
        records.extend(data[begin:begin + period])
        records.extend([0] * (stride - period))
        records.extend(bits(fsum(row[:terms]) / period) for row in right)
        # Cada producto está por debajo de uno y cada celda por debajo de 528.
        # Se acotan las r² operaciones de cola, la suma en orden y el cálculo factorizado.
        row_ulp = ulp(max(initial) + terms * terms + 1.0) / 2
        cell_error = 2.0**-44 + 2.0**-54
        uncertainty = 2 * ((terms + 3) * row_ulp + terms**2 * cell_error + 2.0**-40)
        records.append(bits(uncertainty))
    return digits, records


def interval_source(name, period, digits, records):
    stride = (period + 7) & ~7
    lanes = '\n'.join(f'    result = simdSet(result, {lane}, float(simdGet(octets, {lane})))' for lane in range(8))
    return f'''raw fn {name}Digits(word: u64) -> Simd<float, 8> {{
    let octets = bitcast<Simd<u8, 8>>(word)
    var result = simdSplat<Simd<float, 8>>(0.0)
{lanes}
    return result
}}

trusted fn {name}Interval(terms: int) -> (bool, float, float) {{
    let table = [
        {words_source(digits + records)}
    ]
    let data = slicePtr(table)
    let start = {len(digits)} + terms * {2 * stride + SHORT_TERMS + 1}
    let diagonal = start + {stride}
    let weights = diagonal + {stride}
    var limits = repeat(0.0, {2 * stride})
    var trace = 0.0
    var below = 0.0
    var above = 0.0
    raw {{
        let error = simdSplat<Simd<float, 8>>(bitcast<float>(ptrRead(ptrAdd(data, weights + {SHORT_TERMS}))))
        for block in 0..{stride // 8} {{
            let row = 8 * block
            let base = bitcast<Simd<float, 8>>(simdLoad<Simd<u64, 8>>(ptrAdd(data, start + row)))
            var tail = simdSplat<Simd<float, 8>>(0.0)
            for k in 0..terms {{
                let a = {name}Digits(ptrRead(ptrAdd(data, {stride // 8} * k + block)))
                let b = simdSplat<Simd<float, 8>>(bitcast<float>(ptrRead(ptrAdd(data, weights + k))))
                tail = simdFma(a, b, tail)
            }}
            let approximate = base + tail
            simdToArray(approximate - error, mut limits, row)
            simdToArray(approximate + error, mut limits, row + {stride})
        }}
        // Al sumar ambos extremos en el orden original, el resultado queda entre ellos.
        for cycle in 0..5 {{
            let rows = if cycle < 4 {{ {period} }} else {{ terms }}
            for row in 0..rows {{
                below = below + limits[row]
                above = above + limits[row + {stride}]
                trace = trace + bitcast<float>(ptrRead(ptrAdd(data, diagonal + row)))
            }}
        }}
        let scaled = above * 1000000.0
        let margin = (scaled + 1.0) * 4.440892098500626e-16
        let lowFixed = int(below * 1000000.0 + 0.5 - margin)
        let highFixed = int(scaled + 0.5 + margin)
        return (lowFixed == highFixed, trace, below)
    }}
}}'''


def row_tile(name, columns, stride, left_at, right_at, compensated):
    vector = 'Simd<float, 8>'
    state = f'({vector}, {vector})' if compensated else vector
    lines = [f'''raw fn {name}Tail{columns}(data: Ptr<u64>, row: int, col: int, terms: int, state: {state}) -> {state} {{''']
    for col in range(columns):
        lines.append(f'    var c{col} = {name}Seed(data, row, col + {col})')
    lines += ['    for k in 0..terms {',
              f'        let left = bitcast<{vector}>(simdLoad<Simd<u64, 8>>(ptrAdd(data, {left_at} + k * {stride} + row)))']
    for col in range(columns):
        lines.extend((f'        let right{col} = simdSplat<{vector}>(bitcast<float>(ptrRead(ptrAdd(data, {right_at} + {SHORT_TERMS} * k + col + {col}))))',
                      f'        c{col} = c{col} + left * right{col}'))
    lines.append('    }')
    if compensated:
        lines.extend(('    var hi = state.0', '    var lo = state.1'))
        for col in range(columns):
            lines.extend((f'    let next{col} = hi + c{col}',
                          f'    let delta{col} = simdSelect(simdCmpLe(c{col}, hi), (hi - next{col}) + c{col}, (c{col} - next{col}) + hi)',
                          f'    lo = lo + delta{col}', f'    hi = next{col}'))
        lines.append('    return (hi, lo)')
    else:
        lines.append('    return state' + ''.join(f' + c{col}' for col in range(columns)))
    return '\n'.join(lines + ['}'])


def short_checkpoint_source(name, period, generators, data, cycles, fixed_report=False):
    """Guarda estados parciales de sum(), nunca la traza ni el total final."""
    compensated = sys.version_info >= (3, 12)
    plan = short_data(period, *generators, data, cycles, compensated)
    if plan is None:
        return ''
    table, diag_at, seed_at, left_at, right_at = plan
    stride = (period + 7) & ~7
    phase_at = seed_at + 2 * period + 8
    seed_source = f'''raw fn {name}Seed(table: Ptr<u64>, row: int, col: int) -> Simd<float, 8> {{
    let shift = int(ptrRead(ptrAdd(table, {phase_at} + col)))
    let values = simdLoad<Simd<u64, 8>>(ptrAdd(table, {seed_at} + shift + row))
    let record = ptrRead(ptrAdd(table, {phase_at + period} + col * {stride // 8} + row / 8))
    let shifts = simdFromArray<Simd<u64, 8>>([u64(0), u64(8), u64(16), u64(24), u64(32), u64(40), u64(48), u64(56)], 0)
    let delta = (simdSplat<Simd<u64, 8>>(record) >> shifts) & simdSplat<Simd<u64, 8>>(u64(255))
    return bitcast<Simd<float, 8>>(values + delta - simdSplat<Simd<u64, 8>>(u64(128)))
}}'''
    hi = 'bitcast<Simd<float, 8>>(simdLoad<Simd<u64, 8>>(ptrAdd(view, terms * stride + row)))'
    lo = f'bitcast<Simd<float, 8>>(simdLoad<Simd<u64, 8>>(ptrAdd(view, {SHORT_TERMS * stride} + terms * stride + row)))'
    initial = f'({hi}, {lo})' if compensated else hi
    total = 'state.0 + state.1' if compensated else 'state'
    interval = ''
    attempt = ''
    if fixed_report and cycles == 4:
        interval = interval_source(name, period, *compact_interval_data(period, generators, data, plan, compensated))
        attempt = f'''    let certified = {name}Interval(terms)
    if certified.0 {{ return (certified.1, certified.2) }}
'''
    result_source = f'''trusted fn {name}(n: int) -> (float, float) {{
    let terms = n - {cycles * period}
{attempt}    let data = [
        {words_source(table)}
    ]
    let view = slicePtr(data)
    let period = {period}
    let stride = {stride}
    let complete = terms / 4
    var sums = repeat(0.0, stride)
    for group in 0..(stride / 8) {{
        let row = group * 8
        raw {{
            var state = {initial}
            for block in 0..complete {{
                state = {name}Tail4(view, row, 4 * block, terms, state)
            }}
            for col in (4 * complete)..terms {{
                state = {name}Tail1(view, row, col, terms, state)
            }}
            simdToArray({total}, mut sums, row)
        }}
    }}
    var trace = 0.0
    var total = 0.0
    let diagonal = {diag_at} + terms * period
    let counts = slicePtr(sums)
    // El llamante ya acotó terms. Solo se consumen filas lógicas.
    raw {{
        for cycle in 0..{cycles} {{
            for i in 0..period {{
                trace = trace + bitcast<float>(ptrRead(ptrAdd(view, diagonal + i)))
                total = total + ptrRead(ptrAdd(counts, i))
            }}
        }}
        for i in 0..terms {{
            trace = trace + bitcast<float>(ptrRead(ptrAdd(view, diagonal + i)))
            total = total + ptrRead(ptrAdd(counts, i))
        }}
    }}
    return (trace, total)
}}'''
    sources = (interval, seed_source, row_tile(name, 1, stride, left_at, right_at, compensated),
               row_tile(name, 4, stride, left_at, right_at, compensated), result_source)
    return '\n\n'.join(source for source in sources if source)
