package gram

import (
	_ "embed"
	"math/rand"
	"strings"
)

const (
	CommentOpcode  = 0
	MaxCommentSize = (1023 - 32) / 8
	// MinCommentCount is a floor, not the exact size: the bank is curated by hand, so lines come and
	// go. It only guards against an embed that lost or truncated its contents.
	MinCommentCount = 100
)

//go:embed comments.txt
var commentsFile string

var comments = loadComments()

func loadComments() []string {
	loaded := strings.Split(strings.TrimSuffix(commentsFile, "\n"), "\n")
	if len(loaded) < MinCommentCount {
		panic("gram: embedded comment bank is missing or truncated")
	}
	return loaded
}

// RandomComment returns one comment from the embedded bank.
func RandomComment() string {
	return comments[rand.Intn(len(comments))]
}
