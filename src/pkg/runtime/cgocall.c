// Copyright 2009 The Go Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "runtime.h"
#include "arch.h"
#include "stack.h"
#include "cgocall.h"

// Cgo call and callback support.
//
// To call into the C function f from Go, the cgo-generated code calls
// runtime.cgocall(_cgo_Cfunc_f, frame), where _cgo_Cfunc_f is a
// gcc-compiled function written by cgo.
//
// runtime.cgocall (below) locks g to m, calls entersyscall
// so as not to block other goroutines or the garbage collector,
// and then calls runtime.asmcgocall(_cgo_Cfunc_f, frame). 
//
// runtime.asmcgocall (in $GOARCH/asm.s) switches to the m->g0 stack
// (assumed to be an operating system-allocated stack, so safe to run
// gcc-compiled code on) and calls _cgo_Cfunc_f(frame).
//
// _cgo_Cfunc_f invokes the actual C function f with arguments
// taken from the frame structure, records the results in the frame,
// and returns to runtime.asmcgocall.
//
// After it regains control, runtime.asmcgocall switches back to the
// original g (m->curg)'s stack and returns to runtime.cgocall.
//
// After it regains control, runtime.cgocall calls exitsyscall, which blocks
// until this m can run Go code without violating the $GOMAXPROCS limit,
// and then unlocks g from m.
//
// The above description skipped over the possibility of the gcc-compiled
// function f calling back into Go.  If that happens, we continue down
// the rabbit hole during the execution of f.
//
// To make it possible for gcc-compiled C code to call a Go function p.GoF,
// cgo writes a gcc-compiled function named GoF (not p.GoF, since gcc doesn't
// know about packages).  The gcc-compiled C function f calls GoF.
//
// GoF calls crosscall2(_cgoexp_GoF, frame, framesize).  Crosscall2
// (in cgo/$GOOS.S, a gcc-compiled assembly file) is a two-argument
// adapter from the gcc function call ABI to the 6c function call ABI.
// It is called from gcc to call 6c functions.  In this case it calls
// _cgoexp_GoF(frame, framesize), still running on m->g0's stack
// and outside the $GOMAXPROCS limit.  Thus, this code cannot yet
// call arbitrary Go code directly and must be careful not to allocate
// memory or use up m->g0's stack.
//
// _cgoexp_GoF calls runtime.cgocallback(p.GoF, frame, framesize).
// (The reason for having _cgoexp_GoF instead of writing a crosscall3
// to make this call directly is that _cgoexp_GoF, because it is compiled
// with 6c instead of gcc, can refer to dotted names like
// runtime.cgocallback and p.GoF.)
//
// runtime.cgocallback (in $GOOS/asm.s) switches from m->g0's
// stack to the original g (m->curg)'s stack, on which it calls
// runtime.cgocallbackg(p.GoF, frame, framesize).
// As part of the stack switch, runtime.cgocallback saves the current
// SP as m->g0->sched.sp, so that any use of m->g0's stack during the
// execution of the callback will be done below the existing stack frames.
// Before overwriting m->g0->sched.sp, it pushes the old value on the
// m->g0 stack, so that it can be restored later.
//
// runtime.cgocallbackg (below) is now running on a real goroutine
// stack (not an m->g0 stack).  First it calls runtime.exitsyscall, which will
// block until the $GOMAXPROCS limit allows running this goroutine.
// Once exitsyscall has returned, it is safe to do things like call the memory
// allocator or invoke the Go callback function p.GoF.  runtime.cgocallbackg
// first defers a function to unwind m->g0.sched.sp, so that if p.GoF
// panics, m->g0.sched.sp will be restored to its old value: the m->g0 stack
// and the m->curg stack will be unwound in lock step.
// Then it calls p.GoF.  Finally it pops but does not execute the deferred
// function, calls runtime.entersyscall, and returns to runtime.cgocallback.
//
// After it regains control, runtime.cgocallback switches back to
// m->g0's stack (the pointer is still in m->g0.sched.sp), restores the old
// m->g0.sched.sp value from the stack, and returns to _cgoexp_GoF.
//
// _cgoexp_GoF immediately returns to crosscall2, which restores the
// callee-save registers for gcc and returns to GoF, which returns to f.

