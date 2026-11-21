package main

import (
	"bufio"
	"fmt"
	"os"
)

func main() {
	var seed int64
	if _, err := fmt.Fscan(bufio.NewReader(os.Stdin), &seed); err != nil || seed <= 0 {
		fmt.Fprintln(os.Stderr, "positive int64 seed required")
		os.Exit(1)
	}
	fmt.Println(checksum(seed))
}
