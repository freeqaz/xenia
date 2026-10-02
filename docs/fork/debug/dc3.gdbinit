# gdbinit for debugging DC3 guest PowerPC code via the xenia-headless RSP stub.
#
# Xbox 360 is 64-bit big-endian PowerPC. Host gdb 17.2 SIGSEGVs on
# `target remote` unless the architecture/endianness are set FIRST, so set them
# here before connecting.
#
# Usage:
#   gdb -ix /path/to/xenia/docs/dc3.gdbinit
#   (gdb) target remote 127.0.0.1:9001
#
# NOTE: gdb 17.2 can still crash in its PPC frame unwinder when attaching to a
# paused, symbol-less, multi-threaded target. The robust client is
# docs/dc3_rsp_client.py. If you use gdb, attach to a RUNNING target
# (launch with --dc3_gdb_rsp_break_on_connect=false) and let a breakpoint stop
# it.

set pagination off
set confirm off
set architecture powerpc:common64
set endian big

# Convenience: connect with `dc3connect`, optionally `dc3connect 9001`.
define dc3connect
  if $argc == 0
    target remote 127.0.0.1:9001
  else
    eval "target remote 127.0.0.1:%d", $arg0
  end
end
document dc3connect
Connect to the xenia-headless RSP stub. Usage: dc3connect [port]
end
