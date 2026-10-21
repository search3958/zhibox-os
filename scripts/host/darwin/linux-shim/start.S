/* SPDX-License-Identifier: GPL-2.0 */
/*
 * freestanding Linux バイナリ用のエントリポイント (x86_64)
 * main() を呼び、戻り値を exit(2) に渡す。
 */
	.section .text.start, "ax"
	.globl _start
	.type _start, @function
_start:
	andq	$-16, %rsp
	xorl	%ebp, %ebp
	call	main
	movl	%eax, %edi
	movl	$60, %eax		/* __NR_exit */
	syscall
	hlt
	.size _start, . - _start
