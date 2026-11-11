/* Keep compiler-rt memory helpers local to this executable. Native imports
   and SDL/driver libraries must resolve their own optimized libc routines. */
__asm__(".hidden memcpy\n.hidden memmove\n.hidden memset\n.hidden memcmp\n.hidden bcmp\n");
