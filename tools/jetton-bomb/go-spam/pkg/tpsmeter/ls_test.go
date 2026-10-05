package tpsmeter

import "testing"

func TestClassifyTransactionPage(t *testing.T) {
	tests := []struct {
		name        string
		more        bool
		count       int
		wantDone    bool
		wantPartial bool
	}{
		{name: "complete", count: 10, wantDone: true},
		{name: "continue", more: true, count: 10},
		{name: "empty but more", more: true, wantDone: true, wantPartial: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			done, partial := classifyTransactionPage(test.more, test.count)
			if done != test.wantDone || partial != test.wantPartial {
				t.Fatalf("done=%v partial=%v, want %v/%v", done, partial, test.wantDone, test.wantPartial)
			}
		})
	}
}
