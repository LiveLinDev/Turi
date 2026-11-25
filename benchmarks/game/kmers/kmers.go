// k-mer counting, idiomatic Go: built-in map keyed by substrings.
package main

import (
	"fmt"
	"os"
	"strconv"
)

func main() {
	n := 10000
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	const k = 8
	buf := make([]byte, n)
	seed := 42
	for i := 0; i < n; i++ {
		seed = (seed*1103515245 + 12345) % 2147483648
		buf[i] = "ACGT"[(seed/65536)%4]
	}
	seq := string(buf)
	counts := make(map[string]int)
	for i := 0; i+k <= n; i++ {
		counts[seq[i:i+k]]++
	}
	best, checksum := 0, 0
	for _, c := range counts {
		if c > best {
			best = c
		}
		checksum = (checksum + c*c) % 1000000007
	}
	fmt.Println(len(counts))
	fmt.Println(best)
	fmt.Println(checksum)
}
