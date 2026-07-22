package goentry_test

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/jvc56/MAGPIE/goentry"
)

// magpieDataPath returns the colon-separated data paths for testing.
// Uses MAGPIE_DATA_PATH env var if set, otherwise resolves relative to this
// file so the test works whether invoked from goentry/ or the repo root.
func magpieDataPath(t *testing.T) string {
	t.Helper()
	if v := os.Getenv("MAGPIE_DATA_PATH"); v != "" {
		return v
	}
	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Skip("cannot determine test file path; set MAGPIE_DATA_PATH")
	}
	repoRoot := filepath.Dir(filepath.Dir(thisFile))
	testdata := filepath.Join(repoRoot, "testdata")
	data := filepath.Join(repoRoot, "data")
	if _, err := os.Stat(testdata); err != nil {
		t.Skipf("testdata not found at %s; run ./download_data.sh", testdata)
	}
	return testdata + ":" + data
}

func TestEngineCreateDestroy(t *testing.T) {
	dataPath := magpieDataPath(t)
	eng := goentry.New(dataPath)
	if eng == nil {
		t.Fatal("New returned nil")
	}
	eng.Close()
	eng.Close() // second Close must be a no-op
}

func TestEngineRunSetAndGen(t *testing.T) {
	dataPath := magpieDataPath(t)
	eng := goentry.New(dataPath)
	if eng == nil {
		t.Fatal("New returned nil")
	}
	defer eng.Close()

	out, errMsg := eng.Run("set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all -numplays 1")
	if errMsg != "" {
		t.Fatalf("set command error: %s", errMsg)
	}
	_ = out

	// Standard empty board, rack AAABCDE.
	const cgpCmd = "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AAABCDE/ 0/0 0"
	out, errMsg = eng.Run(cgpCmd)
	if errMsg != "" {
		t.Fatalf("cgp command error: %s", errMsg)
	}
	_ = out

	out, errMsg = eng.Run("gen")
	if errMsg != "" {
		t.Fatalf("gen command error: %s", errMsg)
	}
	if strings.TrimSpace(out) == "" {
		t.Fatal("gen returned empty output; expected move list")
	}
}

func TestEngineThreadStatus(t *testing.T) {
	dataPath := magpieDataPath(t)
	eng := goentry.New(dataPath)
	if eng == nil {
		t.Fatal("New returned nil")
	}
	defer eng.Close()

	// After creation and before any command the status should be uninitialized (0)
	// or finished (3) — anything non-started is acceptable here.
	status := eng.ThreadStatus()
	if status == 1 {
		t.Errorf("unexpected STARTED status before any command: %d", status)
	}
}
