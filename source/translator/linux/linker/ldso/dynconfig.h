//
// Created by 甘尧 on 2024/7/12.
//

#ifndef SWIFTVM_DYNCONFIG_H
#define SWIFTVM_DYNCONFIG_H

#define weak __attribute__((__weak__))
#define hidden __attribute__((__visibility__("hidden")))
#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))

#define a_crash abort

#if defined(__aarch64__)
#define CRTJMP(pc,sp) __asm__ __volatile__( \
	"mov sp,%1 ; br %0" : : "r"(pc), "r"(sp) : "memory" )
#endif

#define DEFAULT_STACK_SIZE 131072
#define DEFAULT_GUARD_SIZE 8192

#define DEFAULT_STACK_MAX (8<<20)
#define DEFAULT_GUARD_MAX (1<<20)

#endif  // SWIFTVM_DYNCONFIG_H
