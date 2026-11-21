import input;

fn main() {
    let seed: i64 = read_seed();
    var total: i64 = 0;
    for r in 0..20000 {
        let n: i64 = 32 + (r + seed) % 2017;
        var a: i64[>..] = [];
        a.resize(n, r % 251);
        for i in 0..n { a[i] = (a[i] + i + seed) % 251; }
        total += a[(r + seed) % n];
    }
    print(total);
}
