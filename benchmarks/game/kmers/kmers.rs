// k-mer counting, idiomatic Rust: HashMap keyed by byte slices of the sequence.
use std::collections::HashMap;

fn main() {
    let n: usize = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(10000);
    let k = 8;
    let mut seq: Vec<u8> = Vec::with_capacity(n);
    let mut seed: i64 = 42;
    for _ in 0..n {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        seq.push(b"ACGT"[((seed / 65536) % 4) as usize]);
    }
    let mut counts: HashMap<&[u8], i64> = HashMap::new();
    for i in 0..(n + 1).saturating_sub(k) {
        *counts.entry(&seq[i..i + k]).or_insert(0) += 1;
    }
    let mut best = 0i64;
    let mut checksum = 0i64;
    for &c in counts.values() {
        if c > best {
            best = c;
        }
        checksum = (checksum + c * c) % 1000000007;
    }
    println!("{}\n{}\n{}", counts.len(), best, checksum);
}
