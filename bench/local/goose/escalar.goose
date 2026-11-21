import input;

fn main() {
    var x: i64 = read_seed();
    for i in 0..8000000 {
        x = (x * 48271 + 1) % 2147483647;
    }
    print(x);
}
