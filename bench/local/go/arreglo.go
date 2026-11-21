//go:build arreglo

package main

func checksum(seed int64) int64 {
	values := make([]int64, 262_144)
	offset := seed
	if seed > int64(1<<63-1)-262_144 {
		offset %= 1009
	}
	for i := range values {
		values[i] = seed
	}
	for i := range values {
		values[i] = (int64(i) + offset) % 1009
	}
	for pass := int64(0); pass < 32; pass++ {
		for i := range values {
			values[i] = (values[i] + pass + 1) % 1009
		}
	}
	var sum int64
	for _, x := range values {
		sum += x
	}
	return sum
}
