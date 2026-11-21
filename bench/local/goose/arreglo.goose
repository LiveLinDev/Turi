import input;

fn main() {
    let seed: i64 = read_seed();
    let n: i64 = 262144;
    var a: i64[>..] = [];
    a.resize(n, seed);
    for i in 0..n { a[i] = (i + seed) % 1009; }
    for p in 0..32 {
        for i in 0..n { a[i] = (a[i] + p + 1) % 1009; }
    }
    var total: i64 = 0;
    for i in 0..n { total += a[i]; }
    print(total);
}
