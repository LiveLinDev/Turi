#[cfg(windows)]
fn rss() -> u64 {
    #[link(name = "kernel32")]
    extern "system" { fn GetCurrentProcess() -> isize; }
    #[link(name = "psapi")]
    extern "system" { fn GetProcessMemoryInfo(h: isize, m: *mut u8, cb: u32) -> i32; }
    #[repr(C)] struct Mem { cb: u32, faults: u32, peak: usize, other: [usize;7] }
    let mut m = Mem { cb: std::mem::size_of::<Mem>() as u32, faults: 0, peak: 0, other: [0;7] };
    unsafe { assert_ne!(GetProcessMemoryInfo(GetCurrentProcess(), &mut m as *mut _ as *mut u8, m.cb), 0); }
    m.peak as u64
}
#[cfg(target_os = "linux")]
fn rss() -> u64 {
    let status = std::fs::read_to_string("/proc/self/status").unwrap();
    status.lines().find(|l| l.starts_with("VmHWM:")).unwrap().split_whitespace().nth(1).unwrap().parse::<u64>().unwrap()*1024
}
fn main() {
    let args: Vec<i64> = std::env::args().skip(1).map(|x| x.parse().unwrap()).collect();
    let start = std::time::Instant::now();
    let mut sum = 0i64;
    for i in 0..args[3] { sum += std::hint::black_box(v_dispatch(args[0],args[1],args[2]+(i%4)*7)); }
    let ns = start.elapsed().as_nanos();
    println!("{} {} {} 0 0 0 0", sum, ns, rss());
}
