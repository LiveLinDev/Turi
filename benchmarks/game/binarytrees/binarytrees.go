// binary-trees, idiomatic Go: garbage-collected pointers.
package main

import (
	"fmt"
	"os"
	"strconv"
)

type Node struct{ left, right *Node }

func build(depth int) *Node {
	if depth == 0 {
		return &Node{}
	}
	return &Node{build(depth - 1), build(depth - 1)}
}

func check(n *Node) int {
	if n.left == nil {
		return 1
	}
	return 1 + check(n.left) + check(n.right)
}

func main() {
	n := 10
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	minDepth := 4
	maxDepth := n
	if minDepth+2 > n {
		maxDepth = minDepth + 2
	}
	stretch := maxDepth + 1
	fmt.Println(check(build(stretch)))
	longTree := build(maxDepth)
	for depth := minDepth; depth <= maxDepth; depth += 2 {
		iterations := 1 << (maxDepth - depth + minDepth)
		total := 0
		for i := 0; i < iterations; i++ {
			total += check(build(depth))
		}
		fmt.Println(total)
	}
	fmt.Println(check(longTree))
}
