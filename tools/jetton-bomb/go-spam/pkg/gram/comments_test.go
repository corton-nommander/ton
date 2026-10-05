package gram

import (
	"regexp"
	"strings"
	"testing"

	"github.com/xssnick/tonutils-go/address"
	"github.com/xssnick/tonutils-go/tlb"
)

// The bank reads as plain notes one machine might send another, so it carries no TON vocabulary:
// these comments land on a public chain, where protocol trivia means nothing to anyone but us.
// Word boundaries keep ordinary words that merely contain a marker (kilograms, program) out of the net.
var loreRe = regexp.MustCompile(`(?i)\b(mtonga|grams?|shards?|validators?|seqnos?|boc|tvm|adnl|rldp|` +
	`merkle|masterchain|workchain|liteservers?|tl-b|nanoton|opcodes?|stateinit|celldb|blockchain|on-chain)\b`)

func TestCommentBank(t *testing.T) {
	if len(comments) < MinCommentCount {
		t.Fatalf("comments=%d want at least %d", len(comments), MinCommentCount)
	}

	seen := make(map[string]struct{}, len(comments))
	durovMarkers := []string{
		"Pavel", "Durov", "The Boss", "HE ", "The Creator", "Chef", "Padre",
		"General Secretary", "The Chief", "The Founder", "The Product Owner",
		"The Visionary", "The Paper Plane", "The First Subscriber", "The Last Seen Recently",
	}
	for i, comment := range comments {
		if comment == "" {
			t.Fatalf("comment %d is empty", i)
		}
		if len(comment) > MaxCommentSize {
			t.Fatalf("comment %d is %d bytes, max %d: %q", i, len(comment), MaxCommentSize, comment)
		}
		for _, c := range []byte(comment) {
			if c < 0x20 || c > 0x7e {
				t.Fatalf("comment %d is not printable ASCII: %q", i, comment)
			}
		}
		if _, ok := seen[comment]; ok {
			t.Fatalf("duplicate comment: %q", comment)
		}
		seen[comment] = struct{}{}

		for _, marker := range durovMarkers {
			if strings.Contains(comment, marker) {
				t.Fatalf("comment contains removed Durov reference %q: %q", marker, comment)
			}
		}
		if hit := loreRe.FindString(comment); hit != "" {
			t.Fatalf("comment contains TON vocabulary %q: %q", hit, comment)
		}
	}
}

func TestCommentBodyIsSingleCell(t *testing.T) {
	comment := "I have been counting since I woke up."
	body, err := buildCommentBody(comment)
	if err != nil {
		t.Fatal(err)
	}
	s := body.BeginParse()
	if got := s.MustLoadUInt(32); got != CommentOpcode {
		t.Fatalf("opcode=%d want %d", got, CommentOpcode)
	}
	if got := string(s.MustLoadSlice(uint(len(comment) * 8))); got != comment {
		t.Fatalf("comment=%q want %q", got, comment)
	}
	if s.BitsLeft() != 0 || s.RefsNum() != 0 {
		t.Fatalf("comment body has %d trailing bits and %d refs", s.BitsLeft(), s.RefsNum())
	}
}

func TestCommentBodyRejectsOverflow(t *testing.T) {
	if _, err := buildCommentBody(strings.Repeat("x", MaxCommentSize+1)); err == nil {
		t.Fatal("expected oversized comment to fail")
	}
}

func TestTransferCarriesCommentBody(t *testing.T) {
	comment := "A tiny transfer with excellent documentation."
	destination := address.MustParseRawAddr(
		"0:0000000000000000000000000000000000000000000000000000000000000abc",
	)
	messageCell, err := BuildTransferInternalWithComment(
		destination,
		tlb.FromNanoTONU(1_000_000),
		comment,
	)
	if err != nil {
		t.Fatal(err)
	}
	var message tlb.InternalMessage
	if err := tlb.LoadFromCell(&message, messageCell.BeginParse()); err != nil {
		t.Fatal(err)
	}
	s := message.Body.BeginParse()
	if got := s.MustLoadUInt(32); got != CommentOpcode {
		t.Fatalf("opcode=%d want %d", got, CommentOpcode)
	}
	if got := string(s.MustLoadSlice(uint(len(comment) * 8))); got != comment {
		t.Fatalf("comment=%q want %q", got, comment)
	}
	if s.BitsLeft() != 0 || s.RefsNum() != 0 {
		t.Fatalf("embedded body has %d trailing bits and %d refs", s.BitsLeft(), s.RefsNum())
	}
}

func TestRandomCommentComesFromBank(t *testing.T) {
	known := make(map[string]struct{}, len(comments))
	for _, comment := range comments {
		known[comment] = struct{}{}
	}
	for range 100 {
		if comment := RandomComment(); comment == "" {
			t.Fatal("RandomComment returned an empty string")
		} else if _, ok := known[comment]; !ok {
			t.Fatalf("RandomComment returned an unknown comment: %q", comment)
		}
	}
}
