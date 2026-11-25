// binary-trees, idiomatic Rust: Box ownership.
struct Node { left: Option<Box<Node>>, right: Option<Box<Node>> }

fn build(depth: i32) -> Box<Node> {
    if depth == 0 {
        Box::new(Node { left: None, right: None })
    } else {
        Box::new(Node { left: Some(build(depth - 1)), right: Some(build(depth - 1)) })
    }
}

fn check(n: &Node) -> i64 {
    match (&n.left, &n.right) {
        (Some(l), Some(r)) => 1 + check(l) + check(r),
        _ => 1,
    }
}

fn main() {
    let n: i32 = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(10);
    let min_depth = 4;
    let max_depth = if min_depth + 2 > n { min_depth + 2 } else { n };
    let stretch = max_depth + 1;
    println!("{}", check(&build(stretch)));
    let long_tree = build(max_depth);
    let mut depth = min_depth;
    while depth <= max_depth {
        let iterations = 1i64 << (max_depth - depth + min_depth);
        let mut total = 0i64;
        for _ in 0..iterations {
            total += check(&build(depth));
        }
        println!("{}", total);
        depth += 2;
    }
    println!("{}", check(&long_tree));
}
