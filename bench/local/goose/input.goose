import std;
import os;

fn read_seed() -> i64 {
    var line: u8[>..] = [];
    guard read_line(line) else { abort("missing seed"); }
    let value, ok = parse_int(trim(line));
    guard ok && value > 0 else { abort("expected a positive decimal seed"); }
    return value;
}
