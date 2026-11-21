use std::io::{self, Read};

#[cfg(escalar)]
fn checksum(seed: i64) -> i64 {
    const MODULUS: i64 = 2_147_483_647;
    let mut x = seed;
    if x > (i64::MAX - 1) / 48_271 {
        x %= MODULUS;
    }
    for _ in 0..8_000_000 {
        x = (x * 48_271 + 1) % MODULUS;
    }
    x
}

#[cfg(arreglo)]
fn checksum(seed: i64) -> i64 {
    let mut values = vec![seed; 262_144];
    let offset = if seed > i64::MAX - 262_144 {
        seed % 1009
    } else {
        seed
    };
    for (i, value) in values.iter_mut().enumerate() {
        *value = (i as i64 + offset) % 1009;
    }
    for pass in 0..32 {
        for value in &mut values {
            *value = (*value + pass + 1) % 1009;
        }
    }
    values.iter().sum()
}

#[cfg(temporales)]
fn checksum(seed: i64) -> i64 {
    let mut total = 0;
    let large = seed > i64::MAX - 20_000;
    let size_offset = if large { seed % 2017 } else { seed };
    let value_offset = if large { seed % 251 } else { seed };
    for round in 0..20_000 {
        let n = 32 + (round + size_offset) % 2017;
        let mut values = vec![round % 251; n as usize];
        for (i, value) in values.iter_mut().enumerate() {
            *value = (*value + i as i64 + value_offset) % 251;
        }
        let index_offset = if large { seed % n } else { seed };
        total += values[((round + index_offset) % n) as usize];
    }
    total
}

fn main() {
    let mut input = String::new();
    io::stdin().read_to_string(&mut input).unwrap();
    let seed: i64 = input.trim().parse().expect("positive int64 seed");
    assert!(seed > 0, "positive int64 seed");
    println!("{}", checksum(seed));
}