void *initcgo;	/* filled in by dynamic linker when Cgo is available */

static void unlockm(void);
static void unwindm(void);

// Call from Go to C.

void
runtime·cgocall(void (*fn)(void*), void *arg)
{
	Defer d;

	if(!runtime·iscgo)
		runtime·throw("cgocall unavailable");

	if(fn == 0)
		runtime·throw("cgocall nil");

	m->ncgocall++;

	/*
	 * Lock g to m to ensure we stay on the same stack if we do a
	 * cgo callback.
	 */
	d.nofree = false;
	if(m->lockedg == nil) {
		m->lockedg = g;
		g->lockedm = m;

		// Add entry to defer stack in case of panic.
		d.fn = (byte*)unlockm;
		d.siz = 0;
		d.link = g->defer;
		d.argp = (void*)-1;  // unused because unlockm never recovers
		d.nofree = true;
		g->defer = &d;
	}

	/*
	 * Announce we are entering a system call
	 * so that the scheduler knows to create another
	 * M to run goroutines while we are in the
	 * foreign code.
	 *
	 * The call to asmcgocall is guaranteed not to
	 * split the stack and does not allocate memory,
	 * so it is safe to call while "in a system call", outside
	 * the $GOMAXPROCS accounting.
	 */
	runtime·entersyscall();
	runtime·asmcgocall(fn, arg);
	runtime·exitsyscall();

	if(d.nofree) {
		if(g->defer != &d || d.fn != (byte*)unlockm)
			runtime·throw("runtime: bad defer entry in cgocallback");
		g->defer = d.link;
		unlockm();
	}
}

static void
unlockm(void)
{
	m->lockedg = nil;
	g->lockedm = nil;
}

void
runtime·Cgocalls(int64 ret)
{
	M *m;

	ret = 0;
	for(m=runtime·atomicloadp(&runtime·allm); m; m=m->alllink)
		ret += m->ncgocall;
	FLUSH(&ret);
}

// Helper functions for cgo code.

void (*_cgo_malloc)(void*);
void (*_cgo_free)(void*);

void*
runtime·cmalloc(uintptr n)
{
	struct {
		uint64 n;
		void *ret;
	} a;

	a.n = n;
	a.ret = nil;
	runtime·cgocall(_cgo_malloc, &a);
	return a.ret;
}

void
runtime·cfree(void *p)
{
	runtime·cgocall(_cgo_free, p);
}

// Call from C back to Go.

void
runtime·cgocallbackg(void (*fn)(void), void *arg, uintptr argsize)
{
	Defer d;

	if(g != m->curg)
		runtime·throw("runtime: bad g in cgocallback");

	runtime·exitsyscall();	// coming out of cgo call

	// Add entry to defer stack in case of panic.
	d.fn = (byte*)unwindm;
	d.siz = 0;
	d.link = g->defer;
	d.argp = (void*)-1;  // unused because unwindm never recovers
	d.nofree = true;
	g->defer = &d;

	// Invoke callback.
	reflect·call((byte*)fn, arg, argsize);

	// Pop defer.
	// Do not unwind m->g0->sched.sp.
	// Our caller, cgocallback, will do that.
	if(g->defer != &d || d.fn != (byte*)unwindm)
		runtime·throw("runtime: bad defer entry in cgocallback");
	g->defer = d.link;

	runtime·entersyscall();	// going back to cgo call
}

static void
unwindm(void)
{
	// Restore sp saved by cgocallback during
	// unwind of g's stack (see comment at top of file).
	switch(thechar){
	default:
		runtime·throw("runtime: unwindm not implemented");
	case '8':
	case '6':
		m->g0->sched.sp = *(void**)m->g0->sched.sp;
		break;
	}
}

void
runtime·badcgocallback(void)	// called from assembly
{
	runtime·throw("runtime: misaligned stack in cgocallback");
}

void
runtime·cgounimpl(void)	// called from (incomplete) assembly
{
	runtime·throw("runtime: cgo not implemented");
}
