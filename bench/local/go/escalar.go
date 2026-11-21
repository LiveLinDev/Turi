//go:build escalar

package main

func checksum(seed int64) int64 {
	const modulus int64 = 2_147_483_647
	x := seed
	if x > (int64(1<<63-1)-1)/48_271 {
		x %= modulus
	}
	for i := 0; i < 8_000_000; i++ {
		x = (x*48_271 + 1) % modulus
	}
	return x
}
