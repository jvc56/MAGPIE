// Package goentry provides a Go interface to the MAGPIE word-game engine.
// It wraps the C cmd_api.h embedding API via cgo.
//
// Each Engine holds an opaque MAGPIE handle and is NOT safe for concurrent
// use. Callers that want parallelism should create one Engine per goroutine.
//
// Prerequisites: libmagpie_core.a must be built before compiling this package:
//
//	make -C .. libmagpie_core.a BUILD=release
package goentry

/*
#cgo CFLAGS: -I${SRCDIR}/..
#cgo LDFLAGS: ${SRCDIR}/../libmagpie_core.a -lpthread -lm
#include "src/impl/cmd_api.h"
#include <stdlib.h>
*/
import "C"
import "unsafe"

// Engine wraps a MAGPIE handle. Create with New; close with Close.
type Engine struct {
	mp *C.Magpie
}

// New creates an Engine backed by the MAGPIE data at dataPaths.
// dataPaths is passed directly to magpie_create (colon-separated on POSIX).
func New(dataPaths string) *Engine {
	cs := C.CString(dataPaths)
	defer C.free(unsafe.Pointer(cs))
	return &Engine{mp: C.magpie_create(cs)}
}

// Close frees the engine. The Engine must not be used after Close returns.
func (e *Engine) Close() {
	if e.mp != nil {
		C.magpie_destroy(e.mp)
		e.mp = nil
	}
}

// Run executes a MAGPIE command string synchronously.
// Returns (output, errMsg). errMsg is empty on success.
func (e *Engine) Run(cmd string) (string, string) {
	cs := C.CString(cmd)
	defer C.free(unsafe.Pointer(cs))
	C.magpie_run_sync(e.mp, cs)
	return goStr(C.magpie_get_last_command_output(e.mp)),
		goStr(C.magpie_get_and_clear_error(e.mp))
}

// Stop signals the engine to interrupt the currently running command.
func (e *Engine) Stop() {
	C.magpie_stop_current_command(e.mp)
}

// ThreadStatus returns the execution state of the current command:
//
//	0 = uninitialized, 1 = started, 2 = user_interrupt, 3 = finished
func (e *Engine) ThreadStatus() int {
	return int(C.magpie_get_thread_status(e.mp))
}

// goStr converts a C string returned by MAGPIE (caller must free) to a Go
// string, then frees the C memory.
func goStr(cs *C.char) string {
	if cs == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(cs))
	return C.GoString(cs)
}
