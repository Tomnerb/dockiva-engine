package main

import (
	"reflect"
	"testing"
)

// The runtime's published table: records, then usage lines (older tables have none).
func TestTopRows(t *testing.T) {
	table := "1 0 0 8 sh\x00-c\x00x\x00\n2 1 70 4 yes\x00\n=boot 1 2 3 4 0.05 0.01 0.00 5\n=1 0 0 0 9760 1596 1 S\n=2 6100 140 0 9740 1564 1 R\n"
	want := [][]string{{"root", "1", "0", "0", "10:00", "?", "00:00:00", "sh -c x"}, {"70", "2", "1", "0", "10:00", "?", "00:01:02", "yes"}}
	if got := topRows([]byte(table), "10:00"); !reflect.DeepEqual(got, want) {
		t.Fatalf("got %q", got)
	}
	if got := topRows([]byte("1 0 0 3 sh\x00\n"), "?"); len(got) != 1 || got[0][6] != "00:00:00" {
		t.Fatalf("old table: %q", got)
	}
}
