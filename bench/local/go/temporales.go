//go:build temporales

package main

func checksum(seed int64) int64 {
	var sum int64
	large := seed > int64(1<<63-1)-20_000
	sizeOffset, valueOffset := seed, seed
	if large {
		sizeOffset %= 2017
		valueOffset %= 251
	}
	for round := int64(0); round < 20_000; round++ {
		n := 32 + (round+sizeOffset)%2017
		values := make([]int64, n)
		for i := range values {
			values[i] = round % 251
		}
		for i := range values {
			values[i] = (values[i] + int64(i) + valueOffset) % 251
		}
		indexOffset := seed
		if large {
			indexOffset %= n
		}
		sum += values[(round+indexOffset)%n]
	}
	return sum
}
