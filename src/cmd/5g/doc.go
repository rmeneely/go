// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

/*

5g is the version of the gc compiler for the ARM.
The $GOARCH for these tools is arm.

It reads .go files and outputs .5 files. The flags are documented in ../gc/doc.go.

There is no instruction optimizer, so the -N flag is a no-op.

*/
package documentation
